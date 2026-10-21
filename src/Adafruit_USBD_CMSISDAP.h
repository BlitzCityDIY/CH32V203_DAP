/*!
 * @file Adafruit_USBD_CMSISDAP.h
 *
 * CMSIS-DAP v2 USB interface: makes the board a debug probe for pyOCD,
 * OpenOCD and other CMSIS-DAP hosts (WinUSB on Windows).
 *
 * Requires Tools > USB support > Adafruit TinyUSB with USBD
 *
 */

#ifndef ADAFRUIT_USBD_CMSISDAP_H
#define ADAFRUIT_USBD_CMSISDAP_H

#include <Arduino.h>

#if defined(USE_TINYUSB)

#include <Adafruit_TinyUSB.h>

#include "Adafruit_DAP_CH32.h"

/*!
 * @brief CMSIS-DAP v2 USB interface running commands on an
 *        Adafruit_DAP_CH32.
 */
class Adafruit_USBD_CMSISDAP : public Adafruit_USBD_Interface {
public:
  bool begin(Adafruit_DAP_CH32 *dap);
  void task(void);
  bool mounted(void);

  /*! @return Commands executed so far. */
  uint32_t commandCount(void) const { return _commands; }
  /*! @return millis() of the last command. */
  uint32_t lastCommandMillis(void) const { return _last_ms; }

  uint16_t getInterfaceDescriptor(uint8_t itfnum_deprecated, uint8_t *buf,
                                  uint16_t bufsize) override;

private:
  Adafruit_DAP_CH32 *_dap = nullptr;
  uint32_t _commands = 0;
  uint32_t _last_ms = 0;
  uint8_t _resp[64];
};

#endif // USE_TINYUSB
#endif
