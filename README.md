# FreeRTOS Multiprocessor EDF — CPSC 538
**Adam Crespi & Jordan Werstiuk** | Branch: `multiprocessor`

Extends the FreeRTOS SMP kernel on the RP2040 dual-core Cortex-M0+ to support
both **Global EDF** and **Partitioned EDF** scheduling, with admission control
and AD2 logic-analyzer verification.

For a full explanation of all four project tasks (EDF, SRP, CBS, MP) see
[`higher_level.md`](higher_level.md).

---

## Hardware

| Component | Detail |
|-----------|--------|
| Raspberry Pi Pico | RP2040, dual-core Cortex-M0+ @ 133 MHz |
| Raspberry Pi Debug Probe | SWD + UART serial |
| Analog Discovery 2 | Logic analyzer for schedule capture |
| LEDs | Red (GP16), Yellow (GP17), Green (GP18) |

AD2 wiring: GP16 → DIO0, GP17 → DIO1, GP18 → DIO2, GND → GND.

---

## Scheduling Modes

| Mode | Flag | Admission control | Task migration |
|------|------|-------------------|----------------|
| Global EDF | `GLOBAL_EDF_ENABLE 1` | Σ Ci/Ti ≤ 2.0 | Yes — tasks run on any core |
| Partitioned EDF | `PARTITIONED_EDF_ENABLE 1` | Per-core Σ Ci/Ti ≤ 1.0 | No — tasks pinned at creation |

If neither flag is set, **Global EDF is the default**.

---

## Build & Flash

Both test binaries share one compiled `FreeRTOS-Kernel`, so only one mode can
be active per build.  Set the flags in `FreeRTOSConfig.h` before each build.

```bash
cd ~/rtos-project/FreeRTOS/FreeRTOS/Demo/ThirdParty/Community-Supported-Demos/CORTEX_M0+_RP2040/build
cmake -DPICO_SDK_PATH=~/rtos-project/pico-sdk ..

# --- Global EDF ---
# Set GLOBAL_EDF_ENABLE=1 / PARTITIONED_EDF_ENABLE=0 in FreeRTOSConfig.h, then:
make mp_global_test -j$(nproc)
picotool load LedTest/mp_global_test.uf2

# --- Partitioned EDF ---
# Set GLOBAL_EDF_ENABLE=0 / PARTITIONED_EDF_ENABLE=1 in FreeRTOSConfig.h, then:
make mp_partitioned_test -j$(nproc)
picotool load LedTest/mp_partitioned_test.uf2
```

Serial monitor:
```bash
minicom -b 115200 -D /dev/ttyACM0
```

---

## Task Sets

### Global EDF (`mp_global_test`) — implicit deadline, D=T

| Task | GPIO | C (ms) | T (ms) | U |
|------|------|--------|--------|---|
| τ1 | GP16 | 300 | 500 | 0.600 |
| τ2 | GP17 | 350 | 700 | 0.500 |
| τ3 | GP18 | 360 | 900 | 0.400 |
| **Total** | | | | **1.500 ≤ 2.0** |

### Partitioned EDF (`mp_partitioned_test`) — implicit deadline, D=T

| Task | GPIO | Core | C (ms) | T (ms) | U |
|------|------|------|--------|--------|---|
| τ1 | GP16 | 0 | 250 | 500 | 0.500 |
| τ2 | GP17 | 0 | 280 | 700 | 0.400 |
| τ3 | GP18 | 1 | 360 | 900 | 0.400 |
| **Core 0** | | | | | **0.900 ≤ 1.0** |
| **Core 1** | | | | | **0.400 ≤ 1.0** |

---

## Gantt Chart Capture

```bash
# Global EDF
python3 capture_gantt_mp.py --mode global --output global_schedule.png

# Partitioned EDF
python3 capture_gantt_mp.py --mode partitioned --output partitioned_schedule.png

# Save raw data for offline replay
python3 capture_gantt_mp.py --mode global --save-csv global.csv
python3 capture_gantt_mp.py --mode global --from-csv global.csv
```

**SMP proof:** any time window where two channels are simultaneously HIGH means
two EDF tasks are executing in parallel on different cores.  The script shades
these windows purple and prints:
```
*** PROOF: both cores executed EDF tasks simultaneously ***
```

---

## Configuration Reference

```c
/* FreeRTOSConfig.h — multiprocessor mode (pick exactly one) */
#define GLOBAL_EDF_ENABLE      1   /* global EDF, tasks migrate freely */
#define PARTITIONED_EDF_ENABLE 0   /* partitioned EDF, tasks pinned */

/* Required for SMP */
#define configNUMBER_OF_CORES               2
#define configRUN_MULTIPLE_PRIORITIES       1
#define configUSE_CORE_AFFINITY             1
#define configTASK_DEFAULT_CORE_AFFINITY    0x3
#define portSUPPORT_SMP                     1
```

---

## Documentation

| File | Contents |
|------|----------|
| [`higher_level.md`](higher_level.md) | Top-down explanation of all 4 tasks (EDF, SRP, CBS, MP) |
| [`design_MP.md`](design_MP.md) | Architecture, algorithm design, assignment questions answered |
| [`changes_MP.md`](changes_MP.md) | All kernel changes and new files |
| [`testing_MP.md`](testing_MP.md) | Test cases, expected serial output, Gantt instructions |
| [`bugs_MP.md`](bugs_MP.md) | Known bugs and fixes (B1–B7) |
| [`future_MP.md`](future_MP.md) | Future improvements (F1–F7) |

---

## Repository Structure

```
├── FreeRTOS/FreeRTOS/
│   ├── Source/
│   │   ├── tasks.c              ← EDF + CBS + SRP + SMP scheduler
│   │   ├── queue.c              ← SRP semaphore take/give
│   │   └── include/
│   │       ├── task.h           ← xTaskCreateEDF, xTaskCreateCBS prototypes
│   │       └── semphr.h         ← SRP semaphore API
│   └── Demo/.../CORTEX_M0+_RP2040/
│       └── LedTest/
│           ├── FreeRTOSConfig.h
│           ├── main_mp_global.c
│           ├── main_mp_partitioned.c
│           └── capture_gantt_mp.py
├── capture_gantt_mp.py          ← (also at root for convenience)
├── higher_level.md
├── design_MP.md
├── changes_MP.md
├── testing_MP.md
├── bugs_MP.md
└── future_MP.md
```
