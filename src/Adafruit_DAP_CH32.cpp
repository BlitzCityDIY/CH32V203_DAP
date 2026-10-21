/*!
 * @file Adafruit_DAP_CH32.cpp
 *
 * CMSIS-DAP SWD probe for the WCH CH32V203.
 *
 * MIT license. The CMSIS-DAP engine in src/engine is BSD-3 (Adafruit_DAP).
 */

#include "Adafruit_DAP_CH32.h"

#include "engine/dap.h"
#include "engine/dap_ch32_platform.h"

// CMSIS-DAP command IDs and response codes (CMSIS-DAP specification)
static const uint8_t ID_DAP_Connect = 0x02;
static const uint8_t ID_DAP_Disconnect = 0x03;
static const uint8_t ID_DAP_TransferConfigure = 0x04;
static const uint8_t ID_DAP_Transfer = 0x05;
static const uint8_t ID_DAP_TransferBlock = 0x06;
static const uint8_t ID_DAP_SWJ_Pins = 0x10;
static const uint8_t ID_DAP_SWJ_Clock = 0x11;
static const uint8_t ID_DAP_SWJ_Sequence = 0x12;
static const uint8_t DAP_OK = 0x00;
static const uint8_t DAP_PORT_SWD = 0x01;
static const uint8_t DAP_TRANSFER_OK = 0x01;
static const uint8_t DAP_TRANSFER_WAIT = 0x02;
static const uint8_t DAP_TRANSFER_FAULT = 0x04;
static const uint8_t DAP_TRANSFER_ERROR = 0x08;
static const uint8_t DAP_TRANSFER_MISMATCH = 0x10;
static const uint8_t DAP_SWJ_nRESET = 7; // bit number in DAP_SWJ_Pins

// DAP_Transfer request bits
static const uint8_t REQ_APnDP = 0x01;
static const uint8_t REQ_RnW = 0x02;

// MEM-AP CSW: 32-bit transfers, HPROT = 0x23, with/without auto-increment
static const uint32_t CSW_WORD = 0x23000002;
static const uint32_t CSW_WORD_INC = 0x23000012;

// Words per DAP_TransferBlock packet (64-byte packets)
static const uint32_t BLOCK_WRITE_WORDS = (64 - 5) / 4;
static const uint32_t BLOCK_READ_WORDS = (64 - 4) / 4;

// Cortex-M debug registers (ARMv6-M / ARMv7-M / ARMv8-M)
static const uint32_t DHCSR = 0xE000EDF0;
static const uint32_t DCRSR = 0xE000EDF4;
static const uint32_t DCRDR = 0xE000EDF8;
static const uint32_t DEMCR = 0xE000EDFC;
static const uint32_t AIRCR = 0xE000ED0C;
static const uint32_t DBGKEY = 0xA05F0000;
static const uint32_t C_DEBUGEN = 1U << 0;
static const uint32_t C_HALT = 1U << 1;
static const uint32_t S_REGRDY = 1U << 16;
static const uint32_t S_HALT = 1U << 17;
static const uint32_t VC_CORERESET = 1U << 0;
static const uint32_t AIRCR_SYSRESETREQ = 0x05FA0004; // VECTKEY | SYSRESETREQ

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/*!
 * @brief Set up the SWD pins and the CMSIS-DAP engine.
 * @param swclk SWCLK pin (Arduino pin number).
 * @param swdio SWDIO pin.
 * @param nreset nRESET pin, or -1 if not connected.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::begin(int swclk, int swdio, int nreset) {
  _ready = false;
  _error = ERR_NOT_READY;

  GPIO_TypeDef *clk_port = digitalPinToPort(swclk);
  GPIO_TypeDef *dio_port = digitalPinToPort(swdio);
  if (!clk_port || !dio_port) {
    return false;
  }

  // Let the core enable the port clocks, then drive the registers directly.
  pinMode(swclk, INPUT);
  pinMode(swdio, INPUT);

  dap_ch32_pins.swclk_port = clk_port;
  dap_ch32_pins.swclk_mask = digitalPinToBitMask(swclk);
  dap_ch32_pins.swdio_port = dio_port;
  dap_ch32_pins.swdio_mask = digitalPinToBitMask(swdio);

  uint32_t index = (uint32_t)__builtin_ctz(dap_ch32_pins.swdio_mask);
  _swdio_shift = (index & 7U) * 4U;
  dap_ch32_pins.swdio_cfg = (index < 8U) ? &dio_port->CFGLR : &dio_port->CFGHR;
  dap_ch32_pins.swdio_cfg_keep = ~(0xFU << _swdio_shift);
  dap_ch32_pins.swdio_cfg_in = DAP_CH32_CFG_IN_PULL << _swdio_shift;
  setPinSpeed(_pin_mhz);

  dap_ch32_pins.nreset_port = nullptr;
  dap_ch32_pins.nreset_mask = 0;
  if (nreset >= 0) {
    GPIO_TypeDef *rst_port = digitalPinToPort(nreset);
    if (rst_port) {
      pinMode(nreset, INPUT);
      dap_ch32_pins.nreset_port = rst_port;
      dap_ch32_pins.nreset_mask = digitalPinToBitMask(nreset);
    }
  }

  dap_ch32_serial_string(dap_ch32_serial);
  dap_init(swclk, swdio, nreset); // pins high-Z, nRESET released

  _ready = true;
  _error = OK;
  _select_valid = false;
  return true;
}

/*!
 * @brief Set the SWCLK/SWDIO output edge speed (CH32 GPIO output mode).
 * @param mhz 2 (default, slowest edges), 10 or 50. Slow edges avoid
 *        ringing on jumper wires; see README.
 * @return False for an unsupported value. Applies at the next connect().
 */
