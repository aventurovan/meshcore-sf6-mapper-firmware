#include "CompanionMapper.h"
#include <target.h>
#include "MyMesh.h"
#include <helpers/AdvertDataHelpers.h>

#define CMAP_COLLECT_MS      10000UL
#define CMAP_STATIONARY      4
#define CMAP_MOVE_M          30.0
#define CMAP_SPREAD_M        20.0
#define CMAP_MIN_SATS        4
#define CMAP_PHONE_HOLD_MS   120000UL   // telefon mapuje: companion nie pinguje przez 2 min od ostatniego pingu telefonu
#define CMAP_SETTINGS        "/sf6cmap"
#define CMAP_MAGIC           0x5C

CompanionMapper mapper;

static double distM(int32_t a1, int32_t o1, int32_t a2, int32_t o2) {
  double la1 = a1 / 1e6 * DEG_TO_RAD, la2 = a2 / 1e6 * DEG_TO_RAD;
  double x = (o2 - o1) / 1e6 * DEG_TO_RAD * cos((la1 + la2) / 2), y = la2 - la1;
  return sqrt(x * x + y * y) * 6371000.0;
}

CompanionMapper::CompanionMapper() : _host(NULL), _cfgfs(NULL), _run(false), _interval(MAP_INTERVAL_DEFAULT), _session(0),
  _next_ping(0), _last_ping(0), _collect_until(0), _phone_ping_at(0), _collecting(false), _tag(0), _resp_n(0),
  _have_pos(false), _last_lat(0), _last_lon(0), _still(0), _mv_count(0), _mv_head(0), _mv_next(0), _fix_since(0),
  _sync(false), _sync_next(0), _sync_last(0), _sync_left(0), _sync_sent(0), _pings(0), _hits(0), _last_n(-1), _last_snr(0) {
  memset(_lim, 0, sizeof(_lim));
  _last_prefix[0] = _last_prefix[1] = 0;
}

void CompanionMapper::begin(MapperHost* host, FILESYSTEM* logfs, FILESYSTEM* cfgfs) {
  _host = host; _cfgfs = cfgfs;
  _log.begin(logfs, logfs == cfgfs ? 64UL * 1024UL : MAP_LOG_MAX_BYTES);
  loadSettings();
  _session++; saveSettings();                       // kazde wlaczenie = nowa sesja
  _next_ping = millis() + 5000;
}

void CompanionMapper::loadSettings() {
  File f = _cfgfs->open(CMAP_SETTINGS, FILE_O_READ);
  if (!f) return;
  uint8_t b[4];
  if (f.read(b, 4) == 4 && b[0] == CMAP_MAGIC) {
    _run = b[1] != 0;
    if (mapIntervalValid(b[2])) _interval = b[2];
    _session = b[3];
  }
  f.close();
}

void CompanionMapper::saveSettings() {
  uint8_t b[4] = { CMAP_MAGIC, (uint8_t)(_run ? 1 : 0), _interval, _session };
  _cfgfs->remove(CMAP_SETTINGS);
  File f = _cfgfs->open(CMAP_SETTINGS, FILE_O_WRITE);
  if (!f) return;
  f.write(b, 4);
  f.close();
}

void CompanionMapper::setRunning(bool on) {
  _run = on;
  if (on) { _next_ping = millis(); _still = 0; }
  saveSettings();
}

bool CompanionMapper::gpsFix() {
  LocationProvider* l = sensors.getLocationProvider();
  return l != NULL && l->isValid() && l->satellitesCount() >= CMAP_MIN_SATS;
}

void CompanionMapper::movementLoop() {
  if (!gpsFix()) { _fix_since = 0; _mv_count = 0; return; }
  unsigned long now = millis();
  if (_fix_since == 0) { _fix_since = now; _mv_count = 0; }
  if (now - _fix_since < 15000UL || (long)(now - _mv_next) < 0) return;
  _mv_next = now + 2000;
  LocationProvider* l = sensors.getLocationProvider();
  _mv_lat[_mv_head] = l->getLatitude(); _mv_lon[_mv_head] = l->getLongitude();
  _mv_head = (_mv_head + 1) % 10;
  if (_mv_count < 10) _mv_count++;
}

