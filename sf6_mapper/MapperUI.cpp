#include "MapperUI.h"
#include "MapperProtocol.h"

/*
 * Obsluga przyciskow:
 *  ekran glowny: lewo/prawo = strony (stan / sesja / GPS), OK = menu, wstecz = zgas ekran
 *  menu:         gora/dol = wybor, OK/prawo = zmiana, lewo = zmiana w druga strone, wstecz = wyjscie
 *  potwierdzenie: przytrzymaj OK (pasek postepu), wstecz = anuluj
 *  blokada:      zgaszony ekran blokuje (gdy wlaczona i bez zasilania USB); odblokowanie = przytrzymanie OK 3 s
 */

#ifdef DISPLAY_CLASS
static MomentaryButton joystick_up(JOYSTICK_UP, 1000, true, false, false);
static MomentaryButton joystick_down(JOYSTICK_DOWN, 1000, true, false, false);
#endif

enum MenuItem { MI_RUN, MI_IVL, MI_SESSION, MI_SOUND, MI_GPS, MI_BLE, MI_LOCK, MI_TIMEOUT, MI_INFO, MI_SERVICE, MI_POWER, MI_EXIT, MI_COUNT };
enum ServiceItem { SI_ERASE, SI_BONDS, SI_PIN, SI_FACTORY, SI_NEWID, SI_BACK, SI_COUNT };
static const char* const TIMEOUT_TXT[4] = {"30 s", "1 min", "2 min", "nigdy"};
static const unsigned long TIMEOUT_MS[4] = {30000UL, 60000UL, 120000UL, 0};
#define MENU_ROWS 5

unsigned long MapperUI::timeoutMs() {
  if (_ctl && _ctl->isUsb()) return 0;              // zasilanie z USB (samochod): ekran nie gasnie
  int i = _ctl ? _ctl->getTimeoutIdx() : 1;
  return TIMEOUT_MS[i < 0 || i > 3 ? 1 : i];
}

void MapperUI::begin(bool has_display, MapperControl* ctl) {
  _ctl = ctl;
#ifdef DISPLAY_CLASS
  _disp = has_display ? &display : NULL;
  if (_disp) _g.attach(_disp);
  user_btn.begin();
  back_btn.begin();
  joystick_left.begin();
  joystick_right.begin();
  joystick_up.begin();
  joystick_down.begin();
#endif
#ifdef PIN_BUZZER
  _buzzer.begin();
#endif
  applyBuzzer();
  screenOn();
  _screen = SCR_SPLASH;                             // animacja startowa
  _splash_start = millis();
}

// koniec animacji: ekran glowny i dwa krotkie piknieca - zawsze, takze przy wyciszonym dzwieku
void MapperUI::endSplash() {
  _screen = SCR_MAIN;
  _last_activity = millis();
  _next_refresh = 0;
#ifdef PIN_BUZZER
  _buzzer.quiet(false);
  _buzzer.play("start:d=16,o=6,b=200:c,p,c");
  _restore_quiet = true;
#endif
}

void MapperUI::applyBuzzer() {
#ifdef PIN_BUZZER
  _buzzer.quiet(_ctl ? !_ctl->isBuzzerOn() : false);
#endif
}

void MapperUI::beep(Beep b) {
#ifdef PIN_BUZZER
  if (!_ctl || !_ctl->isBuzzerOn() || _buzzer.isPlaying()) return;
  if (b == BEEP_HIT) _buzzer.play("hit:d=16,o=6,b=180:c,e");
  else if (b == BEEP_WARN) _buzzer.play("warn:d=8,o=5,b=160:a,p,a,p,a");
  else _buzzer.play("ok:d=16,o=6,b=200:e,g");
#endif
}

void MapperUI::flash(const char* l1, const char* l2, unsigned long ms) {
  strncpy(_flash1, l1 ? l1 : "", MAP_LINE_SZ - 1); _flash1[MAP_LINE_SZ - 1] = 0;
  strncpy(_flash2, l2 ? l2 : "", MAP_LINE_SZ - 1); _flash2[MAP_LINE_SZ - 1] = 0;
  _flash_until = millis() + ms;
  _next_refresh = 0;
}

