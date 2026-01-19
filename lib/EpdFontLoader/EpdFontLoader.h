#pragma once

#include <GfxRenderer.h>

class EpdFontLoader {
 public:
  static void loadFontsFromSd(GfxRenderer& renderer);
  static int getBestFontId(const char* familyName, int size);
};
