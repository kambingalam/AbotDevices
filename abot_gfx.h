#pragma once

// ===========================================================================
//  LAYER GRAFIS
// ===========================================================================
void beginFrame() {
  u8g2.clearBuffer();
}
void presentFrame() {
  u8g2.sendBuffer();
}

void gfxSelectFont(uint8_t size) {
  u8g2.setFont(size >= 2 ? u8g2_font_10x20_tr : u8g2_font_6x10_tr);
  u8g2.setFontMode(1);
  u8g2.setFontPosTop();
}

int gfxTextWidth(const String& text, uint8_t size = 1) {
  gfxSelectFont(size);
  return u8g2.getStrWidth(text.c_str());
}

void gfxText(int x, int y, const String& text, uint8_t size = 1,
             uint8_t color = COLOR_WHITE) {
  gfxSelectFont(size);
  u8g2.setDrawColor(color);
  u8g2.drawStr(x, y, text.c_str());
}

void gfxPixel(int x, int y, uint8_t color = COLOR_WHITE) {
  if (x < 0 || y < 0 || x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) return;
  u8g2.setDrawColor(color);
  u8g2.drawPixel(x, y);
}

void gfxFillRect(int x, int y, int w, int h, uint8_t color = COLOR_WHITE) {
  if (w <= 0 || h <= 0) return;
  u8g2.setDrawColor(color);
  u8g2.drawBox(x, y, w, h);
}

void gfxFrame(int x, int y, int w, int h, uint8_t color = COLOR_WHITE) {
  if (w <= 0 || h <= 0) return;
  u8g2.setDrawColor(color);
  u8g2.drawFrame(x, y, w, h);
}

void gfxFillRoundRect(int x, int y, int w, int h, int r,
                      uint8_t color = COLOR_WHITE) {
  if (w <= 0 || h <= 0) return;
  int maxRadius = (min(w, h) / 2) - 1;
  if (r > maxRadius) r = maxRadius;
  u8g2.setDrawColor(color);
  if (r <= 0) u8g2.drawBox(x, y, w, h);
  else u8g2.drawRBox(x, y, w, h, r);
}

void gfxRoundFrame(int x, int y, int w, int h, int r,
                   uint8_t color = COLOR_WHITE) {
  if (w <= 0 || h <= 0) return;
  int maxRadius = (min(w, h) / 2) - 1;
  if (r > maxRadius) r = maxRadius;
  u8g2.setDrawColor(color);
  if (r <= 0) u8g2.drawFrame(x, y, w, h);
  else u8g2.drawRFrame(x, y, w, h, r);
}

void gfxFillTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                     uint8_t color = COLOR_WHITE) {
  u8g2.setDrawColor(color);
  u8g2.drawTriangle(x0, y0, x1, y1, x2, y2);
}

void gfxCircle(int x, int y, int r, uint8_t color = COLOR_WHITE) {
  u8g2.setDrawColor(color);
  u8g2.drawCircle(x, y, r, U8G2_DRAW_ALL);
}

void gfxFillCircle(int x, int y, int r, uint8_t color = COLOR_WHITE) {
  u8g2.setDrawColor(color);
  u8g2.drawDisc(x, y, r, U8G2_DRAW_ALL);
}

bool gfxClipLine(int& x0, int& y0, int& x1, int& y1) {
  constexpr int XMAX = SCREEN_WIDTH - 1;
  constexpr int YMAX = SCREEN_HEIGHT - 1;

  auto outcode = [](int x, int y) {
    int code = 0;
    if (x < 0) code |= 1;
    else if (x > XMAX) code |= 2;
    if (y < 0) code |= 4;
    else if (y > YMAX) code |= 8;
    return code;
  };

  int c0 = outcode(x0, y0);
  int c1 = outcode(x1, y1);

  while (true) {
    if ((c0 | c1) == 0) return true;
    if (c0 & c1) return false;

    bool first = (c0 != 0);
    int code = first ? c0 : c1;
    int64_t x = 0, y = 0;

    if (code & 8) {
      x = x0 + (int64_t)(x1 - x0) * (YMAX - y0) / (y1 - y0);
      y = YMAX;
    } else if (code & 4) {
      x = x0 + (int64_t)(x1 - x0) * (0 - y0) / (y1 - y0);
      y = 0;
    } else if (code & 2) {
      y = y0 + (int64_t)(y1 - y0) * (XMAX - x0) / (x1 - x0);
      x = XMAX;
    } else {
      y = y0 + (int64_t)(y1 - y0) * (0 - x0) / (x1 - x0);
      x = 0;
    }

    if (first) {
      x0 = (int)x;
      y0 = (int)y;
      c0 = outcode(x0, y0);
    } else {
      x1 = (int)x;
      y1 = (int)y;
      c1 = outcode(x1, y1);
    }
  }
}

void gfxLine(int x0, int y0, int x1, int y1, uint8_t color = COLOR_WHITE) {
  if (!gfxClipLine(x0, y0, x1, y1)) return;
  u8g2.setDrawColor(color);
  u8g2.drawLine(x0, y0, x1, y1);
}