void MapperUI::wake() {
  if (!_on) screenOn();
  _last_activity = millis();
  _next_refresh = 0;
}

void MapperUI::screenOn() {
#ifdef DISPLAY_CLASS
  if (_disp && !_disp->isOn()) _disp->turnOn();
#endif
  _on = true;
  _last_activity = millis();
  _next_refresh = 0;
}

void MapperUI::screenOff() {
#ifdef DISPLAY_CLASS
  if (_disp && _disp->isOn()) _disp->turnOff();
#endif
  _on = false;
  _screen = SCR_MAIN;
  _confirm = CF_NONE;
  _stop_armed = 0;
  if (_ctl && _ctl->isLockOn() && !_ctl->isUsb()) _locked = true;
}

void MapperUI::showNow(const char* l1, const char* l2) {
#ifdef DISPLAY_CLASS
  if (!_disp) return;
  if (!_disp->isOn()) _disp->turnOn();
  _disp->startFrame();
  _g.color(true);
  drawMessageScreen(_g, l1, l2);
  _disp->endFrame();
#endif
}

void MapperUI::menuAction(int item, int dir) {
  if (!_ctl) return;
  switch (item) {
    case MI_RUN:
      if (!_ctl->isRunning()) { _ctl->setRunning(true); _stop_armed = 0; flash("Mapowanie", "WŁĄCZONE"); }
      else if (_stop_armed && millis() - _stop_armed < UI_CONFIRM_MS) { _ctl->setRunning(false); _stop_armed = 0; flash("Mapowanie", "ZATRZYMANE"); }
      else _stop_armed = millis();
      break;
    case MI_IVL: {
      int s = _ctl->getInterval() + (dir < 0 ? -10 : 10);
      if (s > 60) s = 20;
      if (s < 20) s = 60;
      _ctl->setIntervalSecs(s);
      break;
    }
    case MI_SESSION: if (dir >= 0) { _ctl->newSession(); flash("Nowa sesja", "liczniki od zera"); } break;
    case MI_SOUND:   _ctl->setBuzzerOn(!_ctl->isBuzzerOn()); applyBuzzer(); break;
    case MI_GPS:     _ctl->setGpsOn(!_ctl->isGpsOn()); break;
    case MI_BLE:     _ctl->setBleOn(!_ctl->isBleOn()); break;
    case MI_LOCK:    _ctl->setLockOn(!_ctl->isLockOn()); break;
    case MI_TIMEOUT: _ctl->setTimeoutIdx((_ctl->getTimeoutIdx() + (dir < 0 ? 3 : 1)) % 4); break;
    case MI_INFO:    if (dir >= 0) _screen = SCR_INFO; break;
    case MI_SERVICE: if (dir >= 0) { _screen = SCR_SERVICE; _ssel = 0; _stop = 0; } break;
    case MI_POWER:   if (dir >= 0) { _screen = SCR_CONFIRM; _confirm = CF_POWER; _hold_start = 0; } break;
    case MI_EXIT:    if (dir >= 0) _screen = SCR_MAIN; break;
  }
}

void MapperUI::serviceAction(int item) {
  _hold_start = 0;
  switch (item) {
    case SI_ERASE:   _screen = SCR_CONFIRM; _confirm = CF_ERASE; break;
    case SI_BONDS:   _screen = SCR_CONFIRM; _confirm = CF_BONDS; break;
    case SI_PIN:     _screen = SCR_CONFIRM; _confirm = CF_PIN; break;
    case SI_FACTORY: _screen = SCR_CONFIRM; _confirm = CF_FACTORY; break;
    case SI_NEWID:   _screen = SCR_CONFIRM; _confirm = CF_NEWID1; break;
    case SI_BACK:    _screen = SCR_MENU; break;
  }
}

// wykonanie po przytrzymaniu OK
void MapperUI::confirmDone() {
  Confirm c = _confirm;
  _confirm = CF_NONE;
  _hold_start = 0;
  switch (c) {
    case CF_ERASE:   _ctl->eraseData(); _screen = SCR_MAIN; flash("Dane usunięte"); beep(BEEP_OK); break;
    case CF_BONDS:   _ctl->clearBonds(); _screen = SCR_MAIN; flash("Parowania BT", "usunięte"); beep(BEEP_OK); break;
    case CF_PIN:     showNow("Nowy PIN Bluetooth", "restart..."); _ctl->newPin(); break;
    case CF_FACTORY: showNow("Ustawienia fabryczne", "restart..."); _ctl->factoryReset(); break;
    case CF_NEWID2:  showNow("Nowa tożsamość", "restart..."); _ctl->newIdentity(); break;
    case CF_POWER:   _ctl->powerOff(); break;
    default: _screen = SCR_MAIN; break;
  }
}

