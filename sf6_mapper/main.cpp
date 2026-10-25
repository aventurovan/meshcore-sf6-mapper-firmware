/*
 * SF6 Mapper - samodzielny tracker do mapowania zasiegu sieci MeshCore Poludnie (SF6)
 * Platforma: Seeed Wio Tracker L1 / L1 Pro (nRF52840 + SX1262 + GNSS L76K)
 *
 * Dzialanie:
 *  - po uzyskaniu fixu GPS co 20/30/40/50/60 s (ustawienie) wysyla zero-hop discovery (tylko repeatery),
 *  - przez 10 s zbiera odpowiedzi (SNR w obie strony, RSSI),
 *  - kazdy ping (trafienie albo pudlo) zapisuje z pozycja do flasha QSPI,
 *  - po 4 pingach bez ruchu robi pauze do czasu prawdziwego ruchu,
 *  - punkty pobiera aplikacja Mapper SF6 przez BLE i kasuje je dopiero po ACK.
 */
#include <Arduino.h>
#include <Mesh.h>
#include <target.h>
#include <helpers/IdentityStore.h>
#include <helpers/nrf52/SerialBLEInterface.h>
#include <InternalFileSystem.h>
#include <CustomLFS_QSPIFlash.h>
#include <bluefruit.h>

#include "MapperProtocol.h"
#include "MapperMesh.h"
#include "MapLog.h"
#include "MapperUI.h"

// ---------------- konfiguracja ----------------
#define SF6_FREQ              869.618f
#define SF6_BW                62.5f
#define SF6_SF                6
#define SF6_CR                5

#ifndef COLLECT_MS
  #define COLLECT_MS          10000UL   // jak dlugo czekamy na odpowiedzi
#endif
#ifndef STATIONARY_PINGS
  #define STATIONARY_PINGS    4         // tyle pingow w miejscu -> pauza
#endif
#ifndef MOVE_THRESHOLD_M
  #define MOVE_THRESHOLD_M    30.0      // przesuniecie uznawane za ruch (m)
#endif
#ifndef MOVE_SPREAD_M
  #define MOVE_SPREAD_M       25.0      // ruch = przesuniecie o tyle metrow w oknie ostatnich ~20 s
#endif
#define MOVE_SAMPLE_MS        2000UL    // co ile probka pozycji do wykrywania ruchu
#define MOVE_WINDOW           10        // liczba probek w oknie (10 x 2 s = 20 s)
#define MOVE_MIN_SAMPLES      6         // minimum probek do decyzji (~10 s)
#define FIX_WARMUP_MS         15000UL   // pierwsze pozycje po (ponownym) fixie sa niepewne - pomijamy
#ifndef MIN_SATS
  #define MIN_SATS            4
#endif
#define BLE_NAME_PREFIX       "MeshCore-"   // ten sam prefiks co companion - aplikacja znajdzie urzadzenie tym samym filtrem
#define SYNC_MAX_BATCH        200

// bateria (tylko gdy brak zasilania z USB)
#define BATT_LOW_MV           3400      // ponizej: mapowanie wstrzymane
#define BATT_RESUME_MV        3550      // powyzej: wznowienie
#define BATT_OFF_MV           3300      // ponizej: wylaczenie trackera (ochrona danych)
#define BATT_READ_MS          10000UL

// limit odpowiedzi repeatera na discovery: 4 na 2 minuty
#define RPT_LIMIT_COUNT       4
#define RPT_LIMIT_WINDOW_MS   120000UL
#define LIMIT_TABLE           16

// ---------------- obiekty globalne ----------------
StdRNG fast_rng;
SimpleMeshTables tables;
MapperMesh the_mesh(radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);
MapLog maplog;
SerialBLEInterface ble;
MapperUI ui;
MapperStatus status;

static char tracker_name[MAP_NAME_MAX + 1];          // nazwa trackera = nick konta w Mapperze
static char ble_full_name[sizeof(BLE_NAME_PREFIX) + MAP_NAME_MAX + 1];

// podpis (logowanie do Mappera)
static uint8_t  sign_buf[MAP_SIGN_MAX];
static uint16_t sign_len = 0;
static bool     sign_active = false;

// stan mapowania
static bool     run_mapping = true;
static unsigned long next_ping_at = 0;
static unsigned long last_ping_at = 0;
static uint8_t  interval_s = MAP_INTERVAL_DEFAULT;
static unsigned long collect_until = 0;
static MapRecordHdr  ping_hdr;        // pozycja/czas zapamietane w chwili pingu
static bool     have_last_pos = false;
static int32_t  last_lat = 0, last_lon = 0;
static int      still_count = 0;      // ile kolejnych pingow w tym samym miejscu

// ustawienia z menu urzadzenia
static bool     buzzer_on = true;
static bool     gps_on = true;
static bool     ble_on = true;
static bool     lock_on = true;
static uint8_t  timeout_idx = 1;      // 0=30 s, 1=1 min, 2=2 min, 3=nigdy
static uint32_t last_sync_ts = 0;     // czas ostatniego zrzutu danych (UNIX)
static uint32_t ble_pin = 0;          // losowy PIN Bluetooth (6 cyfr), generowany przy pierwszym uruchomieniu

// sesja
static uint8_t  session_no = 0;
static unsigned long sess_start = 0;
static bool     have_sess_pos = false;
static int32_t  sess_lat = 0, sess_lon = 0;
static unsigned long last_resp_at = 0;