void gfxBitmap(int x, int y, const uint8_t* bitmap, int w, int h,
               uint8_t color = COLOR_WHITE) {
  u8g2.setDrawColor(color);
  const int bytesPerRow = (w + 7) / 8;

  for (int row = 0; row < h; row++) {
    int runStart = -1;
    int col = 0;
    while (col < w) {
      const uint8_t packed = pgm_read_byte(&bitmap[row * bytesPerRow + (col >> 3)]);
      const int bitsLeft = min(8 - (col & 7), w - col);
      for (int bit = 0; bit < bitsLeft; bit++, col++) {
        const bool on = (packed & (0x80 >> (col & 7))) != 0;
        if (on && runStart < 0) runStart = col;
        else if (!on && runStart >= 0) {
          u8g2.drawHLine(x + runStart, y + row, col - runStart);
          runStart = -1;
        }
      }
    }
    if (runStart >= 0) u8g2.drawHLine(x + runStart, y + row, w - runStart);
  }
}

// ===========================================================================
//  ROBOEYES PROXY
// ===========================================================================
volatile bool overlayListening = false;
volatile bool overlayThinking = false;
volatile bool overlaySleep = false;

class RoboEyesDisplayProxy {
public:
  explicit RoboEyesDisplayProxy(U8G2& target)
    : gfx_(&target) {}
  void clearDisplay() {
    gfx_->clearBuffer();
  }
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                     int16_t r, uint16_t color) {
    gfxFillRoundRect(x, y, w, h, r, (uint8_t)color);
  }
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                    int16_t x2, int16_t y2, uint16_t color) {
    gfxFillTriangle(x0, y0, x1, y1, x2, y2, (uint8_t)color);
  }
  void display() {
    if (overlayListening) drawListeningOverlay();
    else if (overlayThinking) drawThinkingOverlay();
    else if (overlaySleep) drawSleepOverlay();
    gfx_->sendBuffer();
  }

private:
  U8G2* gfx_;

  static void drawListeningOverlay() {
    gfxFillRect(0, 0, 26, 15, COLOR_BLACK);
    gfxRoundFrame(3, 2, 5, 8, 1);
    gfxLine(1, 7, 1, 9);
    gfxLine(9, 7, 9, 9);
    gfxLine(5, 10, 5, 12);
    gfxLine(3, 13, 7, 13);

    if (!audioMicReady) return;
    const int level = (int)audioMicBars;
    for (int i = 0; i < 4; i++) {
      if (i < level) {
        int h = 2 + i * 2;
        gfxLine(13 + i * 3, 13 - h, 13 + i * 3, 13);
      }
    }
  }

  static void drawSleepOverlay() {
    // z -> zz -> zzz naik, lalu gelembung pecah. 4 tahap, loop.
    const unsigned long STAGE_MS = 1050;
    const unsigned long t = millis() % (STAGE_MS * 4);
    const int stage = (int)(t / STAGE_MS);
    const int st = (int)(t % STAGE_MS);

    const int x = 86 - stage * 6;
    const int y = 44 - stage * 7 - (st * 5 / (int)STAGE_MS);

    // kotak hitam biar z tidak tabrakan dengan mata
    gfxFillRect(x - 5, y - 3, 34, 20, COLOR_BLACK);

    if (stage < 3) {
      const char* txt = (stage == 0) ? "z" : (stage == 1 ? "zz" : "zzz");
      gfxRoundFrame(x - 5, y - 2, 12 + stage * 6, 13, 6, COLOR_WHITE);
      gfxText(x, y, txt, 1, COLOR_WHITE);
      gfxText(x + 17, y + 11, "z", 1, COLOR_WHITE);
    } else {
      const int grow = st * 6 / (int)STAGE_MS;
      const int cx = x - 1;
      const int cy = y + 4;
      gfxRoundFrame(cx - 6 - grow, cy - 4 - grow, 12 + grow * 2, 9 + grow * 2,
                    4, COLOR_WHITE);
      gfxFillCircle(cx, cy, 1, COLOR_WHITE);
      gfxPixel(cx - 9 - grow, cy - 6 - grow, COLOR_WHITE);
      gfxPixel(cx + 9 + grow, cy - 6 - grow, COLOR_WHITE);
      gfxPixel(cx - 9 - grow, cy + 6 + grow, COLOR_WHITE);
      gfxPixel(cx + 9 + grow, cy + 6 + grow, COLOR_WHITE);
    }
  }

  static void drawThinkingOverlay() {
    const int phase = (millis() / 450UL) % 3;
    const char* dots = (phase == 0) ? "." : (phase == 1) ? ".."
                                                         : "...";
    gfxFillRect(0, 0, 20, 8, COLOR_BLACK);
    gfxText(2, 0, dots, 1, COLOR_WHITE);
  }
};

#ifdef N
#undef N
#endif
#ifdef NE
#undef NE
#endif
#ifdef E
#undef E
#endif
#ifdef SE
#undef SE
#endif
#ifdef S
#undef S
#endif
#ifdef SW
#undef SW
#endif
#ifdef W
#undef W
#endif
#ifdef NW
#undef NW
#endif

constexpr uint8_t OJOL_POS_N = 1;
constexpr uint8_t OJOL_POS_NE = 2;
constexpr uint8_t OJOL_POS_E = 3;
constexpr uint8_t OJOL_POS_W = 7;
constexpr uint8_t OJOL_POS_NW = 8;

RoboEyesDisplayProxy roboEyesDisplay(u8g2);
RoboEyes<RoboEyesDisplayProxy> roboEyes(roboEyesDisplay);

