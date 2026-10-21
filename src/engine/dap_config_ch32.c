/*!
 * @file dap_config_ch32.c
 *
 * Runtime pin state and platform functions for DAP_config.h.
 *
 */

#include "DAP_config.h"

#ifndef DAP_CH32_HOST_SIM
/// @cond
extern uint32_t millis(void); // Arduino core (C linkage)
/// @endcond

/*!
 * @brief Monotonic microsecond timer.
 *
 * The core's micros() assumes SysTick counts down, but it counts up, so
 * micros() runs backwards within each millisecond. This reads the real
 * direction and handles a reload not yet seen by the interrupt.
 *
 * @return Microseconds.
 */
uint32_t dap_ch32_micros(void) {
  uint32_t ms, ms2, cnt, sr;
  do {
    ms = millis();
    cnt = (uint32_t)SysTick->CNT; // < CMP + 1 (one millisecond of ticks)
    sr = SysTick->SR;
    ms2 = millis();
  } while (ms != ms2);
  uint32_t ticks = (uint32_t)SysTick->CMP + 1U;
  int down = (SysTick->CTLR & (1U << 4)) != 0;
  uint32_t elapsed = down ? (ticks - 1U - cnt) : cnt; // ticks into this ms
  if ((sr & 1U) && elapsed < ticks / 2U) {
    ms++; // counter reloaded, interrupt not yet run
  }
  return ms * 1000U + (uint32_t)(((uint64_t)elapsed * 1000U) / ticks);
}
#endif

dap_ch32_pins_t dap_ch32_pins; //!< pins used by the engine

/*!
 * @brief Set one pin's 4-bit mode field in CFGLR or CFGHR (setup only).
 * @param port GPIO port.
 * @param mask Pin mask.
 * @param cfg Field value (DAP_CH32_CFG_*).
 */
void dap_ch32_pin_config(GPIO_TypeDef *port, uint32_t mask, uint32_t cfg) {
  uint32_t index = (uint32_t)__builtin_ctz(mask);
  volatile uint32_t *reg = (index < 8U) ? &port->CFGLR : &port->CFGHR;
  uint32_t shift = (index & 7U) * 4U;
  *reg = (*reg & ~(0xFU << shift)) | (cfg << shift);
}

/*!
 * @brief Serial number: the 96-bit chip unique ID as 24 hex digits.
 * @param str Receives the string (at least 26 bytes).
 * @return Length including the terminating NUL.
 */
#ifdef DAP_CH32_HOST_SIM
uint8_t dap_ch32_serial_string(char *str) { // no chip registers on a PC
  static const char s[] = "HOSTSIM000000000000000001";
  memcpy(str, s, sizeof(s));
  return (uint8_t)sizeof(s);
}
#else
uint8_t dap_ch32_serial_string(char *str) {
  static const char hex[] = "0123456789ABCDEF";
  const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFFF7E8;
  uint8_t n = 0;
  for (int w = 0; w < 3; w++) {
    uint32_t v = uid[w];
    for (int i = 7; i >= 0; i--) {
      str[n++] = hex[(v >> (i * 4)) & 0xFU];
    }
  }
  str[n++] = '\0';
  return n;
}
#endif