// wykrywanie ruchu: probki pozycji z ostatnich ~20 s
static int32_t  mv_lat[MOVE_WINDOW], mv_lon[MOVE_WINDOW];
static int      mv_count = 0, mv_head = 0, mv_streak = 0;
static unsigned long mv_next = 0;
static unsigned long fix_since = 0;

// limit repeaterow: czasy ostatnich odpowiedzi kazdego repeatera
struct LimitEntry { uint8_t prefix[MAP_PREFIX_LEN]; unsigned long t[RPT_LIMIT_COUNT]; uint8_t n; bool used; };
static LimitEntry limits[LIMIT_TABLE];

// zasilanie
static bool     usb_power = false;
static bool     low_batt = false;
static uint16_t batt_hist[3] = {0, 0, 0};
static int      batt_idx = 0;
static int      mem_warned = 0;       // ostatni prog ostrzezenia o pamieci (80/90/100)

// stan synchronizacji BLE
static bool     sync_active = false;
static uint32_t sync_next_seq = 0;
static uint16_t sync_left = 0;
static uint16_t sync_sent = 0;
static uint32_t sync_last_seq = 0;

static uint8_t  frame[MAX_FRAME_SIZE];

void halt() {
  while (1) ;
}

// ---------------- pomocnicze ----------------
static double distMeters(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2) {
  const double R = 6371000.0;
  double la1 = lat1 / 1e6 * DEG_TO_RAD, la2 = lat2 / 1e6 * DEG_TO_RAD;
  double dla = la2 - la1;
  double dlo = (lon2 - lon1) / 1e6 * DEG_TO_RAD;
  double x = dlo * cos((la1 + la2) / 2);
  return sqrt(x * x + dla * dla) * R;
}

static LocationProvider* gps() { return sensors.getLocationProvider(); }

static bool gpsFix() {
  LocationProvider* l = gps();
  return gps_on && l != NULL && l->isValid() && l->satellitesCount() >= MIN_SATS;
}

static uint32_t nowUnix() {
  uint32_t t = rtc_clock.getCurrentTime();
  return t > 1700000000UL ? t : 0;
}

static int memPct() {
  return maplog.maxBytes() ? (int)((uint64_t) maplog.fileBytes() * 100 / maplog.maxBytes()) : 0;
}

static uint8_t stateFlags() {
  uint8_t f = 0;
  if (run_mapping) f |= MAP_STATE_RUN;
  if (gpsFix()) f |= MAP_STATE_GPS_FIX;
  if (still_count >= STATIONARY_PINGS) f |= MAP_STATE_STATIONARY;
  if (maplog.isFull()) f |= MAP_STATE_LOG_FULL;
  if (!gps_on) f |= MAP_STATE_GPS_OFF;
  if (low_batt) f |= MAP_STATE_LOW_BATT;
  if (usb_power) f |= MAP_STATE_USB;
  return f;
}

// ---------------- czas lokalny (Polska: CET/CEST, zmiana czasu wg zasad UE) ----------------
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t) doe - 719468;
}
static void civilFromDays(int32_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y = (int)(yoe + era * 400) + (*m <= 2);
}
// ostatnia niedziela miesiaca, 01:00 UTC
static uint32_t lastSundayUtc(int y, unsigned m) {
  int32_t days = daysFromCivil(y, m, 31);
  int wd = (int)((days + 4) % 7);            // 0 = niedziela (1970-01-01 to czwartek)
  if (wd < 0) wd += 7;
  return (uint32_t)(days - wd) * 86400UL + 3600UL;
}
static uint32_t toLocal(uint32_t utc) {
  int y; unsigned m, d;
  civilFromDays((int32_t)(utc / 86400UL), &y, &m, &d);
  bool summer = utc >= lastSundayUtc(y, 3) && utc < lastSundayUtc(y, 10);
  return utc + (summer ? 7200UL : 3600UL);
}
static void localParts(uint32_t utc, uint8_t* hh, uint8_t* mm, uint8_t* ss, uint8_t* day, uint8_t* mon, uint16_t* year) {
  uint32_t t = toLocal(utc);
  int y; unsigned m, d;
  civilFromDays((int32_t)(t / 86400UL), &y, &m, &d);
  uint32_t sec = t % 86400UL;
  if (hh) *hh = sec / 3600;
  if (mm) *mm = (sec / 60) % 60;
  if (ss) *ss = sec % 60;
  if (day) *day = d;
  if (mon) *mon = m;
  if (year) *year = y;
}

// ---------------- ustawienia ----------------
#define SETTINGS_FILE   "/sf6cfg"
#define SESSION_FILE    "/sf6sess"      // licznik sesji - nie jest kasowany przy ustawieniach fabrycznych
#define IDENTITY_FILE   "/_main.id"

struct __attribute__((packed)) MapperSettings {
  uint8_t  magic;
  uint8_t  interval_s;
  char     name[MAP_NAME_MAX + 1];   // od v1.0.2
  uint8_t  buzzer_on;                // od v1.0.3
  uint8_t  gps_on;
  uint8_t  ble_on;
  uint8_t  lock_on;
  uint8_t  timeout_idx;              // od v1.1.0
  uint32_t last_sync_ts;
  uint32_t ble_pin;                  // od v1.1.3
};
#define SETTINGS_MAGIC      0x5A
#define SETTINGS_V102_SIZE  (2 + MAP_NAME_MAX + 1)
#define SETTINGS_V103_SIZE  (SETTINGS_V102_SIZE + 4)
#define SETTINGS_V110_SIZE  (SETTINGS_V103_SIZE + 1 + 4)

