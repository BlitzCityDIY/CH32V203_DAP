/*!
 * @file Adafruit_DAP_CH32.h
 *
 * CMSIS-DAP SWD probe for the WCH CH32V203 (QT Py CH32V203).
 *
 * Runs ARM's CMSIS-DAP engine on the CH32V203 and
 * provides SWD access to a Cortex-M target
 *
 */

#ifndef ADAFRUIT_DAP_CH32_H
#define ADAFRUIT_DAP_CH32_H

#include <Arduino.h>

/*!
 * @brief SWD probe: CMSIS-DAP engine plus helpers for a Cortex-M target.
 */
class Adafruit_DAP_CH32 {
public:
  /*! Error codes reported by lastError(). */
  enum Error : uint8_t {
    OK = 0,        //!< no error
    ERR_NO_ACK,    //!< target did not answer (wiring, power, clock)
    ERR_WAIT,      //!< target kept answering WAIT
    ERR_FAULT,     //!< target answered FAULT (sticky error)
    ERR_PROTOCOL,  //!< parity or protocol error
    ERR_MISMATCH,  //!< value match failed
    ERR_TIMEOUT,   //!< handshake or operation timed out
    ERR_DAP,       //!< CMSIS-DAP command rejected
    ERR_NOT_READY, //!< begin() not called or failed
    ERR_ALIGN,     //!< address or length not a multiple of 4
  };

  /*! Debug Port register addresses (A[3:2]). */
  enum DpReg : uint8_t {
    DP_REG_DPIDR = 0x00,     //!< ID register (read)
    DP_REG_ABORT = 0x00,     //!< abort / clear sticky errors (write)
    DP_REG_CTRL_STAT = 0x04, //!< control and status
    DP_REG_SELECT = 0x08,    //!< access port and bank select (write)
    DP_REG_RDBUFF = 0x0C,    //!< read buffer (read)
  };

  /*! MEM-AP register addresses. */
  enum ApReg : uint8_t {
    AP_REG_CSW = 0x00, //!< control and status word
    AP_REG_TAR = 0x04, //!< transfer address
    AP_REG_DRW = 0x0C, //!< data read/write
    AP_REG_IDR = 0xFC, //!< identification register
  };

  bool begin(int swclk = A2, int swdio = A3, int nreset = A0);

  uint32_t execute(const uint8_t *request, uint8_t *response);
  uint32_t executeHost(uint8_t *request, uint16_t len, uint8_t *response);

  /*!
   * @brief Highest SWD clock a host may select through executeHost().
   * @param hz Clock limit in Hz (0 = no limit). Default 1 MHz.
   */
  void setMaxHostClock(uint32_t hz) { _max_host_clock = hz; }
  /*! @return The host SWD clock limit in Hz. */
  uint32_t maxHostClock(void) const { return _max_host_clock; }

  bool setPinSpeed(uint8_t mhz);
  bool setIdleCycles(uint8_t cycles);
  /*! @return Idle cycles inserted after each transfer. */
  uint8_t idleCycles(void) const { return _idle_cycles; }
  bool setClock(uint32_t hz);
  uint32_t measureClock(void);

  bool connect(uint32_t *dpidr = nullptr);
  void disconnect(void);
  bool powerUp(void);

  bool dpRead(uint8_t reg, uint32_t *value);
  bool dpWrite(uint8_t reg, uint32_t value);
  bool apRead(uint8_t ap, uint8_t reg, uint32_t *value);
  bool apWrite(uint8_t ap, uint8_t reg, uint32_t value);

  bool readMem32(uint32_t addr, uint32_t *value);
  bool writeMem32(uint32_t addr, uint32_t value);
  bool readBlock(uint32_t addr, uint8_t *data, uint32_t len);
  bool writeBlock(uint32_t addr, const uint8_t *data, uint32_t len);

  bool haltCore(void);
  bool resumeCore(bool keep_debug = false);
  bool isHalted(bool *halted);
  bool resetAndHalt(void);
  bool resetAndRun(void);
  bool readCoreRegister(uint8_t reg, uint32_t *value);
  bool setReset(bool asserted);

  static uint32_t crc32(const uint8_t *data, uint32_t len, uint32_t crc = 0);

  /*! @return The error of the last failed operation. */
  Error lastError(void) const { return _error; }
  /*! @return Text for lastError(). */
  const char *errorString(void) const { return errorString(_error); }
  static const char *errorString(Error e);

  /*!
   * @brief Raw response byte of the last DAP_Transfer, for diagnostics.
   * @return Bits 2:0 ACK (1 OK, 2 WAIT, 4 FAULT, 7 no answer), bit 3
   *         protocol error, bit 4 value mismatch.
   */
  uint8_t lastTransferResponse(void) const { return _last_resp; }
  /*!
   * @brief Transfers completed by the last DAP_Transfer.
   * @return 1 with an error on a write: the write was accepted but the
   *         follow-up RDBUFF check failed.
   */
  uint8_t lastTransferCount(void) const { return _last_count; }

private:
  bool transfer(uint8_t request, uint32_t *data);
  bool transferBlock(uint8_t request, uint8_t *data, uint32_t words);
  void setTransferError(uint8_t resp);
  bool select(uint8_t ap, uint8_t reg);
  bool executeChecked(const uint8_t *req, uint8_t expect_cmd);
  bool checkReady(void);
  bool connectPort(void);
  bool blockTransfer(uint32_t addr, uint8_t *data, uint32_t len, bool write);

  uint32_t _swdio_shift = 0;
  uint32_t _max_host_clock = 1000000;
  uint32_t _select = 0;
  uint8_t _last_resp = 0;
  uint8_t _last_count = 0;
  uint8_t _idle_cycles = 2;
  uint8_t _pin_mhz = 2;
  bool _ready = false;
  bool _select_valid = false;
  Error _error = ERR_NOT_READY;
  uint8_t _req[64];
  uint8_t _resp[64];
};

#endif
