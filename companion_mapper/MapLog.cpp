#include <MeshCore.h>
#include "MapLog.h"

#define MAP_DIR         "/sf6map"
#define MAP_LOG_FILE    "/sf6map/log.bin"
#define MAP_TMP_FILE    "/sf6map/log.tmp"
#define MAP_STATE_FILE  "/sf6map/state.bin"

// kompaktowanie, gdy potwierdzona czesc pliku przekroczy ten rozmiar
#define MAP_COMPACT_THRESHOLD  (256UL * 1024UL)

struct __attribute__((packed)) MapState {
  uint32_t magic;
  uint32_t next_seq;
  uint32_t acked_seq;
  uint32_t total;
};
#define MAP_STATE_MAGIC  0x36464D53   // "SMF6"

static File openRead(FILESYSTEM* fs, const char* name) {
  return fs->open(name, FILE_O_READ);
}

static File openWrite(FILESYSTEM* fs, const char* name) {
  return fs->open(name, FILE_O_WRITE);
}

bool MapLog::begin(FILESYSTEM* fs, uint32_t max_bytes) {
  _fs = fs;
  _max_bytes = max_bytes;
  _fs->mkdir(MAP_DIR);
  loadState();
  scan();
  return true;
}

void MapLog::loadState() {
  MapState st;
  File f = openRead(_fs, MAP_STATE_FILE);
  if (f) {
    if (f.read((uint8_t*)&st, sizeof(st)) == sizeof(st) && st.magic == MAP_STATE_MAGIC) {
      _next_seq = st.next_seq;
      _acked_seq = st.acked_seq;
      _total = st.total;
    }
    f.close();
  }
  if (_next_seq == 0) _next_seq = 1;
}

void MapLog::saveState() {
  MapState st;
  st.magic = MAP_STATE_MAGIC;
  st.next_seq = _next_seq;
  st.acked_seq = _acked_seq;
  st.total = _total;
  _fs->remove(MAP_STATE_FILE);
  File f = openWrite(_fs, MAP_STATE_FILE);
  if (f) {
    f.write((const uint8_t*)&st, sizeof(st));
    f.close();
  }
}

// przeglada plik logu: liczy punkty do wyslania, odtwarza next_seq,
// odcina ewentualny niedokonczony ostatni wpis (np. po zaniku zasilania)
void MapLog::scan() {
  _pending = 0;
  _file_bytes = 0;
  _cur_off = 0; _cur_seq = 0;

  File f = openRead(_fs, MAP_LOG_FILE);
  if (!f) return;

  uint32_t size = f.size();
  uint32_t off = 0;
  uint32_t last_seq = 0;
  uint8_t len;
  MapRecordHdr hdr;
  while (off < size) {
    f.seek(off);
    if (f.read(&len, 1) != 1) break;
    if (len < sizeof(MapRecordHdr) || len > MAP_RECORD_MAX_LEN || off + 1 + len > size) break;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) break;
    last_seq = hdr.seq;
    if (hdr.seq > _acked_seq) _pending++;
    off += 1 + len;
  }
  f.close();
  _file_bytes = off;

  if (off < size) {   // uszkodzony koniec pliku - przepisz poprawna czesc
    MESH_DEBUG_PRINTLN("MapLog: obcinam uszkodzony koniec logu %u -> %u", size, off);
    _fs->remove(MAP_TMP_FILE);
    File src = openRead(_fs, MAP_LOG_FILE);
    File dst = openWrite(_fs, MAP_TMP_FILE);
    if (src && dst) {
      uint8_t buf[256];
      uint32_t left = off;
      while (left > 0) {
        int n = src.read(buf, left > sizeof(buf) ? sizeof(buf) : left);
        if (n <= 0) break;
        dst.write(buf, n);
        left -= n;
      }
    }
    if (src) src.close();
    if (dst) dst.close();
    _fs->remove(MAP_LOG_FILE);
    _fs->rename(MAP_TMP_FILE, MAP_LOG_FILE);
  }

  if (last_seq + 1 > _next_seq) _next_seq = last_seq + 1;
}

