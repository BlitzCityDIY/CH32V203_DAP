/*!
 * @file dragdrop_engine.cpp
 *
 * Drag-and-drop programming logic
 *
 */

#include "dragdrop_engine.h"

#include <string.h>

#include "../engine/dap_ch32_platform.h"

static const uint32_t TARGET_FLASH =
    Adafruit_DAP_CH32_STM32F4::STM32_FLASH_BASE;

// Text helpers (this core's printf can't print %lx)
namespace {
struct Text {
  char *buf;
  uint32_t max, len;
  void str(const char *s) {
    while (s && *s && len < max)
      buf[len++] = *s++;
  }
  void hex32(uint32_t v) {
    static const char d[] = "0123456789ABCDEF";
    str("0x");
    for (int i = 28; i >= 0; i -= 4) {
      if (len < max)
        buf[len++] = d[(v >> i) & 0xF];
    }
  }
  void dec(uint32_t v) {
    char t[11];
    int n = 0;
    do {
      t[n++] = (char)('0' + v % 10);
      v /= 10;
    } while (v);
    while (n && len < max)
      buf[len++] = t[--n];
  }
};

uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

// "FEATHE~1BIN" -> "FEATHE~1.BIN"
void name83(const char *n, char *out) {
  int k = 0;
  for (int i = 0; i < 8 && n[i] != ' '; i++)
    out[k++] = n[i];
  if (n[8] != ' ') {
    out[k++] = '.';
    for (int i = 8; i < 11 && n[i] != ' '; i++)
      out[k++] = n[i];
  }
  out[k] = 0;
}
} // namespace

/*!
 * @brief Identify the target (no halt or reset) and set up the drive.
 * @param dap Probe; begin() must have been called on it.
 * @param label Volume label, up to 11 characters.
 */
void DragDropEngine::begin(Adafruit_DAP_CH32 *dap, const char *label) {
  _dap = dap;
  _f4.begin(dap);
  _found = _f4.identify();
  _dap->dpRead(Adafruit_DAP_CH32::DP_REG_DPIDR, &_dpidr);
  _dap->disconnect();
  _fat.setVolumeLabel(label);
  _state = IDLE;
  _result = NONE;
  rebuildFiles();
}

const char *DragDropEngine::targetName(void) const {
  return _found ? _f4.deviceName() : "none";
}

void DragDropEngine::rebuildFiles(void) {
  _fat.clearFiles();
  _fat.addFile("DETAILS TXT", detailsText, this);
  if (_result == FAILED) {
    _fat.addFile("FAIL    TXT", failText, this);
  }
}

// STM32 vector table: initial SP in SRAM (or F4 CCM RAM), reset handler an
// odd (Thumb) address in flash.
bool DragDropEngine::looksLikeImage(const uint8_t *buf) {
  uint32_t sp = get32(buf), reset = get32(buf + 4);
  bool sp_ok = !(sp & 3) && ((sp >= 0x20000000 && sp <= 0x20080000) ||
                             (sp >= 0x10000000 && sp <= 0x10010000));
  bool reset_ok =
      (reset & 1) && reset >= TARGET_FLASH && reset < TARGET_FLASH + 0x100000;
  return sp_ok && reset_ok;
}

void DragDropEngine::parseDir(const uint8_t *buf) {
  const uint32_t first_free = _fat.firstFreeSector();
  for (uint32_t i = 0; i < VirtualFat::SECTOR_SIZE / 32; i++) {
    const uint8_t *e = buf + i * 32;
    uint8_t attr = e[11];
    if (e[0] == 0x00) {
      break; // end of directory
    }
    if (e[0] == 0xE5 || attr == 0x0F || (attr & 0x18)) {
      continue; // deleted, long-name part, volume label or directory
    }
    uint32_t cluster = (uint32_t)e[26] | ((uint32_t)e[27] << 8);
    if (cluster < 2) {
      continue; // empty file (size not written yet)
    }
    uint32_t start = VirtualFat::DATA_START +
                     (cluster - 2) * VirtualFat::SECTORS_PER_CLUSTER;
    if (start < first_free) {
      continue; // one of our generated files
    }
    // Update an existing entry or add (replacing the oldest if full).
    Seen *s = nullptr;
    for (uint8_t k = 0; k < _seen_n; k++) {
      if (_seen[k].lba == start)
        s = &_seen[k];
    }
    if (!s) {
      if (_seen_n < 4) {
        s = &_seen[_seen_n++];
      } else {
        memmove(&_seen[0], &_seen[1], sizeof(Seen) * 3);
        s = &_seen[3];
      }
    }
    s->lba = start;
    s->size = get32(e + 28);
    memcpy(s->name, e, 11);
  }
}