static bool nameValid(const char* n, int len) {
  if (len < MAP_NAME_MIN || len > MAP_NAME_MAX) return false;
  for (int i = 0; i < len; i++) {
    if (n[i] < 0x21 || n[i] > 0x7E) return false;   // ASCII bez spacji i znakow sterujacych
  }
  return true;
}

static void loadSettings() {
  File f = InternalFS.open(SETTINGS_FILE, FILE_O_READ);
  if (f) {
    MapperSettings st;
    memset(&st, 0, sizeof(st));
    int n = f.read((uint8_t*)&st, sizeof(st));
    f.close();
    if (n >= 2 && st.magic == SETTINGS_MAGIC) {
      if (mapIntervalValid(st.interval_s)) interval_s = st.interval_s;
      if (n >= SETTINGS_V102_SIZE) {
        st.name[MAP_NAME_MAX] = 0;
        if (nameValid(st.name, strlen(st.name))) strcpy(tracker_name, st.name);
      }
      if (n >= SETTINGS_V103_SIZE) {
        buzzer_on = st.buzzer_on != 0; gps_on = st.gps_on != 0; ble_on = st.ble_on != 0; lock_on = st.lock_on != 0;
      }
      if (n >= SETTINGS_V110_SIZE) {
        timeout_idx = st.timeout_idx <= 3 ? st.timeout_idx : 1;
        last_sync_ts = st.last_sync_ts;
      }
      if (n == sizeof(st) && st.ble_pin >= 100000 && st.ble_pin <= 999999) ble_pin = st.ble_pin;
    }
  }
  File s = InternalFS.open(SESSION_FILE, FILE_O_READ);
  if (s) { s.read(&session_no, 1); s.close(); }
}

static void saveSettings() {
  MapperSettings st;
  memset(&st, 0, sizeof(st));
  st.magic = SETTINGS_MAGIC;
  st.interval_s = interval_s;
  strncpy(st.name, tracker_name, MAP_NAME_MAX);
  st.buzzer_on = buzzer_on; st.gps_on = gps_on; st.ble_on = ble_on; st.lock_on = lock_on;
  st.timeout_idx = timeout_idx;
  st.last_sync_ts = last_sync_ts;
  st.ble_pin = ble_pin;
  InternalFS.remove(SETTINGS_FILE);
  File f = InternalFS.open(SETTINGS_FILE, FILE_O_WRITE);
  if (!f) return;
  f.write((const uint8_t*)&st, sizeof(st));
  f.close();
}

static void saveSession() {
  InternalFS.remove(SESSION_FILE);
  File f = InternalFS.open(SESSION_FILE, FILE_O_WRITE);
  if (!f) return;
  f.write(&session_no, 1);
  f.close();
}

static bool setInterval(int secs) {
  if (!mapIntervalValid(secs)) return false;
  if (secs != interval_s) {
    interval_s = (uint8_t) secs;
    saveSettings();
    if (last_ping_at != 0 && still_count < STATIONARY_PINGS) {
      next_ping_at = last_ping_at + (unsigned long) interval_s * 1000UL;
    }
  }
  return true;
}

// ---------------- sesja ----------------
static void startSession() {
  session_no++;
  saveSession();
  sess_start = millis();
  have_sess_pos = false;
  status.sess_pings = status.sess_hits = status.sess_dist_m = 0;
  status.last_n = -1;
  memset(status.recent, 0, sizeof(status.recent));
}

// repeater na ekranie: pierwsze 2 bajty klucza (XXXX); nazwy zna serwer
static void rptLabel(const uint8_t* prefix, char* dest, size_t sz) {
  snprintf(dest, sz, "%02X%02X", prefix[0], prefix[1]);
}

// lista ostatnich repeaterow: bez powtorzen, najnowszy na gorze
static void addRecent(const MapResp& r, uint8_t hh, uint8_t mm) {
  char nm[17];
  rptLabel(r.prefix, nm, sizeof(nm));
  int pos = MAP_RECENT - 1;
  for (int i = 0; i < MAP_RECENT; i++) {
    if (status.recent[i].valid && strcmp(status.recent[i].name, nm) == 0) { pos = i; break; }
  }
  for (int i = pos; i > 0; i--) status.recent[i] = status.recent[i - 1];
  MapperRecent& e = status.recent[0];
  e.valid = true; e.hh = hh; e.mm = mm; e.snr_x4 = r.snr_rx;
  strncpy(e.name, nm, sizeof(e.name) - 1); e.name[sizeof(e.name) - 1] = 0;
}

