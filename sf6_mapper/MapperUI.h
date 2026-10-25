#pragma once
#include <Arduino.h>
#include <target.h>
#include "MapperScreens.h"

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif

#define UI_MENU_TIMEOUT_MS   30000UL   // powrot z menu do ekranu glownego
#define UI_CONFIRM_MS        4000UL    // czas na potwierdzenie STOP w menu
#define UI_UNLOCK_MS         3000UL    // przytrzymanie OK, ktore odblokowuje ekran

// sterowanie trackerem z menu (implementacja w main.cpp)
class MapperControl {
public:
  virtual bool isRunning() = 0;        virtual void setRunning(bool on) = 0;
  virtual int  getInterval() = 0;      virtual void setIntervalSecs(int s) = 0;
  virtual bool isGpsOn() = 0;          virtual void setGpsOn(bool on) = 0;
  virtual bool isBleOn() = 0;          virtual void setBleOn(bool on) = 0;
  virtual bool isBuzzerOn() = 0;       virtual void setBuzzerOn(bool on) = 0;
  virtual bool isLockOn() = 0;         virtual void setLockOn(bool on) = 0;
  virtual int  getTimeoutIdx() = 0;    virtual void setTimeoutIdx(int i) = 0;   // 0=30 s, 1=1 min, 2=2 min, 3=nigdy
  virtual bool isUsb() = 0;
  virtual void newSession() = 0;
  virtual uint32_t pendingCount() = 0;
  virtual void clearBonds() = 0;
  virtual void eraseData() = 0;
  virtual void factoryReset() = 0;     // restart trackera
  virtual void newIdentity() = 0;      // restart trackera
  virtual void newPin() = 0;           // nowy losowy PIN BT, usuniecie parowan, restart
  virtual uint32_t getPin() = 0;
  virtual void powerOff() = 0;
  virtual int  getInfoLines(char lines[][MAP_LINE_SZ], int max) = 0;
};

class MapperUI {
public:
  enum Beep { BEEP_HIT, BEEP_WARN, BEEP_OK };
private:
  enum Screen { SCR_SPLASH, SCR_MAIN, SCR_MENU, SCR_SERVICE, SCR_INFO, SCR_CONFIRM };
  enum Confirm { CF_NONE, CF_ERASE, CF_BONDS, CF_PIN, CF_FACTORY, CF_NEWID1, CF_NEWID2, CF_POWER };
#ifdef DISPLAY_CLASS
  DisplayDriver* _disp;
#endif
  MapperGfx      _g;
  MapperControl* _ctl;
  Screen   _screen;
  Confirm  _confirm;
  int      _page;             // ekran glowny: 0 = stan, 1 = sesja, 2 = GPS
  int      _sel, _top;        // menu
  int      _ssel, _stop;      // menu "Serwis"
  bool     _locked;
  bool     _blink;
  bool     _on;               // ekran wlaczony
  unsigned long _last_activity;
  unsigned long _next_refresh;
  unsigned long _stop_armed;
  unsigned long _flash_until;
  unsigned long _hold_start;  // przytrzymanie OK na ekranie potwierdzenia
  unsigned long _splash_start;
  bool     _restore_quiet;    // po dzwieku startowym przywroc ustawienie wyciszenia
  char     _flash1[MAP_LINE_SZ], _flash2[MAP_LINE_SZ];
#ifdef PIN_BUZZER
  genericBuzzer _buzzer;
#endif

  unsigned long timeoutMs();
  void render(const MapperStatus& st);
  void menuAction(int item, int dir);
  void serviceAction(int item);
  void confirmDone();
  void screenOff();
  void screenOn();
  void endSplash();

public:
  MapperUI() : _ctl(NULL), _screen(SCR_MAIN), _confirm(CF_NONE), _page(0), _sel(0), _top(0), _ssel(0), _stop(0),
               _locked(false), _blink(false), _on(false), _last_activity(0), _next_refresh(0), _stop_armed(0),
               _flash_until(0), _hold_start(0), _splash_start(0), _restore_quiet(false) {
#ifdef DISPLAY_CLASS
    _disp = NULL;
#endif
    _flash1[0] = _flash2[0] = 0;
  }
  void begin(bool has_display, MapperControl* ctl);
  void loop(const MapperStatus& st);
  void beep(Beep b);
  void applyBuzzer();
  void flash(const char* l1, const char* l2 = NULL, unsigned long ms = 1800);
  void wake();
  void showNow(const char* l1, const char* l2);   // natychmiastowy komunikat (np. przed wylaczeniem)
  bool isLocked() const { return _locked; }
};
