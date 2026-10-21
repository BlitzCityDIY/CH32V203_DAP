/*!
 * @file dap_config.h
 *
 * CH32V203 port of the free-dap engine taken from Adafruit_DAP (dap.cpp).
 * Implements the engine's DAP_CONFIG_* hooks with direct GPIO register
 * access, using the runtime pin table filled in by Adafruit_DAP_CH32.
 *
 */

#ifndef _DAP_CONFIG_H_
#define _DAP_CONFIG_H_

#include <Arduino.h>

#include "dap_ch32_platform.h"

/// @cond FREE_DAP_CONFIG
// free-dap configuration interface (see dap.cpp).

extern int DAP_CONFIG_SWCLK_PIN;
extern int DAP_CONFIG_SWDIO_PIN;
extern int DAP_CONFIG_nRESET_PIN;

#define DAP_CONFIG_ENABLE_SWD
#define DAP_CONFIG_DEFAULT_PORT DAP_PORT_SWD
#define DAP_CONFIG_DEFAULT_CLOCK dap_ch32_clock_delay(1000000)
#define DAP_CONFIG_PACKET_SIZE 64
#define DAP_CONFIG_PACKET_COUNT 1

// "CMSIS-DAP" in the product string, as the standard asks.
#define DAP_CONFIG_VENDOR_STR "Adafruit"
#define DAP_CONFIG_PRODUCT_STR "Adafruit_DAP_CH32 CMSIS-DAP"
#define DAP_CONFIG_SER_NUM_STR dap_ch32_serial
#define DAP_CONFIG_FW_VER_STR "1.10"
#define DAP_CONFIG_DEVICE_VENDOR_STR NULL
#define DAP_CONFIG_DEVICE_NAME_STR NULL

// SWD bit-banging runs from the zero-wait flash (first 32 KB): plain ".text"
// input sections are linked before all ".text.*" sections.
#define DAP_CONFIG_PERFORMANCE_ATTR __attribute__((section(".text")))

// SWJ_Clock requests are in Hz; the engine's clock setting is a delay count.
#define DAP_CONFIG_CLOCK_DELAY(hz) dap_ch32_clock_delay(hz)

static inline void DAP_CONFIG_SWCLK_TCK_set(void) {
  dap_ch32_pins.swclk_port->BSHR = dap_ch32_pins.swclk_mask;
}

static inline void DAP_CONFIG_SWCLK_TCK_clr(void) {
  dap_ch32_pins.swclk_port->BCR = dap_ch32_pins.swclk_mask;
}

static inline void DAP_CONFIG_SWDIO_TMS_set(void) {
  dap_ch32_pins.swdio_port->BSHR = dap_ch32_pins.swdio_mask;
}

static inline void DAP_CONFIG_SWDIO_TMS_clr(void) {
  dap_ch32_pins.swdio_port->BCR = dap_ch32_pins.swdio_mask;
}

static inline void DAP_CONFIG_SWCLK_TCK_write(int value) {
  if (value) {
    DAP_CONFIG_SWCLK_TCK_set();
  } else {
    DAP_CONFIG_SWCLK_TCK_clr();
  }
}

static inline void DAP_CONFIG_SWDIO_TMS_write(int value) {
  if (value) {
    DAP_CONFIG_SWDIO_TMS_set();
  } else {
    DAP_CONFIG_SWDIO_TMS_clr();
  }
}

static inline int DAP_CONFIG_SWCLK_TCK_read(void) {
  return (dap_ch32_pins.swclk_port->INDR & dap_ch32_pins.swclk_mask) ? 1 : 0;
}

static inline int DAP_CONFIG_SWDIO_TMS_read(void) {
  return (dap_ch32_pins.swdio_port->INDR & dap_ch32_pins.swdio_mask) ? 1 : 0;
}

