#include "UITask.h"
#include <target.h>
#include <InternalFileSystem.h>
#include <helpers/ui/MomentaryButton.h>
#include <helpers/AdvertDataHelpers.h>
#include "../MyMesh.h"
#include "../CompanionMapper.h"
#if defined(QSPIFLASH)
  #include <CustomLFS_QSPIFlash.h>
  #define MSG_FS QSPIFlash
#else
  #define MSG_FS InternalFS
#endif
#define MSG_FILE "/sf6msg"
#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
  static genericBuzzer buzzer;
#endif

#define UI_VERSION       "1.4.0"
#define UI_PREFS_FILE    "/sf6ui"
#define OK_CLICK_MS      600UL     // dluzsze nacisniecie to nie klikniecie
#define OK_MULTI_MS      400UL     // okno na 2./3. klikniecie (skroty)
#define OK_UNLOCK_MS     2000UL
#define OK_SOS_SHOW_MS   5000UL
#define OK_SOS_MS        8000UL
#define CONFIRM_MS       4000UL
#define ROWS             4
#define ROW_H            10

static MomentaryButton joy_up(JOYSTICK_UP, 1000, true, false, false);
static MomentaryButton joy_down(JOYSTICK_DOWN, 1000, true, false, false);
static UITask* g_ui = NULL;
static bool beep_restore = false;

void mapperUiHit() { if (g_ui) g_ui->mapperBeep(); }

// ---------------- pomocnicze ----------------
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2; const int32_t era = (y >= 0 ? y : y - 399) / 400; const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t) doe - 719468;
}
static uint32_t lastSunday(int y, unsigned m) {   // ostatnia niedziela miesiaca, 01:00 UTC
  int32_t d = daysFromCivil(y, m, 31); int wd = (int)((d + 4) % 7); if (wd < 0) wd += 7;
  return (uint32_t)(d - wd) * 86400UL + 3600UL;
}
static uint32_t localTime(uint32_t utc) {         // Polska: CET/CEST wg zasad UE
  int32_t z = (int32_t)(utc / 86400UL) + 719468; const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097); const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = (int)(yoe + era * 400); const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); const unsigned mp = (5 * doy + 2) / 153;
  unsigned m = mp + (mp < 10 ? 3 : -9); y += (m <= 2);
  return utc + ((utc >= lastSunday(y, 3) && utc < lastSunday(y, 10)) ? 7200UL : 3600UL);
}
static void fmtHM(char* d, int sz, uint32_t utc) {
  if (utc < 1600000000UL) { snprintf(d, sz, "--:--"); return; }
  uint32_t t = localTime(utc) % 86400UL;
  snprintf(d, sz, "%02lu:%02lu", (unsigned long)(t / 3600), (unsigned long)((t / 60) % 60));
}
static void fmtAgo(char* d, int sz, uint32_t ts) {
  uint32_t now = rtc_clock.getCurrentTime();
  if (ts < 1600000000UL || now < ts) { snprintf(d, sz, "-"); return; }
  uint32_t s = now - ts;
  if (s < 90) snprintf(d, sz, "teraz");
  else if (s < 3600) snprintf(d, sz, "%lu min", (unsigned long)(s / 60));
  else if (s < 86400) snprintf(d, sz, "%lu h", (unsigned long)(s / 3600));
  else snprintf(d, sz, "%lu dni", (unsigned long)(s / 86400));
}
static void fmtSnr(char* d, int sz, int8_t x4) {
  int t = (int)x4 * 10 / 4;
  snprintf(d, sz, "%s%d,%d", t < 0 ? "-" : "+", abs(t) / 10, abs(t) % 10);
}
// tylko znaki, ktore umie czcionka (bez emotek), bez spacji na koncu
static void cleanText(char* d, int dsz, const char* s) {
  int n = 0;
  const char* p = s;
  while (*p && n < dsz - 4) {
    const char* q = p;
    uint8_t a;
    uint32_t cp = mapperUtf8Next(q);
    if (MapperGfx::glyphFor(cp, &a)) { int len = (int)(q - p); memcpy(d + n, p, len); n += len; }
    p = q;
  }
  while (n > 0 && d[n - 1] == ' ') n--;
  d[n] = 0;
}
static void fmtDist(char* d, int sz, int32_t m) {
  if (m < 0) snprintf(d, sz, "-");
  else if (m < 1000) snprintf(d, sz, "%ld m", (long)m);
  else if (m < 10000) snprintf(d, sz, "%ld,%ld km", (long)(m / 1000), (long)((m % 1000) / 100));
  else snprintf(d, sz, "%ld km", (long)((m + 500) / 1000));
}
static double distM(double la1, double lo1, double la2, double lo2) {
  double a = la1 * DEG_TO_RAD, b = la2 * DEG_TO_RAD;
  double x = (lo2 - lo1) * DEG_TO_RAD * cos((a + b) / 2), y = b - a;
  return sqrt(x * x + y * y) * 6371000.0;
}
static int bearing(double la1, double lo1, double la2, double lo2) {
  double y = sin((lo2 - lo1) * DEG_TO_RAD) * cos(la2 * DEG_TO_RAD);
  double x = cos(la1 * DEG_TO_RAD) * sin(la2 * DEG_TO_RAD) - sin(la1 * DEG_TO_RAD) * cos(la2 * DEG_TO_RAD) * cos((lo2 - lo1) * DEG_TO_RAD);
  int b = (int)(atan2(y, x) * RAD_TO_DEG); return (b + 360) % 360;
}
static const char* compass(int b) {
  static const char* const N[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return N[((b + 22) / 45) % 8];
}
static int battPct(uint16_t mv) {
  if (mv >= 4150) return 100;
  if (mv <= 3300) return 0;
  return (int)((mv - 3300) * 100UL / 850);
}
// zawijanie tekstu UTF-8 po slowach do linii o szerokosci max_w px (6 px na znak)
static int wrapText(const char* s, char out[][32], int max_lines, int max_chars) {
  int n = 0;
  while (*s && n < max_lines) {
    while (*s == ' ') s++;
    const char* p = s; const char* brk = NULL; int chars = 0;
    while (*p && *p != '\n' && chars < max_chars) {
      const char* q = p; mapperUtf8Next(q);
      if (*p == ' ') brk = p;
      p = q; chars++;
    }
    const char* end = (*p && *p != '\n' && brk && chars >= max_chars) ? brk : p;
    int len = (int)(end - s); if (len > 31) len = 31;
    memcpy(out[n], s, len); out[n][len] = 0; n++;
    s = end; if (*s == '\n') s++;
  }
  return n;
}

// ---------------- start ----------------
void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  g_ui = this;
  _display = display; _sensors = sensors; _prefs = node_prefs;
  _g.attach(display);
  joy_up.begin(); joy_down.begin();
  user_btn.begin(); joystick_left.begin(); joystick_right.begin(); back_btn.begin();
#ifdef PIN_BUZZER
  buzzer.begin();
  buzzer.quiet(_prefs->buzzer_quiet);
#endif
  loadUiPrefs();
  loadMsgs();
  _screen = SCR_SPLASH; _splash_at = millis(); _last_input = millis();
  if (_display) _display->turnOn();
}

void UITask::loadUiPrefs() {
  File f = InternalFS.open(UI_PREFS_FILE, FILE_O_READ);
  if (!f) return;
  uint8_t b[4];
  if (f.read(b, 4) == 4 && b[0] == 0x5A) { _timeout_idx = b[1] <= 3 ? b[1] : 1; _lock_en = b[2] != 0; _wake_msg = b[3] != 0; }
  f.close();
}
void UITask::saveUiPrefs() {
  uint8_t b[4] = { 0x5A, _timeout_idx, (uint8_t)(_lock_en ? 1 : 0), (uint8_t)(_wake_msg ? 1 : 0) };
  InternalFS.remove(UI_PREFS_FILE);
  File f = InternalFS.open(UI_PREFS_FILE, FILE_O_WRITE);
  if (!f) return;
  f.write(b, 4); f.close();
}

bool UITask::usbPowered() { return _board->isExternalPowered(); }

void UITask::wake() {
  _last_input = millis();
  if (_display && !_on) { _display->turnOn(); _on = true; }
  _dirty = true;
}

void UITask::popup(const char* l1, const char* l2, const char* l3, unsigned long ms) {
  strncpy(_pop1, l1 ? l1 : "", sizeof(_pop1) - 1); _pop1[sizeof(_pop1) - 1] = 0;
  strncpy(_pop2, l2 ? l2 : "", sizeof(_pop2) - 1); _pop2[sizeof(_pop2) - 1] = 0;
  strncpy(_pop3, l3 ? l3 : "", sizeof(_pop3) - 1); _pop3[sizeof(_pop3) - 1] = 0;
  _pop_until = millis() + ms; _pop_pct = -1; _pop_msg = false;
  _dirty = true;
}

void UITask::mapperBeep() {
#ifdef PIN_BUZZER
  if (buzzer.isPlaying()) return;
  if (buzzer.isQuiet()) { buzzer.quiet(false); beep_restore = true; }
  buzzer.play("hit:d=16,o=6,b=180:c,e");
#endif
}

// ---------------- wiadomosci ----------------
void UITask::addMsg(const char* thread, const char* text, bool chan, bool mine) {
  if (_msg_n == UI_MSG_MAX) { memmove(&_msgs[0], &_msgs[1], sizeof(UiMsg) * (UI_MSG_MAX - 1)); _msg_n--; }
  UiMsg& m = _msgs[_msg_n++];
  m.ts = rtc_clock.getCurrentTime();
  strncpy(m.thread, thread, sizeof(m.thread) - 1); m.thread[sizeof(m.thread) - 1] = 0;
  strncpy(m.text, text, sizeof(m.text) - 1); m.text[sizeof(m.text) - 1] = 0;
  m.chan = chan; m.unread = !mine; m.mine = mine;
  buildThreads();
  _msg_save_at = millis() + 5000;
}

void UITask::buildThreads() {               // watki od najnowszego
  _thread_n = 0;
  for (int i = _msg_n - 1; i >= 0 && _thread_n < UI_THREAD_MAX; i--) {
    bool seen = false;
    for (int k = 0; k < _thread_n; k++) if (strcmp(_threads[k], _msgs[i].thread) == 0) { seen = true; break; }
    if (!seen) { strcpy(_threads[_thread_n++], _msgs[i].thread); }
  }
}

