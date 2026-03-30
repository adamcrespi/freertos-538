# High-Level Overview — FreeRTOS Real-Time Scheduling Extensions

**Platform:** Raspberry Pi Pico (RP2040, dual-core Cortex-M0+ @ 133 MHz)
**Base:** FreeRTOS V202212.00 SMP port
**Authors:** Adam Crespi & Jordan Werstiuk

This document gives a concise, top-down explanation of all four scheduling
extensions added to FreeRTOS for CPSC 538.

---

## Background: How Stock FreeRTOS Schedules Tasks

FreeRTOS uses **fixed-priority preemptive scheduling** by default.  Each task
is assigned a static integer priority at creation.  The scheduler always runs
the highest-priority ready task.  Tasks at equal priority share the CPU via
round-robin time-slicing.

The ready list is an array of doubly-linked lists indexed by priority.  On a
context switch, `taskSELECT_HIGHEST_PRIORITY_TASK` finds the non-empty list
with the highest index and picks the head entry.

All four of our extensions layer on top of this infrastructure rather than
replacing it: EDF tasks all live at priority 1, and we change *how* the
priority-1 ready list is ordered and maintained.

---

## Task 1: Earliest Deadline First (EDF)

### The Problem
Fixed-priority scheduling is suboptimal for periodic real-time workloads.  A
task set is schedulable under fixed priority only if total utilisation U ≤ ln 2
≈ 0.69.  EDF can schedule any task set with U ≤ 1.0 on a single core —
optimal among all single-core schedulers.

### What We Added
**TCB extensions:** each task gets `xPeriod`, `xRelativeDeadline`, `xWCET`,
`xAbsoluteDeadline`, `xNextReleaseTime`, `xIsEDFTask`, `xDeadlineMissCount`.

**Ready list ordering:** EDF tasks are inserted by absolute deadline using
`vListInsert()` instead of `vListInsertEnd()`.  The head of the list is always
the task with the nearest deadline.

**`taskSELECT_HIGHEST_PRIORITY_TASK`** is patched to call `listGET_HEAD_ENTRY()`
for the priority-1 list, picking the front of the deadline-sorted list.

**Deadline update:** `xTaskIncrementTick()` wakes tasks from the delayed list
and assigns a fresh absolute deadline:
```
xAbsoluteDeadline = xNextReleaseTime + xRelativeDeadline
xNextReleaseTime += xPeriod
```

**Admission control:** `xTaskCreateEDF()` calls `prvEDFAdmissionControl()`
before creating any task:
- If all tasks are **implicit deadline** (D = T): uses the LL utilisation bound
  (Σ Ci/Ti ≤ 1).
- If any task is **constrained deadline** (D < T): uses **processor demand
  analysis** — checks that the cumulative demand never exceeds the available
  time over all scheduling points (task deadlines).  This is necessary and
  sufficient for EDF schedulability.

**Deadline miss handling:** log-and-continue.  `xDeadlineMissCount` is
incremented; the task is not dropped or restarted.

### Key API
```c
BaseType_t xTaskCreateEDF(
    TaskFunction_t pxTaskCode,
    const char *pcName,
    uint32_t usStackDepth,
    void *pvParameters,
    TickType_t xPeriod,
    TickType_t xRelativeDeadline,
    TickType_t xWCET,
    TaskHandle_t *pxCreatedTask
);
```

---

## Task 2: Stack Resource Policy (SRP)

### The Problem
EDF alone cannot handle shared resources safely.  Standard blocking (e.g.
priority inheritance) can still cause unbounded priority inversion under EDF.
SRP eliminates blocking-induced inversion by preventing a task from starting
if it cannot run to completion without blocking on a resource.

### What We Added
**Preemption level (π):** each EDF task gets a preemption level inversely
proportional to its deadline — shorter deadline = higher π.  Tasks with the
same deadline share the same π (and therefore the same stack space).

**System ceiling (Π):** a global stack that tracks the maximum resource ceiling
of all currently held resources.  A new task can only preempt the running task
if its π exceeds Π.

**SRP semaphores:** extended `xQueueHandle` with `uxResourceCeiling` and
`xMaxCSLength`.  On `take`, the resource ceiling is pushed onto the system
ceiling stack.  On `give`, it is popped.  Crucially, **a task never blocks** —
if the system ceiling would prevent a preemption, the arriving task is simply
not dispatched until the ceiling drops.

**Admission control extension:** blocking time Bi (worst-case time a task can
be delayed by a lower-priority task holding a resource) is added to the
schedulability test:
```
Σ(Ci + Bi)/Ti ≤ 1   (LL form)
```

**Run-time stack sharing:** under SRP, two tasks at the same π can never
execute simultaneously.  Their stack storage can therefore be aliased — one
physical stack buffer is shared between all tasks at the same preemption level,
reducing peak stack memory proportionally to the number of tasks per level.

