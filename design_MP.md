# Multiprocessor EDF — Design Notes

## Overview

Task 4 extends the single-core EDF scheduler to the RP2040's two Cortex-M0+
cores.  Two scheduling policies are supported, selected at compile time:

| Macro                   | Value | Policy                                     |
|-------------------------|-------|--------------------------------------------|
| `GLOBAL_EDF_ENABLE`     | `1`   | Global EDF — shared queue, tasks migrate   |
| `PARTITIONED_EDF_ENABLE`| `1`   | Partitioned EDF — tasks pinned to one core |

Both macros live in `FreeRTOSConfig.h`.  Only one may be `1` at a time.

---

## Global EDF

### Concept
All EDF tasks share a single deadline-sorted ready list (the existing
`pxReadyTasksLists[1]`).  Both cores independently call
`prvSelectHighestPriorityTask(xCoreID)` on each context switch and pick the
task at the head of the list whose `uxCoreAffinityMask` allows that core.
Tasks are not pinned; the same task may run on core 0 in one job and core 1 in
the next.

### Admission control
Σ(Ci/Ti) ≤ m = 2.  Implemented by comparing
`ulTotalUtil + ulNewUtil` against `EDF_UTIL_SCALE * configNUMBER_OF_CORES`.

### Affinity mask
`uxCoreAffinityMask = 0x3` (both cores allowed).

---

## Partitioned EDF

### Concept
Each task is permanently assigned to one core at creation time.  The assignment
is recorded in `uxCoreAffinityMask = 1 << assignedCore`, so the SMP kernel
never schedules the task on the wrong core.  Each core runs its own independent
EDF queue; from each core's perspective it is identical to single-core EDF.

### Assignment at creation (`xCorePreference`)
`xTaskCreateEDF` gains a new `xCorePreference` parameter:
- `0` or `1` — pin to that specific core; reject if that core's util ≥ 1.
- `-1` — auto-assign using worst-fit: choose the core with the most remaining
  capacity, reject if no core can accommodate the task.

### Admission control
Per-core: Σ(Ci/Ti) over tasks on core k ≤ 1.0.  Maintained in
`ulCoreUtil[k]` (fixed-point, scaled by `EDF_UTIL_SCALE = 1000`).

### Data structures
```c
#define configMAX_EDF_TASKS_PER_CORE  32
static UBaseType_t uxCoreTaskCount[configNUMBER_OF_CORES];
static uint32_t    ulCoreUtil[configNUMBER_OF_CORES];
```

---

## Key Kernel Changes (`tasks.c`)

### 1. `prvSelectHighestPriorityTask` — list insertion fix
FreeRTOS SMP uses `vListInsertEnd` to move the current task to the tail of the
ready list before searching for the next task (round-robin fairness).  This
destroys the deadline-sorted order.  Fix: use `vListInsert` instead for EDF
tasks so the list stays sorted.

```c
#if ( configUSE_EDF_SCHEDULER == 1 )
if( pxCurrentTCBs[ xCoreID ]->xIsEDFTask == pdTRUE )
    vListInsert( &pxReadyTasksLists[...], &xStateListItem );
else
#endif
    vListInsertEnd( ... );  // original round-robin for non-EDF
```

### 2. `prvYieldForEDFTask`
New function called from `xTaskIncrementTick` when a sleeping EDF task wakes
up.  Iterates all cores; if a core is running an EDF task with a later
deadline, requests a yield (`prvYieldCore(xCoreID)`).  Respects
`uxCoreAffinityMask` — a partitioned task only preempts its assigned core.

```c
static void prvYieldForEDFTask( const TCB_t * pxTCB )
{
    for( xCoreID = 0; xCoreID < configNUMBER_OF_CORES; xCoreID++ )
    {
        if( /* affinity check */ )
            continue;
        if( pxTCB->xAbsoluteDeadline < pxCurrentTCBs[xCoreID]->xAbsoluteDeadline )
            prvYieldCore( xCoreID );
    }
}
```

### 3. `xTaskCreateEDF` — new `xCorePreference` parameter
New signature:
```c
BaseType_t xTaskCreateEDF( ..., BaseType_t xCorePreference, TaskHandle_t *pxCreatedTask );
```
Handles both modes transparently.  For global EDF, `xCorePreference` is
ignored; for partitioned EDF it controls the core assignment.

---

## FreeRTOSConfig.h Changes

