#include "FontManager.h"

#include <GfxRenderer.h>  // for EpdFontData usage validation if needed
#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <algorithm>

#include "CustomEpdFont.h"

FontManager& FontManager::getInstance() {
  static FontManager instance;
  return instance;
}

FontManager::~FontManager() {
  for (auto& familyPair : loadedFonts) {
    for (auto& sizePair : familyPair.second) {
      delete sizePair.second;
    }
  }
}

const std::vector<std::string>& FontManager::getAvailableFamilies() {
  if (!scanned) {
    scanFonts();
  }
  return availableFamilies;
}

void FontManager::scanFonts() {
  Serial.println("[FM] Scanning fonts...");
  availableFamilies.clear();
  scanned = true;

  FsFile fontDir;
  if (!SdMan.openFileForRead("FontScan", "/fonts", fontDir)) {
    Serial.println("[FM] Failed to open /fonts directory");
    // Even if failed, we proceed to sort empty list to avoid crashes
    return;
  }

  if (!fontDir.isDirectory()) {
    Serial.println("[FM] /fonts is not a directory");
    fontDir.close();
    return;
  }

  Serial.println("[FM] /fonts opened. Iterating files...");
  FsFile file;
  while (file.openNext(&fontDir, O_READ)) {
    if (!file.isDirectory()) {
      char filename[128];
      file.getName(filename, sizeof(filename));
      Serial.printf("[FM] Checking: %s\n", filename);

      String name = String(filename);
      if (name.endsWith(".epdfont")) {
        // Expected format: Family-Style-Size.epdfont
        int firstDash = name.indexOf('-');
        if (firstDash > 0) {
          String family = name.substring(0, firstDash);
          if (std::find(availableFamilies.begin(), availableFamilies.end(), family.c_str()) ==
              availableFamilies.end()) {
            availableFamilies.push_back(family.c_str());
            Serial.printf("[FM] Added family: %s\n", family.c_str());
          }
        }
      }
    }
    file.close();
  }
  fontDir.close();

  std::sort(availableFamilies.begin(), availableFamilies.end());
  Serial.printf("[FM] Scan complete. Found %d families\n", availableFamilies.size());
}

struct EpdfHeader {
  char magic[4];
  uint32_t intervalCount;
  uint32_t totalGlyphCount;
  uint8_t advanceY;
  int32_t ascender;
  int32_t descender;
  uint8_t is2Bit;
};

// Helper to load a single font file
CustomEpdFont* loadFontFile(const String& path) {
  Serial.printf("[FontMgr] Loading file: %s\n", path.c_str());
  Serial.flush();
  FsFile f;
  if (!SdMan.openFileForRead("FontLoading", path.c_str(), f)) {
    Serial.printf("[FontMgr] Failed to open: %s\n", path.c_str());
    Serial.flush();
    return nullptr;
  }

  // Read custom header format (detected from file dump)
  // 0: Magic (4)
  // 4: IntervalCount (4)
  // 8: FileSize (4)
  // 12: Height (4) -> advanceY
  // 16: GlyphCount (4)
  // 20: Ascender (4)
  // 24: Unknown (4)
  // 28: Descender (4)
  // 32: Unknown (4)
  // 36: OffsetIntervals (4)
  // 40: OffsetGlyphs (4)
  // 44: OffsetBitmaps (4)

  uint32_t buf[12];  // 48 bytes
  if (f.read(buf, 48) != 48) {
    Serial.printf("[FontMgr] Header read failed for %s\n", path.c_str());
    f.close();
    return nullptr;
  }

  if (strncmp((char*)&buf[0], "EPDF", 4) != 0) {
    Serial.printf("[FontMgr] Invalid magic for %s\n", path.c_str());
    f.close();
    return nullptr;
  }

  uint32_t intervalCount = buf[1];
  uint32_t fileSize = buf[2];
  uint32_t height = buf[3];
  uint32_t glyphCount = buf[4];
  int32_t ascender = (int32_t)buf[5];
  int32_t descender = (int32_t)buf[7];

  uint32_t offsetIntervals = buf[9];
  uint32_t offsetGlyphs = buf[10];
  uint32_t offsetBitmaps = buf[11];

  Serial.printf("[FontMgr] parsed header: intv=%u, glyphs=%u, fileSz=%u, h=%u, asc=%d, desc=%d\n", intervalCount,
                glyphCount, fileSize, height, ascender, descender);
  Serial.printf("[FontMgr] offsets: intv=%u, gly=%u, bmp=%u\n", offsetIntervals, offsetGlyphs, offsetBitmaps);

  // Validation
  if (offsetIntervals >= fileSize || offsetGlyphs >= fileSize || offsetBitmaps >= fileSize) {
    Serial.println("[FontMgr] Invalid offsets in header");
    f.close();
    return nullptr;
  }

  // We need to load intervals into RAM
  EpdUnicodeInterval* intervals = new (std::nothrow) EpdUnicodeInterval[intervalCount];
  if (!intervals) {
    Serial.printf("[FontMgr] Failed to allocate intervals: %d\n", intervalCount);
    f.close();
    return nullptr;
  }

  if (!f.seekSet(offsetIntervals)) {
    Serial.println("[FontMgr] Failed to seek to intervals");
    delete[] intervals;
    f.close();
    return nullptr;
  }

  f.read((uint8_t*)intervals, intervalCount * sizeof(EpdUnicodeInterval));

  f.close();

  // Create EpdFontData
  EpdFontData* fontData = new (std::nothrow) EpdFontData();
  if (!fontData) {
    Serial.println("[FontMgr] Failed to allocate EpdFontData! OOM.");
    delete[] intervals;
    return nullptr;
  }
  fontData->intervalCount = intervalCount;
  fontData->intervals = intervals;
  fontData->glyph = nullptr;
  fontData->advanceY = (uint8_t)height;
  fontData->ascender = ascender;
  fontData->descender = descender;
  fontData->descender = descender;
  fontData->is2Bit = (buf[8] != 0);
  fontData->bitmap = nullptr;

  return new CustomEpdFont(path, fontData, offsetIntervals, offsetGlyphs, offsetBitmaps);
}

EpdFontFamily* FontManager::getCustomFontFamily(const std::string& familyName, int fontSize) {
  if (loadedFonts[familyName][fontSize]) {
    return loadedFonts[familyName][fontSize];
  }

  String basePath = "/fonts/" + String(familyName.c_str()) + "-";
  String sizeStr = String(fontSize);

  CustomEpdFont* regular = loadFontFile(basePath + "Regular-" + sizeStr + ".epdfont");
  CustomEpdFont* bold = loadFontFile(basePath + "Bold-" + sizeStr + ".epdfont");
  CustomEpdFont* italic = loadFontFile(basePath + "Italic-" + sizeStr + ".epdfont");
  CustomEpdFont* boldItalic = loadFontFile(basePath + "BoldItalic-" + sizeStr + ".epdfont");

  if (!regular) {
    if (bold) regular = bold;
  }

  if (regular) {
    EpdFontFamily* family = new EpdFontFamily(regular, bold, italic, boldItalic);
    loadedFonts[familyName][fontSize] = family;
    return family;
  }

  return nullptr;
}
