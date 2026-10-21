/*!
 * @file Adafruit_DAP_CH32_STM32F4.h
 *
 * STM32F4 flash programming over SWD with Adafruit_DAP_CH32.
 *
 */

#ifndef ADAFRUIT_DAP_CH32_STM32F4_H
#define ADAFRUIT_DAP_CH32_STM32F4_H

#include "Adafruit_DAP_CH32.h"

/*!
 * @brief Identify, erase, program and verify an STM32F4's flash.
 */
class Adafruit_DAP_CH32_STM32F4 {
public:
  /*! STM32F4 flash base address (FLASH_BASE is a CH32 core macro). */
  static const uint32_t STM32_FLASH_BASE = 0x08000000;

  /*!
   * @brief Progress callback.
   * @param stage "erase", "program" or "verify".
   * @param done Bytes done.
   * @param total Bytes in total.
   */
  typedef void (*ProgressCallback)(const char *stage, uint32_t done,
                                   uint32_t total);

  void begin(Adafruit_DAP_CH32 *dap);
  bool identify(void);
  bool select(void);
  bool deselect(void);

  /*! @return DBGMCU_IDCODE (DEV_ID in bits 11:0). */
  uint32_t deviceId(void) const { return _dev_id; }
  /*! @return Device name, or "unknown". */
  const char *deviceName(void) const { return nameForId(_dev_id); }
  /*! @return Flash size in KB. */
  uint32_t flashSizeKB(void) const { return _flash_kb; }

  bool massErase(void);
  bool eraseRange(uint32_t addr, uint32_t len, ProgressCallback cb = nullptr);
  bool program(uint32_t addr, const uint8_t *data, uint32_t len,
               ProgressCallback cb = nullptr);
  bool verify(uint32_t addr, const uint8_t *data, uint32_t len,
              uint32_t *bad_addr = nullptr, ProgressCallback cb = nullptr);

  const char *errorString(void) const;

  static const char *nameForId(uint32_t idcode);
  static bool sectorAt(uint32_t offset, uint8_t *sector, uint32_t *start,
                       uint32_t *size);

private:
  // FE_ prefix: F_OK is a POSIX macro (unistd.h).
  enum FlashError : uint8_t {
    FE_OK = 0,
    FE_DAP,         // SWD error, see Adafruit_DAP_CH32::errorString()
    FE_UNSUPPORTED, // not a known STM32F4
    FE_RANGE,       // outside flash or not 4-byte aligned
    FE_LOCKED,      // unlock keys rejected
    FE_TIMEOUT,     // flash stayed busy
    FE_WRPERR,      // write protection
    FE_PGAERR,      // programming alignment
    FE_PGPERR,      // programming parallelism
    FE_PGSERR,      // programming sequence
    FE_OPERR,       // operation error
    FE_VERIFY,      // read back differs
  };

  bool fail(FlashError e);
  bool dapFail(void);
  bool readIds(void);
  bool checkRange(uint32_t addr, uint32_t len);
  bool start(uint32_t cr);
  bool finish(bool ok);
  bool waitIdle(uint32_t timeout_ms);
  bool checkStatus(void);

  Adafruit_DAP_CH32 *_dap = nullptr;
  uint32_t _dev_id = 0;
  uint32_t _flash_kb = 0;
  FlashError _ferr = FE_OK;
  const char *_dap_err = nullptr; // probe error text at the time of failure
};

#endif
