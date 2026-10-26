#pragma once
/*
 * Interfejs ekranu „MC SF6” dla Wio Tracker L1: Companion MeshCore + SF6 Mapper.
 * Karuzela ekranow (joystick lewo/prawo): Start, Wiadomosci, Mapper, Repeatery, Ustawienia.
 * Wszystko po polsku (wlasna czcionka z polskimi literami), skroty OK:
 *   przytrzymanie 2 s = odblokowanie, 2x = mapper WL, 3x = mapper STOP, przytrzymanie 8 s = SOS na kanal Public.
 */
#include "../AbstractUITask.h"
#include "../NodePrefs.h"
#include <helpers/SensorManager.h>
#include <helpers/ui/DisplayDriver.h>
#include "MapperGfx.h"

#define UI_MSG_MAX     32
#define UI_THREAD_MAX  12
#define UI_RPT_MAX     40

struct UiMsg {
  uint32_t ts;
  char thread[24];          // nadawca (wiadomosc prywatna) albo nazwa kanalu
  char text[150];
  bool chan, unread, mine;
};

class UITask : public AbstractUITask {
public:
  UITask(mesh::MainBoard* board, MultiSerialInterface* im) : AbstractUITask(board, im) { }
  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);
  void msgRead(int msgcount) override;
  void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) override;
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;
  void mapperBeep();