bool Adafruit_DAP_CH32::setPinSpeed(uint8_t mhz) {
  uint32_t mode;
  switch (mhz) {
  case 2:
    mode = DAP_CH32_CFG_OUT_PP_2M;
    break;
  case 10:
    mode = DAP_CH32_CFG_OUT_PP_10M;
    break;
  case 50:
    mode = DAP_CH32_CFG_OUT_PP;
    break;
  default:
    return false;
  }
  _pin_mhz = mhz;
  dap_ch32_pins.out_mode = mode;
  dap_ch32_pins.swdio_cfg_out = mode << _swdio_shift;
  return true;
}

/*!
 * @brief Set the idle cycles inserted after each transfer.
 * @param cycles Idle cycles. Default 2: an STM32F4 misreads a request that
 *        follows an access port write with fewer.
 * @return True on success. Applies now and at every connect().
 */
bool Adafruit_DAP_CH32::setIdleCycles(uint8_t cycles) {
  _idle_cycles = cycles;
  if (!_ready) {
    return true;
  }
  // idle cycles, WAIT retries (u16), match retries (u16)
  const uint8_t cfg[] = {ID_DAP_TransferConfigure, cycles, 100, 0, 0, 0};
  return executeChecked(cfg, ID_DAP_TransferConfigure);
}

/*!
 * @brief Run one raw CMSIS-DAP command.
 * @param request Command packet.
 * @param response Response buffer (64 bytes).
 * @return Response length in bytes (always a whole 64-byte packet).
 */
uint32_t Adafruit_DAP_CH32::execute(const uint8_t *request, uint8_t *response) {
  // The engine does not report the response length: whole packets.
  dap_process_request(const_cast<uint8_t *>(request), response);
  return sizeof(_resp);
}

/*!
 * @brief Run a command from an external host, applying probe policy.
 *
 * DAP_TransferConfigure asking for fewer idle cycles than idleCycles() is
 * raised to it, and DAP_SWJ_Clock above maxHostClock() is lowered to it
 * (a CMSIS-DAP clock request is an upper limit). Cached SELECT state is
 * dropped since the host may change it.
 *
 * @param request Command packet (may be modified).
 * @param len Packet length.
 * @param response Response buffer (64 bytes).
 * @return Response length in bytes.
 */
uint32_t Adafruit_DAP_CH32::executeHost(uint8_t *request, uint16_t len,
                                        uint8_t *response) {
  if (len >= 2 && request[0] == ID_DAP_TransferConfigure &&
      request[1] < _idle_cycles) {
    request[1] = _idle_cycles;
  }
  if (len >= 5 && request[0] == ID_DAP_SWJ_Clock && _max_host_clock &&
      get32(&request[1]) > _max_host_clock) {
    put32(&request[1], _max_host_clock);
  }
  _select_valid = false;
  return execute(request, response);
}

/*!
 * @brief Run a command whose response is [cmd, status].
 * @return True if the response is for expect_cmd with status DAP_OK.
 */