const DragDropEngine::Seen *DragDropEngine::seenAt(uint32_t lba) const {
  for (uint8_t k = 0; k < _seen_n; k++) {
    if (_seen[k].lba == lba)
      return &_seen[k];
  }
  return nullptr;
}

/*!
 * @brief Classify a sector written by the host.
 * @param lba Sector number.
 * @param buf Sector data; must stay valid until process() if true is
 *        returned.
 * @return True if the sector must be handed to process().
 */
bool DragDropEngine::write(uint32_t lba, const uint8_t *buf) {
  if (_state == REMOUNT) {
    return false;
  }
  if (lba >= VirtualFat::ROOT_START && lba < VirtualFat::DATA_START) {
    parseDir(buf);
    _t_dir = millis();
    if (_state == PROGRAMMING)
      _t_last = _t_dir;
    return false;
  }
  if (lba < VirtualFat::DATA_START || lba >= VirtualFat::TOTAL_SECTORS) {
    return false; // boot sector / FAT: nothing to do
  }
  if (_state == IDLE) {
    if (!looksLikeImage(buf)) {
      return false; // not the start of a firmware image
    }
    _state = PROGRAMMING;
    _start_lba = lba;
    _selected = _failed = false;
    _erased = _bytes = 0;
    _held = false;
    _error = nullptr;
    _t_start = millis();
  }
  _t_last = millis();
  if (_failed || lba < _start_lba) {
    return false; // acknowledged, ignored
  }
  _p_lba = lba;
  _p_buf = buf;
  _pending = true;
  return true;
}

void DragDropEngine::fail(const char *error) {
  if (!_failed) {
    _failed = true;
    _error = error;
  }
}

/*! @brief Program the sector handed over by write(). May take seconds. */
void DragDropEngine::process(void) {
  if (!_pending) {
    return;
  }
  _pending = false;

  // First sector: reset the target and halt it before its firmware runs.
  if (!_selected) {
    _selected = true;
    _found = _f4.select();
    if (!_found) {
      fail(_f4.errorString());
    }
  }
  if (!_failed) {
    // Program the previously held sector, then hold this one.
    if (_held) {
      programSector(_hold_lba, _hold);
    }
    memcpy(_hold, _p_buf, VirtualFat::SECTOR_SIZE);
    _hold_lba = _p_lba;
    _held = true;
  }
  _t_last = millis(); // after the (possibly slow) work
}

