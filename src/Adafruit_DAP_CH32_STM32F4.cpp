/*!
 * @file Adafruit_DAP_CH32_STM32F4.cpp
 *
 * STM32F4 flash programming over SWD.
 *
 */

#include "Adafruit_DAP_CH32_STM32F4.h"

static const uint32_t DBGMCU_IDCODE = 0xE0042000;
static const uint32_t FLASH_SIZE_REG = 0x1FFF7A20; // KB in bits 31:16

static const uint32_t FLASH_KEYR = 0x40023C04;
static const uint32_t FLASH_SR = 0x40023C0C;
static const uint32_t FLASH_CR = 0x40023C10;

static const uint32_t SR_EOP = 1U << 0;
static const uint32_t SR_OPERR = 1U << 1;
static const uint32_t SR_WRPERR = 1U << 4;
static const uint32_t SR_PGAERR = 1U << 5;
static const uint32_t SR_PGPERR = 1U << 6;
static const uint32_t SR_PGSERR = 1U << 7;
static const uint32_t SR_BSY = 1U << 16;
static const uint32_t SR_ERRORS =
    SR_OPERR | SR_WRPERR | SR_PGAERR | SR_PGPERR | SR_PGSERR;

static const uint32_t CR_PG = 1U << 0;
static const uint32_t CR_SER = 1U << 1;
static const uint32_t CR_MER = 1U << 2;
static const uint32_t CR_SNB_POS = 3;
static const uint32_t CR_PSIZE_X32 = 2U << 8;
static const uint32_t CR_STRT = 1U << 16;
static const uint32_t CR_LOCK = 1U << 31;

static const uint32_t KEY1 = 0x45670123;
static const uint32_t KEY2 = 0xCDEF89AB;

static const struct {
  uint16_t id;
  const char *name;
} devices[] = {
    {0x413, "STM32F405/407/415/417"},
    {0x419, "STM32F42x/43x"},
    {0x423, "STM32F401xB/C"},
    {0x433, "STM32F401xD/E"},
    {0x431, "STM32F411"},
    {0x441, "STM32F412"},
    {0x421, "STM32F446"},
};

/*!
 * @brief Set the probe to use.
 * @param dap Probe; begin() must have been called on it.
 */
void Adafruit_DAP_CH32_STM32F4::begin(Adafruit_DAP_CH32 *dap) { _dap = dap; }

/*!
 * @brief Name for a DBGMCU_IDCODE value.
 * @param idcode DBGMCU_IDCODE (DEV_ID in bits 11:0).
 * @return Device name, or "unknown".
 */
const char *Adafruit_DAP_CH32_STM32F4::nameForId(uint32_t idcode) {
  for (auto &d : devices) {
    if (d.id == (idcode & 0xFFF)) {
      return d.name;
    }
  }
  return "unknown";
}

/*!
 * @brief Flash sector containing an offset (first 1 MB).
 * @param offset Bytes from STM32_FLASH_BASE.
 * @param sector Receives the sector number.
 * @param start Receives the sector's start offset.
 * @param size Receives the sector's size.
 * @return False beyond 1 MB.
 */
bool Adafruit_DAP_CH32_STM32F4::sectorAt(uint32_t offset, uint8_t *sector,
                                         uint32_t *start, uint32_t *size) {
  if (offset < 0x10000) {
    *sector = (uint8_t)(offset / 0x4000);
    *start = *sector * 0x4000U;
    *size = 0x4000;
  } else if (offset < 0x20000) {
    *sector = 4;
    *start = 0x10000;
    *size = 0x10000;
  } else if (offset < 0x100000) {
    *sector = (uint8_t)(5 + (offset - 0x20000) / 0x20000);
    *start = 0x20000 + (*sector - 5) * 0x20000U;
    *size = 0x20000;
  } else {
    return false;
  }
  return true;
}

bool Adafruit_DAP_CH32_STM32F4::fail(FlashError e) {
  _ferr = e;
  return false;
}

// Keeps the probe's error text: later cleanup transfers would overwrite it.
bool Adafruit_DAP_CH32_STM32F4::dapFail(void) {
  _dap_err = _dap->errorString();
  return fail(FE_DAP);
}

/*! @return True if DBGMCU_IDCODE names a known STM32F4; reads flash size. */
bool Adafruit_DAP_CH32_STM32F4::readIds(void) {
  uint32_t size = 0;
  _dev_id = 0;
  _flash_kb = 0;
  if (!_dap->readMem32(DBGMCU_IDCODE, &_dev_id)) {
    return dapFail();
  }
  if (strcmp(nameForId(_dev_id), "unknown") == 0) {
    return fail(FE_UNSUPPORTED);
  }
  if (!_dap->readMem32(FLASH_SIZE_REG, &size)) {
    return dapFail();
  }
  _flash_kb = size >> 16;
  return true;
}

/*!
 * @brief Identify the target without halting or resetting it.
 * @return True if a supported STM32F4 was found.
 */
