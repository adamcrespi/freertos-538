# EDF Testing Documentation

## Test Environment

- **Hardware:** Raspberry Pi Pico (RP2040, Cortex-M0+ @ 133MHz)
- **Debug Probe:** Raspberry Pi Debug Probe (SWD + UART)
- **Logic Analyzer:** Analog Discovery 2 (AD2)
- **Tick Rate:** 1000 Hz (1ms per tick, `configTICK_RATE_HZ = 1000`)
- **Capture Tool:** `capture_gantt_edf.py` with AD2 via WaveForms SDK
- **GPIO Mapping:** GP16 (Red/τ1), GP17 (Yellow/τ2), GP18 (Green/τ3)
- **Serial Output:** UART at 115200 baud via Debug Probe

## Test 1: Basic EDF Scheduling (Low Utilization)

**Goal:** Verify EDF runs tasks in deadline order with no preemption needed.

**Task Set:**

| Task   | C (ms) | D (ms) | T (ms) | U     |
|--------|--------|--------|--------|-------|
| Red    | 100    | 250    | 500    | 0.200 |
| Yellow | 150    | 500    | 1000   | 0.150 |
| Green  | 200    | 1000   | 2000   | 0.100 |
| **Total** |     |        |        | **0.450** |

**Expected behavior:**
- All tasks complete well before their deadlines
- Red runs first (earliest deadline D=250ms), then Yellow, then Green
- No preemption — each task finishes before the next one's deadline forces a switch
- Zero deadline misses

**Result:** PASS
- All three tasks created successfully (serial output confirmed)
- Gantt chart shows clean sequential execution per period
- Zero deadline misses detected by capture tool
- U = 0.450, LL bound confirms schedulable

**Capture:** `images/edftest3_fixed.png`

## Test 2: EDF with Preemption (Medium Utilization)

**Goal:** Verify preemption — a task with an earlier deadline interrupts a
running task with a later deadline.

**Task Set:**

| Task   | C (ms) | D (ms) | T (ms) | U     |
|--------|--------|--------|--------|-------|
| Red    | 80     | 200    | 400    | 0.200 |
| Yellow | 150    | 400    | 800    | 0.188 |
| Green  | 400    | 1000   | 1600   | 0.250 |
| **Total** |     |        |        | **0.638** |

**Expected behavior:**
- Green requires 400ms of execution but Red's period is only 400ms, so Red
  will release during Green's execution with an earlier deadline
- Green should be preempted: GPIO goes LOW, Red runs (GPIO HIGH), then Green
  resumes (GPIO HIGH again)
- All deadlines still met (U < 1.0)

**Result:** PASS
- Preemption clearly visible in Gantt chart: Green's execution is split into
  multiple segments with Red and Yellow executing in the gaps
- Trace hooks correctly toggle GPIOs on context switches (not just job
  start/finish), making preemption visible on logic analyzer
- Zero deadline misses
- U = 0.637, schedulable

**Capture:** `images/edftest_4_fixed.png`

## Test 3: Admission Control — Acceptance

**Goal:** Verify that tasks with total U ≤ 1.0 are accepted.

**Task Set:** Same as Test 2 (U = 0.638)

**Serial Output:**
```
Create Red:    OK
Create Yellow: OK
Create Green:  OK
```

**Result:** PASS — all three tasks admitted, LL bound / processor demand
analysis confirmed schedulability.

## Test 4: Admission Control — Rejection

**Goal:** Verify that a task pushing total U > 1.0 is rejected without
affecting existing tasks.

**Setup:** After creating the three tasks from Test 2 (U = 0.638), attempt
to create a fourth task with C=150, D=200, T=200 (U = 0.75). Combined
U = 0.638 + 0.75 = 1.388.

**Serial Output:**
```
Create Red:    OK
Create Yellow: OK
Create Green:  OK
Create Reject: FAIL (expected FAIL)
Starting scheduler...
```

**Result:** PASS
- Fourth task rejected (returned `errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY`)
- No memory allocated for rejected task (admission control runs before
  `prvCreateTask`)
- Existing three tasks unaffected, scheduler started normally

## Test 5: Automatic Test Selection (LL Bound vs Processor Demand)

**Goal:** Verify that admission control automatically selects the correct test
based on whether all tasks have D = T (LL bound) or any have D < T (processor
demand).

**Case A — Implicit deadlines (D = T):**
All tasks created with D = T triggers `prvEDFCheckLLBound()`.

**Case B — Constrained deadlines (D < T):**
Any task with D < T triggers `prvEDFCheckProcessorDemand()`. Our standard test
set has D < T for all tasks (e.g., Red: D=200, T=400), so processor demand is
used.

**Result:** PASS — verified via code inspection and correct accept/reject
behavior in Tests 3 and 4.


## Test 6: 100-Task Admission Control Comparison (LL Bound vs Processor Demand)

**Goal:** Demonstrate that processor demand analysis accepts task sets that the
Liu & Layland bound rejects, as required by the assignment.

**Method:** A dedicated test program (`main_edf_100test.c`) runs admission control
on 100 tasks incrementally. No actual FreeRTOS tasks are created — only the
admission control math executes. A public wrapper `xEDFTestAdmission()` calls
both `prvEDFCheckLLBound()` and `prvEDFCheckProcessorDemand()` on each task and
logs both results via UART.

**Task Set:**

| Parameter | Value |
|-----------|-------|
| C | 5 ticks (5ms) |
| T | 250 ticks (250ms) for all tasks |
| D | Staggered: 30, 35, 40, ..., 525 ticks (D < T for early tasks) |
| U per task | 5/250 = 0.020 |

**Why this task set:** With D < T, tasks have slack between their deadline and
next release. The LL bound ignores this slack and rejects purely based on
Σ(Ci/Ti) > 1.0. Processor demand analysis checks actual time-domain feasibility
and can exploit the slack to accept additional tasks.

**Serial Output:**
`images/admissiontest1.png`
`images/admissiontest2.png`

**Result:** PASS
- LL bound rejected at task 51 (U = 1.020 > 1.0)
- Processor demand accepted task 51 (staggered deadline creates sufficient slack)
- Processor demand rejected at task 52
- Demonstrates that PD is strictly less conservative than LL bound for D < T

**Build and run:**
```bash
make edf_100test -j$(nproc)
cp LedTest/edf_100test.uf2 /media/$USER/RPI-RP2/
minicom -b 115200 -D /dev/ttyACM0
```





TODO
### Test 10: Config Flag Test
**Goal:** Set `configUSE_EDF_SCHEDULER = 0` and verify the system builds and
runs with stock FreeRTOS behavior, no EDF code compiled in.













### Updating Task Parameters
1. Edit `#define TASK*_WCET/DEADLINE/PERIOD` in `main_edf.c`
2. Update `TASK_SET` dictionary in `capture_gantt_edf.py` to match
3. Rebuild, flash, capture