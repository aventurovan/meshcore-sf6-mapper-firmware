#pragma once
/*
 * SF6 Mapper w companionie MeshCore (Wio Tracker L1).
 * Companion dziala normalnie (aplikacja MeshCore, Mapper w telefonie), a dodatkowo moze sam mapowac
 * bez telefonu: co 20-60 s ping discovery (tylko repeatery), zapis odpowiedzi z pozycja GPS do flasha,
 * zrzut danych przez Bluetooth do aplikacji Mapper SF6 (ten sam protokol co tracker SF6 Mapper).
 */
#include <Arduino.h>
#include <Mesh.h>
#include <helpers/BaseSerialInterface.h>
#include "MapperProtocol.h"
#include "MapLog.h"

#define CMAP_STATE_COMPANION  0x80   // flaga HELLO: urzadzenie jest companionem z mapperem (nie samodzielnym trackerem)

// to, czego mapper potrzebuje od companiona (implementuje MyMesh)
class MapperHost {
public:
  virtual bool mpSendDiscover(const uint8_t* data, int len) = 0;
  virtual void mpRandom(uint8_t* dest, int n) = 0;
  virtual const uint8_t* mpPubKey() = 0;
  virtual const char* mpName() = 0;
  virtual bool mpRadioIsSF6() = 0;
  virtual uint16_t mpBattMv() = 0;
  virtual BaseSerialInterface* mpSerial() = 0;
  virtual void mpSetGps(bool on) = 0;        // GPS wl./wyl. (z zapisem w ustawieniach companiona)
};

// interfejs ekranu (UITask): sygnal przy odpowiedzi repeatera
void mapperUiHit();

#define CMAP_RECENT 5
struct MapperRecent { bool valid; uint32_t ts; uint8_t prefix[2]; int8_t snr_x4; };

struct MapperView {          // stan dla ekranu
  bool run, radio_ok, gps_fix, stationary, phone_mapping, log_full, collecting;
  int sats, interval_s, next_ping_s, last_n;
  uint32_t pings, hits, pending;
  uint8_t last_prefix[2];
  int8_t last_snr_x4;
};

class CompanionMapper {
  MapperHost* _host;
  FILESYSTEM* _cfgfs;
  MapLog _log;
  bool _run;
  uint8_t _interval;
  uint8_t _session;
  unsigned long _next_ping, _last_ping, _collect_until, _phone_ping_at;
  bool _collecting;
  uint32_t _tag;
  int _resp_n;
  MapResp _resp[MAP_MAX_RESP];
  MapRecordHdr _hdr;
  // postoj / ruch
  bool _have_pos;
  int32_t _last_lat, _last_lon;
  int _still;
  int32_t _mv_lat[10], _mv_lon[10];
  int _mv_count, _mv_head, _mv_streak;
  unsigned long _mv_next, _fix_since;
  // limit repeatera (4 odpowiedzi / 2 min)
  struct Lim { uint8_t p[MAP_PREFIX_LEN]; unsigned long t[4]; uint8_t n; bool used; } _lim[16];
  // zrzut przez BLE
  bool _sync;
  uint32_t _sync_next, _sync_last;
  uint16_t _sync_left, _sync_sent;
  uint8_t _frame[MAX_FRAME_SIZE];
  // licznik sesji
  uint32_t _pings, _hits;
  int _last_n;
  uint8_t _last_prefix[2];
  int8_t _last_snr;
  bool _sound;
  uint32_t _scan_tag = 0;
  unsigned long _scan_until = 0;
  int _scan_n = 0;
  MapResp _scan[MAP_MAX_RESP];
  MapperRecent _recent[CMAP_RECENT];

  bool gpsFix();
  void movementLoop();
  bool isMoving();
  bool rawMoving();
  void startPing();
  void finishPing();
  void limitRecord(const uint8_t* prefix, unsigned long now);
  bool limitSuspect(unsigned long now);
  void loadSettings();
  void saveSettings();
  uint8_t flags();
  void sendHello(BaseSerialInterface* ser);
  void syncLoop(BaseSerialInterface* ser);
  void sendErr(BaseSerialInterface* ser, uint8_t err);

public:
  CompanionMapper();
  void begin(MapperHost* host, FILESYSTEM* logfs, FILESYSTEM* cfgfs);
  void loop();
  bool handleCmd(const uint8_t* cmd, size_t len);            // 0xA0..0xA7; true = obsluzone
  bool onControl(mesh::Packet* pkt, int8_t snr_x4, int8_t rssi);   // true = odpowiedz na nasz ping (nie do aplikacji)
  // „Co tu slychac?”: jednorazowy ping z ekranu (niezaleznie od mapowania, bez zapisu do logu)
  bool scanStart();
  bool scanActive() const { return _scan_tag != 0 && (long)(millis() - _scan_until) < 0; }
  int  scanCount() const { return _scan_n; }
  const MapResp* scanResults() const { return _scan; }
  void notePhoneDiscover() { _phone_ping_at = millis(); }   // telefon sam mapuje — wstrzymaj pingi
  bool isRunning() const { return _run; }
  void setRunning(bool on);                                  // wlacza/wylacza tez GPS
  int  getInterval() const { return _interval; }
  void setInterval(int s);
  bool soundOn() const { return _sound; }
  void setSound(bool on) { _sound = on; saveSettings(); }
  const MapperRecent* recent() const { return _recent; }
  void newSession() { _session++; _pings = 0; _hits = 0; _last_n = -1; memset(_recent, 0, sizeof(_recent)); saveSettings(); }
  void getView(MapperView& v);
};

extern CompanionMapper mapper;
