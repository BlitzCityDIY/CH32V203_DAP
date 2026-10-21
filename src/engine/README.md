# CMSIS-DAP engine

`dap.cpp` and `dap.h` are the free-dap based CMSIS-DAP engine from
[Adafruit_DAP](https://github.com/adafruit/Adafruit_DAP) (BSD-3, Copyright
(c) 2013-2017 Alex Taradov, Adafruit), unmodified except for lines marked
`CH32:`:

- `DAP_CONFIG_PERFORMANCE_ATTR` on the six SWD bit-bang functions, to place
  them in the CH32V203's zero-wait flash
- `DAP_SWJ_Pins`: the wait is in microseconds (was multiplied by 1000)
- `DAP_SWJ_Clock`: the request is in Hz, converted to the engine's delay
  count with `DAP_CONFIG_CLOCK_DELAY()`

`dap_config.h`, `dap_ch32_platform.h` and `dap_config_ch32.c` are the CH32V203
port (MIT).

Not implemented by this engine: DAP_SWD_Sequence, DAP_Queue/ExecuteCommands,
SWO and UART commands. DAP_SWJ_Sequence with a bit count of 0 sends nothing
(CMSIS-DAP: 256 bits).
