/*!
 * @file cmsis_compiler.h
 *
 * Minimal CMSIS compiler macros for ARM's DAP.h on the CH32V (RISC-V) core,
 * whose headers provide __NOP() but not the CMSIS attribute macros.
 *
 */

#ifndef ADAFRUIT_DAP_CH32_CMSIS_COMPILER_H
#define ADAFRUIT_DAP_CH32_CMSIS_COMPILER_H

#ifndef __ASM
/// @cond CMSIS_COMPILER
#define __ASM __asm
#endif
#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE __attribute__((always_inline)) static inline
#endif
#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif

/// @endcond

#endif
