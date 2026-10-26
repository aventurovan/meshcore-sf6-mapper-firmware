#pragma once
/*
 * Rysowanie na ekranie 128x64: wlasna czcionka 5x8 z polskimi literami (UTF-8), skalowanie x2,
 * ikony paska stanu. Korzysta tylko z DisplayDriver::fillRect, wiec dziala z kazdym sterownikiem.
 */
#include <stdint.h>
#include <stddef.h>
#include <helpers/ui/DisplayDriver.h>

class MapperGfx {
  DisplayDriver* _d;
  void glyphCols(const uint8_t* cols, int x, int y, int scale);
public:
  static const uint8_t* glyphFor(uint32_t cp, uint8_t* accent);   // NULL = znak spoza czcionki
  MapperGfx() : _d(NULL) { }
  void attach(DisplayDriver* d) { _d = d; }
  DisplayDriver* drv() { return _d; }
  int  width() const { return 128; }

  void color(bool on);                               // true = jasny, false = ciemny (na odwroconym tle)
  void rect(int x, int y, int w, int h) { _d->fillRect(x, y, w, h); }
  void hline(int x, int y, int w) { _d->fillRect(x, y, w, 1); }
  void frame(int x, int y, int w, int h);

  // tekst UTF-8; zwraca szerokosc w pikselach. scale 1 = 6 px na znak, 2 = 12 px
  int  text(int x, int y, const char* s, int scale = 1);
  int  textWidth(const char* s, int scale = 1) const;
  int  textRight(int xr, int y, const char* s, int scale = 1);
  int  textCenter(int xm, int y, const char* s, int scale = 1);
  // przycina tekst do max_w pikseli (po calych znakach UTF-8), wynik w dest
  static void fit(char* dest, size_t dsz, const char* s, int max_w);

  // ikony (wysokosc 7 px)
  void iconGps(int x, int y, bool fix);              // 7x7
  void iconBt(int x, int y, bool connected);         // 7x9 (polaczony = odwrocone tlo)
  void iconBattery(int x, int y, int pct, bool usb); // 15x7
  void iconBell(int x, int y, bool on);              // 7x7
  void iconLock(int x, int y);                       // 7x7
  void progress(int x, int y, int w, int h, int pct);
  // gorny luk okregu (fala radiowa) o srodku (cx, cy), tylko punkty powyzej cy i w granicach y >= ymin
  void arcTop(int cx, int cy, int r, int ymin);
};

// pomocnicze: dlugosc znaku UTF-8 i dekodowanie
uint32_t mapperUtf8Next(const char*& p);
