# Design — EDF Scheduler for FreeRTOS

## Overview

This document describes the design of an Earliest Deadline First (EDF) scheduler
implemented within the FreeRTOS kernel for the RP2040 (Cortex-M0+). 

All kernel modifications are wrapped in `#if ( configUSE_EDF_SCHEDULER == 1 )`
guards. Setting this flag to 0 in `FreeRTOSConfig.h` compiles out all EDF code
and restores the unmodified FreeRTOS scheduler.

---

## 1. TCB Extensions

EDF requires per-task timing metadata that stock FreeRTOS does not track. The
following fields were added to `tskTCB` in `tasks.c`:

| Field                | Type         | Purpose                                      |
|----------------------|--------------|----------------------------------------------|
| `xPeriod`            | `TickType_t` | T — task period in ticks                     |
| `xRelativeDeadline`  | `TickType_t` | D — relative deadline in ticks (D ≤ T)       |
| `xWCET`              | `TickType_t` | C — worst-case execution time in ticks       |
| `xAbsoluteDeadline`  | `TickType_t` | Current absolute deadline (release + D)      |
| `xNextReleaseTime`   | `TickType_t` | When the next job becomes ready               |
| `xIsEDFTask`         | `BaseType_t` | `pdTRUE` if this task uses EDF scheduling    |
| `xDeadlineMissCount` | `UBaseType_t`| Number of deadline misses detected            |

The `xIsEDFTask` flag allows EDF and non-EDF tasks to coexist. System tasks
(idle, timer) remain priority-based and are not affected by EDF logic.

---

## 2. Task Creation

EDF tasks are created via `xTaskCreateEDF()`, which accepts period (T), relative
deadline (D), and WCET (C) instead of a fixed priority. All EDF tasks are assigned
priority 1 internally, with priority 0 reserved for the idle task. This ensures
any EDF task preempts idle, while EDF-to-EDF scheduling is handled by deadline
ordering rather than the priority field.

On creation, the absolute deadline is computed as `currentTick + D` and the next
release time as `currentTick + T`. This supports runtime task creation, tasks
added after the scheduler starts get deadlines relative to their actual creation
time, not tick 0.

Admission control runs before any memory allocation. If the new task would make
the system unschedulable, `xTaskCreateEDF()` returns an error immediately with
zero cost (no TCB allocated, no stack allocated).

---

## 3. Ready List Insertion — Sorted by Deadline

FreeRTOS maintains an array of ready lists, one per priority level. Tasks are
placed into `pxReadyTasksLists[uxPriority]`. Stock FreeRTOS uses `listINSERT_END()`
which appends to the tail because ordering doesn't matter because all tasks at the
same priority are round-robined.

For EDF, ordering within the ready list matters, the scheduler must always pick
the task with the earliest absolute deadline. We modified the `prvAddTaskToReadyList`
macro to check `xIsEDFTask`:

- **EDF tasks**: use `vListInsert()`, which inserts sorted by `xItemValue`. Since
  we set `xItemValue` to the absolute deadline, the task with the earliest deadline
  ends up at the head of the list.
- **Non-EDF tasks**: use `listINSERT_END()` as before (no behavior change).

This means in `vTaskSwitchContext()`, selecting the earliest-deadline task is O(1) —
just grab the head of the priority-1 ready list. The O(n) cost is paid once at
insertion time, which happens far less frequently than context switches.

---

## 4. Scheduler Task Selection

The `taskSELECT_HIGHEST_PRIORITY_TASK` macro is called on every context switch.

**Stock FreeRTOS:** Finds the highest non-empty priority list, then calls
`listGET_OWNER_OF_NEXT_ENTRY()` which cycles through tasks round-robin.

**EDF modification:** When `uxTopPriority > 0` (EDF priority level), we call
`listGET_HEAD_ENTRY()` instead, which returns the first item in the list.
Because EDF tasks are inserted via `vListInsert()` sorted by absolute deadline
(stored in `xItemValue`), the head of the list is always the task with the
earliest absolute deadline. This gives O(1) task selection.

The `uxTopPriority > 0` check ensures idle tasks at priority 0 still use the
original round-robin selection. This cleanly separates EDF scheduling from
the system tasks that should not participate in deadline-based scheduling.

---

## 5. Periodic Release Management

EDF tasks are periodic: each job runs, completes, sleeps until the next period,
then repeats. Managing the transition between jobs is critical, the task's
absolute deadline must be updated to reflect the new job before it re-enters the
ready list.

### Where it happens

FreeRTOS wakes delayed tasks inside `xTaskIncrementTick()`. Each tick, it checks
the delayed list for tasks whose wake time has arrived. When a task is removed
from the delayed list, it is placed back into the ready list via
`prvAddTaskToReadyList()`.

We insert EDF logic between removing the task from the delayed list and
adding it to the ready list:

1. Compute the new absolute deadline: `xNextReleaseTime + xRelativeDeadline`
2. Advance the next release time: `xNextReleaseTime += xPeriod`
3. Write the new deadline into `xStateListItem.xItemValue`