// Erase (on first touch), program and verify one sector of the image.
void DragDropEngine::programSector(uint32_t lba, const uint8_t *buf) {
  if (_failed) {
    return;
  }
  uint32_t offset = (lba - _start_lba) * VirtualFat::SECTOR_SIZE;
  uint32_t len = VirtualFat::SECTOR_SIZE;
  const Seen *file = seenAt(_start_lba);
  if (file && file->size) {
    if (offset >= file->size) {
      return; // past the end of the file
    }
    if (file->size - offset < len) {
      len = file->size - offset;
    }
  }
  uint8_t sector;
  uint32_t sec_start, sec_size;
  if (offset + len > _f4.flashSizeKB() * 1024U ||
      !Adafruit_DAP_CH32_STM32F4::sectorAt(offset, &sector, &sec_start,
                                           &sec_size)) {
    fail("image is larger than the target's flash");
    return;
  }
  if (!(_erased & (1UL << sector))) {
    _erased |= 1UL << sector;
    if (!_f4.eraseRange(TARGET_FLASH + sec_start, sec_size)) {
      fail(_f4.errorString());
      return;
    }
  }
  // Erased flash reads 0xFF and this sector was erased above, so a chunk of
  // only 0xFF (e.g. the erased tail of a flash dump) needs no programming.
  bool blank = true;
  for (uint32_t i = 0; i < len && blank; i++) {
    blank = (buf[i] == 0xFF);
  }
  if (blank) {
    if (offset + len > _bytes) {
      _bytes = offset + len;
    }
    return;
  }
  uint32_t addr = TARGET_FLASH + offset;
  uint32_t aligned = len & ~3U;
  uint8_t tail[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  memcpy(tail, buf + aligned, len & 3);
  bool ok = true;
  if (aligned) {
    ok = _f4.program(addr, buf, aligned) && _f4.verify(addr, buf, aligned);
  }
  if (ok && (len & 3)) {
    ok = _f4.program(addr + aligned, tail, 4) &&
         _f4.verify(addr + aligned, tail, 4);
  }
  if (!ok) {
    fail(_f4.errorString());
  } else if (offset + len > _bytes) {
    _bytes = offset + len;
  }
}

void DragDropEngine::finish(uint32_t now_ms, bool unrecognised_hex) {
  const Seen *file = seenAt(_start_lba);
  if (_state == PROGRAMMING) {
    if (_held) { // the last sector: the file size is usually known by now
      programSector(_hold_lba, _hold);
      _held = false;
      _t_last = millis();
    }
    if (_selected) {
      if (!_f4.deselect()) {
        fail(_f4.errorString()); // flash lock / reset failed
      }
    }
    _result = _failed ? FAILED : OK;
    _res_error = _error;
    _res_bytes = _bytes;
    _res_ms = _t_last - _t_start;
  } else { // a .bin/.hex that never started a session
    file = nullptr;
    for (uint8_t k = 0; k < _seen_n; k++) {
      if (_seen[k].size)
        file = &_seen[k];
    }
    _result = FAILED;
    _res_error = unrecognised_hex
                     ? "Intel HEX files are not supported yet: use a .bin"
                     : "not recognised as STM32 firmware (no valid vector "
                       "table at the start of the file)";
    _res_bytes = _res_ms = 0;
  }
  if (file) {
    name83(file->name, _res_name);
  } else {
    strcpy(_res_name, "(unknown)");
  }

  _state = REMOUNT; // drive reports "not ready": host re-reads it
  _t_remount = now_ms;
  _pending = _selected = _failed = _held = false;
  _seen_n = 0;
  rebuildFiles();
}

/*!
 * @brief End drops after a write gap and finish remounts. Call often.
 * @param now_ms Current millis().
 */
void DragDropEngine::poll(uint32_t now_ms) {
  if (_state == PROGRAMMING && !_pending && now_ms - _t_last > QUIET_MS) {
    finish(now_ms, false);
  } else if (_state == REMOUNT && now_ms - _t_remount > REMOUNT_MS) {
    _state = IDLE;
  } else if (_state == IDLE && _seen_n && now_ms - _t_dir > UNRECOGNISED_MS) {
    // A firmware-named file was written but never started a session.
    for (uint8_t k = 0; k < _seen_n; k++) {
      const char *ext = _seen[k].name + 8;
      bool bin = memcmp(ext, "BIN", 3) == 0, hex = memcmp(ext, "HEX", 3) == 0;
      if (_seen[k].size && (bin || hex)) {
        finish(now_ms, hex);
        return;
      }
    }
    _seen_n = 0; // other files (notes, OS metadata): ignore
  }
}

uint32_t DragDropEngine::detailsText(void *context, char *buf, uint32_t max) {
  DragDropEngine *self = (DragDropEngine *)context;
  Text t = {buf, max, 0};

  t.str("Adafruit_DAP_CH32 drag-and-drop programmer\r\n\r\n");
  t.str("Probe     : QT Py CH32V203\r\n");
  char serial[32];
  dap_ch32_serial_string(serial);
  t.str("Unique ID : ");
  t.str(serial);
  t.str("\r\n");
  t.str("Interfaces: this drive, USB serial bridge\r\n\r\n");
  if (self->_found) {
    t.str("Target    : ");
    t.str(self->_f4.deviceName());
    t.str("\r\nDEV_ID    : ");
    t.hex32(self->_f4.deviceId() & 0xFFF);
    t.str("\r\nFlash     : ");
    t.dec(self->_f4.flashSizeKB());
    t.str(" KB\r\nSWD DPIDR : ");
    t.hex32(self->_dpidr);
    t.str("\r\n\r\n");
  } else {
    t.str("Target    : none detected (check SWD wiring and target "
          "power)\r\n\r\n");
  }
  t.str("Drop a firmware .bin onto this drive to program the target.\r\n");
  t.str("Last drop : ");
  if (self->_result == NONE) {
    t.str("none yet\r\n");
  } else {
    t.str(self->_res_name);
    if (self->_result == OK) {
      t.str(": OK, ");
      t.dec(self->_res_bytes);
      t.str(" bytes in ");
      t.dec(self->_res_ms / 1000);
      t.str(".");
      t.dec((self->_res_ms % 1000) / 100);
      t.str(" s\r\n");
    } else {
      t.str(": FAILED (see FAIL.TXT)\r\n");
    }
  }
  return t.len;
}

uint32_t DragDropEngine::failText(void *context, char *buf, uint32_t max) {
  DragDropEngine *self = (DragDropEngine *)context;
  Text t = {buf, max, 0};
  t.str("Programming failed\r\n\r\nFile : ");
  t.str(self->_res_name);
  t.str("\r\nError: ");
  t.str(self->_res_error ? self->_res_error : "unknown");
  t.str("\r\n");
  return t.len;
}
