#include "CustomEpdFont.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <algorithm>

CustomEpdFont::CustomEpdFont(const String& filePath, const EpdFontData* data, uint32_t offsetIntervals,
                             uint32_t offsetGlyphs, uint32_t offsetBitmaps)
    : EpdFont(data),
      filePath(filePath),
      offsetIntervals(offsetIntervals),
      offsetGlyphs(offsetGlyphs),
      offsetBitmaps(offsetBitmaps) {
  // Initialize bitmap cache
  for (size_t i = 0; i < BITMAP_CACHE_CAPACITY; i++) {
    bitmapCache[i].data = nullptr;
    bitmapCache[i].size = 0;
    bitmapCache[i].codePoint = 0;
    bitmapCache[i].lastAccess = 0;
  }
  // Initialize glyph cache
  for (size_t i = 0; i < GLYPH_CACHE_CAPACITY; i++) {
    glyphCache[i].codePoint = 0xFFFFFFFF;
    glyphCache[i].lastAccess = 0;
  }
}

CustomEpdFont::~CustomEpdFont() {
  clearCache();
  if (fontFile.isOpen()) {
    fontFile.close();
  }
}

void CustomEpdFont::clearCache() const {
  for (size_t i = 0; i < BITMAP_CACHE_CAPACITY; i++) {
    if (bitmapCache[i].data) {
      free(bitmapCache[i].data);
      bitmapCache[i].data = nullptr;
    }
    bitmapCache[i].size = 0;
  }
}