bool MapLog::append(MapRecordHdr& hdr, const MapResp* resp) {
  if (hdr.n > MAP_MAX_RESP) hdr.n = MAP_MAX_RESP;
  uint8_t len = sizeof(MapRecordHdr) + hdr.n * sizeof(MapResp);
  if (_file_bytes + 1 + len > _max_bytes) return false;

  hdr.seq = _next_seq;

  uint8_t buf[1 + MAP_RECORD_MAX_LEN];
  buf[0] = len;
  memcpy(&buf[1], &hdr, sizeof(hdr));
  if (hdr.n > 0) memcpy(&buf[1 + sizeof(hdr)], resp, hdr.n * sizeof(MapResp));

  File f = openWrite(_fs, MAP_LOG_FILE);
  if (!f) return false;
  f.seek(f.size());
  size_t written = f.write(buf, 1 + len);
  f.close();
  if (written != (size_t)(1 + len)) return false;

  _file_bytes += 1 + len;
  _next_seq++;
  _pending++;
  _total++;
  return true;
}

int MapLog::readFrom(uint32_t from_seq, uint8_t* dest, uint32_t* seq_out) {
  if (from_seq <= _acked_seq) from_seq = _acked_seq + 1;
  if (from_seq >= _next_seq) return 0;

  File f = openRead(_fs, MAP_LOG_FILE);
  if (!f) return 0;

  uint32_t size = f.size();
  uint32_t off = 0;
  if (_cur_seq != 0 && _cur_seq <= from_seq && _cur_off < size) off = _cur_off;   // start z kursora

  int result = 0;
  uint8_t len;
  while (off < size) {
    f.seek(off);
    if (f.read(&len, 1) != 1) break;
    if (len < sizeof(MapRecordHdr) || len > MAP_RECORD_MAX_LEN) break;
    if (f.read(dest, len) != len) break;
    MapRecordHdr* hdr = (MapRecordHdr*) dest;
    if (hdr->seq >= from_seq) {
      *seq_out = hdr->seq;
      _cur_seq = hdr->seq;
      _cur_off = off;
      result = len;
      break;
    }
    off += 1 + len;
  }
  f.close();
  return result;
}

void MapLog::ack(uint32_t up_to_seq) {
  if (up_to_seq >= _next_seq) up_to_seq = _next_seq - 1;
  if (up_to_seq <= _acked_seq) return;
  _acked_seq = up_to_seq;

  if (_acked_seq >= _next_seq - 1) {
    // wszystko wyslane - kasujemy log
    _fs->remove(MAP_LOG_FILE);
    _file_bytes = 0;
    _pending = 0;
    _cur_off = 0; _cur_seq = 0;
    saveState();
    return;
  }
  saveState();
  _pending = (_next_seq - 1) - _acked_seq;   // numery w logu sa ciagle

  // jesli duza czesc pliku jest juz potwierdzona - przepisz reszte do nowego pliku
  uint32_t acked_bytes = 0;
  {
    File f = openRead(_fs, MAP_LOG_FILE);
    if (f) {
      uint32_t size = f.size(), off = 0;
      uint8_t len;
      MapRecordHdr hdr;
      while (off < size) {
        f.seek(off);
        if (f.read(&len, 1) != 1) break;
        if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) break;
        if (hdr.seq > _acked_seq) break;
        off += 1 + len;
      }
      acked_bytes = off;
      f.close();
    }
  }
  if (acked_bytes >= MAP_COMPACT_THRESHOLD) compact();
}

void MapLog::compact() {
  _fs->remove(MAP_TMP_FILE);
  File src = openRead(_fs, MAP_LOG_FILE);
  File dst = openWrite(_fs, MAP_TMP_FILE);
  if (!src || !dst) {
    if (src) src.close();
    if (dst) dst.close();
    return;
  }
  uint32_t size = src.size(), off = 0;
  uint8_t buf[1 + MAP_RECORD_MAX_LEN];
  while (off < size) {
    src.seek(off);
    if (src.read(buf, 1) != 1) break;
    uint8_t len = buf[0];
    if (len < sizeof(MapRecordHdr) || len > MAP_RECORD_MAX_LEN) break;
    if (src.read(&buf[1], len) != len) break;
    MapRecordHdr* hdr = (MapRecordHdr*) &buf[1];
    if (hdr->seq > _acked_seq) dst.write(buf, 1 + len);
    off += 1 + len;
  }
  src.close();
  dst.close();
  _fs->remove(MAP_LOG_FILE);
  _fs->rename(MAP_TMP_FILE, MAP_LOG_FILE);
  scan();
}

void MapLog::eraseAll() {
  _fs->remove(MAP_LOG_FILE);
  _fs->remove(MAP_TMP_FILE);
  _acked_seq = _next_seq - 1;
  _file_bytes = 0;
  _pending = 0;
  _cur_off = 0; _cur_seq = 0;
  saveState();
}
