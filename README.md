# FreeRTOS EDF Scheduler — CPSC 538
**Adam Crespi & Jordan Werstiuk** | Branch: `edf`

Extends the FreeRTOS kernel on the RP2040 Cortex-M0+ with **Earliest Deadline
First (EDF)** scheduling and kernel-level admission control.  EDF is optimal
for single-core periodic real-time systems: any task set with total utilisation
U ≤ 1.0 is schedulable.

---

## Hardware

| Component | Detail |
|-----------|--------|
| Raspberry Pi Pico | RP2040, Cortex-M0+ @ 133 MHz |
| Raspberry Pi Debug Probe | SWD + UART serial |
| Analog Discovery 2 | Logic analyzer for schedule capture |
| LEDs | Red (GP16/τ1), Yellow (GP17/τ2), Green (GP18/τ3) |

AD2 wiring: GP16 → DIO0, GP17 → DIO1, GP18 → DIO2, GND → GND.

---

## What Was Implemented

- **EDF scheduling** — ready list sorted by absolute deadline; O(1) task selection
- **Preemption** — context switch triggered when a newly-ready task has an earlier deadline than the running task
- **Periodic releases** — absolute deadline updated on each job wakeup in `xTaskIncrementTick`
- **Admission control** — two tests, selected automatically:
  - **LL bound** (D = T): Σ Ci/Ti ≤ 1.0
  - **Processor demand** (D < T): h(L) = Σ⌊(L−Di)/Ti+1⌋·Ci ≤ L at every scheduling point
- **Runtime task creation** — tasks added after `vTaskStartScheduler` get deadlines relative to their creation time
- **Deadline miss detection** — logged per-task, no crash (log-and-continue)
- **Config flag** — `configUSE_EDF_SCHEDULER 0` compiles out all EDF code, restoring stock FreeRTOS

---

## Build & Flash

```bash
cd ~/rtos-project/FreeRTOS/FreeRTOS/Demo/ThirdParty/Community-Supported-Demos/CORTEX_M0+_RP2040/build
cmake -DPICO_SDK_PATH=~/rtos-project/pico-sdk ..

# EDF scheduling test (3 tasks, preemption visible on AD2)
make led_test -j$(nproc)
picotool load LedTest/led_test.uf2

# 100-task admission control comparison (LL bound vs processor demand)
make edf_100test -j$(nproc)
picotool load LedTest/edf_100test.uf2
```

Serial monitor:
```bash
minicom -b 115200 -D /dev/ttyACM0
```

---

## Task Sets

### Test 1 — Low utilisation, no preemption

| Task | GPIO | C (ms) | D (ms) | T (ms) | U |
|------|------|--------|--------|--------|---|
| Red | GP16 | 100 | 250 | 500 | 0.200 |
| Yellow | GP17 | 150 | 500 | 1000 | 0.150 |
| Green | GP18 | 200 | 1000 | 2000 | 0.100 |
| **Total** | | | | | **0.450** |

Tasks complete well before their deadlines. Red runs first every period (earliest deadline), then Yellow, then Green. No preemption needed.

### Test 2 — Medium utilisation, preemption visible

| Task | GPIO | C (ms) | D (ms) | T (ms) | U |
|------|------|--------|--------|--------|---|
| Red | GP16 | 80 | 200 | 400 | 0.200 |
| Yellow | GP17 | 150 | 400 | 800 | 0.188 |
| Green | GP18 | 400 | 1000 | 1600 | 0.250 |
| **Total** | | | | | **0.638** |

Green runs for 400ms but Red's period is 400ms — Red releases mid-way through Green's execution with an earlier deadline, preempting it. Visible on the AD2 as Green's GPIO going LOW, Red going HIGH, then Green resuming.

---

## Gantt Charts

Captured with `capture_gantt_edf.py` via AD2 logic analyzer.

### Test 1 — Sequential execution, no preemption

<!-- Add Gantt chart image here -->

### Test 2 — Preemption visible

<!-- Add Gantt chart image here -->

### Admission Control Test (100 tasks, LL bound vs processor demand)

<!-- Add serial output screenshot here -->

---

## Gantt Capture Tool

```bash
# Live capture (6 seconds)
python3 capture_gantt_edf.py --output schedule.png

# Save raw data for replay
python3 capture_gantt_edf.py --save-csv capture.csv --output schedule.png

# Replay from saved data
python3 capture_gantt_edf.py --from-csv capture.csv --output schedule.png

# Zoom into a time window
python3 capture_gantt_edf.py --from-csv capture.csv --zoom-start 0 --zoom-end 3
```

Install dependency: `pip install pydwf`

---

## Admission Control Test (100 tasks)

`edf_100test` runs admission control only — no FreeRTOS tasks are actually
created.  It feeds 100 tasks incrementally and logs both LL bound and processor
demand results for each.

Task parameters: C=5ms, T=250ms, D staggered 30→525ms (D < T for early tasks).

Expected result:
```
LL bound rejected at task 51   (Σ Ci/Ti = 1.020 > 1.0)
Processor demand accepted task 51
Processor demand rejected at task 52
```
Processor demand admits 1 more task than LL bound — proof that PD is strictly
less conservative when D < T.

---

## Configuration

```c
/* FreeRTOSConfig.h */
#define configUSE_EDF_SCHEDULER    1   /* 0 = stock FreeRTOS, 1 = EDF */
#define configUSE_APPLICATION_TASK_TAG  1  /* required for GPIO trace hooks */
```

Setting `configUSE_EDF_SCHEDULER 0` compiles out all EDF code with zero overhead.

### Key API

```c
BaseType_t xTaskCreateEDF(
    TaskFunction_t pxTaskCode,
    const char    *pcName,
    uint32_t       usStackDepth,
    void          *pvParameters,
    TickType_t     xPeriod,
    TickType_t     xRelativeDeadline,
    TickType_t     xWCET,
    TaskHandle_t  *pxCreatedTask
);
```

Returns `pdPASS` if admitted and created, error code if rejected by admission control.

---

## Documentation

| File | Contents |
|------|----------|
| [`design_EDF.md`](design_EDF.md) | Architecture, algorithm design, all design choices |
| [`changes_EDF.md`](changes_EDF.md) | Every kernel change and new function |
| [`testing_EDF.md`](testing_EDF.md) | Test cases, expected output, results |
| [`bugs_EDF.md`](bugs_EDF.md) | Known bugs and limitations |
| [`future_EDF.md`](future_EDF.md) | Future improvements |

---

## Repository Structure

```
├── FreeRTOS/FreeRTOS/
│   ├── Source/
│   │   ├── tasks.c              ← EDF scheduler (TCB extensions, admission control,
│   │   │                          sorted ready list, deadline-aware preemption)
│   │   └── include/
│   │       └── task.h           ← xTaskCreateEDF prototype
│   └── Demo/.../CORTEX_M0+_RP2040/
│       └── LedTest/
│           ├── FreeRTOSConfig.h ← configUSE_EDF_SCHEDULER flag + trace hooks
│           ├── main_edf.c       ← 3-task EDF test (Tests 1 & 2)
│           └── main_edf_100test.c ← 100-task admission control comparison
├── capture_gantt_edf.py         ← AD2 capture + Gantt chart tool
├── design_EDF.md
├── changes_EDF.md
├── testing_EDF.md
├── bugs_EDF.md
└── future_EDF.md
```