void MapperUI::loop(const MapperStatus& st) {
#ifdef PIN_BUZZER
  _buzzer.loop();
  if (_restore_quiet && !_buzzer.isPlaying()) { _restore_quiet = false; applyBuzzer(); }
#endif
#ifdef DISPLAY_CLASS
  if (_screen == SCR_SPLASH) {
    bool skip = user_btn.check() || back_btn.check() || joystick_left.check() || joystick_right.check()
             || joystick_up.check() || joystick_down.check();
    unsigned long t = millis() - _splash_start;
    if (skip || t >= SPLASH_MS || _disp == NULL) { endSplash(); return; }
    if ((long)(millis() - _next_refresh) < 0) return;
    _next_refresh = millis() + 40;                  // ok. 25 klatek/s
    _disp->startFrame();
    _g.color(true);
    drawSplashScreen(_g, t, st.name, MAPPER_FW_VERSION);
    _disp->endFrame();
    return;
  }
  int ev_ok    = user_btn.check();
  int ev_back  = back_btn.check();
  int ev_left  = joystick_left.check();
  int ev_right = joystick_right.check();
  int ev_up    = joystick_up.check();
  int ev_down  = joystick_down.check();
  bool any = ev_ok || ev_back || ev_left || ev_right || ev_up || ev_down;
  if (_ctl && _ctl->isUsb()) _locked = false;       // tryb samochodowy: bez blokady

  if (any) {
    if (!_on) {
      wake();                                       // zgaszony ekran: przycisk tylko budzi
    } else if (_locked) {
      wake();
      if (ev_back == BUTTON_EVENT_CLICK) screenOff();   // odblokowanie: przytrzymanie OK (ponizej)
    } else {
      wake();
      switch (_screen) {
        case SCR_SPLASH: break;
        case SCR_MAIN:
          if (ev_ok == BUTTON_EVENT_CLICK) { _screen = SCR_MENU; _stop_armed = 0; }
          else if (ev_back == BUTTON_EVENT_CLICK) screenOff();
          else if (ev_right == BUTTON_EVENT_CLICK) _page = (_page + 1) % 3;
          else if (ev_left == BUTTON_EVENT_CLICK) _page = (_page + 2) % 3;
          break;
        case SCR_MENU:
          if (ev_up == BUTTON_EVENT_CLICK)   { _sel = (_sel + MI_COUNT - 1) % MI_COUNT; _stop_armed = 0; }
          if (ev_down == BUTTON_EVENT_CLICK) { _sel = (_sel + 1) % MI_COUNT; _stop_armed = 0; }
          if (ev_ok == BUTTON_EVENT_CLICK || ev_right == BUTTON_EVENT_CLICK) menuAction(_sel, +1);
          if (ev_left == BUTTON_EVENT_CLICK) {
            if (_sel == MI_IVL || _sel == MI_TIMEOUT) menuAction(_sel, -1);
            else if (_sel >= MI_INFO || _sel == MI_SESSION) _screen = SCR_MAIN;
            else menuAction(_sel, -1);
          }
          if (ev_back == BUTTON_EVENT_CLICK) _screen = SCR_MAIN;
          if (_sel < _top) _top = _sel;
          if (_sel >= _top + MENU_ROWS) _top = _sel - MENU_ROWS + 1;
          break;
        case SCR_SERVICE:
          if (ev_up == BUTTON_EVENT_CLICK)   _ssel = (_ssel + SI_COUNT - 1) % SI_COUNT;
          if (ev_down == BUTTON_EVENT_CLICK) _ssel = (_ssel + 1) % SI_COUNT;
          if (ev_ok == BUTTON_EVENT_CLICK || ev_right == BUTTON_EVENT_CLICK) serviceAction(_ssel);
          if (ev_back == BUTTON_EVENT_CLICK || ev_left == BUTTON_EVENT_CLICK) _screen = SCR_MENU;
          if (_ssel < _stop) _stop = _ssel;
          if (_ssel >= _stop + MENU_ROWS) _stop = _ssel - MENU_ROWS + 1;
          break;
        case SCR_INFO:
          if (ev_ok == BUTTON_EVENT_CLICK || ev_back == BUTTON_EVENT_CLICK || ev_left == BUTTON_EVENT_CLICK) _screen = SCR_MENU;
          break;
        case SCR_CONFIRM:
          if (ev_back == BUTTON_EVENT_CLICK || ev_left == BUTTON_EVENT_CLICK) {
            _screen = (_confirm == CF_POWER) ? SCR_MENU : SCR_SERVICE;
            _confirm = CF_NONE; _hold_start = 0;
          } else if (_confirm == CF_NEWID1 && ev_ok == BUTTON_EVENT_CLICK) {
            _confirm = CF_NEWID2; _hold_start = 0;  // drugi krok
          }
          break;
      }
    }
    _next_refresh = 0;
  }

  // odblokowanie: przytrzymanie OK przez 3 s
  if (_on && _locked) {
    if (user_btn.isPressed()) {
      if (_hold_start == 0) _hold_start = millis();
      _last_activity = millis();
      if (millis() - _hold_start >= UI_UNLOCK_MS) { _locked = false; _hold_start = 0; flash("Odblokowano", NULL, 900); }
      _next_refresh = 0;
    } else _hold_start = 0;
  }

  // przytrzymanie OK na ekranie potwierdzenia
  if (_on && !_locked && _screen == SCR_CONFIRM && _confirm != CF_NEWID1) {
    if (user_btn.isPressed()) {
      if (_hold_start == 0) _hold_start = millis();
      unsigned long need = (_confirm == CF_POWER || _confirm == CF_BONDS || _confirm == CF_PIN) ? 2000UL : 3000UL;
      if (millis() - _hold_start >= need) confirmDone();
      _next_refresh = 0;
    } else _hold_start = 0;
  }

  if (_disp == NULL) return;

  if (_screen != SCR_MAIN && millis() - _last_activity > UI_MENU_TIMEOUT_MS) { _screen = SCR_MAIN; _confirm = CF_NONE; _next_refresh = 0; }
  unsigned long to = timeoutMs();
  if (_on && to && millis() - _last_activity > to) { screenOff(); return; }
  if (!_on) return;

  if ((long)(millis() - _next_refresh) < 0) return;
  _next_refresh = millis() + (_hold_start ? 100 : 500);
  _blink = !_blink;
  render(st);
#endif
}

