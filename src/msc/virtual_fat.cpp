/*!
 * @file virtual_fat.cpp
 *
 * Generated FAT12 drive
 *
 */

#include "virtual_fat.h"

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

// FAT12: entry n occupies 12 bits starting at byte n * 1.5.
static void setFat12(uint8_t *fat, uint32_t n, uint16_t v) {
  uint32_t off = n * 3 / 2;
  if (n & 1) {
    fat[off] = (uint8_t)((fat[off] & 0x0F) | ((v << 4) & 0xF0));
    fat[off + 1] = (uint8_t)(v >> 4);
  } else {
    fat[off] = (uint8_t)v;
    fat[off + 1] = (uint8_t)((fat[off + 1] & 0xF0) | ((v >> 8) & 0x0F));
  }
}

// FAT date: bits 15-9 year since 1980, 8-5 month, 4-0 day.
static const uint16_t FILE_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1;

void VirtualFat::setVolumeLabel(const char *label) {
  for (uint8_t i = 0; i < 11; i++) {
    char c = *label ? *label++ : ' ';
    _label[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; // upper case
  }
}

bool VirtualFat::addFile(const char *name83, Generator gen, void *context) {
  if (_count >= MAX_FILES) {
    return false;
  }
  memcpy(_files[_count].name, name83, 11);
  _files[_count].gen = gen;
  _files[_count].context = context;
  _count++;
  return true;
}

void VirtualFat::bootSector(uint8_t *buf) {
  buf[0] = 0xEB; // jump to boot code (x86), required by some hosts
  buf[1] = 0x3C;
  buf[2] = 0x90;
  memcpy(buf + 3, "MSWIN4.1", 8);  // OEM name
  put16(buf + 11, SECTOR_SIZE);    // bytes per sector
  buf[13] = SECTORS_PER_CLUSTER;   // sectors per cluster
  put16(buf + 14, FAT_START);      // reserved sectors
  buf[16] = 1;                     // number of FATs
  put16(buf + 17, ROOT_ENTRIES);   // root directory entries
  put16(buf + 19, TOTAL_SECTORS);  // total sectors (16-bit)
  buf[21] = 0xF8;                  // media: fixed disk
  put16(buf + 22, FAT_SECTORS);    // sectors per FAT
  put16(buf + 24, 63);             // sectors per track
  put16(buf + 26, 255);            // heads
  buf[36] = 0x80;                  // drive number
  buf[38] = 0x29;                  // extended boot signature
  put32(buf + 39, 0x0DA4C032);     // volume serial number
  memcpy(buf + 43, _label, 11);    // volume label
  memcpy(buf + 54, "FAT12   ", 8); // file system type
  buf[510] = 0x55;
  buf[511] = 0xAA;
}

void VirtualFat::fatSector(uint32_t index, uint8_t *buf) {
  if (index != 0) {
    return; // all generated files fit in the first FAT sector
  }
  setFat12(buf, 0, 0xFF8); // media descriptor
  setFat12(buf, 1, 0xFFF);
  for (uint8_t i = 0; i < _count; i++) {
    setFat12(buf, 2 + i, 0xFFF); // one cluster each: end of chain
  }
}

void VirtualFat::rootSector(uint32_t index, uint8_t *buf) {
  if (index != 0) {
    return;
  }
  // Run each generator once to learn its size (using buf as scratch space).
  uint32_t sizes[MAX_FILES];
  for (uint8_t i = 0; i < _count; i++) {
    sizes[i] = _files[i].gen(_files[i].context, (char *)buf, SECTOR_SIZE);
    if (sizes[i] > SECTOR_SIZE) {
      sizes[i] = SECTOR_SIZE;
    }
  }
  memset(buf, 0, SECTOR_SIZE);

  uint8_t *e = buf;
  memcpy(e, _label, 11); // volume label entry
  e[11] = 0x08;
  put16(e + 24, FILE_DATE);
  e += 32;
  for (uint8_t i = 0; i < _count; i++, e += 32) {
    memcpy(e, _files[i].name, 11);
    e[11] = 0x01;                     // read-only
    put16(e + 16, FILE_DATE);         // created
    put16(e + 18, FILE_DATE);         // accessed
    put16(e + 24, FILE_DATE);         // modified
    put16(e + 26, (uint16_t)(2 + i)); // first cluster
    put32(e + 28, sizes[i]);          // size in bytes
  }
}

void VirtualFat::readSector(uint32_t lba, uint8_t *buf) {
  memset(buf, 0, SECTOR_SIZE);
  if (lba == 0) {
    bootSector(buf);
  } else if (lba >= FAT_START && lba < ROOT_START) {
    fatSector(lba - FAT_START, buf);
  } else if (lba >= ROOT_START && lba < DATA_START) {
    rootSector(lba - ROOT_START, buf);
  } else if (lba < TOTAL_SECTORS) {
    uint32_t rel = lba - DATA_START;
    uint32_t file = rel / SECTORS_PER_CLUSTER;
    if (file < _count && rel % SECTORS_PER_CLUSTER == 0) {
      uint32_t n =
          _files[file].gen(_files[file].context, (char *)buf, SECTOR_SIZE);
      if (n < SECTOR_SIZE) {
        memset(buf + n, 0, SECTOR_SIZE - n);
      }
    }
  }
}