int UITask::unreadCount(const char* thread) {
  int n = 0;
  for (int i = 0; i < _msg_n; i++) if (_msgs[i].unread && (!thread || strcmp(_msgs[i].thread, thread) == 0)) n++;
  return n;
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
  bool chan = false;                         // kanal czy wiadomosc prywatna
  ChannelDetails cd;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (!the_mesh.getChannel(i, cd)) break;
    if (cd.name[0] && strcmp(cd.name, from_name) == 0) { chan = true; break; }
  }
  addMsg(from_name, text, chan, false);
  if (_wake_msg) wake();
  char l1[32], l2[32];
  snprintf(l1, sizeof(l1), "> %s", from_name);
  MapperGfx::fit(l2, sizeof(l2), text, 112);
  popup(l1, l2, NULL, 6000);
  _pop_msg = true;
#ifdef PIN_BUZZER
  if (!_prefs->buzzer_quiet && !buzzer.isPlaying()) buzzer.play(chan ? "ch:d=16,o=6,b=200:e" : "dm:d=16,o=6,b=200:e,g,c7");
#endif
}

void UITask::msgRead(int msgcount) {
  if (msgcount == 0) { for (int i = 0; i < _msg_n; i++) _msgs[i].unread = false; _dirty = true; }
}

void UITask::notify(UIEventType t) { _dirty = true; }

void UITask::saveMsgs() {
  MSG_FS.remove(MSG_FILE);
  File f = MSG_FS.open(MSG_FILE, FILE_O_WRITE);
  if (!f) return;
  uint8_t h[4] = { 'S', 'M', 1, (uint8_t) _msg_n };
  f.write(h, 4);
  f.write((const uint8_t*) _msgs, sizeof(UiMsg) * _msg_n);
  f.close();
}
void UITask::loadMsgs() {
  File f = MSG_FS.open(MSG_FILE, FILE_O_READ);
  if (!f) return;
  uint8_t h[4];
  if (f.read(h, 4) == 4 && h[0] == 'S' && h[1] == 'M' && h[2] == 1 && h[3] <= UI_MSG_MAX) {
    if (f.read((uint8_t*) _msgs, sizeof(UiMsg) * h[3]) == (int)(sizeof(UiMsg) * h[3])) _msg_n = h[3];
  }
  f.close();
  buildThreads();
}

// ---------------- pisanie ----------------
void UITask::myPosition(char* out, int sz) {
  LocationProvider* l = _sensors ? _sensors->getLocationProvider() : NULL;
  double la = 0, lo = 0; bool have = false, old = false;
  if (l && l->isValid()) { la = l->getLatitude() / 1e6; lo = l->getLongitude() / 1e6; have = true; }
  else if (sensors.node_lat != 0 || sensors.node_lon != 0) { la = sensors.node_lat; lo = sensors.node_lon; have = true; old = true; }
  if (!have) { out[0] = 0; return; }
  snprintf(out, sz, "Moja pozycja%s: %.5f,%.5f https://maps.google.com/?q=%.5f,%.5f", old ? " (ostatnia znana)" : "", la, lo, la, lo);
}

void UITask::composeTo(const char* to, bool chan) {
  strncpy(_compose_to, to, sizeof(_compose_to) - 1); _compose_to[sizeof(_compose_to) - 1] = 0;
  _compose_chan = chan;
}

bool UITask::sendText(const char* text) {
  if (!text || !text[0]) return false;
  bool ok;
  if (_compose_chan) { int idx = the_mesh.uiChannelIndex(_compose_to); ok = idx >= 0 && the_mesh.uiSendChannelText(idx, text); }
  else ok = the_mesh.uiSendDirectText(_compose_to, text);
  if (ok) {
    addMsg(_compose_to, text, _compose_chan, true);
    for (int t = 0; t < _thread_n; t++) if (strcmp(_threads[t], _compose_to) == 0) { _thread_sel = t; break; }
    _thread_msg = -1; _thread_line = 0; _screen = SCR_THREAD;
    popup("Wysłano", _compose_to);
  } else popup("Błąd wysyłki", _compose_chan ? "brak kanału" : "brak kontaktu");
  return ok;
}