bool Adafruit_DAP_CH32::executeChecked(const uint8_t *req, uint8_t expect_cmd) {
  execute(req, _resp);
  if (_resp[0] != expect_cmd || _resp[1] != DAP_OK) {
    _error = ERR_DAP;
    return false;
  }
  return true;
}

/*! @return True if begin() succeeded; else sets ERR_NOT_READY. */
bool Adafruit_DAP_CH32::checkReady(void) {
  if (!_ready) {
    _error = ERR_NOT_READY;
  }
  return _ready;
}

/*! @return True if the engine switched its pins to SWD mode. */
bool Adafruit_DAP_CH32::connectPort(void) {
  _req[0] = ID_DAP_Connect;
  _req[1] = DAP_PORT_SWD;
  execute(_req, _resp);
  _select_valid = false;
  if (_resp[1] != DAP_PORT_SWD) {
    _error = ERR_DAP;
    return false;
  }
  return true;
}

/*!
 * @brief Set the SWD clock.
 * @param hz Clock in Hz.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::setClock(uint32_t hz) {
  if (!checkReady()) {
    return false;
  }
  _req[0] = ID_DAP_SWJ_Clock;
  put32(&_req[1], hz);
  return executeChecked(_req, ID_DAP_SWJ_Clock);
}

/*!
 * @brief Measure the actual SWCLK frequency over 16384 clocks.
 * @return Frequency in Hz, or 0 on error. The target needs connect() after.
 */
uint32_t Adafruit_DAP_CH32::measureClock(void) {
  if (!checkReady() || !connectPort()) {
    return 0;
  }
  // 128 one bits per command: a long line reset, harmless to the target.
  _req[0] = ID_DAP_SWJ_Sequence;
  _req[1] = 128;
  memset(&_req[2], 0xFF, 16);
  const uint32_t commands = 128;
  uint32_t t0 = dap_ch32_micros();
  for (uint32_t i = 0; i < commands; i++) {
    execute(_req, _resp);
  }
  uint32_t dt = dap_ch32_micros() - t0;
  return dt ? (uint32_t)((uint64_t)commands * 128U * 1000000ULL / dt) : 0;
}

/*!
 * @brief Connect: line reset, JTAG-to-SWD switch, read DPIDR, clear errors.
 * @param dpidr Optional: receives the DPIDR value.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::connect(uint32_t *dpidr) {
  if (!checkReady() || !connectPort() || !setIdleCycles(_idle_cycles)) {
    return false;
  }
  const uint8_t line_reset[] = {
      ID_DAP_SWJ_Sequence, 56, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t jtag_to_swd[] = {ID_DAP_SWJ_Sequence, 16, 0x9E, 0xE7};
  const uint8_t idle[] = {ID_DAP_SWJ_Sequence, 8, 0x00};
  if (!executeChecked(line_reset, ID_DAP_SWJ_Sequence) ||
      !executeChecked(jtag_to_swd, ID_DAP_SWJ_Sequence) ||
      !executeChecked(line_reset, ID_DAP_SWJ_Sequence) ||
      !executeChecked(idle, ID_DAP_SWJ_Sequence)) {
    return false;
  }
  uint32_t id = 0;
  if (!dpRead(DP_REG_DPIDR, &id)) {
    return false;
  }
  if (dpidr) {
    *dpidr = id;
  }
  return dpWrite(DP_REG_ABORT, 0x1E); // clear sticky errors
}

/*! @brief Release the SWD pins (high impedance). */
void Adafruit_DAP_CH32::disconnect(void) {
  if (!_ready) {
    return;
  }
  _req[0] = ID_DAP_Disconnect;
  execute(_req, _resp);
  _select_valid = false;
}

/*!
 * @brief Power up the debug and system domains.
 * @return True once both power-up requests are acknowledged.
 */
bool Adafruit_DAP_CH32::powerUp(void) {
  const uint32_t REQ = 0x50000000;  // CSYSPWRUPREQ | CDBGPWRUPREQ
  const uint32_t ACKS = 0xA0000000; // CSYSPWRUPACK | CDBGPWRUPACK
  if (!dpWrite(DP_REG_SELECT, 0) || !dpWrite(DP_REG_CTRL_STAT, REQ)) {
    return false;
  }
  for (int i = 0; i < 100; i++) {
    uint32_t stat = 0;
    if (!dpRead(DP_REG_CTRL_STAT, &stat)) {
      return false;
    }
    if ((stat & ACKS) == ACKS) {
      return true;
    }
    delay(1);
  }
  _error = ERR_TIMEOUT;
  return false;
}

