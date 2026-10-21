/*!
 * @file dap_ch32_platform.h
 *
 * CH32V203 platform layer shared by the free-dap engine port (dap_config.h)
 * and the library: runtime pin table, GPIO modes, timer, serial number and
 * SWD clock conversion.
 *
 */

#ifndef ADAFRUIT_DAP_CH32_PLATFORM_H
#define ADAFRUIT_DAP_CH32_PLATFORM_H

#include <stdint.h>

#include "ch32v20x.h"

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

/*! Serial number string (24 hex digits), filled in by begin(). */
extern char dap_ch32_serial[26];

/*!
 * @brief Set one pin's 4-bit mode field in CFGLR or CFGHR.
 * @param port GPIO port.
 * @param mask Pin mask.
 * @param cfg Field value (DAP_CH32_CFG_*).
 */
void dap_ch32_pin_config(GPIO_TypeDef *port, uint32_t mask, uint32_t cfg);

/*! @return Monotonic microseconds (the core's micros() runs backwards). */
uint32_t dap_ch32_micros(void);

/*!
 * @brief Serial number: the 96-bit chip unique ID as 24 hex digits.
 * @param str Receives the string (at least 26 bytes).
 * @return Length including the terminating NUL.
 */
uint8_t dap_ch32_serial_string(char *str);

/*!
 * @brief free-dap delay-loop count for an SWD clock.
 * @param hz SWD clock in Hz.
 * @return Delay count for dap_setup_clock() (0 = fastest, no delay loop).
 */
int dap_ch32_clock_delay(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif
