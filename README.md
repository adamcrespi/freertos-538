# FreeRTOS Real-Time Scheduling Extensions — CPSC 538

Extensions to the FreeRTOS kernel to support dynamic-priority scheduling and resource access control, running on a Raspberry Pi Pico (RP2040).

## Project Overview

This project extends FreeRTOS with four real-time scheduling mechanisms:

| Task | Description | Weight |
|------|-------------|--------|
| **EDF** | Earliest Deadline First scheduling with admission control | 15% |
| **SRP** | Stack Resource Policy for binary semaphores with runtime stack sharing | 15% |
| **CBS** | Constant Bandwidth Server for aperiodic soft real-time tasks | 15% |
| **MP** | Multiprocessor real-time scheduling | 15% |

Each extension is developed on its own branch and includes configuration flags to enable/disable the feature, falling back to stock FreeRTOS behavior when disabled.

## Hardware

- Raspberry Pi Pico (RP2040, dual-core Cortex-M0+ @ 133 MHz)
- Raspberry Pi Debug Probe (SWD + UART)
- Analog Discovery 2 (logic analyzer for schedule capture)
- 3x LEDs (Red, Yellow, Green) on GP16, GP17, GP18

## Repository Structure

```
├── FreeRTOS/
│   └── FreeRTOS/
│       ├── Source/                  ← Kernel source (modified for EDF/SRP/CBS/MP)
│       │   ├── tasks.c             ← Scheduler
│       │   ├── queue.c             ← Semaphore implementation
│       │   └── include/
│       │       ├── task.h          ← TCB definition, task API
│       │       └── semphr.h        ← Semaphore API
│       └── Demo/ThirdParty/Community-Supported-Demos/
│           └── CORTEX_M0+_RP2040/
│               ├── LedTest/        ← Test programs
│               └── CMakeLists.txt
├── capture_gantt.py                ← AD2 logic analyzer capture + Gantt chart tool
├── changes_EDF.md                  ← Change log per task
├── design_EDF.md                   ← Design documentation per task
├── testing_EDF.md                  ← Test cases and results per task
├── bugs_EDF.md                     ← Known bugs per task
└── future_EDF.md                   ← Future improvements per task
```

## Branches

- `main` — Clean FreeRTOS baseline + tooling
- `edf` — EDF scheduler + admission control
- `srp` — SRP (branched from edf)
- `cbs` — CBS (branched from edf)
- `multiprocessor` — MP support

## Build Instructions

### Prerequisites (Ubuntu/Debian)

```bash
sudo apt install build-essential cmake ninja-build gcc-arm-none-eabi \
    libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib git python3 \
    minicom openocd gdb-multiarch
```

### Clone and Setup

```bash
git clone https://github.com/adamcrespi/freertos-538.git
cd freertos-538

# Clone the Pico SDK separately (not tracked in this repo)
git clone https://github.com/raspberrypi/pico-sdk.git --recurse-submodules ../pico-sdk
```

### Build

```bash
cd FreeRTOS/FreeRTOS/Demo/ThirdParty/Community-Supported-Demos/CORTEX_M0+_RP2040
mkdir build && cd build
cmake -DPICO_SDK_PATH=~/rtos-project/pico-sdk ..
make led_test -j$(nproc)
```

### Flash

1. Hold BOOTSEL on Pico, plug in USB, release
2. `cp LedTest/led_test.uf2 /media/$USER/RPI-RP2/`

### Serial Monitor

```bash
minicom -b 115200 -D /dev/ttyACM0
```

## Schedule Capture Tool

The `capture_gantt.py` script uses an Analog Discovery 2 to capture task execution via GPIO signals and generates a Gantt chart.

### Requirements

```bash
pip3 install pydwf matplotlib numpy
```

Also requires [Digilent WaveForms](https://digilent.com/shop/software/digilent-waveforms/) and Adept Runtime installed.

### Usage

```bash
# Close WaveForms GUI first — only one app can use the AD2
python3 capture_gantt.py
```

### Wiring

| AD2 Channel | Pico GPIO | Task |
|-------------|-----------|------|
| D0 | GP16 | τ1 |
| D1 | GP17 | τ2 |
| D2 | GP18 | τ3 |
| GND | GND | Shared |

## Configuration

In `FreeRTOSConfig.h`:

```c
#define configUSE_EDF_SCHEDULER    1  // 0 = default FreeRTOS, 1 = EDF
#define configUSE_SRP              1  // 0 = no SRP, 1 = SRP (requires EDF)
#define configUSE_CBS              1  // 0 = no CBS, 1 = CBS (requires EDF)
```

Setting all flags to 0 gives stock FreeRTOS behavior.