/*! @brief Map a failed transfer's response byte to an Error. */
void Adafruit_DAP_CH32::setTransferError(uint8_t resp) {
  uint8_t ack = resp & 0x07;
  if (resp & DAP_TRANSFER_ERROR) {
    _error = ERR_PROTOCOL;
  } else if (resp & DAP_TRANSFER_MISMATCH) {
    _error = ERR_MISMATCH;
  } else if (ack == DAP_TRANSFER_WAIT) {
    _error = ERR_WAIT;
  } else if (ack == DAP_TRANSFER_FAULT) {
    _error = ERR_FAULT;
  } else {
    _error = ERR_NO_ACK;
  }
}

/*!
 * @brief One DAP_Transfer.
 * @param request Transfer request (APnDP, RnW, A[3:2]).
 * @param data Value to write, or receives the value read.
 * @return True on an OK ACK.
 */
bool Adafruit_DAP_CH32::transfer(uint8_t request, uint32_t *data) {
  if (!checkReady()) {
    return false;
  }
  _req[0] = ID_DAP_Transfer;
  _req[1] = 0; // DAP index
  _req[2] = 1; // transfer count
  _req[3] = request;
  if (!(request & REQ_RnW)) {
    put32(&_req[4], *data);
  }
  execute(_req, _resp);

  uint8_t resp = _resp[2];
  _last_resp = resp;
  _last_count = _resp[1];
  if (_resp[0] == ID_DAP_Transfer && _resp[1] == 1 &&
      (resp & 0x07) == DAP_TRANSFER_OK &&
      !(resp & (DAP_TRANSFER_ERROR | DAP_TRANSFER_MISMATCH))) {
    if (request & REQ_RnW) {
      *data = get32(&_resp[3]);
    }
    _error = OK;
    return true;
  }
  setTransferError(resp);
  return false;
}

/*!
 * @brief One DAP_TransferBlock to a single register.
 * @param request Transfer request.
 * @param data Words to write, or receives the words read.
 * @param words Number of words (packet limits: 14 write, 15 read).
 * @return True if all words transferred.
 */
bool Adafruit_DAP_CH32::transferBlock(uint8_t request, uint8_t *data,
                                      uint32_t words) {
  const bool write = !(request & REQ_RnW);
  _req[0] = ID_DAP_TransferBlock;
  _req[1] = 0;
  _req[2] = (uint8_t)words;
  _req[3] = (uint8_t)(words >> 8);
  _req[4] = request;
  if (write) {
    memcpy(&_req[5], data, words * 4);
  }
  execute(_req, _resp);

  uint32_t done = (uint32_t)_resp[1] | ((uint32_t)_resp[2] << 8);
  uint8_t resp = _resp[3];
  _last_resp = resp;
  _last_count = (uint8_t)done;
  if (_resp[0] == ID_DAP_TransferBlock && done == words &&
      (resp & 0x07) == DAP_TRANSFER_OK && !(resp & DAP_TRANSFER_ERROR)) {
    if (!write) {
      memcpy(data, &_resp[4], words * 4);
    }
    _error = OK;
    return true;
  }
  setTransferError(resp);
  return false;
}

/*!
 * @brief Read a Debug Port register.
 * @param reg Register (DP_*).
 * @param value Receives the value.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::dpRead(uint8_t reg, uint32_t *value) {
  return transfer(REQ_RnW | (reg & 0x0C), value);
}

/*!
 * @brief Write a Debug Port register.
 * @param reg Register (DP_*).
 * @param value Value to write.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::dpWrite(uint8_t reg, uint32_t value) {
  if ((reg & 0x0C) != DP_REG_SELECT) {
    return transfer(reg & 0x0C, &value);
  }
  _select = value;
  _select_valid = transfer(reg & 0x0C, &value);
  return _select_valid;
}

/*! @return True once SELECT addresses the AP and bank of reg. */
bool Adafruit_DAP_CH32::select(uint8_t ap, uint8_t reg) {
  uint32_t want = ((uint32_t)ap << 24) | (reg & 0xF0);
  return (_select_valid && _select == want) || dpWrite(DP_REG_SELECT, want);
}