// SWDIO direction: rewrite the pin's 4-bit CFGxR field.
static inline void DAP_CONFIG_SWDIO_TMS_in(void) {
  dap_ch32_pins.swdio_port->BSHR = dap_ch32_pins.swdio_mask; // pull-up
  *dap_ch32_pins.swdio_cfg =
      (*dap_ch32_pins.swdio_cfg & dap_ch32_pins.swdio_cfg_keep) |
      dap_ch32_pins.swdio_cfg_in;
}

static inline void DAP_CONFIG_SWDIO_TMS_out(void) {
  *dap_ch32_pins.swdio_cfg =
      (*dap_ch32_pins.swdio_cfg & dap_ch32_pins.swdio_cfg_keep) |
      dap_ch32_pins.swdio_cfg_out;
}

// JTAG-only pins: not present.
static inline void DAP_CONFIG_TDO_write(int value) { (void)value; }
static inline void DAP_CONFIG_nTRST_write(int value) { (void)value; }
static inline int DAP_CONFIG_TDI_read(void) { return 0; }
static inline int DAP_CONFIG_TDO_read(void) { return 0; }
static inline int DAP_CONFIG_nTRST_read(void) { return 0; }

// nRESET: open drain, 0 = assert reset, 1 = release.
static inline void DAP_CONFIG_nRESET_write(int value) {
  if (!dap_ch32_pins.nreset_port) {
    return;
  }
  if (value) {
    dap_ch32_pins.nreset_port->BSHR = dap_ch32_pins.nreset_mask;
  } else {
    dap_ch32_pins.nreset_port->BCR = dap_ch32_pins.nreset_mask;
  }
}

static inline int DAP_CONFIG_nRESET_read(void) {
  if (!dap_ch32_pins.nreset_port) {
    return 1;
  }
  return (dap_ch32_pins.nreset_port->INDR & dap_ch32_pins.nreset_mask) ? 1 : 0;
}

// SWCLK and SWDIO high-Z, nRESET released (open drain high).
static inline void DAP_CONFIG_SETUP(void) {
  dap_ch32_pin_config(dap_ch32_pins.swclk_port, dap_ch32_pins.swclk_mask,
                      DAP_CH32_CFG_IN_FLOAT);
  dap_ch32_pin_config(dap_ch32_pins.swdio_port, dap_ch32_pins.swdio_mask,
                      DAP_CH32_CFG_IN_FLOAT);
  if (dap_ch32_pins.nreset_port) {
    dap_ch32_pins.nreset_port->BSHR = dap_ch32_pins.nreset_mask;
    dap_ch32_pin_config(dap_ch32_pins.nreset_port, dap_ch32_pins.nreset_mask,
                        DAP_CH32_CFG_OUT_OD);
  }
}

static inline void DAP_CONFIG_DISCONNECT(void) {
  dap_ch32_pin_config(dap_ch32_pins.swclk_port, dap_ch32_pins.swclk_mask,
                      DAP_CH32_CFG_IN_FLOAT);
  dap_ch32_pin_config(dap_ch32_pins.swdio_port, dap_ch32_pins.swdio_mask,
                      DAP_CH32_CFG_IN_FLOAT);
}

// SWCLK and SWDIO push-pull outputs (edge speed per out_mode), driven high.
static inline void DAP_CONFIG_CONNECT_SWD(void) {
  dap_ch32_pins.swclk_port->BSHR = dap_ch32_pins.swclk_mask;
  dap_ch32_pins.swdio_port->BSHR = dap_ch32_pins.swdio_mask;
  dap_ch32_pin_config(dap_ch32_pins.swclk_port, dap_ch32_pins.swclk_mask,
                      dap_ch32_pins.out_mode);
  dap_ch32_pin_config(dap_ch32_pins.swdio_port, dap_ch32_pins.swdio_mask,
                      dap_ch32_pins.out_mode);
}

static inline void DAP_CONFIG_CONNECT_JTAG(void) {}

// No status LEDs driven by the engine (sketches show status instead).
static inline void DAP_CONFIG_LED(int index, int state) {
  (void)index;
  (void)state;
}

/// @endcond

#endif