// ---------------- limit repeaterow ----------------
static void limitRecord(const uint8_t* prefix, unsigned long now) {
  LimitEntry* e = NULL;
  for (int i = 0; i < LIMIT_TABLE; i++) if (limits[i].used && memcmp(limits[i].prefix, prefix, MAP_PREFIX_LEN) == 0) { e = &limits[i]; break; }
  if (!e) {
    int pick = 0; unsigned long oldest = 0xFFFFFFFFUL;      // wolne miejsce albo najdawniej slyszany
    for (int i = 0; i < LIMIT_TABLE; i++) {
      if (!limits[i].used) { pick = i; break; }
      unsigned long last = limits[i].t[(limits[i].n + RPT_LIMIT_COUNT - 1) % RPT_LIMIT_COUNT];
      if (last < oldest) { oldest = last; pick = i; }
    }
    e = &limits[pick];
    memset(e, 0, sizeof(*e));
    e->used = true;
    memcpy(e->prefix, prefix, MAP_PREFIX_LEN);
  }
  e->t[e->n % RPT_LIMIT_COUNT] = now;
  e->n++;
}
// pudlo moglo wynikac z limitu, jesli jakis repeater odpowiedzial nam juz 4 razy w ostatnich 2 minutach
static bool limitSuspect(unsigned long now) {
  for (int i = 0; i < LIMIT_TABLE; i++) {
    const LimitEntry& e = limits[i];
    if (!e.used || e.n < RPT_LIMIT_COUNT) continue;
    bool all = true;
    for (int k = 0; k < RPT_LIMIT_COUNT; k++) if (now - e.t[k] > RPT_LIMIT_WINDOW_MS) { all = false; break; }
    if (all) return true;
  }
  return false;
}

// ---------------- wykrywanie ruchu ----------------
static bool rawMoving();
static void movementLoop() {
  if (!gpsFix()) { fix_since = 0; mv_count = 0; mv_streak = 0; return; }
  unsigned long now = millis();
  if (fix_since == 0) { fix_since = now; mv_count = 0; mv_streak = 0; }
  if (now - fix_since < FIX_WARMUP_MS) return;
  if ((long)(now - mv_next) < 0) return;
  mv_next = now + MOVE_SAMPLE_MS;
  LocationProvider* l = gps();
  mv_lat[mv_head] = l->getLatitude();
  mv_lon[mv_head] = l->getLongitude();
  mv_head = (mv_head + 1) % MOVE_WINDOW;
  if (mv_count < MOVE_WINDOW) mv_count++;
  mv_streak = rawMoving() ? mv_streak + 1 : 0;
}

// Ruch = przesuniecie >= 25 m w oknie ~20 s po w miare prostej drodze. Drgania GPS w budynku (losowe skoki
// po kilkadziesiat metrow) maja dluga droge i male przesuniecie netto, wiec nie sa ruchem.
static bool rawMoving() {
  if (mv_count < MOVE_MIN_SAMPLES) return false;
  int oldest = (mv_head + MOVE_WINDOW - mv_count) % MOVE_WINDOW;
  int newest = (mv_head + MOVE_WINDOW - 1) % MOVE_WINDOW;
  int mid    = (oldest + mv_count / 2) % MOVE_WINDOW;
  double net = distMeters(mv_lat[oldest], mv_lon[oldest], mv_lat[newest], mv_lon[newest]);
  if (net < MOVE_SPREAD_M) return false;
  double path = 0;
  for (int k = 0; k < mv_count - 1; k++) {
    int i = (oldest + k) % MOVE_WINDOW, j = (oldest + k + 1) % MOVE_WINDOW;
    path += distMeters(mv_lat[i], mv_lon[i], mv_lat[j], mv_lon[j]);
  }
  if (path <= 0 || net / path < 0.6) return false;
  return distMeters(mv_lat[oldest], mv_lon[oldest], mv_lat[mid], mv_lon[mid]) >= MOVE_SPREAD_M * 0.3
      && distMeters(mv_lat[mid], mv_lon[mid], mv_lat[newest], mv_lon[newest]) >= MOVE_SPREAD_M * 0.3;
}

// ruch potwierdzony w 3 kolejnych probkach (~6 s), zeby pojedynczy skok GPS nie wznawial pingow
static bool isMoving() { return mv_streak >= 3; }

// ---------------- mapowanie ----------------
static void startPing() {
  LocationProvider* l = gps();
  int32_t lat = l->getLatitude();
  int32_t lon = l->getLongitude();

  // postoj: 4 pingi bez ruchu = pauza; wznowienie tylko przy potwierdzonym ruchu
  bool moving = isMoving();
  if (still_count >= STATIONARY_PINGS) {
    if (!moving) { next_ping_at = millis() + 2000; return; }
    still_count = 1;
  } else if (moving) {
    still_count = 1;
  } else {
    still_count++;
  }
  last_lat = lat; last_lon = lon; have_last_pos = true;

  // dystans sesji (miedzy kolejnymi pingami; drobne drgania GPS i skoki pomijane)
  if (have_sess_pos) {
    double dd = distMeters(sess_lat, sess_lon, lat, lon);
    if (dd >= 15.0 && dd < 5000.0) { status.sess_dist_m += (uint32_t) dd; sess_lat = lat; sess_lon = lon; }
  } else { have_sess_pos = true; sess_lat = lat; sess_lon = lon; }

  memset(&ping_hdr, 0, sizeof(ping_hdr));
  long ts = l->getTimestamp();
  ping_hdr.ts   = (ts > 1700000000L) ? (uint32_t) ts : nowUnix();
  ping_hdr.lat  = lat;
  ping_hdr.lon  = lon;
  ping_hdr.alt  = (int16_t)(l->getAltitude() / 1000);
  ping_hdr.sats = (uint8_t) l->satellitesCount();
  ping_hdr.flags = ((interval_s / 10) & MAP_REC_INTERVAL_MASK) | ((session_no & 0x0F) << MAP_REC_SESSION_SHIFT);

  if (the_mesh.sendDiscover()) {
    collect_until = millis() + COLLECT_MS;
    status.sess_pings++;
  }
  last_ping_at = millis();
  next_ping_at = last_ping_at + (unsigned long) interval_s * 1000UL;
}