The task then enters the ready list sorted by its updated deadline, not the old
one from the previous job. This is essential, without this update, a task that
finished early would re-enter the ready list with a stale (past) deadline and
incorrectly receive highest priority.

### Deadline-aware preemption

Stock FreeRTOS triggers a context switch when an unblocked task has higher
`uxPriority` than the currently running task. Since all EDF tasks share priority
1, this check would never trigger a switch between EDF tasks.

We added a deadline comparison: if both the waking task and the current task are
EDF tasks, compare `xAbsoluteDeadline`. If the waking task's deadline is earlier,
a context switch is requested. This is what enables preemption.

### Task execution model

Each EDF task follows this loop:

1. Wake up (moved to ready list with updated deadline)
2. Get scheduled (earliest deadline wins)
3. Execute for C ticks (busy-wait simulating computation)
4. Call `vTaskDelayUntil()` to sleep until the next period
5. Repeat from step 1

The GPIO trace hooks (`traceTASK_SWITCHED_IN` / `traceTASK_SWITCHED_OUT`) toggle
a per-task GPIO pin on every context switch. This makes preemption visible on a
logic analyzer: when Green is preempted by Red, Green's GPIO goes LOW, Red's goes
HIGH, and when Red finishes, Green's GPIO goes HIGH again as it resumes.

---

## 6. Admission Control

EDF can schedule any task set with total utilization ≤ 1.0 when D = T, but
this guarantee requires checking before admitting each new task. The assignment
requires two admission tests: the Liu & Layland utilization bound and processor
demand analysis.

### When admission control runs

Admission control runs at the very beginning of `xTaskCreateEDF()`, before any
memory allocation. If the test fails, the function returns immediately with an
error code. This means rejected tasks have zero cost — no TCB allocated, no
stack allocated, no list insertion.

### Task registry

A static array `xEDFTaskRegistry[]` stores the (C, T, D) parameters of every
admitted EDF task. This avoids walking FreeRTOS internal lists (which would
require iterating ready lists, delayed lists, and suspended lists to find all
EDF tasks). The registry is append-only — tasks are added on successful creation.

The maximum number of EDF tasks is `configMAX_EDF_TASKS` (default 128), which
is sufficient for the 100-task test required by the assignment.

### Liu & Layland bound (implicit deadline, D = T)

When every task has D = T, the LL bound is exact for EDF:

    Σ(Ci / Ti) ≤ 1.0  →  schedulable

Since the Cortex-M0+ has no FPU, we use fixed-point integer arithmetic with a
scale factor of 10000:

    Σ(Ci × 10000 / Ti) ≤ 10000

This gives 0.01% precision, which is more than sufficient for tick-based timing.

### Processor demand analysis (constrained deadline, D ≤ T)

When any task has D < T, the LL bound is no longer exact. Processor demand
analysis checks that at every "testing point" L (each absolute deadline within
the time horizon), the total processor demand does not exceed L:

    h(L) = Σ floor((L - Di) / Ti + 1) × Ci  ≤  L

Testing points are the absolute deadlines of all tasks: D, D+T, D+2T, etc.
The time horizon is capped at 4× the longest period or 60 seconds, whichever
is smaller, to avoid combinatorial explosion with many tasks.

### Automatic test selection

`prvEDFAdmissionControl()` scans all existing tasks plus the candidate. If
every task has D == T, it uses the LL bound (cheaper). If any task has D < T,
it falls back to processor demand analysis (more expensive but necessary).

This means the system automatically uses the correct test — the user does not
need to specify which admission test to run.

### Why processor demand admits more than LL bound

The LL bound rejects any task set with Σ(Ci/Ti) > 1.0, even if the deadlines
are structured such that the system is actually schedulable. Processor demand
analysis checks the actual time-domain feasibility, which can accept task sets
that the LL bound rejects. The assignment requires testing with ~100 tasks to
demonstrate this difference.

---

## 7. Deadline Miss Handling

The `xDeadlineMissCount` field in each TCB tracks the number of detected deadline
misses. Detection occurs in `xTaskIncrementTick()` — if the current tick exceeds
a ready EDF task's `xAbsoluteDeadline`, the counter is incremented.

The current approach is log-and-continue: the miss is recorded but the task
continues executing.

---

## 8. Configuration

All EDF code is guarded by:

```c
#if ( configUSE_EDF_SCHEDULER == 1 )
    // EDF logic
#endif
```

Setting `configUSE_EDF_SCHEDULER` to 0 in `FreeRTOSConfig.h`:

- Compiles out all TCB extensions
- Removes `xTaskCreateEDF()` and admission control functions
- Restores `prvAddTaskToReadyList` to use `listINSERT_END()` unconditionally
- Restores `taskSELECT_HIGHEST_PRIORITY_TASK` to use `listGET_OWNER_OF_NEXT_ENTRY()`
- Restores the priority-only preemption check in `xTaskIncrementTick()`

The system behaves as completely unmodified FreeRTOS when the flag is off.