bool CompanionMapper::isMoving() {
  if (_mv_count < 6) return false;
  int nw = (_mv_head + 9) % 10, od = (_mv_head + 10 - _mv_count) % 10, md = (od + _mv_count / 2) % 10;
  return distM(_mv_lat[od], _mv_lon[od], _mv_lat[nw], _mv_lon[nw]) >= CMAP_SPREAD_M
      && distM(_mv_lat[od], _mv_lon[od], _mv_lat[md], _mv_lon[md]) >= CMAP_SPREAD_M * 0.3
      && distM(_mv_lat[md], _mv_lon[md], _mv_lat[nw], _mv_lon[nw]) >= CMAP_SPREAD_M * 0.3;
}

void CompanionMapper::startPing() {
  LocationProvider* l = sensors.getLocationProvider();
  int32_t lat = l->getLatitude(), lon = l->getLongitude();
  bool far = !_have_pos || distM(_last_lat, _last_lon, lat, lon) >= CMAP_MOVE_M;
  if (_still >= CMAP_STATIONARY) {
    if (!far) { _next_ping = millis() + 2000; return; }
    if (!isMoving()) { _last_lat = lat; _last_lon = lon; _next_ping = millis() + 2000; return; }
    _still = 1; _last_lat = lat; _last_lon = lon; _have_pos = true;
  } else if (!far) {
    _still++;
  } else {
    _still = 1; _last_lat = lat; _last_lon = lon; _have_pos = true;
  }

  memset(&_hdr, 0, sizeof(_hdr));
  long ts = l->getTimestamp();
  _hdr.ts = ts > 1700000000L ? (uint32_t) ts : rtc_clock.getCurrentTime();
  _hdr.lat = lat; _hdr.lon = lon;
  _hdr.alt = (int16_t)(l->getAltitude() / 1000);
  _hdr.sats = (uint8_t) l->satellitesCount();
  _hdr.flags = ((_interval / 10) & MAP_REC_INTERVAL_MASK) | ((_session & 0x0F) << MAP_REC_SESSION_SHIFT);

  uint8_t d[10];
  d[0] = 0x80 | 0x01;                               // DISCOVER_REQ, prefix_only
  d[1] = (1 << ADV_TYPE_REPEATER);
  _host->mpRandom(&d[2], 4);
  memcpy(&_tag, &d[2], 4);
  memset(&d[6], 0, 4);
  _resp_n = 0;
  if (_host->mpSendDiscover(d, sizeof(d))) {
    _collecting = true;
    _collect_until = millis() + CMAP_COLLECT_MS;
    _pings++;
  } else _tag = 0;
  _last_ping = millis();
  _next_ping = _last_ping + (unsigned long) _interval * 1000UL;
}

bool CompanionMapper::onControl(mesh::Packet* pkt, int8_t snr_x4, int8_t rssi) {
  if (!_collecting || _tag == 0 || pkt->payload_len < 6 + MAP_PREFIX_LEN) return false;
  if ((pkt->payload[0] & 0xF0) != 0x90) return false;
  uint32_t tag; memcpy(&tag, &pkt->payload[2], 4);
  if (tag != _tag) return false;                    // to nie nasz ping — odpowiedz idzie do aplikacji
  if ((pkt->payload[0] & 0x0F) != ADV_TYPE_REPEATER) return true;
  const uint8_t* key = &pkt->payload[6];
  for (int i = 0; i < _resp_n; i++) if (memcmp(_resp[i].prefix, key, MAP_PREFIX_LEN) == 0) return true;
  if (_resp_n >= MAP_MAX_RESP) return true;
  MapResp& r = _resp[_resp_n++];
  memcpy(r.prefix, key, MAP_PREFIX_LEN);
  r.snr_rx = snr_x4; r.snr_tx = (int8_t) pkt->payload[1]; r.rssi = rssi;
  return true;
}

void CompanionMapper::limitRecord(const uint8_t* prefix, unsigned long now) {
  Lim* e = NULL;
  for (int i = 0; i < 16; i++) if (_lim[i].used && memcmp(_lim[i].p, prefix, MAP_PREFIX_LEN) == 0) { e = &_lim[i]; break; }
  if (!e) {
    int pick = 0; unsigned long oldest = 0xFFFFFFFFUL;
    for (int i = 0; i < 16; i++) {
      if (!_lim[i].used) { pick = i; break; }
      unsigned long last = _lim[i].t[(_lim[i].n + 3) % 4];
      if (last < oldest) { oldest = last; pick = i; }
    }
    e = &_lim[pick]; memset(e, 0, sizeof(*e)); e->used = true; memcpy(e->p, prefix, MAP_PREFIX_LEN);
  }
  e->t[e->n % 4] = now; e->n++;
}