### Key API
```c
SemaphoreHandle_t xSemaphoreCreateBinarySRP(uxCeiling, xMaxCSLength);
void vSRPPushCeiling(uxCeiling);
void vSRPPopCeiling(void);
```

---

## Task 3: Constant Bandwidth Server (CBS)

### The Problem
EDF handles periodic hard real-time tasks well but cannot directly accommodate
**aperiodic** or **soft real-time** tasks — those that arrive at irregular
intervals and may occasionally burst beyond their nominal CPU budget.  Naively
admitting an aperiodic task into the EDF queue can cause deadline misses for
the periodic tasks.

### What We Added
CBS wraps an aperiodic task in a **server** with a fixed budget Qs and server
period Ts.  The server appears to the EDF scheduler as a regular periodic task
with utilisation Qs/Ts, providing bandwidth isolation.

**TCB extensions:** `xIsCBSTask`, `xCBSBudget`, `xCBSMaxBudget`,
`xCBSServerPeriod`.

**Budget accounting:** each tick, `xTaskIncrementTick()` decrements the
running CBS task's budget.

**Rule 1 — budget exhaustion:** when budget hits zero, the server's deadline is
postponed by Ts and the budget is replenished:
```
xAbsoluteDeadline += Ts
xCBSBudget = Qs
```
The server is re-inserted into the ready list at its new deadline, and the
scheduler switches to the next-earliest-deadline task.

**Rule 2 — job arrival:** when a new CBS job arrives at time r with residual
budget qs and current server deadline ds, if:
```
qs / Ts > (ds - r) / Qs
```
the server would unfairly borrow bandwidth from future periods.  To prevent
this, a fresh deadline is assigned: `ds = r + Ts`, `qs = Qs`.

**Tie-breaking:** a CBS task always wins over a periodic task with the same
absolute deadline (preventing a periodic task from starving the server at the
boundary).

**Admission control:** CBS registers itself as a synthetic periodic task
(Qs, Ts, Ts) so the existing EDF admission control transparently accounts for
server bandwidth.

### Key API
```c
BaseType_t xTaskCreateCBS(
    TaskFunction_t pxTaskCode,
    const char *pcName,
    uint32_t usStackDepth,
    void *pvParameters,
    TickType_t xBudget,
    TickType_t xServerPeriod,
    TaskHandle_t *pxCreatedTask
);
```

---

## Task 4: Multiprocessor EDF (SMP)

### The Problem
The RP2040 has two identical Cortex-M0+ cores sharing SRAM.  Stock FreeRTOS
SMP uses priority-based preemption across cores, which breaks EDF ordering.
Two multiprocessor scheduling strategies are needed: **global** (one shared
queue, tasks migrate freely) and **partitioned** (tasks pinned to one core).

### Architecture

```
Core 0                          Core 1
──────────────────────          ──────────────────────
Tick ISR (SysTick)              No tick ISR
xTaskIncrementTick()            Receives FIFO interrupt
prvYieldForEDFTask()            vTaskSwitchContext(1)
prvSelectHighestPriority(0)     prvSelectHighestPriority(1)
         │                               │
         └──────── shared ready list ────┘
                  pxReadyTasksLists[1]
                  (sorted by xAbsoluteDeadline)
```

A single SysTick on core 0 drives the tick for both cores.  When a sleeping
EDF task wakes, `prvYieldForEDFTask()` compares its deadline against whatever
each core is currently running and sends a cross-core FIFO interrupt if
preemption is warranted.

### Global EDF

All EDF tasks share one deadline-sorted ready list.  Both cores call
`prvSelectHighestPriorityTask(coreID)` on every context switch and pick the
front entry whose `uxCoreAffinityMask` permits that core.  Tasks are not
pinned; the same task may execute on core 0 in one job and core 1 in the next
(migration).

**Admission control:** Σ Ci/Ti ≤ m = 2.

**Affinity mask:** `0x3` (both cores allowed).

### Partitioned EDF

Each task is permanently assigned to one core at creation via `xCorePreference`.
The affinity mask is set to `1 << assignedCore`, so `prvSelectHighestPriorityTask`
on core k never picks a task pinned to core (1−k).  Each core effectively runs
its own independent EDF queue.

**Assignment heuristic:** worst-fit bin packing — assign to the core with the
most remaining capacity.  Explicit core pinning (`xCorePreference = 0` or `1`)
is also supported.

**Admission control:** per-core Σ Ci/Ti ≤ 1.0, maintained in `ulCoreUtil[k]`.

### Key Kernel Fixes Required for SMP + EDF

