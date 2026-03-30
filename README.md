# FreeRTOS Real-Time Scheduling Extensions — CPSC 538
**Adam Crespi & Jordan Werstiuk**

Four real-time scheduling extensions to the FreeRTOS kernel, running on a
Raspberry Pi Pico (RP2040, dual-core Cortex-M0+ @ 133 MHz).

---

## The Four Tasks

| Branch | What it does |
|--------|-------------|
| `edf` | EDF scheduler with admission control (LL bound + processor demand analysis) |
| `srp` | Stack Resource Policy — SRP semaphores, system ceiling, run-time stack sharing |
| `cbs` | Constant Bandwidth Server — aperiodic tasks alongside hard real-time EDF tasks |
| `multiprocessor` | Global EDF and Partitioned EDF on both RP2040 cores |

Each branch has its own `README.md` with build instructions, task parameters,
and expected output.  All extensions are gated behind `FreeRTOSConfig.h` flags
— setting them to 0 restores unmodified FreeRTOS.

---

## Hardware

- Raspberry Pi Pico (RP2040)
- Raspberry Pi Debug Probe (SWD + UART)
- Analog Discovery 2 (logic analyzer)
- LEDs on GP16 (Red), GP17 (Yellow), GP18 (Green)

---

## Quick Start

```bash
git clone https://github.com/adamcrespi/freertos-538.git
cd freertos-538

# Pico SDK (not in this repo)
git clone https://github.com/raspberrypi/pico-sdk.git --recurse-submodules ../pico-sdk

# Checkout the branch you want
git checkout edf   # or srp, cbs, multiprocessor

# Build
cd FreeRTOS/FreeRTOS/Demo/ThirdParty/Community-Supported-Demos/CORTEX_M0+_RP2040/build
cmake -DPICO_SDK_PATH=~/rtos-project/pico-sdk ..
make led_test -j$(nproc)

# Flash
picotool load LedTest/led_test.uf2

# Serial
minicom -b 115200 -D /dev/ttyACM0
```

Dependencies: `sudo apt install gcc-arm-none-eabi cmake ninja-build` and `pip install pydwf matplotlib numpy`
