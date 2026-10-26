#pragma once
/*
 * SF6 Mapper - protokol BLE (NUS / BLEUart, ta sama usluga co companion MeshCore).
 * Kazda ramka = jeden zapis BLE, pierwszy bajt = kod komendy/odpowiedzi.
 * Liczby wielobajtowe: little-endian. Opis w PROTOCOL.md.
 */
#include <stdint.h>

#define MAPPER_PROTO_VER      4
#define MAPPER_FW_VERSION     "1.5.4"

// aplikacja -> tracker
#define CMD_MAP_HELLO         0xA0   // []                       -> RESP_MAP_HELLO
#define CMD_MAP_GET           0xA1   // [from_seq u32][max u8]   -> n x RESP_MAP_RECORD + RESP_MAP_END
#define CMD_MAP_ACK           0xA3   // [up_to_seq u32]          -> RESP_MAP_ACK
#define CMD_MAP_SET_RUN       0xA4   // [run u8]                 -> RESP_MAP_RUN
#define CMD_MAP_SET_INTERVAL  0xA5   // [secs u8] 20/30/40/50/60 -> RESP_MAP_INTERVAL
#define CMD_MAP_SET_NAME      0xA7   // [nazwa ASCII 2-20 B]     -> RESP_MAP_NAME

// podpis Ed25519 kluczem trackera - te same kody i odpowiedzi co companion MeshCore,
// wiec aplikacja loguje sie trackerem tym samym kodem co companionem
#define CMD_SIGN_START        33     // []                       -> RESP_SIGN_START
#define CMD_SIGN_DATA         34     // [dane...]                -> RESP_OK
#define CMD_SIGN_FINISH       35     // []                       -> RESP_SIGNATURE

// tracker -> aplikacja
#define RESP_MAP_ERR          0x01   // [err u8]  (ten sam kod co RESP_CODE_ERR companiona)
#define RESP_MAP_HELLO        0xA0
#define RESP_MAP_RECORD       0xA1
#define RESP_MAP_END          0xA2
#define RESP_MAP_ACK          0xA3
#define RESP_MAP_RUN          0xA4
#define RESP_MAP_INTERVAL     0xA5
#define RESP_MAP_NAME         0xA7
#define RESP_OK               0      // jak RESP_CODE_OK companiona
#define RESP_SIGN_START       19     // [0][max_len u32]
#define RESP_SIGNATURE        20     // [podpis 64 B]

#define MAP_ERR_UNKNOWN_CMD   1
#define MAP_ERR_BAD_ARGS      2
#define MAP_ERR_BUSY          3
#define MAP_ERR_BAD_STATE     4      // np. SIGN_DATA bez SIGN_START
#define MAP_ERR_TOO_LONG      5      // za duzo danych do podpisu
#define MAP_ERR_NOT_ALLOWED   6      // podpis odrzucony: to nie jest wyzwanie logowania Mappera

#define MAP_NAME_MIN          2
#define MAP_NAME_MAX          20     // "MeshCore-" + 20 znakow miesci sie w rozgloszeniu BLE
#define MAP_SIGN_MAX          512    // max bajtow danych do podpisu
#define MAP_SIGN_PREFIX       "mmr1|"  // tracker podpisuje tylko wyzwania logowania Mappera (format serwera)

// flagi stanu w RESP_MAP_HELLO
#define MAP_STATE_RUN         0x01   // mapowanie wlaczone
#define MAP_STATE_GPS_FIX     0x02   // jest fix GPS
#define MAP_STATE_STATIONARY  0x04   // pauza - postoj (4 pingi bez ruchu)
#define MAP_STATE_LOG_FULL    0x08   // pamiec pelna, nowe punkty nie sa zapisywane
#define MAP_STATE_GPS_OFF     0x10   // GPS wylaczony w menu trackera (od 1.0.3)
#define MAP_STATE_LOW_BATT    0x20   // slaba bateria - mapowanie wstrzymane (od 1.1.0)
#define MAP_STATE_USB         0x40   // zasilanie z USB (od 1.1.0)

// interwal pingow (s)
#define MAP_INTERVAL_MIN      20
#define MAP_INTERVAL_MAX      60
#define MAP_INTERVAL_STEP     10
#define MAP_INTERVAL_DEFAULT  20
inline bool mapIntervalValid(int s) {
  return s >= MAP_INTERVAL_MIN && s <= MAP_INTERVAL_MAX && (s % MAP_INTERVAL_STEP) == 0;
}

// pole flags rekordu: bity 0-2 = interwal / 10 (2..6), bit 3 = mozliwy limit repeatera (pudlo),
// bity 4-7 = numer sesji (0-15, cyklicznie)
#define MAP_REC_INTERVAL_MASK 0x07
#define MAP_REC_LIMIT         0x08
#define MAP_REC_SESSION_SHIFT 4

#define MAP_PREFIX_LEN        8      // bajty klucza publicznego repeatera
#define MAP_MAX_RESP          13     // max odpowiedzi w jednym punkcie (miesci sie w ramce 176 B)

struct __attribute__((packed)) MapResp {
  uint8_t prefix[MAP_PREFIX_LEN]; // poczatek klucza publicznego repeatera
  int8_t  snr_rx;                 // SNR odpowiedzi repeatera u nas  (dB x 4)
  int8_t  snr_tx;                 // SNR naszego pingu u repeatera   (dB x 4)
  int8_t  rssi;                   // RSSI odpowiedzi u nas (dBm)
};

struct __attribute__((packed)) MapRecordHdr {
  uint32_t seq;     // numer kolejny punktu (rosnacy, unikalny na urzadzeniu)
  uint32_t ts;      // czas UNIX (UTC) z GPS
  int32_t  lat;     // stopnie x 1e6
  int32_t  lon;     // stopnie x 1e6
  int16_t  alt;     // wysokosc npm (m)
  uint8_t  sats;    // liczba satelitow
  uint8_t  flags;   // bity 0-2: interwal / 10 s, bit 3: mozliwy limit, bity 4-7: sesja
  uint8_t  n;       // liczba odpowiedzi; 0 = pudlo (brak odpowiedzi)
};

#define MAP_RECORD_MAX_LEN  (sizeof(MapRecordHdr) + MAP_MAX_RESP * sizeof(MapResp))   // 164