void MapperUI::render(const MapperStatus& st) {
#ifdef DISPLAY_CLASS
  _disp->startFrame();
  _disp->setTextSize(1);
  _g.color(true);
  if (_flash_until && (long)(millis() - _flash_until) < 0) {
    drawMessageScreen(_g, _flash1, _flash2[0] ? _flash2 : NULL);
    _disp->endFrame();
    return;
  }
  _flash_until = 0;

  switch (_screen) {
    case SCR_SPLASH: break;
    case SCR_MAIN:
      if (_page == 1 && !_locked) drawSessionScreen(_g, st);
      else if (_page == 2 && !_locked) drawGpsScreen(_g, st);
      else drawMainScreen(_g, st, _locked ? (_hold_start ? (int)((millis() - _hold_start) * 100 / UI_UNLOCK_MS) : 0) : -1, _blink);
      break;
    case SCR_MENU: {
      char v[MI_COUNT][12];
      const char* labels[MI_COUNT] = {"Mapowanie", "Ping co", "Nowa sesja", "Dźwięk", "GPS", "Bluetooth", "Blokada",
                                      "Wygaszanie", "Informacje", "Serwis", "Wyłącz", "Wyjście"};
      for (int i = 0; i < MI_COUNT; i++) v[i][0] = 0;
      if (_stop_armed && millis() - _stop_armed < UI_CONFIRM_MS) strcpy(v[MI_RUN], "STOP? OK");
      else strcpy(v[MI_RUN], _ctl->isRunning() ? "WŁ" : "STOP");
      snprintf(v[MI_IVL], 12, "%d s", _ctl->getInterval());
      strcpy(v[MI_SOUND], _ctl->isBuzzerOn() ? "WŁ" : "WYŁ");
      strcpy(v[MI_GPS], _ctl->isGpsOn() ? "WŁ" : "WYŁ");
      strcpy(v[MI_BLE], _ctl->isBleOn() ? "WŁ" : "WYŁ");
      strcpy(v[MI_LOCK], _ctl->isLockOn() ? "WŁ" : "WYŁ");
      strcpy(v[MI_TIMEOUT], _ctl->isUsb() ? "USB: nigdy" : TIMEOUT_TXT[_ctl->getTimeoutIdx() & 3]);
      strcpy(v[MI_INFO], ">");
      strcpy(v[MI_SERVICE], ">");
      const char* vals[MI_COUNT];
      for (int i = 0; i < MI_COUNT; i++) vals[i] = v[i];
      drawMenuScreen(_g, "MENU", labels, vals, MI_COUNT, _sel, _top);
      break;
    }
    case SCR_SERVICE: {
      const char* labels[SI_COUNT] = {"Usuń dane", "Usuń parowania BT", "Nowy PIN BT", "Ust. fabryczne", "Nowa tożsamość", "Wstecz"};
      const char* vals[SI_COUNT] = {"", "", "", "", "", ""};
      drawMenuScreen(_g, "SERWIS", labels, vals, SI_COUNT, _ssel, _stop);
      break;
    }
    case SCR_INFO: {
      char lines[6][MAP_LINE_SZ];
      int n = _ctl->getInfoLines(lines, 6);
      drawLinesScreen(_g, "INFORMACJE", lines, n);
      break;
    }
    case SCR_CONFIRM: {
      char l1[MAP_LINE_SZ];
      unsigned long need = (_confirm == CF_POWER || _confirm == CF_BONDS || _confirm == CF_PIN) ? 2000UL : 3000UL;
      int pct = _hold_start ? (int)((millis() - _hold_start) * 100 / need) : 0;
      uint32_t p = _ctl->pendingCount();
      switch (_confirm) {
        case CF_ERASE:
          if (p) snprintf(l1, sizeof(l1), "%lu pkt NIEWYSŁANYCH!", (unsigned long) p);
          else strcpy(l1, "Wszystko wysłane.");
          drawConfirmScreen(_g, "USUŃ DANE", l1, p ? "Zostaną skasowane." : "Wyczyść pamięć?", "Trzymaj OK 3 s", pct);
          break;
        case CF_BONDS:
          drawConfirmScreen(_g, "PAROWANIA BT", "Telefony trzeba będzie", "sparować ponownie.", "Trzymaj OK 2 s", pct);
          break;
        case CF_PIN: {
          char l2[MAP_LINE_SZ];
          snprintf(l2, sizeof(l2), "Obecny: %06lu", (unsigned long) _ctl->getPin());
          drawConfirmScreen(_g, "NOWY PIN BT", "Losuje nowy PIN i usuwa", l2, "Trzymaj OK 2 s", pct);
          break;
        }
        case CF_FACTORY:
          if (p) snprintf(l1, sizeof(l1), "Usuwa %lu niewysł. pkt", (unsigned long) p);
          else strcpy(l1, "Ustawienia, nazwa, dane");
          drawConfirmScreen(_g, "UST. FABRYCZNE", l1, "Klucz (konto) zostaje.", "Trzymaj OK 3 s", pct);
          break;
        case CF_NEWID1:
          drawConfirmScreen(_g, "NOWA TOŻSAMOŚĆ", "Nowy klucz = NOWE", "KONTO + reset danych", "OK = dalej · wstecz", -1);
          break;
        case CF_NEWID2:
          drawConfirmScreen(_g, "NA PEWNO?", "Starego konta nie da", "się odzyskać.", "Trzymaj OK 3 s", pct);
          break;
        case CF_POWER:
          drawConfirmScreen(_g, "WYŁĄCZ", "Włączenie: naciśnij", "środek joysticka.", "Trzymaj OK 2 s", pct);
          break;
        default: break;
      }
      break;
    }
  }
  _disp->endFrame();
#endif
}