static void finishPing() {
  MapResp resp[MAP_MAX_RESP];
  int n = the_mesh.finishCollect(resp);
  unsigned long now = millis();
  ping_hdr.n = (uint8_t) n;
  if (n == 0 && limitSuspect(now)) ping_hdr.flags |= MAP_REC_LIMIT;
  for (int i = 0; i < n; i++) limitRecord(resp[i].prefix, now);

  if (!maplog.append(ping_hdr, resp)) {
    MESH_DEBUG_PRINTLN("MapLog: nie zapisano punktu (pamiec pelna?)");
  }

  status.last_n = n;
  last_resp_at = now;
  if (n > 0) {
    status.sess_hits++;
    // najlepszy na gorze listy ostatnich
    int order[MAP_MAX_RESP];
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (resp[order[j]].snr_rx > resp[order[i]].snr_rx) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    uint8_t hh = 0, mm = 0;
    uint32_t t = nowUnix();
    if (t) localParts(t, &hh, &mm, NULL, NULL, NULL, NULL);
    for (int i = n - 1; i >= 0; i--) addRecent(resp[order[i]], hh, mm);
    rptLabel(resp[order[0]].prefix, status.last_name, sizeof(status.last_name));
    status.last_snr_x4 = resp[order[0]].snr_rx;
    ui.beep(MapperUI::BEEP_HIT);
  }
}

static void mappingLoop() {
  if (the_mesh.isCollecting()) {
    if ((long)(millis() - collect_until) >= 0) finishPing();
    return;
  }
  if (!run_mapping || low_batt || maplog.isFull() || !gpsFix()) return;
  if ((long)(millis() - next_ping_at) >= 0) startPing();
}

// ---------------- nazwa BLE ----------------
static void applyBleName() {
  snprintf(ble_full_name, sizeof(ble_full_name), "%s%s", BLE_NAME_PREFIX, tracker_name);
  Bluefruit.setName(ble_full_name);
  Bluefruit.ScanResponse.clearData();
  Bluefruit.ScanResponse.addName();
  strncpy(status.name, tracker_name, sizeof(status.name) - 1);
  status.name[sizeof(status.name) - 1] = 0;
}