bool Adafruit_DAP_CH32_STM32F4::identify(void) {
  _ferr = FE_OK;
  if (!_dap->connect() || !_dap->powerUp()) {
    return dapFail();
  }
  return readIds();
}

/*!
 * @brief Connect, reset the chip and halt it before its firmware runs, and
 *        identify it. Required before erase/program/verify.
 * @return True if a supported STM32F4 is halted.
 */
bool Adafruit_DAP_CH32_STM32F4::select(void) {
  _ferr = FE_OK;
  if (!_dap->connect() || !_dap->powerUp() || !_dap->resetAndHalt()) {
    return dapFail();
  }
  return readIds();
}

/*!
 * @brief Lock the flash, reset the chip and let it run.
 * @return True on success.
 */
bool Adafruit_DAP_CH32_STM32F4::deselect(void) {
  _dap->writeMem32(FLASH_CR, CR_LOCK);
  return _dap->resetAndRun() ? true : dapFail();
}

/*!
 * @brief Text for the last error.
 * @return Description.
 */
const char *Adafruit_DAP_CH32_STM32F4::errorString(void) const {
  switch (_ferr) {
  case FE_OK:
    return "OK";
  case FE_DAP:
    return _dap_err;
  case FE_UNSUPPORTED:
    return "not a supported STM32F4";
  case FE_RANGE:
    return "address range outside flash or not 4-byte aligned";
  case FE_LOCKED:
    return "flash unlock rejected";
  case FE_TIMEOUT:
    return "flash stayed busy (timeout)";
  case FE_WRPERR:
    return "flash write protection error (WRPERR)";
  case FE_PGAERR:
    return "flash programming alignment error (PGAERR)";
  case FE_PGPERR:
    return "flash programming parallelism error (PGPERR)";
  case FE_PGSERR:
    return "flash programming sequence error (PGSERR)";
  case FE_OPERR:
    return "flash operation error (OPERR)";
  case FE_VERIFY:
    return "verify failed: flash differs from the image";
  }
  return "unknown";
}

bool Adafruit_DAP_CH32_STM32F4::checkRange(uint32_t addr, uint32_t len) {
  uint32_t limit = _flash_kb * 1024U;
  if (limit > 0x100000) {
    limit = 0x100000; // first bank only
  }
  if (((addr | len) & 3) || addr < STM32_FLASH_BASE ||
      addr - STM32_FLASH_BASE > limit ||
      len > limit - (addr - STM32_FLASH_BASE)) {
    return fail(FE_RANGE);
  }
  return true;
}

bool Adafruit_DAP_CH32_STM32F4::waitIdle(uint32_t timeout_ms) {
  uint32_t t0 = millis();
  do {
    uint32_t sr = 0;
    if (!_dap->readMem32(FLASH_SR, &sr)) {
      return dapFail();
    }
    if (!(sr & SR_BSY)) {
      return true;
    }
    delay(1);
  } while (millis() - t0 < timeout_ms);
  return fail(FE_TIMEOUT);
}

/*! @return True if no flash error flag is set (flags are cleared). */
bool Adafruit_DAP_CH32_STM32F4::checkStatus(void) {
  uint32_t sr = 0;
  if (!_dap->readMem32(FLASH_SR, &sr)) {
    return dapFail();
  }
  if (!(sr & SR_ERRORS)) {
    return true;
  }
  _dap->writeMem32(FLASH_SR, SR_ERRORS | SR_EOP);
  if (sr & SR_WRPERR)
    return fail(FE_WRPERR);
  if (sr & SR_PGAERR)
    return fail(FE_PGAERR);
  if (sr & SR_PGPERR)
    return fail(FE_PGPERR);
  if (sr & SR_PGSERR)
    return fail(FE_PGSERR);
  return fail(FE_OPERR);
}

/*!
 * @brief Unlock the flash, clear stale errors, wait until idle and write CR.
 * @param cr Value for FLASH_CR.
 * @return True on success.
 */
bool Adafruit_DAP_CH32_STM32F4::start(uint32_t cr) {
  uint32_t v = 0;
  if (!_dap->readMem32(FLASH_CR, &v)) {
    return dapFail();
  }
  if (v & CR_LOCK) {
    if (!_dap->writeMem32(FLASH_KEYR, KEY1) ||
        !_dap->writeMem32(FLASH_KEYR, KEY2) || !_dap->readMem32(FLASH_CR, &v)) {
      return dapFail();
    }
    if (v & CR_LOCK) {
      return fail(FE_LOCKED);
    }
  }
  if (!_dap->writeMem32(FLASH_SR, SR_ERRORS | SR_EOP)) {
    return dapFail();
  }
  if (!waitIdle(100)) {
    return false;
  }
  return _dap->writeMem32(FLASH_CR, cr) ? true : dapFail();
}

/*!
 * @brief End a flash operation: clear CR and lock the flash.
 * @param ok Result of the operation.
 * @return ok (false if locking failed).
 */