| Problem | Root cause | Fix |
|---------|-----------|-----|
| EDF ready list corrupted on context switch | `vListInsertEnd` destroys deadline order | Use `vListInsert` for EDF tasks in `prvSelectHighestPriorityTask` |
| Cross-core preemption ignores deadlines | `prvYieldForTask` compares priorities; all EDF tasks share priority 1 | New `prvYieldForEDFTask` compares `xAbsoluteDeadline` |
| Core 1 deadlocks after first time-slice | `traceTASK_SWITCHED_OUT` calls `xTaskGetApplicationTaskTag` → `vTaskEnterCritical` → `prvCheckForRunStateChange` spins on `taskTASK_SCHEDULED_TO_YIELD` | Use `xTaskGetApplicationTaskTagFromISR` in trace hooks |
| `configRUN_MULTIPLE_PRIORITIES = 0` prevents dual-core use | FreeRTOS blocks lower-priority cores while any higher-priority task runs | Set `configRUN_MULTIPLE_PRIORITIES 1` |

### Default Mode
If neither `GLOBAL_EDF_ENABLE` nor `PARTITIONED_EDF_ENABLE` is set to 1, the
kernel defaults to global EDF at compile time.

### Configuration
```c
/* In FreeRTOSConfig.h — pick exactly one: */
#define GLOBAL_EDF_ENABLE      1   /* global EDF (default) */
#define PARTITIONED_EDF_ENABLE 0

/* Required SMP config: */
#define configNUMBER_OF_CORES               2
#define configRUN_MULTIPLE_PRIORITIES       1
#define configUSE_CORE_AFFINITY             1
#define configTASK_DEFAULT_CORE_AFFINITY    0x3
#define portSUPPORT_SMP                     1
```

### Key API
```c
/* Extended xTaskCreateEDF for MP — xCorePreference: -1=global, 0/1=pin to core */
BaseType_t xTaskCreateEDF(
    TaskFunction_t pxTaskCode,
    const char *pcName,
    uint32_t usStackDepth,
    void *pvParameters,
    TickType_t xPeriod,
    TickType_t xRelativeDeadline,
    TickType_t xWCET,
    BaseType_t xCorePreference,
    TaskHandle_t *pxCreatedTask
);

/* Change core affinity at runtime (FreeRTOS SMP built-in): */
void vTaskCoreAffinitySet(TaskHandle_t xTask, UBaseType_t uxCoreAffinityMask);
UBaseType_t vTaskCoreAffinityGet(TaskHandle_t xTask);
```

---

## How the Pieces Fit Together

```
FreeRTOS kernel (tasks.c)
│
├── EDF scheduler (configUSE_EDF_SCHEDULER=1)
│   ├── xTaskCreateEDF — admission control + TCB init
│   ├── xTaskIncrementTick — deadline update, deadline-miss detection
│   ├── taskSELECT_HIGHEST_PRIORITY_TASK — picks head of sorted list
│   │
│   ├── CBS layer (configUSE_CBS=1)
│   │   └── budget decrement + Rule 1/2 in xTaskIncrementTick
│   │
│   └── SMP layer (configNUMBER_OF_CORES > 1)
│       ├── prvYieldForEDFTask — cross-core deadline preemption
│       ├── GLOBAL_EDF_ENABLE  — shared queue, mask=0x3
│       └── PARTITIONED_EDF_ENABLE — per-core queue, mask=1<<core
│
└── SRP (configUSE_SRP=1)
    ├── System ceiling stack (uxSystemCeiling, uxCeilingStack[])
    ├── vSRPPushCeiling / vSRPPopCeiling (called from queue.c)
    └── Stack sharing — tasks at same preemption level share stack buffer
```

All extensions are independently selectable via `FreeRTOSConfig.h` flags.
Disabling all flags (`configUSE_EDF_SCHEDULER=0`) restores the stock
priority-based FreeRTOS scheduler with no overhead.

---

## File Map

| Location | Contents |
|----------|----------|
| `FreeRTOS/Source/tasks.c` | All scheduler modifications (EDF, CBS, SRP ceiling, SMP) |
| `FreeRTOS/Source/include/task.h` | `xTaskCreateEDF`, `xTaskCreateCBS` prototypes; TCB extensions |
| `FreeRTOS/Source/queue.c` / `queue.h` | SRP semaphore take/give; ceiling push/pop |
| `LedTest/FreeRTOSConfig.h` | All configuration flags |
| `LedTest/main_edf.c` | Single-core EDF + admission control test |
| `LedTest/main_srp.c` | SRP semaphores + stack sharing test |
| `LedTest/main_cbs.c` | CBS mixed-workload test |
| `LedTest/main_mp_global.c` | Global EDF dual-core test |
| `LedTest/main_mp_partitioned.c` | Partitioned EDF dual-core test |
| `capture_gantt_edf.py` | AD2 logic analyzer capture + Gantt (single-core) |
| `LedTest/capture_gantt_mp.py` | AD2 capture + Gantt with SMP parallel-execution highlighting |