/*!
 * @brief Read an Access Port register.
 * @param ap Access port number.
 * @param reg Register (AP_*).
 * @param value Receives the value.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::apRead(uint8_t ap, uint8_t reg, uint32_t *value) {
  return select(ap, reg) && transfer(REQ_APnDP | REQ_RnW | (reg & 0x0C), value);
}

/*!
 * @brief Write an Access Port register.
 * @param ap Access port number.
 * @param reg Register (AP_*).
 * @param value Value to write.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::apWrite(uint8_t ap, uint8_t reg, uint32_t value) {
  return select(ap, reg) && transfer(REQ_APnDP | (reg & 0x0C), &value);
}

/*!
 * @brief Read a 32-bit word through MEM-AP 0.
 * @param addr Target address.
 * @param value Receives the word.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::readMem32(uint32_t addr, uint32_t *value) {
  return apWrite(0, AP_REG_CSW, CSW_WORD) && apWrite(0, AP_REG_TAR, addr) &&
         apRead(0, AP_REG_DRW, value);
}

/*!
 * @brief Write a 32-bit word through MEM-AP 0.
 * @param addr Target address.
 * @param value Word to write.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::writeMem32(uint32_t addr, uint32_t value) {
  return apWrite(0, AP_REG_CSW, CSW_WORD) && apWrite(0, AP_REG_TAR, addr) &&
         apWrite(0, AP_REG_DRW, value);
}

/*!
 * @brief Block transfer through MEM-AP 0 with address auto-increment.
 *
 * TAR is reloaded at every 1 KB boundary, where MEM-AP auto-increment
 * wraps.
 */
bool Adafruit_DAP_CH32::blockTransfer(uint32_t addr, uint8_t *data,
                                      uint32_t len, bool write) {
  if ((addr | len) & 3) {
    _error = ERR_ALIGN;
    return false;
  }
  if (!apWrite(0, AP_REG_CSW, CSW_WORD_INC)) {
    return false;
  }
  const uint32_t max = (write ? BLOCK_WRITE_WORDS : BLOCK_READ_WORDS) * 4;
  const uint8_t request = REQ_APnDP | AP_REG_DRW | (write ? 0 : REQ_RnW);
  bool tar_valid = false;
  while (len) {
    if (!tar_valid || (addr & 0x3FF) == 0) {
      if (!apWrite(0, AP_REG_TAR, addr)) {
        return false;
      }
      tar_valid = true;
    }
    uint32_t n = 0x400 - (addr & 0x3FF);
    if (n > max)
      n = max;
    if (n > len)
      n = len;
    if (!transferBlock(request, data, n / 4)) {
      return false;
    }
    addr += n;
    data += n;
    len -= n;
  }
  return true;
}

/*!
 * @brief Read a block of words through MEM-AP 0.
 * @param addr Target address (multiple of 4).
 * @param data Receives the data.
 * @param len Length in bytes (multiple of 4).
 * @return True on success.
 */
bool Adafruit_DAP_CH32::readBlock(uint32_t addr, uint8_t *data, uint32_t len) {
  return blockTransfer(addr, data, len, false);
}

/*!
 * @brief Write a block of words through MEM-AP 0.
 * @param addr Target address (multiple of 4).
 * @param data Data to write (may be in this chip's flash).
 * @param len Length in bytes (multiple of 4).
 * @return True on success.
 */
bool Adafruit_DAP_CH32::writeBlock(uint32_t addr, const uint8_t *data,
                                   uint32_t len) {
  // Only read when writing (copied into the command packet).
  return blockTransfer(addr, const_cast<uint8_t *>(data), len, true);
}

/*!
 * @brief CRC-32 (zlib polynomial).
 * @param data Data.
 * @param len Length in bytes.
 * @param crc Previous result, to chain calls.
 * @return The CRC.
 */
