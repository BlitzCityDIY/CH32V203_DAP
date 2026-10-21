/*!
 * @file DAP_config.h
 *
 * CMSIS-DAP hardware configuration for the WCH CH32V203 (SWD only).
 *
 * Structure from ARM's CMSIS-DAP template (Apache-2.0); CH32 GPIO handling
 * after XIVN1987/DAPLink's CH32V203 port (MIT). Pins are chosen at runtime
 * by Adafruit_DAP_CH32::begin().
 */

#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__

#include "ch32v20x.h"
#include <stdint.h>
#include <string.h>

#include "cmsis_compiler.h"

/*! Place time-critical SWD code in the zero-wait flash (first 32 KB): plain
 *  ".text" input sections are linked before all ".text.*" sections. */
#define DAP_FAST_CODE __attribute__((section(".text")))

#ifdef __cplusplus
extern "C" {
#endif

/*! Runtime pin configuration, filled in by Adafruit_DAP_CH32::begin(). */
typedef struct {
  GPIO_TypeDef *swclk_port;     //!< SWCLK port
  uint32_t swclk_mask;          //!< SWCLK pin mask
  GPIO_TypeDef *swdio_port;     //!< SWDIO port
  uint32_t swdio_mask;          //!< SWDIO pin mask
  volatile uint32_t *swdio_cfg; //!< &CFGLR or &CFGHR of the SWDIO port
  uint32_t swdio_cfg_keep;      //!< mask that clears SWDIO's 4-bit field
  uint32_t swdio_cfg_out;       //!< field value: push-pull output
  uint32_t swdio_cfg_in;        //!< field value: input with pull-up
  uint32_t out_mode;            //!< output mode for SWCLK/SWDIO (edge speed)
  GPIO_TypeDef *nreset_port;    //!< nRESET port, NULL if not used
  uint32_t nreset_mask;         //!< nRESET pin mask
} dap_ch32_pins_t;

extern dap_ch32_pins_t dap_ch32_pins; //!< pins used by the engine

// 4-bit CFGxR field values (MODE[1:0] | CNF[1:0] << 2)
#define DAP_CH32_CFG_IN_FLOAT 0x4U   //!< input, floating
#define DAP_CH32_CFG_IN_PULL 0x8U    //!< input, pull-up/down (OUTDR selects)
#define DAP_CH32_CFG_OUT_PP 0x3U     //!< output push-pull, 50 MHz edges
#define DAP_CH32_CFG_OUT_PP_10M 0x1U //!< output push-pull, 10 MHz edges
#define DAP_CH32_CFG_OUT_PP_2M 0x2U  //!< output push-pull, 2 MHz edges
#define DAP_CH32_CFG_OUT_OD 0x7U     //!< output open-drain, 50 MHz

void dap_ch32_pin_config(GPIO_TypeDef *port, uint32_t mask, uint32_t cfg);
#include "dap_ch32_platform.h"

#ifdef __cplusplus
}
#endif

/// @cond ARM_CMSIS_DAP_CONFIG
// ARM's CMSIS-DAP configuration interface (documented in ARM's CMSIS-DAP
// DAP_config.h template), implemented for the CH32V203.

// ---------------------------------------------------------------------------
// Debug unit configuration
#define CPU_CLOCK SystemCoreClock // CPU clock in Hz (runtime value)
// SWCLK timing: per half period, 3 cycles per delay iteration plus the
// pin-access overhead (CH32 GPIO stores cross a bus bridge, unlike ARM's
// single-cycle template value of 2). 11 keeps SWCLK at or below the request
// on both the read and write paths. Measured values: see README.
#define IO_PORT_WRITE_CYCLES 11U // fixed cycles per half SWCLK period
#define DELAY_SLOW_CYCLES 3U     // cycles per PIN_DELAY_SLOW iteration

#define DAP_SWD 1                      // SWD available
#define DAP_JTAG 0                     // JTAG not available (yet)
#define DAP_JTAG_DEV_CNT 0U            // no JTAG scan chain
#define DAP_DEFAULT_PORT 1U            // default port: SWD
#define DAP_DEFAULT_SWJ_CLOCK 1000000U // conservative 1 MHz default