const EpdGlyph* CustomEpdFont::getGlyph(uint32_t cp, const EpdFontStyles::Style style) const {
  // Serial.printf("CustomEpdFont::getGlyph cp=%u style=%d this=%p\n", cp, style, this);

  // Check glyph cache first
  for (size_t i = 0; i < GLYPH_CACHE_CAPACITY; i++) {
    if (glyphCache[i].codePoint == cp) {
      glyphCache[i].lastAccess = ++currentAccessCount;
      // Serial.printf("  Cache hit: %p\n", &glyphCache[i].glyph);
      return &glyphCache[i].glyph;
    }
  }

  const EpdFontData* data = getData(style);
  if (!data) {
    Serial.println("CustomEpdFont::getGlyph: No data!");
    return nullptr;
  }

  const EpdUnicodeInterval* intervals = data->intervals;
  const int count = data->intervalCount;

  uint32_t currentCp = cp;
  bool triedFallback = false;

  // Loop to allow for fallback attempts
  while (true) {
    // Check glyph cache first
    for (size_t i = 0; i < GLYPH_CACHE_CAPACITY; i++) {
      if (glyphCache[i].codePoint == currentCp) {
        glyphCache[i].lastAccess = ++currentAccessCount;
        // Serial.printf("  Cache hit: %p\n", &glyphCache[i].glyph);
        return &glyphCache[i].glyph;
      }
    }

    const EpdFontData* data = getData(style);
    if (!data) {
      Serial.println("CustomEpdFont::getGlyph: No data!");
      return nullptr;
    }

    const EpdUnicodeInterval* intervals = data->intervals;
    const int count = data->intervalCount;

    int left = 0;
    int right = count - 1;

    bool foundInterval = false;
    uint32_t glyphIndex = 0;
    const EpdUnicodeInterval* foundIntervalPtr = nullptr;

    while (left <= right) {
      const int mid = left + (right - left) / 2;
      const EpdUnicodeInterval* interval = &intervals[mid];

      if (currentCp < interval->first) {
        right = mid - 1;
      } else if (currentCp > interval->last) {
        left = mid + 1;
      } else {
        // Found interval. Calculate index.
        glyphIndex = interval->offset + (currentCp - interval->first);
        foundIntervalPtr = interval;
        foundInterval = true;
        break;
      }
    }

    if (foundInterval) {
      // Calculate total glyphs to ensure bounds safety
      uint32_t totalGlyphCount = (offsetBitmaps - offsetGlyphs) / 13;
      if (glyphIndex >= totalGlyphCount) {
        Serial.printf("CustomEpdFont: Glyph index %u out of bounds (total %u)\n", glyphIndex, totalGlyphCount);
        // If out of bounds, and we haven't tried fallback, try it.
        if (!triedFallback) {
          if (currentCp == 0x2018 || currentCp == 0x2019) {
            currentCp = 0x0027;
            triedFallback = true;
            continue;
          } else if (currentCp == 0x201C || currentCp == 0x201D) {
            currentCp = 0x0022;
            triedFallback = true;
            continue;
          }
        }
        return nullptr;
      }

      uint32_t glyphFileOffset = offsetGlyphs + (glyphIndex * 13);

      if (!fontFile.isOpen()) {
        if (!SdMan.openFileForRead("CustomFont", filePath.c_str(), fontFile)) {
          Serial.printf("CustomEpdFont: Failed to open file %s\n", filePath.c_str());
          return nullptr;
        }
      }

      if (!fontFile.seekSet(glyphFileOffset)) {
        Serial.printf("CustomEpdFont: Failed to seek to glyph offset %u\n", glyphFileOffset);
        fontFile.close();
        return nullptr;
      }

      uint8_t glyphBuf[13];
      if (fontFile.read(glyphBuf, 13) != 13) {
        Serial.println("CustomEpdFont: Read failed (glyph entry)");
        fontFile.close();
        return nullptr;
      }

      uint8_t w = glyphBuf[0];
      uint8_t h = glyphBuf[1];
      uint8_t adv = glyphBuf[2];
      int8_t l = (int8_t)glyphBuf[3];
      // glyphBuf[4] unused
      int8_t t = (int8_t)glyphBuf[5];
      // glyphBuf[6] unused
      uint16_t dLen = glyphBuf[7] | (glyphBuf[8] << 8);
      uint32_t dOffset = glyphBuf[9] | (glyphBuf[10] << 8) | (glyphBuf[11] << 16) | (glyphBuf[12] << 24);

      /*
      Serial.printf("[CEF] Parsed Glyph %u: Off=%u, Len=%u, W=%u, H=%u, L=%d, T=%d\n",
                    glyphIndex, dOffset, dLen, w, h, l, t);
      */

      // Removed individual reads since we read all 13 bytes

      // fontFile.close();  // Keep file open for performance

      // Find slot in glyph cache (LRU)
      int slotIndex = -1;
      uint32_t minAccess = 0xFFFFFFFF;
      for (size_t i = 0; i < GLYPH_CACHE_CAPACITY; i++) {
        if (glyphCache[i].codePoint == 0xFFFFFFFF) {
          slotIndex = i;
          break;
        }
        if (glyphCache[i].lastAccess < minAccess) {
          minAccess = glyphCache[i].lastAccess;
          slotIndex = i;
        }
      }

      // Populate cache
      glyphCache[slotIndex].codePoint = currentCp;
      glyphCache[slotIndex].lastAccess = ++currentAccessCount;
      glyphCache[slotIndex].glyph.dataOffset = dOffset;
      glyphCache[slotIndex].glyph.dataLength = dLen;
      glyphCache[slotIndex].glyph.width = w;
      glyphCache[slotIndex].glyph.height = h;
      glyphCache[slotIndex].glyph.advanceX = adv;
      glyphCache[slotIndex].glyph.left = l;
      glyphCache[slotIndex].glyph.top = t;

      // Serial.printf("  Loaded to cache[%d]: %p\n", slotIndex, &glyphCache[slotIndex].glyph);
      return &glyphCache[slotIndex].glyph;
    }
    // Not found in intervals. Try fallback.
    if (!triedFallback) {
      if (currentCp == 0x2018 || currentCp == 0x2019) {  // Left/Right single quote
        currentCp = 0x0027;                              // ASCII apostrophe
        triedFallback = true;
        continue;                                               // Retry with fallback CP
      } else if (currentCp == 0x201C || currentCp == 0x201D) {  // Left/Right double quote
        currentCp = 0x0022;                                     // ASCII double quote
        triedFallback = true;
        continue;                     // Retry with fallback CP
      } else if (currentCp == 160) {  // Non-breaking space
        currentCp = 32;               // Space
        triedFallback = true;
        continue;
      }
    }

    return nullptr;
  }

  return nullptr;
}

