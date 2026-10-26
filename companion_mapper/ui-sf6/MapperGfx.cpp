#include "MapperGfx.h"
#include "MapperFont.h"
#include <string.h>

uint32_t mapperUtf8Next(const char*& p) {
  const uint8_t* s = (const uint8_t*) p;
  uint32_t c = s[0];
  if (c < 0x80) { p += 1; return c; }
  if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { p += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
  if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    p += 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
  }
  if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { p += 4; return '?'; }
  p += 1; return '?';
}

const uint8_t* MapperGfx::glyphFor(uint32_t cp, uint8_t* accent) {
  *accent = 0;
  if (cp >= 32 && cp <= 126) return MAPPER_FONT_ASCII[cp - 32];
  for (int i = 0; i < MAPPER_FONT_EXTRA; i++) {
    if (MAPPER_FONT_EXTRA_CP[i] == cp) { *accent = MAPPER_FONT_ACCENT[i]; return MAPPER_FONT_EXTRA_GLYPH[i]; }
  }
  return MAPPER_FONT_ASCII['?' - 32];
}

void MapperGfx::color(bool on) {
  _d->setColor(on ? UIColor::primary_txt : UIColor::window_bkg);
}

void MapperGfx::frame(int x, int y, int w, int h) {
  _d->fillRect(x, y, w, 1); _d->fillRect(x, y + h - 1, w, 1);
  _d->fillRect(x, y, 1, h); _d->fillRect(x + w - 1, y, 1, h);
}

// rysuje kolumny znaku; ciagle odcinki w kolumnie jednym fillRect
void MapperGfx::glyphCols(const uint8_t* cols, int x, int y, int scale) {
  for (int c = 0; c < 5; c++) {
    uint8_t b = cols[c];
    int r = 0;
    while (r < 8) {
      if (b & (1 << r)) {
        int start = r;
        while (r < 8 && (b & (1 << r))) r++;
        _d->fillRect(x + c * scale, y + start * scale, scale, (r - start) * scale);
      } else r++;
    }
  }
}

int MapperGfx::text(int x, int y, const char* s, int scale) {
  int x0 = x;
  const char* p = s;
  while (*p) {
    uint32_t cp = mapperUtf8Next(p);
    uint8_t accent;
    const uint8_t* g = glyphFor(cp, &accent);
    glyphCols(g, x, y, scale);
    if (accent == 1) { _d->fillRect(x + 2 * scale, y - scale, 2 * scale, scale); }       // kreska nad wielka litera
    else if (accent == 2) { _d->fillRect(x + 2 * scale, y - scale, scale, scale); }      // kropka
    x += 6 * scale;
  }
  return x - x0;
}

int MapperGfx::textWidth(const char* s, int scale) const {
  int n = 0;
  const char* p = s;
  while (*p) { mapperUtf8Next(p); n++; }
  return n * 6 * scale;
}

int MapperGfx::textRight(int xr, int y, const char* s, int scale) {
  int w = textWidth(s, scale);
  text(xr - w + 1, y, s, scale);
  return w;
}

int MapperGfx::textCenter(int xm, int y, const char* s, int scale) {
  int w = textWidth(s, scale);
  text(xm - w / 2, y, s, scale);
  return w;
}

void MapperGfx::fit(char* dest, size_t dsz, const char* s, int max_w) {
  int max_chars = max_w / 6;
  const char* p = s;
  int n = 0;
  while (*p && n < max_chars) {
    const char* q = p;
    mapperUtf8Next(q);
    if ((size_t)(q - s) >= dsz) break;
    p = q; n++;
  }
  size_t len = p - s;
  memcpy(dest, s, len);
  dest[len] = 0;
}

// ikony: kolumny, bit 0 = gora
static const uint8_t ICON_GPS_RING[7] = {0x1C, 0x22, 0x41, 0x41, 0x41, 0x22, 0x1C};
static const uint8_t ICON_BT[5]       = {0x22, 0x14, 0x7F, 0x55, 0x22};
static const uint8_t ICON_BELL[7]     = {0x30, 0x3C, 0x3E, 0x7F, 0x3E, 0x3C, 0x30};
static const uint8_t ICON_LOCK[7]     = {0x78, 0x7E, 0x79, 0x69, 0x79, 0x7E, 0x78};

static void drawCols(DisplayDriver* d, const uint8_t* cols, int n, int x, int y) {
  for (int c = 0; c < n; c++) {
    uint8_t b = cols[c];
    for (int r = 0; r < 8; r++) if (b & (1 << r)) {
      int s = r; while (r < 8 && (b & (1 << r))) r++;
      d->fillRect(x + c, y + s, 1, r - s);
    }
  }
}

void MapperGfx::iconGps(int x, int y, bool fix) {
  drawCols(_d, ICON_GPS_RING, 7, x, y);
  if (fix) _d->fillRect(x + 2, y + 2, 3, 3);       // pelny srodek = fix
  else _d->fillRect(x + 3, y + 3, 1, 1);
}

void MapperGfx::iconBt(int x, int y, bool connected) {
  if (connected) {
    _d->fillRect(x, y - 1, 7, 9);
    color(false);
    drawCols(_d, ICON_BT, 5, x + 1, y);
    color(true);
  } else {
    drawCols(_d, ICON_BT, 5, x + 1, y);
  }
}

void MapperGfx::iconBattery(int x, int y, int pct, bool usb) {
  frame(x, y, 13, 7);
  _d->fillRect(x + 13, y + 2, 2, 3);                // biegun
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  int w = (pct * 9 + 50) / 100;
  if (w > 0) _d->fillRect(x + 2, y + 2, w, 3);
  if (usb) {                                        // ladowanie: "piorun" w odwroconym kolorze
    color(w < 5);
    _d->fillRect(x + 6, y + 1, 1, 2); _d->fillRect(x + 5, y + 3, 3, 1); _d->fillRect(x + 6, y + 4, 1, 2);
    color(true);
  }
}

void MapperGfx::iconBell(int x, int y, bool on) {
  drawCols(_d, ICON_BELL, 7, x, y);
  if (!on) { color(false); for (int i = 0; i < 7; i++) _d->fillRect(x + i, y + i, 1, 1); color(true);
             for (int i = 0; i < 7; i++) _d->fillRect(x + i, y + 6 - i, 1, 1); }
}

void MapperGfx::iconLock(int x, int y) { drawCols(_d, ICON_LOCK, 7, x, y); }

void MapperGfx::progress(int x, int y, int w, int h, int pct) {
  frame(x, y, w, h);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  int fw = (w - 4) * pct / 100;
  if (fw > 0) _d->fillRect(x + 2, y + 2, fw, h - 4);
}

void MapperGfx::arcTop(int cx, int cy, int r, int ymin) {
  if (r <= 0) return;
  int x = r, y = 0, err = 1 - r;                    // algorytm punktu srodkowego, tylko gorna polowa
  while (x >= y) {
    const int px[4] = { cx + x, cx - x, cx + y, cx - y };
    const int py[4] = { cy - y, cy - y, cy - x, cy - x };
    for (int i = 0; i < 4; i++) if (py[i] >= ymin && py[i] <= cy) _d->fillRect(px[i], py[i], 1, 1);
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}
