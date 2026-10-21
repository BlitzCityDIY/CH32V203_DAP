/*!
 * @file dap_ch32_platform.h
 *
 * CH32V203 platform functions used by the CMSIS-DAP engine and the library.
 *
 */

#ifndef ADAFRUIT_DAP_CH32_PLATFORM_H
#define ADAFRUIT_DAP_CH32_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t dap_ch32_micros(void);
uint8_t dap_ch32_serial_string(char *str);

#ifdef __cplusplus
}
#endif

#endif