#define DAP_PACKET_SIZE 64U // packet size (USB full-speed)
#define DAP_PACKET_COUNT 1U // packets buffered (the USB driver holds one)

#define SWO_UART 0
#define SWO_UART_DRIVER 0
#define SWO_UART_MAX_BAUDRATE 10000000U
#define SWO_MANCHESTER 0
#define SWO_BUFFER_SIZE 4096U
#define SWO_STREAM 0

// DAP_SWJ_Pins times out with TIMESTAMP_GET(); a constant would hang it.
#define TIMESTAMP_CLOCK 1000000U // 1 MHz, from dap_ch32_micros()

#define DAP_UART 0
#define DAP_UART_DRIVER 0
#define DAP_UART_RX_BUFFER_SIZE 1024U
#define DAP_UART_TX_BUFFER_SIZE 1024U
#define DAP_UART_USB_COM_PORT 0

#define TARGET_FIXED 0

// ---------------------------------------------------------------------------
// Identification strings (max 60 chars). Return length incl. NUL, 0 = none.
__STATIC_INLINE uint8_t DAP_GetVendorString(char *str) {
  static const char s[] = "Adafruit";
  memcpy(str, s, sizeof(s));
  return (uint8_t)sizeof(s);
}

__STATIC_INLINE uint8_t DAP_GetProductString(char *str) {
  static const char s[] = "Adafruit_DAP_CH32 CMSIS-DAP";
  memcpy(str, s, sizeof(s));
  return (uint8_t)sizeof(s);
}

__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str) {
  return dap_ch32_serial_string(str);
}

__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString(char *str) {
  (void)str;
  return 0U;
}

__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString(char *str) {
  (void)str;
  return 0U;
}

__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString(char *str) {
  (void)str;
  return 0U;
}

__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString(char *str) {
  (void)str;
  return 0U;
}

__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString(char *str) {
  static const char s[] = "0.1.0";
  memcpy(str, s, sizeof(s));
  return (uint8_t)sizeof(s);
}

// ---------------------------------------------------------------------------
// Port setup

// JTAG is disabled; DAP.c does not call this when DAP_JTAG == 0.
__STATIC_INLINE void PORT_JTAG_SETUP(void) {}

// SWCLK and SWDIO to push-pull outputs (edge speed per out_mode), driven
// high. nRESET released.
__STATIC_INLINE void PORT_SWD_SETUP(void) {
  dap_ch32_pins.swclk_port->BSHR = dap_ch32_pins.swclk_mask;
  dap_ch32_pins.swdio_port->BSHR = dap_ch32_pins.swdio_mask;
  dap_ch32_pin_config(dap_ch32_pins.swclk_port, dap_ch32_pins.swclk_mask,
                      dap_ch32_pins.out_mode);
  dap_ch32_pin_config(dap_ch32_pins.swdio_port, dap_ch32_pins.swdio_mask,
                      dap_ch32_pins.out_mode);
  if (dap_ch32_pins.nreset_port) {
    dap_ch32_pins.nreset_port->BSHR = dap_ch32_pins.nreset_mask;
    dap_ch32_pin_config(dap_ch32_pins.nreset_port, dap_ch32_pins.nreset_mask,
                        DAP_CH32_CFG_OUT_OD);
  }
}

// SWCLK and SWDIO to high-Z. nRESET stays released (open-drain high).
__STATIC_INLINE void PORT_OFF(void) {
  dap_ch32_pin_config(dap_ch32_pins.swclk_port, dap_ch32_pins.swclk_mask,
                      DAP_CH32_CFG_IN_FLOAT);
  dap_ch32_pin_config(dap_ch32_pins.swdio_port, dap_ch32_pins.swdio_mask,
                      DAP_CH32_CFG_IN_FLOAT);
}

// ---------------------------------------------------------------------------
// SWCLK / SWDIO pin access (time critical: single register accesses)

__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void) {
  return (dap_ch32_pins.swclk_port->INDR & dap_ch32_pins.swclk_mask) ? 1U : 0U;
}

__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void) {
  dap_ch32_pins.swclk_port->BSHR = dap_ch32_pins.swclk_mask;
}

__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void) {
  dap_ch32_pins.swclk_port->BCR = dap_ch32_pins.swclk_mask;
}

__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void) {
  return (dap_ch32_pins.swdio_port->INDR & dap_ch32_pins.swdio_mask) ? 1U : 0U;
}

__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void) {
  dap_ch32_pins.swdio_port->BSHR = dap_ch32_pins.swdio_mask;
}

__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void) {
  dap_ch32_pins.swdio_port->BCR = dap_ch32_pins.swdio_mask;
}

__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN(void) {
  return (dap_ch32_pins.swdio_port->INDR & dap_ch32_pins.swdio_mask) ? 1U : 0U;
}

__STATIC_FORCEINLINE void PIN_SWDIO_OUT(uint32_t bit) {
  if (bit & 1U) {
    dap_ch32_pins.swdio_port->BSHR = dap_ch32_pins.swdio_mask;
  } else {
    dap_ch32_pins.swdio_port->BCR = dap_ch32_pins.swdio_mask;
  }
}

// SWDIO direction switch: rewrite the pin's 4-bit CFGxR field.
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void) {
  dap_ch32_pins.swdio_port->BCR = dap_ch32_pins.swdio_mask;
  *dap_ch32_pins.swdio_cfg =
      (*dap_ch32_pins.swdio_cfg & dap_ch32_pins.swdio_cfg_keep) |
      dap_ch32_pins.swdio_cfg_out;
}

// Input with pull-up (OUTDR bit set first selects pull-up).
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void) {
  dap_ch32_pins.swdio_port->BSHR = dap_ch32_pins.swdio_mask;
  *dap_ch32_pins.swdio_cfg =
      (*dap_ch32_pins.swdio_cfg & dap_ch32_pins.swdio_cfg_keep) |
      dap_ch32_pins.swdio_cfg_in;
}

// JTAG-only pins: not present.
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void) { return 0U; }
__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit) { (void)bit; }
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void) { return 0U; }
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void) { return 0U; }
__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit) { (void)bit; }

// nRESET: open-drain, 0 = assert reset, 1 = release.
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void) {
  if (!dap_ch32_pins.nreset_port) {
    return 1U;
  }
  return (dap_ch32_pins.nreset_port->INDR & dap_ch32_pins.nreset_mask) ? 1U
                                                                       : 0U;
}

__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit) {
  if (!dap_ch32_pins.nreset_port) {
    return;
  }
  if (bit & 1U) {
    dap_ch32_pins.nreset_port->BSHR = dap_ch32_pins.nreset_mask;
  } else {
    dap_ch32_pins.nreset_port->BCR = dap_ch32_pins.nreset_mask;
  }
}

// ---------------------------------------------------------------------------
// Status LEDs: not driven from the engine (sketches show status instead).
__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit) { (void)bit; }
__STATIC_INLINE void LED_RUNNING_OUT(uint32_t bit) { (void)bit; }

__STATIC_INLINE uint32_t TIMESTAMP_GET(void) { return dap_ch32_micros(); }

// Called once by DAP_Setup(): pins high-Z, nRESET released.
__STATIC_INLINE void DAP_SETUP(void) {
  PORT_OFF();
  if (dap_ch32_pins.nreset_port) {
    dap_ch32_pins.nreset_port->BSHR = dap_ch32_pins.nreset_mask;
    dap_ch32_pin_config(dap_ch32_pins.nreset_port, dap_ch32_pins.nreset_mask,
                        DAP_CH32_CFG_OUT_OD);
  }
}

// No device-specific reset sequence. Hardware reset is done by the host (or
// the Arduino wrapper) driving nRESET through the DAP_SWJ_Pins command.
__STATIC_INLINE uint8_t RESET_TARGET(void) { return 0U; }

/// @endcond

#endif /* __DAP_CONFIG_H__ */
