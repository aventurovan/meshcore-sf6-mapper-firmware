#pragma once
/*
 * Trwaly log punktow mapowania (plik append-only na flashu QSPI).
 * Wpis w pliku: [len u8][MapRecordHdr][MapResp x n]
 * Punkty sa usuwane dopiero po potwierdzeniu (ACK) przez aplikacje,
 * czyli po zapisaniu ich na serwerze.
 */
#include <Arduino.h>
#include <helpers/IdentityStore.h>   // definicja FILESYSTEM
#include "MapperProtocol.h"

#ifndef MAP_LOG_MAX_BYTES
  #define MAP_LOG_MAX_BYTES  (1536UL * 1024UL)   // 1.5 MB z 2 MB flasha QSPI
#endif

class MapLog {
  FILESYSTEM* _fs;
  uint32_t _max_bytes;
  uint32_t _next_seq;     // numer dla nastepnego punktu
  uint32_t _acked_seq;    // wszystko <= tej wartosci jest juz na serwerze
  uint32_t _file_bytes;   // rozmiar pliku logu
  uint32_t _pending;      // liczba punktow czekajacych na wyslanie
  uint32_t _total;        // licznik wszystkich zapisanych punktow (od poczatku)

  // pamiec podreczna kursora odczytu (zeby nie skanowac pliku od poczatku)
  uint32_t _cur_off;      // offset wpisu o numerze _cur_seq
  uint32_t _cur_seq;

  void loadState();
  void saveState();
  void scan();
  void compact();

public:
  MapLog() : _fs(NULL), _max_bytes(MAP_LOG_MAX_BYTES), _next_seq(1), _acked_seq(0),
             _file_bytes(0), _pending(0), _total(0), _cur_off(0), _cur_seq(0) { }

  bool begin(FILESYSTEM* fs, uint32_t max_bytes = MAP_LOG_MAX_BYTES);

  // dopisuje punkt; nadaje mu numer seq. false = brak miejsca / blad zapisu
  bool append(MapRecordHdr& hdr, const MapResp* resp);

  // czyta pierwszy punkt o numerze >= from_seq do dest (bez dlugosci).
  // zwraca dlugosc rekordu albo 0 gdy nie ma wiecej punktow.
  int readFrom(uint32_t from_seq, uint8_t* dest, uint32_t* seq_out);

  // aplikacja potwierdza zapis na serwerze wszystkich punktow <= up_to_seq
  void ack(uint32_t up_to_seq);

  // kasuje wszystkie punkty; numeracja leci dalej (serwer odrzuca duplikaty po numerze)
  void eraseAll();

  uint32_t pending() const { return _pending; }
  uint32_t total() const { return _total; }
  uint32_t lastSeq() const { return _next_seq - 1; }
  uint32_t ackedSeq() const { return _acked_seq; }
  uint32_t fileBytes() const { return _file_bytes; }
  uint32_t maxBytes() const { return _max_bytes; }
  bool isFull() const { return _file_bytes + 1 + MAP_RECORD_MAX_LEN > _max_bytes; }
};