bool CompanionMapper::limitSuspect(unsigned long now) {
  for (int i = 0; i < 16; i++) {
    if (!_lim[i].used || _lim[i].n < 4) continue;
    bool all = true;
    for (int k = 0; k < 4; k++) if (now - _lim[i].t[k] > 120000UL) { all = false; break; }
    if (all) return true;
  }
  return false;
}

void CompanionMapper::finishPing() {
  _collecting = false;
  unsigned long now = millis();
  _hdr.n = (uint8_t) _resp_n;
  if (_resp_n == 0 && limitSuspect(now)) _hdr.flags |= MAP_REC_LIMIT;
  for (int i = 0; i < _resp_n; i++) limitRecord(_resp[i].prefix, now);
  _log.append(_hdr, _resp);
  _last_n = _resp_n;
  if (_resp_n > 0) {
    _hits++;
    int best = 0;
    for (int i = 1; i < _resp_n; i++) if (_resp[i].snr_rx > _resp[best].snr_rx) best = i;
    _last_prefix[0] = _resp[best].prefix[0]; _last_prefix[1] = _resp[best].prefix[1];
    _last_snr = _resp[best].snr_rx;
  }
  _tag = 0;
}

void CompanionMapper::loop() {
  if (!_host) return;
  movementLoop();
  syncLoop(_host->mpSerial());
  if (_collecting) {
    if ((long)(millis() - _collect_until) >= 0) finishPing();
    return;
  }
  if (!_run || _log.isFull() || !gpsFix() || !_host->mpRadioIsSF6()) return;
  if (_phone_ping_at && millis() - _phone_ping_at < CMAP_PHONE_HOLD_MS) return;
  if ((long)(millis() - _next_ping) >= 0) startPing();
}

uint8_t CompanionMapper::flags() {
  uint8_t f = CMAP_STATE_COMPANION;
  if (_run) f |= MAP_STATE_RUN;
  if (gpsFix()) f |= MAP_STATE_GPS_FIX;
  if (_still >= CMAP_STATIONARY) f |= MAP_STATE_STATIONARY;
  if (_log.isFull()) f |= MAP_STATE_LOG_FULL;
  return f;
}

void CompanionMapper::sendErr(BaseSerialInterface* ser, uint8_t err) {
  uint8_t f[2] = { RESP_MAP_ERR, err };
  ser->writeFrame(f, 2);
}

void CompanionMapper::sendHello(BaseSerialInterface* ser) {
  int i = 0; uint32_t v;
  _frame[i++] = RESP_MAP_HELLO;
  _frame[i++] = 4;                                  // wersja protokolu
  _frame[i++] = flags();
  memcpy(&_frame[i], _host->mpPubKey(), PUB_KEY_SIZE); i += PUB_KEY_SIZE;
  v = _log.pending();  memcpy(&_frame[i], &v, 4); i += 4;
  v = _log.ackedSeq(); memcpy(&_frame[i], &v, 4); i += 4;
  v = _log.lastSeq();  memcpy(&_frame[i], &v, 4); i += 4;
  v = _log.total();    memcpy(&_frame[i], &v, 4); i += 4;
  uint16_t mv = _host->mpBattMv(); memcpy(&_frame[i], &mv, 2); i += 2;
  LocationProvider* l = sensors.getLocationProvider();
  _frame[i++] = l ? (uint8_t) l->satellitesCount() : 0;
  int32_t lat = l && l->isValid() ? l->getLatitude() : 0, lon = l && l->isValid() ? l->getLongitude() : 0;
  memcpy(&_frame[i], &lat, 4); i += 4;
  memcpy(&_frame[i], &lon, 4); i += 4;
  v = _log.fileBytes(); memcpy(&_frame[i], &v, 4); i += 4;
  v = _log.maxBytes();  memcpy(&_frame[i], &v, 4); i += 4;
  _frame[i++] = _interval;
  const char* fw = "C" FIRMWARE_VERSION;            // "C" = companion z mapperem
  int fl = strlen(fw); if (fl > 20) fl = 20;
  _frame[i++] = (uint8_t) fl; memcpy(&_frame[i], fw, fl); i += fl;
  const char* nm = _host->mpName();
  int nl = strlen(nm); if (nl > 31) nl = 31;
  _frame[i++] = (uint8_t) nl; memcpy(&_frame[i], nm, nl); i += nl;
  v = 0; memcpy(&_frame[i], &v, 4); i += 4;          // ostatni zrzut (nieznany)
  _frame[i++] = _session;
  _frame[i++] = (uint8_t)(_log.maxBytes() ? (uint64_t)_log.fileBytes() * 100 / _log.maxBytes() : 0);
  ser->writeFrame(_frame, i);
}