// klawiatura: 4 rzedy po 13 znakow + rzad klawiszy (Aa, spacja, usun, WYSLIJ)
static const char* const KB[4][13] = {
  { "a", "ą", "b", "c", "ć", "d", "e", "ę", "f", "g", "h", "i", "j" },
  { "k", "l", "ł", "m", "n", "ń", "o", "ó", "p", "r", "s", "ś", "t" },
  { "u", "w", "y", "z", "ź", "ż", "q", "v", "x", ".", ",", "?", "!" },
  { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-", ":", "/" } };
static const char* const KB_UP[] = { "ą", "Ą", "ć", "Ć", "ę", "Ę", "ł", "Ł", "ń", "Ń", "ó", "Ó", "ś", "Ś", "ź", "Ź", "ż", "Ż" };
static const char* kbUpper(const char* k) {
  static char b[4];
  if (k[1] == 0) { b[0] = (k[0] >= 'a' && k[0] <= 'z') ? k[0] - 32 : k[0]; b[1] = 0; return b; }
  for (int i = 0; i < 18; i += 2) if (strcmp(KB_UP[i], k) == 0) return KB_UP[i + 1];
  return k;
}

void UITask::kbdKey(char k) {
  int len = strlen(_kbd);
  if (k == 'U') { if (_kr == 4) _kc = _kc * 13 / 4 + 1; _kr = (_kr + 4) % 5; if (_kr == 4) _kc = _kc * 4 / 13; return; }
  if (k == 'D') { if (_kr == 4) _kc = _kc * 13 / 4 + 1; _kr = (_kr + 1) % 5; if (_kr == 4) _kc = _kc * 4 / 13; if (_kc > 12) _kc = 12; return; }
  int cols = _kr == 4 ? 4 : 13;
  if (k == 'L') { _kc = (_kc + cols - 1) % cols; return; }
  if (k == 'R') { _kc = (_kc + 1) % cols; return; }
  if (k == 'B') {                                  // usun znak (UTF-8); pusty tekst = wyjscie
    if (!len) { _screen = _kbd_from; return; }
    int i = len - 1; while (i > 0 && ((uint8_t)_kbd[i] & 0xC0) == 0x80) i--;
    _kbd[i] = 0; return;
  }
  if (k != 'O') return;
  if (_kr == 4) {
    if (_kc == 0) _kupper = !_kupper;
    else if (_kc == 1) { if (len < 130) { _kbd[len] = ' '; _kbd[len + 1] = 0; } }
    else if (_kc == 2) kbdKey('B');
    else if (_kc == 3) kbdSubmit();
    return;
  }
  const char* c = KB[_kr][_kc];
  if (_kupper) { c = kbUpper(c); _kupper = false; }
  int cl = strlen(c);
  if (len + cl < 131) { memcpy(_kbd + len, c, cl + 1); }
}

void UITask::drawKbd() {
  char b[32], to[20];
  MapperGfx::fit(to, sizeof(to), _compose_to, 84);
  static const char* const H[4] = { "Do: %s", "Hasło: %s", "Nazwa: %s", "Komenda: %s" };
  snprintf(b, sizeof(b), H[_kbd_mode & 3], _kbd_mode == 0 ? to : "");
  _g.text(0, 0, b);
  snprintf(b, sizeof(b), "%d", (int) strlen(_kbd));
  _g.textRight(127, 0, b);
  _g.hline(0, 9, 128);
  // koniec tekstu (ostatnie 20 znakow) + kursor
  const char* p = _kbd; int n = 0; { const char* q = _kbd; while (*q) { mapperUtf8Next(q); n++; } }
  int skip = n > 20 ? n - 20 : 0;
  while (skip-- > 0) mapperUtf8Next(p);
  int w;
  if (_kbd_mode == 1) {                              // haslo: gwiazdki, ostatni znak widoczny
    char m[48]; int k2 = 0;
    const char* q = p;
    while (*q && k2 < 40) {
      const char* q0 = q; mapperUtf8Next(q);
      if (*q) m[k2++] = '*';
      else { int l = (int)(q - q0); memcpy(m + k2, q0, l); k2 += l; }
    }
    m[k2] = 0; w = _g.text(0, 11, m);
  } else w = _g.text(0, 11, p);
  if ((millis() / 400) % 2) _g.rect(w + 1, 18, 5, 1);
  _g.hline(0, 20, 128);
  for (int r = 0; r < 4; r++) for (int c = 0; c < 13; c++) {
    int x = 1 + c * 10 - (c > 0 ? 0 : 0), y = 22 + r * 8;
    const char* t = _kupper ? kbUpper(KB[r][c]) : KB[r][c];
    char tc[4]; strncpy(tc, t, 3); tc[3] = 0;
    if (r == _kr && c == _kc) { _g.rect(x, y - 1, 8, 9); _g.color(false); _g.text(x + 1, y, tc); _g.color(true); }
    else _g.text(x + 1, y, tc);
  }
  _g.hline(0, 54, 128);
  static const char* const BK0[4] = { "Aa", "spacja", "usuń", "WYŚLIJ" };
  static const char* const BK1[4] = { "Aa", "spacja", "usuń", "OK" };
  const char* const* BK = _kbd_mode == 0 ? BK0 : BK1;
  static const int BX[4] = { 0, 16, 56, 88 };
  for (int i = 0; i < 4; i++) {
    int wv = _g.textWidth(BK[i]) + 2;
    bool sel = _kr == 4 && _kc == i;
    bool up = i == 0 && _kupper;
    if (sel || up) { _g.rect(BX[i], 55, wv, 9); _g.color(false); _g.text(BX[i] + 1, 56, BK[i]); _g.color(true); }
    else _g.text(BX[i] + 1, 56, BK[i]);
  }
}

// ---------------- repeatery ----------------
void UITask::buildRpts() {
  _rpt_n = 0; _rpt_at = millis();
  LocationProvider* l = _sensors ? _sensors->getLocationProvider() : NULL;
  bool have = l && l->isValid();
  double mla = have ? l->getLatitude() / 1e6 : 0, mlo = have ? l->getLongitude() / 1e6 : 0;
  ContactInfo c;
  int total = the_mesh.getNumContacts();
  for (int i = 0; i < total; i++) {
    if (!the_mesh.getContactByIdx(i, c) || c.type != ADV_TYPE_REPEATER) continue;
    Rpt r;
    strncpy(r.name, c.name, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = 0;
    memcpy(r.key, c.id.pub_key, 32);
    r.fav = (c.flags & 0x01) != 0;
    r.last = c.last_advert_timestamp; r.lat = c.gps_lat; r.lon = c.gps_lon;
    r.dist_m = (have && (c.gps_lat || c.gps_lon)) ? (int32_t) distM(mla, mlo, c.gps_lat / 1e6, c.gps_lon / 1e6) : -1;
    // wstawianie: najpierw po odleglosci, bez pozycji na koncu (po czasie)
    int pos = _rpt_n;
    while (pos > 0) {
      Rpt& p = _rpts[pos - 1];
      bool before = (r.fav && !p.fav) || (r.fav == p.fav &&
        ((r.dist_m >= 0 && (p.dist_m < 0 || r.dist_m < p.dist_m)) || (r.dist_m < 0 && p.dist_m < 0 && r.last > p.last)));
      if (!before) break;
      pos--;
    }
    if (pos >= UI_RPT_MAX) continue;
    int last = _rpt_n < UI_RPT_MAX ? _rpt_n : UI_RPT_MAX - 1;
    for (int k = last; k > pos; k--) _rpts[k] = _rpts[k - 1];
    _rpts[pos] = r;
    if (_rpt_n < UI_RPT_MAX) _rpt_n++;
  }
}

int UITask::rptIndexByKey(const uint8_t* key) {
  for (int i = 0; i < _rpt_n; i++) if (memcmp(_rpts[i].key, key, 32) == 0) return i;
  return -1;
}

bool UITask::rptName(const uint8_t* p2, char* dest, int dsz) {
  ContactInfo c;
  int total = the_mesh.getNumContacts();
  for (int i = 0; i < total; i++) {
    if (!the_mesh.getContactByIdx(i, c) || c.type != ADV_TYPE_REPEATER) continue;
    if (c.id.pub_key[0] == p2[0] && c.id.pub_key[1] == p2[1]) { strncpy(dest, c.name, dsz - 1); dest[dsz - 1] = 0; return true; }
  }
  snprintf(dest, dsz, "#%02X%02X", p2[0], p2[1]);
  return false;
}

// ---------------- akcje ----------------
void UITask::mapperSet(bool on) {
  if (mapper.isRunning() == on) { popup(on ? "Mapper już działa" : "Mapper już stoi"); return; }
  mapper.setRunning(on);
  popup(on ? "Mapper WŁĄCZONY" : "Mapper ZATRZYMANY", on ? "GPS włączony" : "GPS wyłączony");
#ifdef PIN_BUZZER
  if (!buzzer.isPlaying()) { if (buzzer.isQuiet()) { buzzer.quiet(false); beep_restore = true; } buzzer.play(on ? "on:d=16,o=6,b=200:c,g" : "off:d=16,o=6,b=200:g,c"); }
#endif
}

void UITask::sendSOS() {
  char msg[150], pos[48];
  LocationProvider* l = _sensors ? _sensors->getLocationProvider() : NULL;
  double la = 0, lo = 0; bool have = false, old = false;
  if (l && l->isValid()) { la = l->getLatitude() / 1e6; lo = l->getLongitude() / 1e6; have = true; }
  else if (sensors.node_lat != 0 || sensors.node_lon != 0) { la = sensors.node_lat; lo = sensors.node_lon; have = true; old = true; }
  if (have) {
    snprintf(pos, sizeof(pos), "%.5f,%.5f", la, lo);
    snprintf(msg, sizeof(msg), "SOS! Potrzebuję pomocy. Pozycja%s: %s https://maps.google.com/?q=%s", old ? " (ostatnia znana)" : "", pos, pos);
  } else {
    snprintf(msg, sizeof(msg), "SOS! Potrzebuję pomocy. Brak pozycji GPS.");
    strcpy(pos, "brak pozycji GPS");
  }
  bool ok = the_mesh.uiSendChannelText(0, msg);
  if (ok) addMsg("Public", msg, true, true);
  popup(ok ? "SOS WYSŁANY" : "SOS: błąd wysyłki", ok ? "kanał Public" : "brak kanału Public", pos, 8000);
#ifdef PIN_BUZZER
  if (buzzer.isQuiet()) { buzzer.quiet(false); beep_restore = true; }
  buzzer.play("sos:d=8,o=6,b=180:a,a,a");
#endif
}

void UITask::okClicks(int n) {
  if (n >= 3) { mapperSet(false); return; }
  if (n == 2) { mapperSet(true); return; }
  key('O');
}

// przycisk OK: klikniecia (1/2/3), przytrzymanie 2 s (odblokowanie) i 8 s (SOS)
void UITask::okLoop() {
  bool down = user_btn.isPressed();
  unsigned long now = millis();
  if (down && !_ok_down) {
    _ok_down = true; _ok_t0 = now; _unlock_done = false; _sos_sent = false;
    _ok_woke = !_on;
    wake();
  }
  if (down && _ok_down) {
    unsigned long held = now - _ok_t0;
    if (_locked && !_unlock_done) {
      _pop_pct = (int)(held * 100 / OK_UNLOCK_MS); if (_pop_pct > 100) _pop_pct = 100;
      if (_pop_until < now + 300) { strcpy(_pop1, "Zablokowane"); strcpy(_pop2, "trzymaj OK 2 s"); _pop3[0] = 0; _pop_until = now + 400; _pop_msg = false; }
      _dirty = true;
      if (held >= OK_UNLOCK_MS) { _locked = false; _unlock_done = true; popup("Odblokowano", "dalsze trzymanie: SOS", NULL, 1500); }
    }
    if (held >= OK_SOS_SHOW_MS && !_sos_sent) {
      char l1[32]; unsigned long left = (OK_SOS_MS - (held < OK_SOS_MS ? held : OK_SOS_MS) + 999) / 1000;
      snprintf(l1, sizeof(l1), "SOS za %lu s", left);
      strcpy(_pop1, l1); strcpy(_pop2, "puść OK = anuluj"); _pop3[0] = 0;
      _pop_pct = (int)((held - OK_SOS_SHOW_MS) * 100 / (OK_SOS_MS - OK_SOS_SHOW_MS)); if (_pop_pct > 100) _pop_pct = 100;
      _pop_until = now + 400; _pop_msg = false; _dirty = true;
      if (held >= OK_SOS_MS) { _sos_sent = true; _locked = false; sendSOS(); }
    }
  }
  if (!down && _ok_down) {
    _ok_down = false;
    unsigned long held = now - _ok_t0;
    if (!_sos_sent && held >= OK_SOS_SHOW_MS) popup("SOS anulowane");
    else if (held < OK_CLICK_MS && !_ok_woke && !_locked) { _clicks++; _click_t = now; }
    else if (held < OK_CLICK_MS && _locked) popup("Zablokowane", "trzymaj OK 2 s");
    _pop_pct = -1;
  }
  if (_clicks && !_ok_down) {
    if (!shortcutsHere()) { int n = _clicks; _clicks = 0; for (int i = 0; i < n; i++) key('O'); }
    else if (now - _click_t > OK_MULTI_MS) { int n = _clicks; _clicks = 0; okClicks(n); }
  }
}

// ---------------- petla ----------------
void UITask::loop() {
  unsigned long now = millis();
#ifdef PIN_BUZZER
  if (buzzer.isPlaying()) buzzer.loop();
  if (beep_restore && !buzzer.isPlaying()) { beep_restore = false; buzzer.quiet(_prefs->buzzer_quiet); }
#endif
  if (_screen == SCR_SPLASH) {
    unsigned long t = now - _splash_at;
#ifdef PIN_BUZZER
    static bool beeped = false;
    if (!beeped && t >= 1900) { beeped = true; if (buzzer.isQuiet()) { buzzer.quiet(false); beep_restore = true; } buzzer.play("st:d=16,o=6,b=200:c,p,c"); }
#endif
    bool any = user_btn.isPressed() || joystick_left.isPressed() || joystick_right.isPressed() || back_btn.isPressed();
    if (t >= 2400 || (t > 400 && any)) { _screen = SCR_HOME; _page = P_START; _dirty = true; _last_input = now; }
    else if (now >= _next_render) { render(); _next_render = now + 30; }
    return;
  }

  if (_msg_save_at && (long)(now - _msg_save_at) >= 0) { _msg_save_at = 0; saveMsgs(); }
  adminLoop();
  okLoop();
  char k = 0;
  if (joystick_left.check() == BUTTON_EVENT_CLICK) k = 'L';
  if (joystick_right.check() == BUTTON_EVENT_CLICK) k = 'R';
  if (joy_up.check() == BUTTON_EVENT_CLICK) k = 'U';
  if (joy_down.check() == BUTTON_EVENT_CLICK) k = 'D';
  int bev = back_btn.check();
  if (bev == BUTTON_EVENT_CLICK) k = 'B';
  else if (bev == BUTTON_EVENT_LONG_PRESS) k = 'H';
  if (k) {
    bool was_off = !_on;
    wake();
    if (!was_off) {
      if (_locked) popup("Zablokowane", "trzymaj OK 2 s");
      else key(k);
    } else if (_locked) popup("Zablokowane", "trzymaj OK 2 s");
  }

  // wygaszanie i blokada (na zasilaniu USB ekran zawsze wlaczony, bez blokady)
  static const unsigned long TO[4] = { 15000UL, 30000UL, 60000UL, 0 };
  unsigned long to = TO[_timeout_idx & 3];
  if (_on && to && !usbPowered() && !_ok_down && now - _last_input > to) {
    if (_display) _display->turnOff();
    _on = false;
    if (_lock_en) _locked = true;
    _pop_until = 0;
  }
  if (usbPowered()) _locked = false;

  if (_on && (_dirty || now >= _next_render)) {
    render(); _dirty = false; _next_render = now + 500;
  }
}

// ---------------- klawisze ----------------
void UITask::key(char k) {
  if (k == 'H' && _screen == SCR_KBD) { _screen = _kbd_from; return; }
  if (k == 'H') { _screen = SCR_HOME; _page = P_START; _sel = 0; _top = 0; _mapper_list = false; return; }
  if (_pop_msg && _pop_until > millis() && k == 'O') {       // OK na okienku nowej wiadomosci: otworz watek
    _pop_until = 0; _pop_msg = false;
    buildThreads();
    _thread_sel = 0; _thread_msg = -1; _thread_line = 0; _screen = SCR_THREAD;
    return;
  }
  _pop_until = 0;
  switch (_screen) {
    case SCR_HOME: homeKey(k); break;
    case SCR_MENU: menuKey(k); break;
    case SCR_THREAD: {
      int idx[UI_MSG_MAX], n = 0;
      for (int i = 0; i < _msg_n; i++) if (strcmp(_msgs[i].thread, _threads[_thread_sel]) == 0) idx[n++] = i;
      if (_thread_msg < 0 || _thread_msg >= n) _thread_msg = n - 1;
      char lines[12][32]; int nl = n ? wrapText(_msgs[idx[_thread_msg]].text, lines, 12, 21) : 0;
      if (k == 'D') { if (_thread_line + 4 < nl) _thread_line++; else if (_thread_msg < n - 1) { _thread_msg++; _thread_line = 0; } }
      else if (k == 'U') { if (_thread_line > 0) _thread_line--; else if (_thread_msg > 0) { _thread_msg--; _thread_line = 0; } }
      else if (k == 'B' || k == 'L') { _screen = SCR_HOME; _page = P_MSGS; }
      else if (k == 'O' && n) {                   // odpowiedz: napisz / szybka odpowiedz / moja pozycja
        composeTo(_msgs[idx[_thread_msg]].thread, _msgs[idx[_thread_msg]].chan);
        openMenu(8);
      }
      break;
    }
    case SCR_KBD: kbdKey(k); break;
    case SCR_SCAN:
      if (k == 'O') { if (!mapper.scanActive()) mapper.scanStart(); }
      else if (k == 'B' || k == 'L') _screen = SCR_HOME;
      break;
    case SCR_LINES:
      if (k == 'D' && _lines_top + 4 < _nlines) _lines_top++;
      else if (k == 'U' && _lines_top > 0) _lines_top--;
      else if (k == 'B' || k == 'L' || k == 'O') { _screen = _lines_back; }
      break;
    case SCR_WAIT:
      if (k == 'B' || k == 'L') { _wait_kind = 0; the_mesh.ui_admin.cli_wait = false; _screen = SCR_MENU; }
      break;
    case SCR_RPT:
      if (k == 'O') openMenu(10);
      else if (k == 'B' || k == 'L') { _screen = SCR_HOME; }
      break;
    case SCR_FREQ:
      if (k == 'U' || k == 'D') { int d = _fdig[_fcur] - '0'; d = (d + (k == 'U' ? 1 : 9)) % 10; _fdig[_fcur] = '0' + d; }
      else if (k == 'L') { if (_fcur > 0) _fcur--; }
      else if (k == 'R') { if (_fcur < 5) _fcur++; }
      else if (k == 'O') {
        uint32_t f = (uint32_t) atol(_fdig);
        if (f < 137000 || f > 1020000) popup("Zła częstotliwość", "137–1020 MHz");
        else { _rf_khz = f; _screen = SCR_MENU; }
      } else if (k == 'B') _screen = SCR_MENU;
      break;
  }
}

void UITask::homeKey(char k) {
  if (k == 'L' || k == 'R') {
    _page = (_page + (k == 'R' ? 1 : P_COUNT - 1)) % P_COUNT;
    _sel = 0; _top = 0; _mapper_list = false; _armed_item = -1;
    if (_page == P_RPT) buildRpts();
    if (_page == P_MSGS) buildThreads();
    return;
  }
  if (k == 'B') { if (_page != P_START) { _page = P_START; _sel = 0; _top = 0; } return; }
  int count = _page == P_MSGS ? _thread_n + 1 : _page == P_RPT ? _rpt_n + 1 : _page == P_SET ? menuCount(0) : 0;
  if (_page == P_MAPPER && (k == 'U' || k == 'D')) { _mapper_list = (k == 'D'); return; }
  if ((k == 'U' || k == 'D') && count > 0) {
    _sel = (_sel + (k == 'D' ? 1 : count - 1)) % count;
    if (_sel < _top) _top = _sel;
    if (_sel >= _top + ROWS) _top = _sel - ROWS + 1;
    _armed_item = -1;
    return;
  }
  if (k != 'O') return;
  switch (_page) {
    case P_START:
      if (unreadCount() > 0 && _thread_n > 0) {
        for (int t = 0; t < _thread_n; t++) if (unreadCount(_threads[t])) { _thread_sel = t; break; }
        _thread_msg = -1; _thread_line = 0; _screen = SCR_THREAD;
      } else { _page = P_MSGS; buildThreads(); }
      break;
    case P_MSGS:
      if (_sel == 0) {                              // „Napisz na Public”
        int idx = the_mesh.uiChannelIndex("Public");
        ChannelDetails cd;
        if (idx < 0 && the_mesh.getChannel(0, cd)) composeTo(cd.name, true); else composeTo("Public", true);
        kbdOpen(0, SCR_HOME);
      } else if (_sel - 1 < _thread_n) { _thread_sel = _sel - 1; _thread_msg = -1; _thread_line = 0; _screen = SCR_THREAD; }
      break;
    case P_RPT:
      if (_sel == 0) { mapper.scanStart(); _screen = SCR_SCAN; }
      else if (_sel - 1 < _rpt_n) { _rpt_sel = _sel - 1; _screen = SCR_RPT; }
      break;
    case P_SET:
      _menu = 0; menuAction(+1);
      break;
  }
}

// ---------------- menu ustawien ----------------
// 0 = glowne (strona Ustawienia), 1 Mapper, 2 Bluetooth, 3 Radio, 4 GPS, 5 Advert, 6 Ekran, 7 Dzwiek
static const uint8_t TXP[] = { 2, 5, 8, 11, 14, 17, 20, 22 };
static const char* const QUICK[8] = { "OK", "Jadę", "Jestem na miejscu", "Odbiór?", "Słyszę Cię dobrze", "Za 10 minut", "Dzięki!", "Zadzwoń do mnie" };
static const char* const TO_LABEL[4] = { "15 s", "30 s", "60 s", "nigdy" };

int UITask::menuCount(int id) {
  static const uint8_t C[15] = { 11, 4, 3, 7, 3, 2, 3, 2, 3, 8, 3, 5, 2, 7, 7 };
  return id >= 0 && id < 15 ? C[id] : 0;
}

void UITask::menuItem(int id, int i, char* L, char* V) {
  V[0] = 0;
  bool armed = _armed_item == id * 16 + i && millis() - _stop_armed < CONFIRM_MS;
  switch (id) {
    case 0: {
      static const char* const N[11] = { "Mapper", "Bluetooth", "Radio", "GPS", "Advert", "Ekran", "Dźwięk", "Informacje", "Hasła repeaterów", "Restart", "Wyłącz" };
      strcpy(L, N[i]);
      if (i == 0) strcpy(V, mapper.isRunning() ? "WŁ" : "STOP");
      if (i == 1) strcpy(V, isBluetoothEnabled() ? (hasConnection() ? "połącz." : "WŁ") : "WYŁ");
      if (i == 2) strcpy(V, the_mesh.mpRadioIsSF6() ? "SF6" : "inne!");
      if (i == 3) strcpy(V, _prefs->gps_enabled ? "WŁ" : "WYŁ");
      if (i == 5) strcpy(V, TO_LABEL[_timeout_idx & 3]);
      if (i == 8) { if (armed) strcpy(V, "usuń?"); else snprintf(V, 8, "%d", pwCount()); }
      if ((i == 9 || i == 10) && armed) strcpy(V, "OK?");
      break;
    }
    case 1: {
      static const char* const N[4] = { "Mapowanie", "Ping co", "Dźwięk", "Nowa sesja" };
      strcpy(L, N[i]);
      if (i == 0) strcpy(V, armed ? "STOP? OK" : mapper.isRunning() ? "WŁ" : "STOP");
      if (i == 1) snprintf(V, 12, "%d s", mapper.getInterval());
      if (i == 2) strcpy(V, mapper.soundOn() ? "WŁ" : "WYŁ");
      if (i == 3 && armed) strcpy(V, "OK?");
      break;
    }
    case 2: {
      static const char* const N[3] = { "Bluetooth", "PIN", "Telefon" };
      strcpy(L, N[i]);
      if (i == 0) strcpy(V, isBluetoothEnabled() ? "WŁ" : "WYŁ");
      if (i == 1) { uint32_t p = the_mesh.getBLEPin(); if (p) snprintf(V, 12, "%06lu", (unsigned long)p); else strcpy(V, "brak"); }
      if (i == 2) strcpy(V, hasConnection() ? "połączony" : "brak");
      break;
    }
    case 3: {
      static const char* const N[7] = { "Preset SF6", "Moc nadawania", "Częstotliwość", "Szerokość BW", "SF", "CR", "Zastosuj" };
      strcpy(L, N[i]);
      if (i == 0) strcpy(V, the_mesh.mpRadioIsSF6() ? "aktywny" : "ustaw");
      if (i == 1) snprintf(V, 12, "%d dBm", (int)_prefs->tx_power_dbm);
      if (i == 2) snprintf(V, 12, "%lu,%03lu", (unsigned long)(_rf_khz / 1000), (unsigned long)(_rf_khz % 1000));
      if (i == 3) { int b = (int)(_rbw * 10 + 0.5f); if (b % 10) snprintf(V, 14, "%d,%d", b / 10, b % 10); else snprintf(V, 14, "%d", b / 10); }
      if (i == 4) snprintf(V, 8, "%d", _rsf);
      if (i == 5) snprintf(V, 8, "4/%d", _rcr);
      if (i == 6) strcpy(V, armed ? "OK?" : radioChanged() ? "zmiany!" : "");
      break;
    }
    case 4: {
      static const char* const N[3] = { "GPS", "Satelity", "Pozycja w advert" };
      strcpy(L, N[i]);
      LocationProvider* l = _sensors ? _sensors->getLocationProvider() : NULL;
      if (i == 0) strcpy(V, _prefs->gps_enabled ? "WŁ" : "WYŁ");
      if (i == 1) { if (l && l->isValid()) snprintf(V, 12, "%d (fix)", (int)l->satellitesCount()); else snprintf(V, 12, "%d", l ? (int)l->satellitesCount() : 0); }
      if (i == 2) strcpy(V, _prefs->advert_loc_policy ? "WŁ" : "WYŁ");
      break;
    }
    case 5: {
      static const char* const N[2] = { "Wyślij: sąsiedzi", "Wyślij: cała sieć" };
      strcpy(L, N[i]);
      break;
    }
    case 6: {
      static const char* const N[3] = { "Wygaszanie", "Blokada", "Budzenie: wiad." };
      strcpy(L, N[i]);
      if (i == 0) strcpy(V, TO_LABEL[_timeout_idx & 3]);
      if (i == 1) strcpy(V, _lock_en ? "WŁ" : "WYŁ");
      if (i == 2) strcpy(V, _wake_msg ? "WŁ" : "WYŁ");
      break;
    }
    case 8: {
      static const char* const N[3] = { "Napisz", "Szybka odpowiedź", "Moja pozycja" };
      strcpy(L, N[i]);
      break;
    }
    case 9: strcpy(L, QUICK[i]); break;
    case 10: {
      static const char* const N[3] = { "Ulubiony", "Zaloguj jako admin", "Zaloguj jako gość" };
      strcpy(L, N[i]);
      if (i == 0 && _rpt_sel < _rpt_n) strcpy(V, _rpts[_rpt_sel].fav ? "TAK" : "NIE");
      if (i == 1 && _rpt_sel < _rpt_n) { char pw[24]; if (pwGet(_rpts[_rpt_sel].key, pw)) strcpy(V, "*"); }
      break;
    }
    case 11: {
      static const char* const N[5] = { "Stan", "Sąsiedzi", "Akcje", "Konsola", "Wyloguj" };
      strcpy(L, N[i]);
      break;
    }
    case 12: {
      static const char* const N[2] = { "Zapamiętaj hasło", "Nie zapamiętuj" };
      strcpy(L, N[i]);
      break;
    }
    case 13: {
      static const char* const N[7] = { "Wyślij advert", "Moc nadawania", "Preset SF6", "Pozycja = moja", "Zmień nazwę", "Synchronizuj czas", "Restart" };
      strcpy(L, N[i]);
      if (i == 1) snprintf(V, 12, "%d dBm", (int)_txp_sel);
      if (i == 6 && armed) strcpy(V, "OK?");
      break;
    }
    case 14: {
      static const char* const N[7] = { "ver", "get radio", "get tx", "get name", "stats", "clock", "> własna komenda" };
      strcpy(L, N[i]);
      break;
    }
    case 7: {
      static const char* const N[2] = { "Wiadomości", "Mapper (RPT)" };
      strcpy(L, N[i]);
      if (i == 0) strcpy(V, _prefs->buzzer_quiet ? "WYŁ" : "WŁ");
      if (i == 1) strcpy(V, mapper.soundOn() ? "WŁ" : "WYŁ");
      break;
    }
  }
}

void UITask::openMenu(int id) { _menu = id; _msel[id] = 0; _screen = SCR_MENU; _armed_item = -1; if (id == 3) radioLoad(); if (id == 13) _txp_sel = 22; }

static const float BWS[] = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f };
void UITask::radioLoad() {
  _rf_khz = (uint32_t)(_prefs->freq * 1000.0f + 0.5f); _rbw = _prefs->bw; _rsf = _prefs->sf; _rcr = _prefs->cr;
}
bool UITask::radioChanged() {
  return _rf_khz != (uint32_t)(_prefs->freq * 1000.0f + 0.5f) || fabsf(_rbw - _prefs->bw) > 0.05f || _rsf != _prefs->sf || _rcr != _prefs->cr;
}

void UITask::menuKey(char k) {
  int n = menuCount(_menu), &s = _msel[_menu];
  if (k == 'U' || k == 'D') { s = (s + (k == 'D' ? 1 : n - 1)) % n; _armed_item = -1; return; }
  bool cyc2 = _menu == 13 && s == 1;
  if (k == 'B' || (k == 'L' && _menu >= 8 && !cyc2)) {
    if (_menu == 8 || _menu == 9) { _screen = SCR_THREAD; return; }
    if (_menu == 10) { _screen = SCR_RPT; return; }
    if (_menu == 11 || _menu == 12) { openMenu(10); return; }
    if (_menu == 13 || _menu == 14) { _menu = 11; _screen = SCR_MENU; return; }
    _screen = SCR_HOME; _page = P_SET; return;
  }
  if (cyc2 && (k == 'L' || k == 'R')) { int v = _txp_sel + (k == 'R' ? 1 : -1); if (v > the_mesh.uiMaxTxPower()) v = 2; if (v < 2) v = the_mesh.uiMaxTxPower(); _txp_sel = v; return; }
  bool cycle = (_menu == 1 && s == 1) || (_menu == 3 && (s == 1 || (s >= 3 && s <= 5))) || (_menu == 6 && s == 0);
  if (k == 'L') { if (cycle) menuAction(-1); else { _screen = SCR_HOME; _page = P_SET; } return; }
  if (k == 'O' || k == 'R') menuAction(+1);
}

void UITask::menuAction(int dir) {
  int id = _menu;
  int i = id == 0 ? _sel : _msel[id];
  unsigned long now = millis();
  bool armed = _armed_item == id * 16 + i && now - _stop_armed < CONFIRM_MS;
  auto arm = [&]() { _armed_item = id * 16 + i; _stop_armed = now; };
  switch (id) {
    case 0:
      if (i >= 0 && i <= 6) { openMenu(i + 1); return; }
      if (i == 7) {                               // Informacje
        char b[32];
        _nlines = 0; strcpy(_lines_title, "Informacje");
        cleanText(_lines[_nlines++], 32, _prefs->node_name);
        const uint8_t* k = the_mesh.mpPubKey();
        snprintf(_lines[_nlines++], 32, "klucz %02X%02X%02X%02X%02X%02X", k[0], k[1], k[2], k[3], k[4], k[5]);
        snprintf(_lines[_nlines++], 32, "MC SF6 %s", UI_VERSION);
        snprintf(_lines[_nlines++], 32, "MeshCore %s", FIRMWARE_VERSION);
        snprintf(_lines[_nlines++], 32, "kontakty %d/%d", the_mesh.getNumContacts(), MAX_CONTACTS);
        uint16_t mv = getBattMilliVolts();
        snprintf(_lines[_nlines++], 32, "bateria %d,%02d V %d%%", mv / 1000, (mv % 1000) / 10, battPct(mv));
        MapperView v; mapper.getView(v);
        snprintf(b, sizeof(b), "mapper: do wysł. %lu", (unsigned long) v.pending);
        strcpy(_lines[_nlines++], b);
        _lines_top = 0; _lines_back = SCR_HOME; _screen = SCR_LINES;
        return;
      }
      if (i == 8) { if (!pwCount()) { popup("Brak zapamiętanych haseł"); return; } if (!armed) { arm(); return; } pwClear(); _armed_item = -1; popup("Hasła usunięte"); return; }
      if (i == 9) { if (!armed) { arm(); return; } popup("Restart..."); render(); delay(400); NVIC_SystemReset(); }
      if (i == 10) {
        if (!armed) { arm(); return; }
        popup("Wyłączanie..."); render(); delay(600);
        if (_display) _display->turnOff();
        _board->powerOff();
      }
      return;
    case 1:
      if (i == 0) {
        if (!mapper.isRunning()) { mapperSet(true); _armed_item = -1; }
        else if (armed) { mapperSet(false); _armed_item = -1; }
        else arm();
      }
      if (i == 1) { int s = mapper.getInterval() + dir * 10; if (s > 60) s = 20; if (s < 20) s = 60; mapper.setInterval(s); }
      if (i == 2) mapper.setSound(!mapper.soundOn());
      if (i == 3) { if (!armed) { arm(); return; } mapper.newSession(); _armed_item = -1; popup("Nowa sesja", "liczniki od zera"); }
      return;
    case 2:
      if (i == 0) { if (isBluetoothEnabled()) disableBluetooth(); else enableBluetooth(); popup(isBluetoothEnabled() ? "Bluetooth WŁ" : "Bluetooth WYŁ"); }
      return;
    case 3:
      if (i == 0) { the_mesh.uiSetRadioSF6(); radioLoad(); popup("Radio: preset SF6", "869,618 · SF6 · 62,5"); }
      if (i == 2) { snprintf(_fdig, sizeof(_fdig), "%06lu", (unsigned long) _rf_khz); _fcur = 3; _screen = SCR_FREQ; }
      if (i == 3) { int n = sizeof(BWS) / sizeof(BWS[0]), k = 0; while (k < n && fabsf(BWS[k] - _rbw) > 0.05f) k++; k = (k >= n ? 6 : k + dir + n) % n; _rbw = BWS[k]; }
      if (i == 4) { int v = _rsf + dir; if (v > 12) v = 5; if (v < 5) v = 12; _rsf = v; }
      if (i == 5) { int v = _rcr + dir; if (v > 8) v = 5; if (v < 5) v = 8; _rcr = v; }
      if (i == 6) {
        if (!radioChanged()) { popup("Brak zmian"); return; }
        if (!armed) { arm(); return; }
        the_mesh.uiSetRadio(_rf_khz / 1000.0f, _rbw, _rsf, _rcr); _armed_item = -1;
        bool sf6 = the_mesh.mpRadioIsSF6();
        popup("Radio ustawione", sf6 ? "preset SF6" : "UWAGA: to nie SF6", sf6 ? NULL : "mapper nie pinguje");
      }
      if (i == 1) {
        int cur = _prefs->tx_power_dbm, n = sizeof(TXP), k = 0;
        while (k < n && TXP[k] < cur) k++;
        if (k < n && TXP[k] == cur) k += dir; else if (dir < 0) k--;
        if (k >= n || TXP[(k < 0 ? 0 : k)] > the_mesh.uiMaxTxPower()) k = 0;
        if (k < 0) { k = n - 1; while (k > 0 && TXP[k] > the_mesh.uiMaxTxPower()) k--; }
        the_mesh.uiSetTxPower(TXP[k]);
      }
      return;
    case 4:
      if (i == 0) {
        bool on = !_prefs->gps_enabled;
        the_mesh.mpSetGps(on);
        popup(on ? "GPS WŁ" : "GPS WYŁ", (!on && mapper.isRunning()) ? "mapper nie będzie pingował" : NULL);
      }
      if (i == 2) { _prefs->advert_loc_policy = _prefs->advert_loc_policy ? ADVERT_LOC_NONE : ADVERT_LOC_SHARE; the_mesh.savePrefs(); }
      return;
    case 5:
      if (i == 0) popup(the_mesh.advert() ? "Advert wysłany" : "Błąd advertu", "do sąsiadów (0 hop)");
      if (i == 1) popup(the_mesh.uiAdvertFlood() ? "Advert wysłany" : "Błąd advertu", "do całej sieci");
      return;
    case 6:
      if (i == 0) _timeout_idx = (_timeout_idx + (dir > 0 ? 1 : 3)) % 4;
      if (i == 1) _lock_en = !_lock_en;
      if (i == 2) _wake_msg = !_wake_msg;
      saveUiPrefs();
      return;
    case 8:
      if (i == 0) kbdOpen(0, SCR_THREAD);
      if (i == 1) openMenu(9);
      if (i == 2) { char b[140]; myPosition(b, sizeof(b)); if (b[0]) sendText(b); else popup("Brak pozycji GPS"); }
      return;
    case 9:
      sendText(QUICK[i]);
      return;
    case 10:
      if (_rpt_sel >= _rpt_n) return;
      if (i == 1) { adminStartLogin(true); return; }
      if (i == 2) { adminStartLogin(false); return; }
      if (i == 0) {
        uint8_t key[32]; memcpy(key, _rpts[_rpt_sel].key, 32);
        int r = the_mesh.uiToggleFavourite(key);
        buildRpts();
        int ni = rptIndexByKey(key); if (ni >= 0) _rpt_sel = ni;
        popup(r == 1 ? "Ulubiony" : r == 0 ? "Usunięto z ulubionych" : "Brak kontaktu", r == 1 ? "zawsze na górze listy" : NULL);
      }
      return;
    case 11:
      if (i == 0) { if (the_mesh.uiAdminStatus()) adminWait(2); else popup("Błąd wysyłki"); }
      if (i == 1) { if (the_mesh.uiAdminNeighbours()) adminWait(3); else popup("Błąd wysyłki"); }
      if (i == 2 || i == 3) { if (!the_mesh.ui_admin.is_admin) { popup("Tylko dla admina"); return; } openMenu(i == 2 ? 13 : 14); }
      if (i == 4) { the_mesh.uiAdminLogout(); _screen = SCR_RPT; popup("Wylogowano"); }
      return;
    case 12:
      if (i == 0 && _rpt_sel < _rpt_n) { pwSet(_rpts[_rpt_sel].key, _pw_typed); popup("Hasło zapamiętane"); }
      _menu = 11; _msel[11] = 0; _screen = SCR_MENU;
      return;
    case 13:
      if (i == 0) adminCli("advert");
      if (i == 1) { char c[24]; snprintf(c, sizeof(c), "set tx %d", (int)_txp_sel); adminCli(c); }
      if (i == 2) adminCli("set radio 869.618,62.5,6,5");
      if (i == 3) {
        LocationProvider* l = _sensors ? _sensors->getLocationProvider() : NULL;
        if (!l || !l->isValid()) { popup("Brak pozycji GPS"); return; }
        char c[40];
        snprintf(c, sizeof(c), "set lat %.6f", l->getLatitude() / 1e6);
        snprintf(_cli_next, sizeof(_cli_next), "set lon %.6f", l->getLongitude() / 1e6);
        adminCli(c);
      }
      if (i == 4) kbdOpen(2, SCR_MENU, _rpt_sel < _rpt_n ? _rpts[_rpt_sel].name : "");
      if (i == 5) adminCli("clock sync");
      if (i == 6) {
        if (!armed) { arm(); return; }
        _armed_item = -1;
        if (the_mesh.uiAdminCli("reboot")) popup("Wysłano: reboot", "repeater się restartuje"); else popup("Błąd wysyłki");
      }
      return;
    case 14:
      if (i < 6) { static const char* const C[6] = { "ver", "get radio", "get tx", "get name", "stats", "clock" }; adminCli(C[i]); }
      else kbdOpen(3, SCR_MENU);
      return;
    case 7:
      if (i == 0) {
        _prefs->buzzer_quiet = _prefs->buzzer_quiet ? 0 : 1; the_mesh.savePrefs();
#ifdef PIN_BUZZER
        buzzer.quiet(_prefs->buzzer_quiet);
#endif
      }
      if (i == 1) mapper.setSound(!mapper.soundOn());
      return;
  }
}

// ---------------- rysowanie ----------------
void UITask::render() {
  if (!_display) return;
  _display->startFrame();
  _g.color(true);
  if (_screen == SCR_SPLASH) { drawSplash(millis() - _splash_at); _display->endFrame(); return; }
  if (_screen == SCR_KBD) { drawKbd(); if (_pop_until > millis()) drawPopup(); _display->endFrame(); return; }
  statusBar();
  switch (_screen) {
    case SCR_HOME:
      switch (_page) {
        case P_START: drawStart(); break;
        case P_MSGS: drawMsgs(); break;
        case P_MAPPER: drawMapper(); break;
        case P_RPT: drawRpts(); break;
        case P_SET: drawSettings(); break;
      }
      break;
    case SCR_THREAD: drawThread(); break;
    case SCR_MENU: drawMenu(); break;
    case SCR_LINES: drawLines(); break;
    case SCR_RPT: drawRptDetail(); break;
    case SCR_FREQ: drawFreq(); break;
    case SCR_KBD: break;
    case SCR_WAIT: drawWait(); break;
    case SCR_SCAN: drawScan(); break;
  }
  if (_pop_until > millis()) drawPopup();
  _display->endFrame();
}

void UITask::statusBar() {
  char b[16];
  fmtHM(b, sizeof(b), rtc_clock.getCurrentTime());
  _g.text(0, 0, b);
  int x = 113;
  uint16_t mv = getBattMilliVolts();
  _g.iconBattery(x, 1, battPct(mv), usbPowered());
  if (isBluetoothEnabled()) { x -= 10; _g.iconBt(x, 0, hasConnection()); }
  LocationProvider* l = _sensors ? _sensors->getLocationProvider() : NULL;
  if (_prefs->gps_enabled && l) {
    snprintf(b, sizeof(b), "%d", (int) l->satellitesCount());
    x -= _g.textWidth(b) + 1; _g.text(x, 0, b);
    x -= 9; _g.iconGps(x, 1, l->isValid());
  }
  int un = unreadCount();
  if (un) {
    snprintf(b, sizeof(b), "%d", un);
    x -= _g.textWidth(b) + 1; _g.text(x, 0, b);
    x -= 11; _g.frame(x, 1, 9, 7);
    for (int i = 0; i < 4; i++) { _g.rect(x + 1 + i, 2 + i, 1, 1); _g.rect(x + 7 - i, 2 + i, 1, 1); }
  }
  if (_locked) { x -= 9; _g.iconLock(x, 1); }
  if (mapper.isRunning()) {
    MapperView v; mapper.getView(v);
    bool blink = v.collecting && (millis() / 250) % 2;
    x -= 7; if (!blink) _g.rect(x, 2, 5, 5); else _g.frame(x, 2, 5, 5);
  }
  _g.hline(0, 9, 128);
}

static void titleRow(MapperGfx& g, const char* t, const char* right) {
  g.text(0, 12, t);
  if (right && right[0]) g.textRight(127, 12, right);
  g.hline(0, 21, 128);
}

void UITask::drawStart() {
  char b[40], nm[32];
  cleanText(nm, sizeof(nm), _prefs->node_name);
  _g.textCenter(64, 12, nm);
  fmtHM(b, sizeof(b), rtc_clock.getCurrentTime());
  _g.textCenter(64, 22, b, 2);
  int un = unreadCount();
  if (un && _thread_n) {
    const char* who = _threads[0];
    for (int t = 0; t < _thread_n; t++) if (unreadCount(_threads[t])) { who = _threads[t]; break; }
    _g.frame(0, 41, 9, 7); for (int i = 0; i < 4; i++) { _g.rect(1 + i, 42 + i, 1, 1); _g.rect(7 - i, 42 + i, 1, 1); }
    char w[24]; MapperGfx::fit(w, sizeof(w), who, 60);
    snprintf(b, sizeof(b), "%d %s · %s", un, un == 1 ? "nowa" : "nowe", w);
    _g.text(12, 40, b);
  } else _g.text(0, 40, "brak nowych wiadomości");
  MapperView v; mapper.getView(v);
  if (v.run) {
    _g.rect(1, 52, 4, 4);
    const char* st = !v.radio_ok ? "radio nie SF6!" : !v.gps_fix ? "czekam na GPS" : v.phone_mapping ? "mapuje telefon"
                   : v.stationary ? "pauza (postój)" : "mapuje";
    snprintf(b, sizeof(b), "%s · %lu/%lu", st, (unsigned long) v.hits, (unsigned long) v.pings);
    _g.text(8, 50, b);
  } else _g.text(0, 50, "mapper wyłączony");
}

void UITask::drawMsgs() {
  titleRow(_g, "< Wiadomości >", NULL);
  int n = _thread_n + 1;
  if (_sel >= n) _sel = n - 1;
  for (int r = 0; r < ROWS; r++) {
    int i = _top + r; if (i >= n) break;
    int y = 24 + r * ROW_H;
    char nm[24], c[8]; c[0] = 0;
    if (i == 0) strcpy(nm, "> Napisz na Public");
    else { MapperGfx::fit(nm, sizeof(nm), _threads[i - 1], 100); int u = unreadCount(_threads[i - 1]); if (u) snprintf(c, sizeof(c), "%d", u); }
    if (i == _sel) { _g.rect(0, y - 1, 128, ROW_H); _g.color(false); }
    _g.text(2, y, nm); if (c[0]) _g.textRight(125, y, c);
    if (i == _sel) _g.color(true);
  }
}

void UITask::drawThread() {
  int idx[UI_MSG_MAX], n = 0;
  for (int i = 0; i < _msg_n; i++) if (_thread_sel < _thread_n && strcmp(_msgs[i].thread, _threads[_thread_sel]) == 0) idx[n++] = i;
  if (!n) { _screen = SCR_HOME; _page = P_MSGS; return; }
  if (_thread_msg < 0 || _thread_msg >= n) _thread_msg = n - 1;
  UiMsg& m = _msgs[idx[_thread_msg]];
  m.unread = false;
  char t[16], name[24];
  fmtHM(t, sizeof(t), m.ts);
  MapperGfx::fit(name, sizeof(name), m.mine ? "Ty" : m.thread, 90);
  titleRow(_g, name, t);
  char lines[12][32]; int nl = wrapText(m.text, lines, 12, 21);
  if (_thread_line > nl - 1) _thread_line = nl > 4 ? nl - 4 : 0;
  for (int r = 0; r < 3 && _thread_line + r < nl; r++) _g.text(0, 24 + r * 10, lines[_thread_line + r]);
  char f[24]; snprintf(f, sizeof(f), "%d/%d", _thread_msg + 1, n);
  _g.hline(0, 54, 128);
  _g.text(0, 56, nl > _thread_line + 3 ? "w dół: dalej" : "OK: odpowiedz");
  _g.textRight(127, 56, f);
}

void UITask::drawMapper() {
  MapperView v; mapper.getView(v);
  titleRow(_g, "< Mapper >", v.run ? "WŁ" : "STOP");
  char b[32];
  if (!_mapper_list) {
    const char* st = !v.run ? "wyłączony" : !v.radio_ok ? "radio nie SF6!" : v.log_full ? "pamięć pełna!"
                   : v.phone_mapping ? "mapuje telefon" : !v.gps_fix ? "czekam na GPS" : v.stationary ? "pauza (postój)"
                   : v.collecting ? "ping..." : "mapuje";
    _g.text(0, 24, st);
    if (v.run && v.radio_ok && v.gps_fix && !v.stationary && !v.phone_mapping && !v.collecting) { snprintf(b, sizeof(b), "za %d s", v.next_ping_s); _g.textRight(127, 24, b); }
    _g.text(0, 34, "odpowiedzi"); snprintf(b, sizeof(b), "%lu / %lu", (unsigned long) v.hits, (unsigned long) v.pings); _g.textRight(127, 34, b);
    _g.text(0, 44, "do wysłania"); snprintf(b, sizeof(b), "%lu", (unsigned long) v.pending); _g.textRight(127, 44, b);
    if (v.last_n > 0) {
      char n[24], s[12]; rptName(v.last_prefix, n, sizeof(n)); fmtSnr(s, sizeof(s), v.last_snr_x4);
      char nf[24]; MapperGfx::fit(nf, sizeof(nf), n, 88);
      _g.text(0, 54, nf); _g.textRight(127, 54, s);
    } else _g.text(0, 54, "w dół: ostatnie RPT");
  } else {
    const MapperRecent* r = mapper.recent();
    int shown = 0;
    for (int i = 0; i < CMAP_RECENT && shown < 4; i++) {
      if (!r[i].valid) continue;
      int y = 24 + shown * ROW_H;
      char t[8], n[24], nf[20], s[12];
      fmtHM(t, sizeof(t), r[i].ts); rptName(r[i].prefix, n, sizeof(n)); MapperGfx::fit(nf, sizeof(nf), n, 58); fmtSnr(s, sizeof(s), r[i].snr_x4);
      _g.text(0, y, t); _g.text(33, y, nf); _g.textRight(127, y, s);
      shown++;
    }
    if (!shown) _g.textCenter(64, 34, "brak odpowiedzi");
  }
}

void UITask::drawRpts() {
  titleRow(_g, "< Repeatery >", NULL);
  if (millis() - _rpt_at > 10000) buildRpts();
  int n = _rpt_n + 1;
  if (_sel >= n) _sel = n - 1;
  for (int r = 0; r < ROWS; r++) {
    int i = _top + r; if (i >= n) break;
    int y = 24 + r * ROW_H;
    char nm[28], d[16]; d[0] = 0;
    if (i == 0) strcpy(nm, "> Co tu słychać?");
    else {
      Rpt& rp = _rpts[i - 1];
      if (rp.dist_m >= 0) fmtDist(d, sizeof(d), rp.dist_m); else fmtAgo(d, sizeof(d), rp.last);
      char t[24]; MapperGfx::fit(t, sizeof(t), rp.name, 118 - _g.textWidth(d) - (rp.fav ? 6 : 0));
      snprintf(nm, sizeof(nm), "%s%s", rp.fav ? "*" : "", t);
    }
    if (i == _sel) { _g.rect(0, y - 1, 128, ROW_H); _g.color(false); }
    _g.text(2, y, nm); if (d[0]) _g.textRight(125, y, d);
    if (i == _sel) _g.color(true);
  }
}

void UITask::drawRptDetail() {
  if (_rpt_sel >= _rpt_n) { _screen = SCR_HOME; return; }
  Rpt& r = _rpts[_rpt_sel];
  char b[32], n[24];
  MapperGfx::fit(n, sizeof(n), r.name, 124);
  titleRow(_g, n, NULL);
  snprintf(b, sizeof(b), "klucz %02X%02X%02X%02X%s", r.key[0], r.key[1], r.key[2], r.key[3], r.fav ? "  *ulub." : "");
  _g.text(0, 24, b);
  LocationProvider* l = _sensors ? _sensors->getLocationProvider() : NULL;
  if (r.lat || r.lon) {
    if (r.dist_m >= 0 && l && l->isValid()) {
      char d[16]; fmtDist(d, sizeof(d), r.dist_m);
      int br = bearing(l->getLatitude() / 1e6, l->getLongitude() / 1e6, r.lat / 1e6, r.lon / 1e6);
      snprintf(b, sizeof(b), "%s · %s %d°", d, compass(br), br);
    } else snprintf(b, sizeof(b), "odległość: brak GPS");
    _g.text(0, 34, b);
    snprintf(b, sizeof(b), "%.4f %.4f", r.lat / 1e6, r.lon / 1e6);
    _g.text(0, 44, b);
  } else _g.text(0, 34, "brak pozycji");
  char a[16]; fmtAgo(a, sizeof(a), r.last);
  snprintf(b, sizeof(b), "advert: %s", a);
  _g.text(0, 54, b);
  _g.textRight(127, 54, "OK >");
}

// ---------------- zarzadzanie repeaterem ----------------
#define PW_FILE "/sf6pw"
#define PW_MAX  8
struct PwEntry { uint8_t key[6]; char pw[18]; };
static PwEntry pw_tab[PW_MAX]; static bool pw_loaded = false;
static void pwLoad() {
  if (pw_loaded) return; pw_loaded = true; memset(pw_tab, 0, sizeof(pw_tab));
  File f = InternalFS.open(PW_FILE, FILE_O_READ);
  if (f) { f.read((uint8_t*) pw_tab, sizeof(pw_tab)); f.close(); }
}
static void pwSave() {
  InternalFS.remove(PW_FILE);
  File f = InternalFS.open(PW_FILE, FILE_O_WRITE);
  if (f) { f.write((const uint8_t*) pw_tab, sizeof(pw_tab)); f.close(); }
}
bool UITask::pwGet(const uint8_t* key, char* out) {
  pwLoad();
  for (int i = 0; i < PW_MAX; i++) if (pw_tab[i].pw[0] && memcmp(pw_tab[i].key, key, 6) == 0) { strcpy(out, pw_tab[i].pw); return true; }
  return false;
}
void UITask::pwSet(const uint8_t* key, const char* pw) {
  pwLoad();
  int slot = -1;
  for (int i = 0; i < PW_MAX; i++) if (memcmp(pw_tab[i].key, key, 6) == 0) { slot = i; break; }
  if (slot < 0) for (int i = 0; i < PW_MAX; i++) if (!pw_tab[i].pw[0]) { slot = i; break; }
  if (slot < 0) { memmove(&pw_tab[0], &pw_tab[1], sizeof(PwEntry) * (PW_MAX - 1)); slot = PW_MAX - 1; }
  memcpy(pw_tab[slot].key, key, 6);
  strncpy(pw_tab[slot].pw, pw, sizeof(pw_tab[slot].pw) - 1); pw_tab[slot].pw[sizeof(pw_tab[slot].pw) - 1] = 0;
  pwSave();
}
int UITask::pwCount() { pwLoad(); int n = 0; for (int i = 0; i < PW_MAX; i++) if (pw_tab[i].pw[0]) n++; return n; }
void UITask::pwClear() { pwLoad(); memset(pw_tab, 0, sizeof(pw_tab)); pwSave(); }

void UITask::kbdOpen(uint8_t mode, uint8_t from, const char* init) {
  _kbd_mode = mode; _kbd_from = from;
  strncpy(_kbd, init ? init : "", sizeof(_kbd) - 1); _kbd[sizeof(_kbd) - 1] = 0;
  _kr = 0; _kc = 0; _kupper = (mode == 0 || mode == 2) && !_kbd[0];
  _screen = SCR_KBD;
}

void UITask::kbdSubmit() {
  if (_kbd_mode == 0) { if (_kbd[0]) sendText(_kbd); else popup("Pusta wiadomość"); return; }
  if (_kbd_mode == 1) {
    strncpy(_pw_typed, _kbd, sizeof(_pw_typed) - 1); _pw_typed[sizeof(_pw_typed) - 1] = 0; _pw_new = true;
    if (_rpt_sel < _rpt_n && the_mesh.uiAdminLogin(_rpts[_rpt_sel].key, _pw_typed)) adminWait(1);
    else { popup("Błąd wysyłki"); _screen = SCR_MENU; }
    return;
  }
  if (!_kbd[0]) { popup("Pusty tekst"); return; }
  if (_kbd_mode == 2) { char c[48]; snprintf(c, sizeof(c), "set name %s", _kbd); _menu = 13; adminCli(c); return; }
  if (_kbd_mode == 3) { _menu = 14; adminCli(_kbd); return; }
}

void UITask::adminStartLogin(bool admin) {
  _pw_new = false;
  const uint8_t* key = _rpts[_rpt_sel].key;
  if (admin) {
    char pw[24];
    if (!pwGet(key, pw)) { kbdOpen(1, SCR_MENU); return; }
    strcpy(_pw_typed, pw);
    if (the_mesh.uiAdminLogin(key, pw)) adminWait(1); else popup("Błąd wysyłki");
  } else {
    _pw_typed[0] = 0;
    if (the_mesh.uiAdminLogin(key, "")) adminWait(1); else popup("Błąd wysyłki");
  }
}

void UITask::adminWait(uint8_t kind) {
  _wait_kind = kind; _wait_t0 = millis();
  uint32_t est = the_mesh.ui_admin.est_timeout;
  _wait_ms = est + 6000; if (_wait_ms < 8000) _wait_ms = 8000; if (_wait_ms > 40000) _wait_ms = 40000;
  _screen = SCR_WAIT; _dirty = true;
}

void UITask::adminCli(const char* cmd) {
  strncpy(_cli_cmd, cmd, sizeof(_cli_cmd) - 1); _cli_cmd[sizeof(_cli_cmd) - 1] = 0;
  if (the_mesh.uiAdminCli(cmd)) adminWait(4); else { popup("Błąd wysyłki"); _cli_next[0] = 0; _screen = SCR_MENU; }
}

void UITask::adminLoop() {
  if (_screen != SCR_WAIT || !_wait_kind) return;
  MyMesh::UiAdmin& a = the_mesh.ui_admin;
  unsigned long now = millis();
  char b[40];
  if (_wait_kind == 1 && a.login >= 2) {
    _wait_kind = 0;
    if (a.login == 2) {
      if (_pw_new && _pw_typed[0]) { _menu = 12; _msel[12] = 0; _screen = SCR_MENU; }
      else { _menu = 11; _msel[11] = 0; _screen = SCR_MENU; }
      popup("Zalogowano", a.is_admin ? "jako admin" : "jako gość");
    } else {
      if (!_pw_new && _pw_typed[0] && _rpt_sel < _rpt_n) pwSet(_rpts[_rpt_sel].key, "");   // zapamietane haslo nie pasuje
      _menu = 10; _screen = SCR_MENU;
      popup("Logowanie nieudane", _pw_typed[0] ? "złe hasło?" : "gość niedozwolony?");
    }
    return;
  }
  if (_wait_kind == 2 && a.status_ready) {
    _wait_kind = 0;
    struct __attribute__((packed)) St { uint16_t mv, queue; int16_t noise, rssi; uint32_t recv, sent, air, up, sf, sd, rf, rd; uint16_t err; int16_t snr; } st;
    memset(&st, 0, sizeof(st)); memcpy(&st, a.status, a.status_len < sizeof(st) ? a.status_len : sizeof(st));
    _nlines = 0; strcpy(_lines_title, "Stan");
    snprintf(_lines[_nlines++], 32, "działa\t%lud %luh", (unsigned long)(st.up / 86400), (unsigned long)((st.up / 3600) % 24));
    snprintf(_lines[_nlines++], 32, "bateria\t%u,%02u V", st.mv / 1000, (st.mv % 1000) / 10);
    snprintf(_lines[_nlines++], 32, "szum radia\t%d dBm", st.noise);
    char sn[12]; fmtSnr(sn, sizeof(sn), (int8_t) st.snr);
    snprintf(_lines[_nlines++], 32, "ost. RSSI/SNR\t%d/%s", st.rssi, sn);
    snprintf(_lines[_nlines++], 32, "odebrane\t%lu", (unsigned long) st.recv);
    snprintf(_lines[_nlines++], 32, "wysłane\t%lu", (unsigned long) st.sent);
    snprintf(_lines[_nlines++], 32, "kolejka / błędy\t%u / %u", st.queue, st.err);
    _lines_top = 0; _lines_back = SCR_MENU; _menu = 11; _screen = SCR_LINES;
    return;
  }
  if (_wait_kind == 3 && a.nb_ready) {
    _wait_kind = 0;
    uint16_t total = 0, cnt = 0;
    if (a.nb_len >= 4) { memcpy(&total, a.nb, 2); memcpy(&cnt, a.nb + 2, 2); }
    snprintf(_lines_title, sizeof(_lines_title), "Sąsiedzi (%u)", total);
    _nlines = 0;
    for (int i = 0; i < cnt && _nlines < 8; i++) {
      int o = 4 + i * 11; if (o + 11 > a.nb_len) break;
      char nm[24], nf[20], sn[12];
      rptName(a.nb + o, nm, sizeof(nm));
      if (nm[0] == '#') snprintf(nm, sizeof(nm), "#%02X%02X%02X", a.nb[o], a.nb[o + 1], a.nb[o + 2]);
      MapperGfx::fit(nf, sizeof(nf), nm, 84);
      fmtSnr(sn, sizeof(sn), (int8_t) a.nb[o + 10]);
      snprintf(_lines[_nlines++], 32, "%s\t%s", nf, sn);
    }
    if (!_nlines) strcpy(_lines[_nlines++], "brak sąsiadów");
    _lines_top = 0; _lines_back = SCR_MENU; _menu = 11; _screen = SCR_LINES;
    return;
  }
  if (_wait_kind == 4 && a.cli_ready) {
    a.cli_ready = false;
    if (_cli_next[0]) { char nx[48]; strcpy(nx, _cli_next); _cli_next[0] = 0; adminCli(nx); return; }
    _wait_kind = 0;
    snprintf(_lines_title, sizeof(_lines_title), "> %s", _cli_cmd);
    char tmp[12][32]; int n = wrapText(a.cli, tmp, 8, 21);
    _nlines = 0; for (int i = 0; i < n; i++) strcpy(_lines[_nlines++], tmp[i]);
    if (!_nlines) strcpy(_lines[_nlines++], "(pusta odpowiedź)");
    _lines_top = 0; _lines_back = SCR_MENU; _screen = SCR_LINES;
    return;
  }
  if (now - _wait_t0 > _wait_ms) {
    _wait_kind = 0; a.cli_wait = false; _cli_next[0] = 0;
    snprintf(b, sizeof(b), "po %lu s", (unsigned long)(_wait_ms / 1000));
    popup("Brak odpowiedzi", b);
    if (a.login == 1) { a.login = 3; _menu = 10; }
    _screen = SCR_MENU;
  } else _dirty = true;
}

void UITask::drawWait() {
  static const char* const T[5] = { "", "Logowanie", "Stan", "Sąsiedzi", "Konsola" };
  char n[24];
  if (_rpt_sel < _rpt_n) MapperGfx::fit(n, sizeof(n), _rpts[_rpt_sel].name, 124); else n[0] = 0;
  titleRow(_g, n, NULL);
  _g.textCenter(64, 25, T[_wait_kind % 5]);
  _g.textCenter(64, 35, "czekam na odpowiedź...");
  unsigned long el = millis() - _wait_t0;
  _g.progress(8, 45, 112, 6, (int)(el * 100 / (_wait_ms ? _wait_ms : 1)));
  _g.text(0, 55, "wstecz: przerwij");
}

// „Co tu slychac?”: wynik pingu z ekranu (repeatery, ktore odpowiedzialy, SNR odbior/nadawanie)
void UITask::drawScan() {
  bool act = mapper.scanActive();
  titleRow(_g, "Co tu słychać?", act ? "..." : "");
  int n = mapper.scanCount();
  const MapResp* r = mapper.scanResults();
  if (!n) { _g.textCenter(64, 32, act ? "nasłuchuję..." : "nikt nie odpowiedział"); if (!act) _g.textCenter(64, 44, "OK: ponów"); return; }
  for (int i = 0; i < n && i < 4; i++) {
    int y = 24 + i * ROW_H;
    char nm[24], nf[20], a[10], c[10], v[22];
    rptName(r[i].prefix, nm, sizeof(nm)); MapperGfx::fit(nf, sizeof(nf), nm, 70);
    fmtSnr(a, sizeof(a), r[i].snr_rx); fmtSnr(c, sizeof(c), r[i].snr_tx);
    snprintf(v, sizeof(v), "%s/%s", a, c);
    _g.text(0, y, nf); _g.textRight(127, y, v);
  }
}


void UITask::drawSettings() {
  titleRow(_g, "< Ustawienia >", NULL);
  int n = menuCount(0);
  if (_sel >= n) _sel = n - 1;
  for (int r = 0; r < ROWS; r++) {
    int i = _top + r; if (i >= n) break;
    int y = 24 + r * ROW_H;
    char L[28], V[16]; menuItem(0, i, L, V);
    if (i == _sel) { _g.rect(0, y - 1, 128, ROW_H); _g.color(false); }
    _g.text(2, y, L); if (V[0]) _g.textRight(125, y, V);
    if (i == _sel) _g.color(true);
  }
}

void UITask::drawMenu() {
  static const char* const T[15] = { "Ustawienia", "Mapper", "Bluetooth", "Radio", "GPS", "Advert", "Ekran", "Dźwięk",
                                     "Odpowiedz", "Szybka odpowiedź", "Repeater", "Repeater", "Hasło", "Akcje", "Konsola" };
  char tt[24];
  if (_menu == 8 || _menu == 9) { MapperGfx::fit(tt, sizeof(tt), _compose_to, 120); titleRow(_g, tt, NULL); }
  else if ((_menu == 10 || _menu == 11) && _rpt_sel < _rpt_n) {
    const char* tag = _menu == 11 ? (the_mesh.ui_admin.is_admin ? "ADMIN" : "GOŚĆ") : NULL;
    MapperGfx::fit(tt, sizeof(tt), _rpts[_rpt_sel].name, tag ? 84 : 124); titleRow(_g, tt, tag);
  }
  else titleRow(_g, T[_menu], NULL);
  int n = menuCount(_menu), s = _msel[_menu];
  int top = s >= ROWS ? s - ROWS + 1 : 0;
  for (int r = 0; r < ROWS; r++) {
    int i = top + r; if (i >= n) break;
    int y = 24 + r * ROW_H;
    char L[28], V[16]; menuItem(_menu, i, L, V);
    if (i == s) { _g.rect(0, y - 1, 128, ROW_H); _g.color(false); }
    _g.text(2, y, L); if (V[0]) _g.textRight(125, y, V);
    if (i == s) _g.color(true);
  }
}

void UITask::drawLines() {                       // 4 linie, gora/dol przewija
  char pg[12] = "";
  if (_nlines > 4) snprintf(pg, sizeof(pg), "%d/%d", _lines_top + 1, _nlines - 3);
  titleRow(_g, _lines_title, pg);
  for (int r = 0; r < 4 && _lines_top + r < _nlines; r++) {
    char ln[32]; strcpy(ln, _lines[_lines_top + r]);
    char* tab = strchr(ln, '\t');
    if (tab) { *tab = 0; _g.textRight(127, 24 + r * 10, tab + 1); }
    _g.text(0, 24 + r * 10, ln);
  }
}

void UITask::drawFreq() {                        // edycja czestotliwosci cyfra po cyfrze
  titleRow(_g, "Częstotliwość", "MHz");
  char s[12]; snprintf(s, sizeof(s), "%c%c%c,%c%c%c", _fdig[0], _fdig[1], _fdig[2], _fdig[3], _fdig[4], _fdig[5]);
  const int x0 = 64 - _g.textWidth(s, 2) / 2;
  _g.text(x0, 27, s, 2);
  int pos = _fcur < 3 ? _fcur : _fcur + 1;          // przecinek zajmuje jedno miejsce
  _g.rect(x0 + pos * 12, 45, 10, 2);
  _g.text(0, 54, "góra/dół: cyfra  OK");
}

void UITask::drawPopup() {
  _g.color(false); _g.rect(5, 13, 118, 48); _g.color(true);
  _g.frame(6, 14, 116, 46); _g.frame(7, 15, 114, 44);
  int y = _pop3[0] || _pop_pct >= 0 ? 19 : 24;
  _g.textCenter(64, y, _pop1);
  if (_pop2[0]) _g.textCenter(64, y + 11, _pop2);
  if (_pop_pct >= 0) _g.progress(16, 43, 96, 8, _pop_pct);
  else if (_pop3[0]) _g.textCenter(64, y + 22, _pop3);
}

// Animacja startowa: „MC SF6” wjezdza z prawej, fale radiowe, nazwa i wersja
void UITask::drawSplash(unsigned long t) {
  const char* title = "MC SF6";
  const int w = _g.textWidth(title, 3);
  const int x_end = (128 - w) / 2;
  int x = x_end;
  if (t < 500) {
    long p = (long) t * 1000 / 500, inv = 1000 - p;
    long ease = 1000 - inv * inv * inv / 1000000;
    x = 128 - (int)((128 - x_end) * ease / 1000);
  }
  _g.text(x, 3, title, 3);
  if (t >= 300) {
    const int cx = 64, cy = 42;
    _g.rect(cx - 1, cy - 1, 3, 3);
    unsigned long tw = t - 300;
    for (int k = 0; k < 3; k++) { int r = (int)((tw / 45 + k * 6) % 15) + 3; _g.arcTop(cx, cy, r, 29); }
  }
  if (t >= 600 && _prefs) { char nm[32]; cleanText(nm, sizeof(nm), _prefs->node_name); _g.textCenter(64, 46, nm); }
  if (t >= 900) _g.textRight(127, 56, "v" UI_VERSION);
}