```c
#define configNUMBER_OF_CORES               2
#define configTICK_CORE                     0
#define configRUN_MULTIPLE_PRIORITIES       1   // both cores run priority-1 tasks
#define configUSE_CORE_AFFINITY             1
#define configTASK_DEFAULT_CORE_AFFINITY    0x3
#define portSUPPORT_SMP                     1
#define configUSE_PASSIVE_IDLE_HOOK         0
#define GLOBAL_EDF_ENABLE                   1   // or 0 for partitioned
#define PARTITIONED_EDF_ENABLE              0   // or 1 for partitioned
```

`configRUN_MULTIPLE_PRIORITIES = 1` is required so both cores can
simultaneously execute priority-1 EDF tasks.  With `0`, the second core would
be blocked from running while any priority-1 task is running on core 0.

---

## Architecture Questions (from assignment spec)

### Q1: Interrupt handling and timer — one per core or shared?

The RP2040 has a single SysTick timer.  The FreeRTOS SMP port configures it on
**core 0 only** (`configTICK_CORE 0`).  The tick ISR fires on core 0,
increments the tick count, and issues a FIFO interrupt to core 1 when a
preemption is needed.  This shared-timer design avoids clock drift between cores
and keeps a single authoritative tick count.  Core 1 does not run a separate
tick ISR; it only receives cross-core yield requests.

### Q2: How to stop/start each core

The RP2040 SMP port handles this transparently.  `vTaskStartScheduler()` on
core 0 launches the FreeRTOS SMP kernel which starts core 1 via the RP2040
inter-core FIFO.  Each core runs an idle task when no real-time task is ready.
No application-level start/stop is needed; affinity masks and the scheduler
together control which core each task runs on.

### Q3: Dispatching tasks to cores and task migration

**Dispatching** is done via `uxCoreAffinityMask` in the TCB:
- Global EDF: mask = `0x3` (both cores), so either core may pick up the task.
- Partitioned EDF: mask = `1 << assignedCore`, enforced by `prvSelectHighestPriorityTask`.

**Migration** (global EDF only): because the affinity mask allows both cores,
different jobs of the same task naturally execute on whichever core picks the
task from the shared ready list first.  No explicit migration call is needed —
the scheduler handles it automatically.  The application can also call
`vTaskCoreAffinitySet(handle, newMask)` at runtime to change affinity
dynamically (exposed via `INCLUDE_vTaskCoreAffinitySet 1`).

**Removing a task from a core** is done by narrowing its affinity mask to
exclude that core:
```c
vTaskCoreAffinitySet( xHandle, 1 << otherCore );   // restrict to one core
vTaskCoreAffinitySet( xHandle, tskNO_AFFINITY );   // restore to any core
```

### Q4: Separate scheduler per core?

**Partitioned EDF:** conceptually yes — each core has its own subset of tasks
pinned by affinity mask.  Each core runs independent EDF over its own tasks.
The global ready list is still shared in memory, but the affinity mask prevents
each core from touching the other's tasks.

**Global EDF:** no — one shared ready list, one EDF order.  Both cores pull
from the same queue.  There is no per-core queue; the single
`pxReadyTasksLists[1]` sorted by absolute deadline serves both cores.

---

## Default Scheduling Mode

If neither `GLOBAL_EDF_ENABLE` nor `PARTITIONED_EDF_ENABLE` is set to `1`,
the kernel defaults to Global EDF at compile time (via a guard in `tasks.c`).
This satisfies the assignment requirement: _"If no multi-core scheduling
algorithm is specified by the user, make global EDF the default scheduler."_

---

## AD2 Demo Strategy

GPIO pins GP16–GP18 connect to AD2 channels D0–D2 via the
`traceTASK_SWITCHED_IN/OUT` hooks.  Each pin is stored as the task tag:

```c
vTaskSetApplicationTaskTag( NULL, (TaskHookFunction_t)(uintptr_t)gpio_pin );
// hooks read it with xTaskGetApplicationTaskTagFromISR(NULL)
```

**SMP proof**: when two AD2 channels are HIGH simultaneously, two tasks are
executing at the same time on different cores.  The `capture_gantt_mp.py`
script detects these overlapping intervals and shades them purple on the chart.

For **global EDF**: watch how the same task (e.g. τ1) alternates between
cores across jobs — the serial output reports `core=0` for some jobs and
`core=1` for others.

For **partitioned EDF**: tasks τ1/τ2 always report `core=0`, task τ3 always
reports `core=1`.  The migration-detection assert in the task body confirms
this.