bool CompanionMapper::handleCmd(const uint8_t* cmd, size_t len) {
  BaseSerialInterface* ser = _host ? _host->mpSerial() : NULL;
  if (!ser) return false;
  switch (cmd[0]) {
    case CMD_MAP_HELLO: sendHello(ser); return true;
    case CMD_MAP_GET: {
      if (len < 6) { sendErr(ser, MAP_ERR_BAD_ARGS); return true; }
      memcpy(&_sync_next, &cmd[1], 4);
      _sync_left = cmd[5] ? cmd[5] : 200; if (_sync_left > 200) _sync_left = 200;
      _sync_sent = 0; _sync_last = 0; _sync = true;
      return true;
    }
    case CMD_MAP_ACK: {
      if (len < 5) { sendErr(ser, MAP_ERR_BAD_ARGS); return true; }
      if (_sync) { sendErr(ser, MAP_ERR_BUSY); return true; }
      uint32_t up; memcpy(&up, &cmd[1], 4);
      _log.ack(up);
      uint8_t f[9]; f[0] = RESP_MAP_ACK; uint32_t v = _log.ackedSeq(); memcpy(&f[1], &v, 4); v = _log.pending(); memcpy(&f[5], &v, 4);
      ser->writeFrame(f, 9);
      return true;
    }
    case CMD_MAP_SET_RUN: {
      if (len < 2) { sendErr(ser, MAP_ERR_BAD_ARGS); return true; }
      setRunning(cmd[1] != 0);
      uint8_t f[3] = { RESP_MAP_RUN, (uint8_t)(_run ? 1 : 0), flags() };
      ser->writeFrame(f, 3);
      return true;
    }
    case CMD_MAP_SET_INTERVAL: {
      if (len < 2 || !mapIntervalValid(cmd[1])) { sendErr(ser, MAP_ERR_BAD_ARGS); return true; }
      _interval = cmd[1]; saveSettings();
      if (_last_ping && _still < CMAP_STATIONARY) _next_ping = _last_ping + (unsigned long) _interval * 1000UL;
      uint8_t f[2] = { RESP_MAP_INTERVAL, _interval };
      ser->writeFrame(f, 2);
      return true;
    }
    case CMD_MAP_SET_NAME:                          // nazwa = nazwa companiona (zmiana w aplikacji MeshCore)
      sendErr(ser, MAP_ERR_BAD_STATE); return true;
    default:
      return false;
  }
}

void CompanionMapper::syncLoop(BaseSerialInterface* ser) {
  if (!_sync || !ser) return;
  if (!ser->isConnected()) { _sync = false; return; }
  if (ser->isWriteBusy()) return;
  if (_sync_left > 0) {
    uint32_t seq;
    int len = _log.readFrom(_sync_next, &_frame[1], &seq);
    if (len > 0) {
      _frame[0] = RESP_MAP_RECORD;
      ser->writeFrame(_frame, 1 + len);
      _sync_next = seq + 1; _sync_last = seq; _sync_left--; _sync_sent++;
      return;
    }
    _sync_left = 0;
  }
  int i = 0;
  _frame[i++] = RESP_MAP_END;
  memcpy(&_frame[i], &_sync_sent, 2); i += 2;
  memcpy(&_frame[i], &_sync_last, 4); i += 4;
  _frame[i++] = (_sync_sent > 0 && _sync_last < _log.lastSeq()) ? 1 : 0;
  ser->writeFrame(_frame, i);
  _sync = false;
}

void CompanionMapper::getView(MapperView& v) {
  LocationProvider* l = sensors.getLocationProvider();
  v.run = _run; v.radio_ok = _host && _host->mpRadioIsSF6(); v.gps_fix = gpsFix();
  v.sats = l ? (int) l->satellitesCount() : 0;
  v.stationary = _still >= CMAP_STATIONARY;
  v.phone_mapping = _phone_ping_at && millis() - _phone_ping_at < CMAP_PHONE_HOLD_MS;
  v.log_full = _log.isFull(); v.collecting = _collecting;
  v.interval_s = _interval;
  long s = ((long)(_next_ping - millis()) + 999) / 1000; v.next_ping_s = s < 0 ? 0 : (int) s;
  v.pings = _pings; v.hits = _hits; v.pending = _log.pending();
  v.last_n = _last_n; v.last_prefix[0] = _last_prefix[0]; v.last_prefix[1] = _last_prefix[1]; v.last_snr_x4 = _last_snr;
}