// ---------------- wylaczanie i resety ----------------
static void doPowerOff(bool wait_release) {
  ui.showNow("Wyłączono", "Włącz: naciśnij OK");
  if (wait_release) {                               // przycisk musi byc puszczony, inaczej od razu sie wybudzi
    unsigned long t0 = millis();
    while (user_btn.isPressed() && millis() - t0 < 8000) delay(20);
  }
  delay(1500);
#ifdef DISPLAY_CLASS
  display.turnOff();
#endif
  ble.disable();
  nrf_gpio_cfg_sense_input(g_ADigitalPinMap[PIN_USER_BTN], NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
  board.powerOff();
}

static uint32_t randomPin() {
  uint32_t r = 0;
  the_mesh.getRNG()->random((uint8_t*)&r, sizeof(r));
  return 100000UL + r % 900000UL;                   // 6 cyfr, bez zera na poczatku
}

static void wipeAndRestart(bool new_identity) {
  ble.disable();
  Bluefruit.Periph.clearBonds();
  maplog.eraseAll();
  InternalFS.remove(SETTINGS_FILE);
  if (new_identity) InternalFS.remove(IDENTITY_FILE);
  delay(800);
  NVIC_SystemReset();
}

// ---------------- sterowanie z menu urzadzenia ----------------
static void applyGps() { sensors.setSettingValue("gps", gps_on ? "1" : "0"); if (!gps_on) { fix_since = 0; mv_count = 0; } }
static void applyBle() { if (ble_on) ble.enable(); else { sync_active = false; ble.disable(); } }

class TrackerControl : public MapperControl {
public:
  bool isRunning() override { return run_mapping; }
  void setRunning(bool on) override { run_mapping = on; if (on) { next_ping_at = millis(); still_count = 0; } }
  int  getInterval() override { return interval_s; }
  void setIntervalSecs(int s) override { setInterval(s); }
  bool isGpsOn() override { return gps_on; }
  void setGpsOn(bool on) override { gps_on = on; saveSettings(); applyGps(); }
  bool isBleOn() override { return ble_on; }
  void setBleOn(bool on) override { ble_on = on; saveSettings(); applyBle(); }
  bool isBuzzerOn() override { return buzzer_on; }
  void setBuzzerOn(bool on) override { buzzer_on = on; saveSettings(); }
  bool isLockOn() override { return lock_on; }
  void setLockOn(bool on) override { lock_on = on; saveSettings(); }
  int  getTimeoutIdx() override { return timeout_idx; }
  void setTimeoutIdx(int i) override { timeout_idx = (uint8_t)(i & 3); saveSettings(); }
  bool isUsb() override { return usb_power; }
  void newSession() override { startSession(); }
  uint32_t pendingCount() override { return maplog.pending(); }
  void clearBonds() override { ble.disable(); Bluefruit.Periph.clearBonds(); if (ble_on) ble.enable(); }
  void eraseData() override { maplog.eraseAll(); mem_warned = 0; }
  void factoryReset() override { wipeAndRestart(false); }
  void newPin() override {                          // nowy PIN: sparowane telefony usuniete, restart z nowym PIN-em
    ble_pin = randomPin();
    saveSettings();
    ble.disable();
    Bluefruit.Periph.clearBonds();
    delay(500);
    NVIC_SystemReset();
  }
  uint32_t getPin() override { return ble_pin; }
  void newIdentity() override { wipeAndRestart(true); }
  void powerOff() override { doPowerOff(true); }
  int  getInfoLines(char lines[][MAP_LINE_SZ], int max) override {
    int n = 0;
    if (n < max) snprintf(lines[n++], MAP_LINE_SZ, "%s", tracker_name);
    if (n < max) snprintf(lines[n++], MAP_LINE_SZ, "PIN %06lu · %02X%02X%02X%02X", (unsigned long) ble_pin,
                          the_mesh.self_id.pub_key[0], the_mesh.self_id.pub_key[1], the_mesh.self_id.pub_key[2], the_mesh.self_id.pub_key[3]);
    if (n < max) snprintf(lines[n++], MAP_LINE_SZ, "Firmware: %s", MAPPER_FW_VERSION);
    if (n < max) snprintf(lines[n++], MAP_LINE_SZ, "Pamięć: %d%% · %lu pkt", memPct(), (unsigned long) maplog.pending());
    if (n < max) {
      if (last_sync_ts) {
        uint8_t hh, mm, d, mo;
        localParts(last_sync_ts, &hh, &mm, NULL, &d, &mo, NULL);
        snprintf(lines[n++], MAP_LINE_SZ, "Zrzut: %02u.%02u %02u:%02u", d, mo, hh, mm);
      } else snprintf(lines[n++], MAP_LINE_SZ, "Zrzut: jeszcze nie było");
    }
    if (n < max) {
      uint16_t mv = status.batt_mv;
      if (usb_power) snprintf(lines[n++], MAP_LINE_SZ, "Bateria: %u,%02u V USB", mv / 1000, (mv % 1000) / 10);
      else snprintf(lines[n++], MAP_LINE_SZ, "Bateria: %u,%02u V %d%%", mv / 1000, (mv % 1000) / 10, status.batt_pct);
    }
    return n;
  }
};
static TrackerControl control;

// ---------------- bateria i pamiec ----------------
static void powerLoop() {
  static unsigned long next_read = 0;
  if ((long)(millis() - next_read) < 0) return;
  next_read = millis() + BATT_READ_MS;

  usb_power = board.isExternalPowered();
  uint16_t mv = board.getBattMilliVolts();
  batt_hist[batt_idx++ % 3] = mv;
  uint32_t sum = 0; int cnt = 0;
  for (int i = 0; i < 3; i++) if (batt_hist[i]) { sum += batt_hist[i]; cnt++; }
  uint16_t avg = cnt ? sum / cnt : mv;
  status.batt_mv = avg;
  int pct = ((int) avg - 3300) * 100 / (4150 - 3300);
  status.batt_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;

  if (usb_power) {
    if (low_batt) { low_batt = false; next_ping_at = millis(); }
  } else if (cnt == 3) {
    if (!low_batt && avg < BATT_LOW_MV) {
      low_batt = true;
      ui.wake(); ui.flash("Słaba bateria", "mapowanie wstrzymane", 3000); ui.beep(MapperUI::BEEP_WARN);
    } else if (low_batt && avg > BATT_RESUME_MV) {
      low_batt = false; next_ping_at = millis();
    }
    if (avg < BATT_OFF_MV) {                        // ochrona danych: wylacz zanim zasilanie zaniknie
      ui.flash("Bateria rozładowana", "wyłączam", 3000);
      doPowerOff(false);
    }
  }

  // ostrzezenia o zapelnieniu pamieci: 80%, 90%, 100%
  int mp = memPct();
  int level = maplog.isFull() ? 100 : mp >= 90 ? 90 : mp >= 80 ? 80 : 0;
  if (level > mem_warned) {
    char l1[MAP_LINE_SZ];
    if (level == 100) strcpy(l1, "Pamięć pełna!");
    else snprintf(l1, sizeof(l1), "Pamięć %d%%", level);
    ui.wake(); ui.flash(l1, "zrzuć dane w aplikacji", 3000); ui.beep(MapperUI::BEEP_WARN);
  }
  mem_warned = level;
}

// ---------------- protokol BLE ----------------
static void sendErr(uint8_t err) {
  uint8_t f[2] = { RESP_MAP_ERR, err };
  ble.writeFrame(f, 2);
}

static void sendHello() {
  int i = 0;
  frame[i++] = RESP_MAP_HELLO;
  frame[i++] = MAPPER_PROTO_VER;
  frame[i++] = stateFlags();
  memcpy(&frame[i], the_mesh.self_id.pub_key, PUB_KEY_SIZE); i += PUB_KEY_SIZE;
  uint32_t v;
  v = maplog.pending();  memcpy(&frame[i], &v, 4); i += 4;
  v = maplog.ackedSeq(); memcpy(&frame[i], &v, 4); i += 4;
  v = maplog.lastSeq();  memcpy(&frame[i], &v, 4); i += 4;
  v = maplog.total();    memcpy(&frame[i], &v, 4); i += 4;
  uint16_t mv = status.batt_mv ? status.batt_mv : board.getBattMilliVolts(); memcpy(&frame[i], &mv, 2); i += 2;
  LocationProvider* l = gps();
  frame[i++] = l ? (uint8_t) l->satellitesCount() : 0;
  int32_t lat = l && l->isValid() ? l->getLatitude() : 0;
  int32_t lon = l && l->isValid() ? l->getLongitude() : 0;
  memcpy(&frame[i], &lat, 4); i += 4;
  memcpy(&frame[i], &lon, 4); i += 4;
  uint32_t fill = maplog.fileBytes(), cap = maplog.maxBytes();
  memcpy(&frame[i], &fill, 4); i += 4;
  memcpy(&frame[i], &cap, 4); i += 4;
  frame[i++] = interval_s;
  const char* fw = MAPPER_FW_VERSION;
  int fl = strlen(fw);
  frame[i++] = (uint8_t) fl;
  memcpy(&frame[i], fw, fl); i += fl;
  int nl = strlen(tracker_name);
  frame[i++] = (uint8_t) nl;
  memcpy(&frame[i], tracker_name, nl); i += nl;
  // wersja 4: ostatni zrzut, sesja, pamiec
  v = last_sync_ts;      memcpy(&frame[i], &v, 4); i += 4;
  frame[i++] = session_no;
  frame[i++] = (uint8_t) memPct();
  ble.writeFrame(frame, i);
}

static void handleBleFrame(const uint8_t* cmd, size_t len) {
  switch (cmd[0]) {
    case CMD_MAP_HELLO:
      sendHello();
      break;

    case CMD_MAP_GET: {
      if (len < 6) { sendErr(MAP_ERR_BAD_ARGS); break; }
      uint32_t from;
      memcpy(&from, &cmd[1], 4);
      uint8_t max = cmd[5];
      if (max == 0 || max > SYNC_MAX_BATCH) max = SYNC_MAX_BATCH;
      sync_active = true;
      sync_next_seq = from;
      sync_left = max;
      sync_sent = 0;
      sync_last_seq = 0;
      break;
    }

    case CMD_MAP_ACK: {
      if (len < 5) { sendErr(MAP_ERR_BAD_ARGS); break; }
      if (sync_active) { sendErr(MAP_ERR_BUSY); break; }
      uint32_t up_to;
      memcpy(&up_to, &cmd[1], 4);
      maplog.ack(up_to);
      uint32_t t = nowUnix();
      if (t) { last_sync_ts = t; saveSettings(); }
      mem_warned = 0;
      int i = 0;
      frame[i++] = RESP_MAP_ACK;
      uint32_t v = maplog.ackedSeq(); memcpy(&frame[i], &v, 4); i += 4;
      v = maplog.pending();           memcpy(&frame[i], &v, 4); i += 4;
      ble.writeFrame(frame, i);
      break;
    }

    case CMD_MAP_SET_RUN: {
      if (len < 2) { sendErr(MAP_ERR_BAD_ARGS); break; }
      run_mapping = cmd[1] != 0;
      if (run_mapping) { next_ping_at = millis(); still_count = 0; }
      uint8_t f[3] = { RESP_MAP_RUN, (uint8_t)(run_mapping ? 1 : 0), stateFlags() };
      ble.writeFrame(f, 3);
      break;
    }

    case CMD_MAP_SET_INTERVAL: {
      if (len < 2 || !setInterval(cmd[1])) { sendErr(MAP_ERR_BAD_ARGS); break; }
      uint8_t f[2] = { RESP_MAP_INTERVAL, interval_s };
      ble.writeFrame(f, 2);
      break;
    }

    case CMD_MAP_SET_NAME: {
      int nl = (int)len - 1;
      if (!nameValid((const char*)&cmd[1], nl)) { sendErr(MAP_ERR_BAD_ARGS); break; }
      memcpy(tracker_name, &cmd[1], nl);
      tracker_name[nl] = 0;
      saveSettings();
      applyBleName();
      frame[0] = RESP_MAP_NAME;
      memcpy(&frame[1], tracker_name, nl);
      ble.writeFrame(frame, 1 + nl);
      break;
    }

    case CMD_SIGN_START: {
      sign_active = true;
      sign_len = 0;
      uint8_t f[6] = { RESP_SIGN_START, 0, 0, 0, 0, 0 };
      uint32_t mx = MAP_SIGN_MAX;
      memcpy(&f[2], &mx, 4);
      ble.writeFrame(f, 6);
      break;
    }

    case CMD_SIGN_DATA: {
      if (!sign_active) { sendErr(MAP_ERR_BAD_STATE); break; }
      if (len < 2) { sendErr(MAP_ERR_BAD_ARGS); break; }
      if (sign_len + (len - 1) > MAP_SIGN_MAX) { sign_active = false; sendErr(MAP_ERR_TOO_LONG); break; }
      memcpy(&sign_buf[sign_len], &cmd[1], len - 1);
      sign_len += len - 1;
      uint8_t f[1] = { RESP_OK };
      ble.writeFrame(f, 1);
      break;
    }

    case CMD_SIGN_FINISH: {
      if (!sign_active) { sendErr(MAP_ERR_BAD_STATE); break; }
      // tylko wyzwanie logowania Mappera: "mmr1|czas|losowe|podpis serwera" (zwykly tekst, bez dowolnych danych)
      if (sign_len < 20 || sign_len > 80 || memcmp(sign_buf, MAP_SIGN_PREFIX, strlen(MAP_SIGN_PREFIX)) != 0) {
        sign_active = false; sign_len = 0; sendErr(MAP_ERR_NOT_ALLOWED); break;
      }
      frame[0] = RESP_SIGNATURE;
      the_mesh.self_id.sign(&frame[1], sign_buf, sign_len);
      sign_active = false;
      sign_len = 0;
      ble.writeFrame(frame, 1 + SIGNATURE_SIZE);
      break;
    }

    default:
      sendErr(MAP_ERR_UNKNOWN_CMD);   // np. komendy companiona - aplikacja wie, ze to tracker
      break;
  }
}

static void syncLoop() {
  if (!sync_active) return;
  if (!ble.isConnected()) { sync_active = false; return; }
  if (ble.isWriteBusy()) return;

  if (sync_left > 0) {
    uint32_t seq;
    int len = maplog.readFrom(sync_next_seq, &frame[1], &seq);
    if (len > 0) {
      frame[0] = RESP_MAP_RECORD;
      ble.writeFrame(frame, 1 + len);
      sync_next_seq = seq + 1;
      sync_last_seq = seq;
      sync_left--;
      sync_sent++;
      return;
    }
    sync_left = 0;
  }

  // koniec paczki: [END][liczba u16][ostatni seq u32][czy sa dalsze u8]
  int i = 0;
  frame[i++] = RESP_MAP_END;
  memcpy(&frame[i], &sync_sent, 2); i += 2;
  memcpy(&frame[i], &sync_last_seq, 4); i += 4;
  frame[i++] = (sync_sent > 0 && sync_last_seq < maplog.lastSeq()) ? 1 : 0;
  ble.writeFrame(frame, i);
  sync_active = false;
}

static void bleLoop() {
  ble.loop();
  size_t len = ble.checkRecvFrame(frame);
  if (len > 0) {
    uint8_t cmd[MAX_FRAME_SIZE];
    memcpy(cmd, frame, len);
    handleBleFrame(cmd, len);
  }
  syncLoop();
}

// ---------------- stan dla ekranu ----------------
static void updateStatus() {
  LocationProvider* l = gps();
  uint32_t t = nowUnix();
  status.time_valid = t != 0;
  if (t) localParts(t, &status.hh, &status.mm, &status.ss, &status.day, &status.mon, &status.year);
  status.usb = usb_power;
  status.low_batt = low_batt;
  status.gps_on = gps_on;
  status.gps_fix = gpsFix();
  status.sats = (gps_on && l) ? (int) l->satellitesCount() : 0;
  status.moving = isMoving();
  if (status.gps_fix) { status.lat = l->getLatitude(); status.lon = l->getLongitude(); status.alt = (int)(l->getAltitude() / 1000); }
  status.ble_on = ble_on;
  status.ble_connected = ble_on && ble.isConnected();
  status.syncing = sync_active;
  status.run = run_mapping;
  status.stationary = still_count >= STATIONARY_PINGS;
  status.log_full = maplog.isFull();
  status.mem_pct = memPct();
  status.interval_s = interval_s;
  long secs = ((long)(next_ping_at - millis()) + 999) / 1000;
  status.next_ping_secs = secs < 0 ? 0 : (int) secs;
  status.pending = maplog.pending();
  status.buzzer_on = buzzer_on;
  status.sess_no = session_no;
  status.sess_secs = (millis() - sess_start) / 1000;
  status.last_age_s = (millis() - last_resp_at) / 1000;
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  board.begin();

  bool has_display = false;
#ifdef DISPLAY_CLASS
  if (display.begin()) {
    has_display = true;
    display.startFrame();                           // pusty ekran; animacja startowa rusza w ui.begin()
    display.endFrame();
  }
#endif

  if (!radio_init()) { halt(); }
  fast_rng.begin(radio_driver.getRngSeed());
  radio_driver.setParams(SF6_FREQ, SF6_BW, SF6_SF, SF6_CR);   // preset SF6 na sztywno

  // tozsamosc (klucz) i ustawienia w pamieci wewnetrznej
  InternalFS.begin();
  loadSettings();
  IdentityStore store(InternalFS, "");
  if (!store.load("_main", the_mesh.self_id)) {
    the_mesh.self_id = radio_new_identity();
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  // log punktow na flashu QSPI (2 MB); awaryjnie w pamieci wewnetrznej
  if (QSPIFlash.begin()) {
    maplog.begin(&QSPIFlash);
    QSPIFlash.remove("/sf6map/names.bin");          // pozostalosc po 1.1.0 (nazwy repeaterow nie sa juz uzywane)
  } else {
    MESH_DEBUG_PRINTLN("QSPI flash niedostepny - log w InternalFS");
    maplog.begin(&InternalFS, 16 * 1024);
  }

  the_mesh.begin();

  sensors.begin();
  applyGps();

  if (tracker_name[0] == 0) {   // domyslna nazwa: SF6Map-XXXX (poczatek klucza)
    snprintf(tracker_name, sizeof(tracker_name), "SF6Map-%02X%02X", the_mesh.self_id.pub_key[0], the_mesh.self_id.pub_key[1]);
  }
  if (ble_pin < 100000 || ble_pin > 999999) {       // pierwsze uruchomienie (albo po ust. fabrycznych): losowy PIN
    ble_pin = randomPin();
    saveSettings();
  }
  ble.begin(BLE_NAME_PREFIX, tracker_name, ble_pin);
  if (ble_on) ble.enable();

  memset(&status, 0, sizeof(status));
  strncpy(status.name, tracker_name, sizeof(status.name) - 1);
  startSession();                                   // kazde wlaczenie = nowa sesja
  usb_power = board.isExternalPowered();

  ui.begin(has_display, &control);
  next_ping_at = millis() + 3000;

  board.onBootComplete();
}

void loop() {
  the_mesh.loop();
  sensors.loop();
  bleLoop();
  powerLoop();
  movementLoop();
  mappingLoop();
  updateStatus();
  ui.loop(status);
  rtc_clock.tick();
}