const uint8_t* CustomEpdFont::loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                              const EpdFontStyles::Style style) const {
  if (!glyph) return nullptr;
  // Serial.printf("CustomEpdFont::loadGlyphBitmap glyph=%p len=%u\n", glyph, glyph->dataLength);

  if (glyph->dataLength == 0) {
    return nullptr;  // Empty glyph
  }
  if (glyph->dataLength > 32768) {
    Serial.printf("CustomEpdFont: Glyph too large (%u)\n", glyph->dataLength);
    return nullptr;
  }

  // Serial.printf("[CEF] loadGlyphBitmap: len=%u, off=%u\n", glyph->dataLength, glyph->dataOffset);

  uint32_t offset = glyph->dataOffset;

  // Check bitmap cache
  for (size_t i = 0; i < BITMAP_CACHE_CAPACITY; i++) {
    if (bitmapCache[i].data && bitmapCache[i].codePoint == offset) {
      bitmapCache[i].lastAccess = ++currentAccessCount;
      if (buffer) {
        memcpy(buffer, bitmapCache[i].data, std::min((size_t)glyph->dataLength, (size_t)bitmapCache[i].size));
        return buffer;
      }
      return bitmapCache[i].data;
    }
  }

  // Cache miss - read from SD
  if (!fontFile.isOpen()) {
    if (!SdMan.openFileForRead("CustomFont", filePath.c_str(), fontFile)) {
      Serial.printf("Failed to open font file: %s\n", filePath.c_str());
      return nullptr;
    }
  }

  if (!fontFile.seekSet(offsetBitmaps + offset)) {
    Serial.printf("CustomEpdFont: Failed to seek to bitmap offset %u\n", offsetBitmaps + offset);
    fontFile.close();
    return nullptr;
  }

  // Allocate memory manually
  uint8_t* newData = (uint8_t*)malloc(glyph->dataLength);
  if (!newData) {
    Serial.println("CustomEpdFont: MALLOC FAILED");
    fontFile.close();
    return nullptr;
  }

  size_t bytesRead = fontFile.read(newData, glyph->dataLength);
  // fontFile.close(); // Keep file open

  if (bytesRead != glyph->dataLength) {
    Serial.printf("CustomEpdFont: Read mismatch. Expected %u, got %u\n", glyph->dataLength, bytesRead);
    free(newData);
    return nullptr;
  }

  // Find slot in bitmap cache (LRU)
  int slotIndex = -1;
  for (size_t i = 0; i < BITMAP_CACHE_CAPACITY; i++) {
    if (bitmapCache[i].data == nullptr) {
      slotIndex = i;
      break;
    }
  }

  if (slotIndex == -1) {
    uint32_t minAccess = 0xFFFFFFFF;
    for (size_t i = 0; i < BITMAP_CACHE_CAPACITY; i++) {
      if (bitmapCache[i].lastAccess < minAccess) {
        minAccess = bitmapCache[i].lastAccess;
        slotIndex = i;
      }
    }

    // Free evicted slot
    if (bitmapCache[slotIndex].data) {
      free(bitmapCache[slotIndex].data);
      bitmapCache[slotIndex].data = nullptr;
    }
  }

  // Store in cache
  bitmapCache[slotIndex].codePoint = offset;
  bitmapCache[slotIndex].lastAccess = ++currentAccessCount;
  bitmapCache[slotIndex].data = newData;
  bitmapCache[slotIndex].size = glyph->dataLength;

  if (buffer) {
    memcpy(buffer, newData, glyph->dataLength);
    return buffer;
  }

  return newData;
}
