# CMSIS-DAP engine

`DAP.c`, `SW_DP.c`, `DAP_vendor.c` and `DAP.h` are ARM's CMSIS-DAP firmware
v2.1.2 (Apache-2.0, see `LICENSE-Apache-2.0.txt`), taken from
https://github.com/ARM-software/CMSIS-DAP at commit `12636590eec66fae2d1bba4518749426ad5a4595`.

## Modifications to ARM's files

- **DAP.h**: added a `__riscv` branch to `PIN_DELAY_SLOW()` with a RISC-V
  delay loop. 
- **SW_DP.c**: added `DAP_FAST_CODE` to `SWJ_Sequence`, `SWD_Sequence`,
  `SWD_Transfer` and the `SWD_TransferFast/Slow` functions. On the CH32V203G6,
  code above the first 32 KB of flash runs about 2.4x slower, so the
  bit-banging code is pinned to the start of flash for consistent SWD timing.

## CH32-specific files (MIT)

- `DAP_config.h`: hardware configuration. Follows ARM's template; the CH32
  GPIO handling is based on XIVN1987/DAPLink's CH32V203 port (MIT), changed to
  runtime-selectable pins.
- `dap_config_ch32.c`: runtime pin state, pin-mode helper, serial number.
- `cmsis_compiler.h`: CMSIS macros the WCH headers don't provide.