uint32_t Adafruit_DAP_CH32::crc32(const uint8_t *data, uint32_t len,
                                  uint32_t crc) {
  crc = ~crc;
  while (len--) {
    crc ^= *data++;
    for (int i = 0; i < 8; i++) {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

/*!
 * @brief Check whether the core is halted.
 * @param halted Receives the state.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::isHalted(bool *halted) {
  uint32_t v = 0;
  if (!readMem32(DHCSR, &v)) {
    return false;
  }
  *halted = (v & S_HALT) != 0;
  return true;
}

/*! @return True once the core reports halted. */
bool Adafruit_DAP_CH32::haltCore(void) {
  if (!writeMem32(DHCSR, DBGKEY | C_DEBUGEN | C_HALT)) {
    return false;
  }
  for (int i = 0; i < 100; i++) {
    bool halted = false;
    if (!isHalted(&halted)) {
      return false;
    }
    if (halted) {
      return true;
    }
    delay(1);
  }
  _error = ERR_TIMEOUT;
  return false;
}

/*!
 * @brief Let the core run.
 * @param keep_debug True to stay in debug mode (halt again later).
 * @return True on success.
 */
bool Adafruit_DAP_CH32::resumeCore(bool keep_debug) {
  return writeMem32(DHCSR, DBGKEY | C_DEBUGEN) &&
         (keep_debug || writeMem32(DHCSR, DBGKEY));
}

/*!
 * @brief Reset the chip and halt it at its first instruction.
 *
 * DHCSR halt, DEMCR reset vector catch, AIRCR system reset. The reset may
 * cut off the AIRCR write and disturb the link, which is re-synced.
 *
 * @return True once halted after the reset.
 */
bool Adafruit_DAP_CH32::resetAndHalt(void) {
  uint32_t demcr = 0;
  if (!writeMem32(DHCSR, DBGKEY | C_DEBUGEN | C_HALT) ||
      !readMem32(DEMCR, &demcr) || !writeMem32(DEMCR, demcr | VC_CORERESET)) {
    return false;
  }
  writeMem32(AIRCR, AIRCR_SYSRESETREQ);
  delay(10);
  for (int i = 0; i < 100; i++) {
    bool halted = false;
    if (!isHalted(&halted)) {
      connect();
      powerUp();
      continue;
    }
    if (halted) {
      return readMem32(DEMCR, &demcr) &&
             writeMem32(DEMCR, demcr & ~VC_CORERESET);
    }
    delay(1);
  }
  _error = ERR_TIMEOUT;
  return false;
}

/*!
 * @brief Reset the chip and let it run, leaving debug mode.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::resetAndRun(void) {
  uint32_t demcr = 0;
  if (!readMem32(DEMCR, &demcr) || !writeMem32(DEMCR, demcr & ~VC_CORERESET) ||
      !writeMem32(DHCSR, DBGKEY | C_DEBUGEN)) {
    return false;
  }
  writeMem32(AIRCR, AIRCR_SYSRESETREQ);
  delay(10);
  return writeMem32(DHCSR, DBGKEY) ||
         (connect() && powerUp() && writeMem32(DHCSR, DBGKEY));
}

/*!
 * @brief Read a core register of the halted core.
 * @param reg 0-12 = R0-R12, 13 = SP, 14 = LR, 15 = PC, 16 = xPSR.
 * @param value Receives the value.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::readCoreRegister(uint8_t reg, uint32_t *value) {
  if (!writeMem32(DCRSR, reg & 0x7F)) {
    return false;
  }
  for (int i = 0; i < 100; i++) {
    uint32_t v = 0;
    if (!readMem32(DHCSR, &v)) {
      return false;
    }
    if (v & S_REGRDY) {
      return readMem32(DCRDR, value);
    }
  }
  _error = ERR_TIMEOUT;
  return false;
}

/*!
 * @brief Drive the target's nRESET pin.
 * @param asserted True to hold the target in reset.
 * @return True on success.
 */
bool Adafruit_DAP_CH32::setReset(bool asserted) {
  if (!checkReady()) {
    return false;
  }
  const uint8_t nreset_bit = 1U << DAP_SWJ_nRESET;
  _req[0] = ID_DAP_SWJ_Pins;
  _req[1] = asserted ? 0 : nreset_bit;
  _req[2] = nreset_bit;
  put32(&_req[3], 0);
  execute(_req, _resp);
  _error = OK;
  return true;
}

/*!
 * @brief Text for an error code.
 * @param e Error.
 * @return Description.
 */
const char *Adafruit_DAP_CH32::errorString(Error e) {
  switch (e) {
  case OK:
    return "OK";
  case ERR_NO_ACK:
    return "no ACK from target (check wiring, GND, target power, clock)";
  case ERR_WAIT:
    return "target kept answering WAIT";
  case ERR_FAULT:
    return "target answered FAULT";
  case ERR_PROTOCOL:
    return "SWD parity/protocol error (signal integrity or clock too fast?)";
  case ERR_MISMATCH:
    return "value mismatch";
  case ERR_TIMEOUT:
    return "operation timed out";
  case ERR_DAP:
    return "CMSIS-DAP command rejected";
  case ERR_ALIGN:
    return "address or length not a multiple of 4";
  case ERR_NOT_READY:
    return "begin() not called or failed";
  }
  return "unknown";
}