private:
  DisplayDriver* _display = NULL;
  SensorManager* _sensors = NULL;
  NodePrefs* _prefs = NULL;
  MapperGfx _g;

  // ekrany
  enum Screen { SCR_SPLASH, SCR_HOME, SCR_THREAD, SCR_MENU, SCR_LINES, SCR_RPT, SCR_FREQ, SCR_KBD, SCR_SCAN, SCR_WAIT, SCR_HEARD };
  enum Page { P_START, P_MSGS, P_CONTACTS, P_MAPPER, P_RPT, P_SET, P_COUNT };
  uint8_t _screen = SCR_SPLASH, _page = P_START;
  unsigned long _splash_at = 0;
  bool _dirty = true;
  unsigned long _next_render = 0;

  // listy / menu
  int _sel = 0, _top = 0;           // zaznaczenie na aktualnej liscie
  int _menu = 0;                    // id podmenu (SCR_MENU)
  int _msel[8] = {0};               // zapamietane zaznaczenie w menu
  int _thread_sel = 0, _thread_msg = 0, _thread_line = 0;
  bool _mapper_list = false;        // ekran Mappera: false = stan, true = ostatnie RPT
  unsigned long _stop_armed = 0;    // potwierdzenie STOP / restart / wylacz
  int _armed_item = -1;
  char _lines[8][32]; int _nlines = 0, _lines_top = 0; char _lines_title[24];
  // radio: zmiany przygotowywane w menu, zapisywane dopiero przez „Zastosuj”
  uint32_t _rf_khz = 869618; float _rbw = 62.5f; uint8_t _rsf = 6, _rcr = 5;
  char _fdig[7]; int _fcur = 0;
  void radioLoad();
  bool radioChanged();
  void drawFreq();

  // wiadomosci
  UiMsg _msgs[UI_MSG_MAX]; int _msg_n = 0;
  char _threads[UI_THREAD_MAX][24]; int _thread_n = 0;
  void buildThreads();
  int  unreadCount(const char* thread = NULL);
  void addMsg(const char* thread, const char* text, bool chan, bool mine);

  // repeatery
  struct Rpt { char name[24]; uint8_t key[32]; int32_t dist_m; uint32_t last; int32_t lat, lon; bool fav; };
  Rpt _rpts[UI_RPT_MAX]; int _rpt_n = 0; unsigned long _rpt_at = 0;
  void buildRpts();
  bool rptName(const uint8_t* prefix2, char* dest, int dsz);

  // ekran / blokada / ustawienia UI
  bool _on = true, _locked = false;
  unsigned long _last_input = 0;
  uint8_t _timeout_idx = 1;         // 15 s / 30 s / 60 s / nigdy
  bool _lock_en = true, _wake_msg = true;
  void loadUiPrefs(); void saveUiPrefs();
  void wake();
  bool usbPowered();

  // przycisk OK (wlasna obsluga: klikniecia, przytrzymania)
  bool _ok_down = false, _ok_woke = false, _unlock_done = false, _sos_sent = false, _ok_from_lock = false;
  // przewijanie dlugiej nazwy na zaznaczonym wierszu (po chwili postoju)
  unsigned long _sel_at = 0; int _sel_key = -1; bool _marquee = false;
  void selRow(const char* name, char* out, int outsz, int max_w);
  unsigned long _ok_t0 = 0, _click_t = 0;
  bool _ok_raw = false; unsigned long _ok_raw_t = 0;   // odbijanie styku przycisku (debounce)
  int _clicks = 0;
  void okLoop();
  void okClicks(int n);
  bool shortcutsHere() const { return _screen == SCR_HOME && (_page == P_START || _page == P_MAPPER); }

  // okienka
  char _pop1[32] = "", _pop2[32] = "", _pop3[32] = "";
  unsigned long _pop_until = 0;
  int _pop_pct = -1;
  bool _pop_msg = false;            // okienko nowej wiadomosci (OK otwiera watek)
  void popup(const char* l1, const char* l2 = NULL, const char* l3 = NULL, unsigned long ms = 1800);

  // akcje
  void key(char k);                 // 'L','R','U','D','O','B'
  void homeKey(char k);
  void menuKey(char k);
  void menuAction(int dir);
  void openMenu(int id);
  void sendSOS();
  void mapperSet(bool on);

  // rysowanie
  void render();
  void statusBar();
  void drawSplash(unsigned long t);
  void drawStart();
  uint32_t _beat_hits = 0; unsigned long _beat_at = 0;      // „bicie serca” na Starcie przy odpowiedzi repeatera
  void drawMsgs();
  void drawThread();
  void drawMapper();
  void drawRpts();
  void drawRptDetail();
  void drawSettings();
  void drawMenu();
  void drawLines();
  void drawPopup();
  int  menuCount(int id);
  // pisanie wiadomosci (klawiatura ekranowa)
  char _compose_to[24]; bool _compose_chan = true;
  char _kbd[136]; int _kr = 0, _kc = 0; bool _kupper = false; uint8_t _kbd_from = 0;
  uint8_t _kbd_mode = 0;           // 0 wiadomosc, 1 haslo, 2 nazwa repeatera, 3 komenda konsoli
  void kbdOpen(uint8_t mode, uint8_t from, const char* init = "");
  void kbdSubmit();
  void composeTo(const char* to, bool chan);
  void kbdKey(char k);
  void drawKbd();
  bool sendText(const char* text);
  void myPosition(char* out, int sz);
  // wiadomosci zapisane w pamieci (zostaja po restarcie)
  unsigned long _msg_save_at = 0;
  void saveMsgs(); void loadMsgs();
  void drawScan();
  // zarzadzanie repeaterem
  uint8_t _wait_kind = 0;          // 1 logowanie, 2 stan, 3 sasiedzi, 4 komenda
  unsigned long _wait_t0 = 0, _wait_ms = 0;
  uint8_t _lines_back = 0;         // dokad wraca ekran z liniami
  char _pw_typed[24]; bool _pw_new = false;
  char _cli_cmd[48];
  char _cli_next[48];              // druga komenda w kolejce (np. set lon po set lat)
  int8_t _txp_sel = 22;
  void adminWait(uint8_t kind);
  void adminLoop();
  void adminCli(const char* cmd);
  void adminStartLogin(bool admin);
  bool pwGet(const uint8_t* key, char* out);
  void pwSet(const uint8_t* key, const char* pw);
  int  pwCount();
  void pwClear();
  void drawWait();
  int  rptIndexByKey(const uint8_t* key);
  int  _rpt_sel = 0;
  // biezacy watek (kanal albo rozmowa prywatna)
  char _cur_thread[24] = ""; bool _cur_chan = true;
  // lista na ekranie Wiadomosci: kanaly, potem rozmowy prywatne
  char _mrow[28][24]; bool _mrow_chan[28]; int _mrow_n = 0;
  void buildMsgRows();
  void openThread(const char* name, bool chan);
  // biezacy kontakt (z Kontaktow albo z Repeaterow)
  uint8_t _cur_key[32]; char _cur_name[24] = ""; uint8_t _cur_type = 0; bool _cur_fav = false; uint8_t _cur_from = 0;
  void setCur(const uint8_t* key, const char* name, uint8_t type, bool fav, uint8_t from);
  // Kontakty: ulubione na gorze, potem ostatnio slyszane
  struct Ct { uint16_t idx; uint8_t type, fav; uint32_t last; };
  Ct _cts[120]; int _ct_n = 0; unsigned long _ct_at = 0;
  void buildContacts();
  void drawContacts();
  int  _heard_sel = 0;
  void drawHeard();
  int  ctMenu(uint8_t* acts);
  void menuItem(int id, int i, char* label, char* value);
};