bool Adafruit_DAP_CH32_STM32F4::finish(bool ok) {
  _dap->writeMem32(FLASH_CR, 0);
  if (!_dap->writeMem32(FLASH_CR, CR_LOCK)) {
    return ok ? dapFail() : false;
  }
  return ok;
}

/*!
 * @brief Erase the whole flash (blocks up to ~16 s on a 1 MB part).
 * @return True on success.
 */
bool Adafruit_DAP_CH32_STM32F4::massErase(void) {
  _ferr = FE_OK;
  if (!start(CR_MER | CR_PSIZE_X32)) {
    return finish(false);
  }
  bool ok = _dap->writeMem32(FLASH_CR, CR_MER | CR_PSIZE_X32 | CR_STRT)
                ? (waitIdle(40000) && checkStatus())
                : dapFail();
  return finish(ok);
}

/*!
 * @brief Erase the sectors covering a range.
 * @param addr Start address.
 * @param len Length in bytes.
 * @param cb Optional progress callback.
 * @return True on success.
 */
bool Adafruit_DAP_CH32_STM32F4::eraseRange(uint32_t addr, uint32_t len,
                                           ProgressCallback cb) {
  _ferr = FE_OK;
  if (len == 0) {
    return true;
  }
  if (!checkRange(addr, len)) {
    return false;
  }
  if (!start(0)) {
    return finish(false);
  }
  uint32_t offset = addr - STM32_FLASH_BASE, end = offset + len, done = 0;
  while (offset < end) {
    uint8_t sector;
    uint32_t sec_start, sec_size;
    if (!sectorAt(offset, &sector, &sec_start, &sec_size)) {
      return finish(fail(FE_RANGE));
    }
    uint32_t cr = CR_SER | ((uint32_t)sector << CR_SNB_POS) | CR_PSIZE_X32;
    if (!_dap->writeMem32(FLASH_CR, cr) ||
        !_dap->writeMem32(FLASH_CR, cr | CR_STRT)) {
      return finish(dapFail());
    }
    // 128 KB sectors take up to ~4 s, 16 KB sectors up to ~0.5 s.
    if (!waitIdle(sec_size >= 0x20000 ? 6000 : 2000) || !checkStatus()) {
      return finish(false);
    }
    uint32_t next = sec_start + sec_size;
    done += (next > end ? end : next) - offset;
    offset = next;
    if (cb) {
      cb("erase", done, len);
    }
  }
  return finish(true);
}

/*!
 * @brief Program erased flash.
 * @param addr Start address (multiple of 4).
 * @param data Data (may be in this chip's flash).
 * @param len Length in bytes (multiple of 4).
 * @param cb Optional progress callback (every 1 KB).
 * @return True on success.
 */
bool Adafruit_DAP_CH32_STM32F4::program(uint32_t addr, const uint8_t *data,
                                        uint32_t len, ProgressCallback cb) {
  _ferr = FE_OK;
  if (!checkRange(addr, len)) {
    return false;
  }
  if (!start(CR_PG | CR_PSIZE_X32)) {
    return finish(false);
  }
  for (uint32_t done = 0; done < len;) {
    uint32_t n = len - done < 1024 ? len - done : 1024;
    if (!_dap->writeBlock(addr + done, data + done, n)) {
      return finish(dapFail());
    }
    if (!waitIdle(100) || !checkStatus()) {
      return finish(false);
    }
    done += n;
    if (cb) {
      cb("program", done, len);
    }
  }
  return finish(true);
}

/*!
 * @brief Compare flash with data.
 * @param addr Start address (multiple of 4).
 * @param data Expected data.
 * @param len Length in bytes (multiple of 4).
 * @param bad_addr Optional: receives the first differing word's address.
 * @param cb Optional progress callback (every 1 KB).
 * @return True if flash matches.
 */
bool Adafruit_DAP_CH32_STM32F4::verify(uint32_t addr, const uint8_t *data,
                                       uint32_t len, uint32_t *bad_addr,
                                       ProgressCallback cb) {
  _ferr = FE_OK;
  if (!checkRange(addr, len)) {
    return false;
  }
  uint8_t buf[240]; // four DAP_TransferBlock reads per readBlock()
  for (uint32_t done = 0; done < len;) {
    uint32_t n = len - done < sizeof(buf) ? len - done : sizeof(buf);
    if (!_dap->readBlock(addr + done, buf, n)) {
      return dapFail();
    }
    for (uint32_t i = 0; i < n; i += 4) {
      if (memcmp(&buf[i], data + done + i, 4) != 0) {
        if (bad_addr) {
          *bad_addr = addr + done + i;
        }
        return fail(FE_VERIFY);
      }
    }
    uint32_t before = done;
    done += n;
    if (cb && ((before >> 10) != (done >> 10) || done == len)) {
      cb("verify", done, len);
    }
  }
  return true;
}
