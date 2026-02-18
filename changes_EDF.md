# Changes — EDF Scheduler

All kernel modifications to support EDF scheduling. Every change is wrapped in
`#if ( configUSE_EDF_SCHEDULER == 1 )` unless otherwise noted.

---

## Modified Files Summary

| File | What Changed |
|------|-------------|
| `tasks.c` | TCB struct, task creation, ready list insertion, task selection, periodic releases, preemption, admission control |
| `task.h` | `xTaskCreateEDF()` prototype |
| `FreeRTOSConfig.h` | `configUSE_EDF_SCHEDULER`, `configUSE_APPLICATION_TASK_TAG`, trace hook macros |

---

## `FreeRTOS/FreeRTOS/Source/tasks.c`

### 1. TCB Extensions — `tskTCB` struct MODIFIED

Added fields inside `#if ( configUSE_EDF_SCHEDULER == 1 )`:

| Field | Type | Purpose |
|-------|------|---------|
| `xPeriod` | `TickType_t` | T — task period in ticks |
| `xRelativeDeadline` | `TickType_t` | D — relative deadline (D ≤ T) |
| `xWCET` | `TickType_t` | C — worst-case execution time |
| `xAbsoluteDeadline` | `TickType_t` | Current absolute deadline (release + D) |
| `xNextReleaseTime` | `TickType_t` | When the next job becomes ready |
| `xIsEDFTask` | `BaseType_t` | `pdTRUE` if task uses EDF scheduling |
| `xDeadlineMissCount` | `UBaseType_t` | Number of deadline misses detected |

### 2. Task Creation — `xTaskCreateEDF()` NEW FUNCTION

Creates an EDF-scheduled task. Wraps `prvCreateTask()` with priority 1, then
fills EDF-specific TCB fields. Sets `xStateListItem.xItemValue` to the absolute
deadline for sorted insertion into ready lists.

- Calls `prvEDFAdmissionControl()` before `prvCreateTask()` — rejected tasks
  cost zero memory
- Returns `errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY` on rejection
- Checks `uxEDFTaskCount >= configMAX_EDF_TASKS` before proceeding
- On success, registers task parameters in `xEDFTaskRegistry[]`

### 3. Ready List Insertion — `prvAddTaskToReadyList` macro MODIFIED

Changed from always using `listINSERT_END()` (unsorted append) to conditionally
using `vListInsert()` (sorted by `xItemValue`) when `xIsEDFTask == pdTRUE`.
Non-EDF tasks (idle, timer) still use `listINSERT_END()` — no behavior change.

### 4. Task Selection — `taskSELECT_HIGHEST_PRIORITY_TASK` macro MODIFIED

Added EDF branch: when `configUSE_EDF_SCHEDULER == 1` and `uxTopPriority > 0`
(not the idle task), selects the head of the sorted ready list via
`listGET_HEAD_ENTRY()` instead of round-robin via `listGET_OWNER_OF_NEXT_ENTRY()`.
The head is always the task with the earliest absolute deadline.

### 5. Periodic Releases — `xTaskIncrementTick()` MODIFIED

**Deadline update (before `prvAddTaskToReadyList`):**
When a task moves from the delayed list back to the ready list, if it is an EDF
task, its timing fields are updated before re-insertion:
- `xAbsoluteDeadline = xNextReleaseTime + xRelativeDeadline`
- `xNextReleaseTime += xPeriod`
- `xStateListItem.xItemValue` set to new absolute deadline

**Preemption check (after `prvAddTaskToReadyList`):**
Added EDF branch to the existing preemption check. If both the waking task and
the currently running task are EDF tasks, compares `xAbsoluteDeadline` instead
of `uxPriority`. Context switch triggered if the waking task has an earlier
deadline.

### 6. Admission Control — NEW FUNCTIONS

**`xEDFTaskRegistry[]`** — static array of `{xWCET, xPeriod, xRelativeDeadline}`
structs, up to `configMAX_EDF_TASKS` (default 128). Append-only — each successful
`xTaskCreateEDF()` adds to it.

**`prvEDFCheckLLBound()`** — Liu & Layland utilization bound. Computes
`Σ(Ci/Ti)` using fixed-point integer arithmetic (scale factor 10000) to avoid
floating point on Cortex-M0+. Returns `pdTRUE` if U ≤ 1.0. Exact for EDF when
all tasks have D = T.

**`prvEDFCheckProcessorDemand()`** — Processor demand analysis. For constrained-
deadline systems (D ≤ T), checks every absolute deadline L up to 4× the longest
period: `h(L) = Σ floor((L - Dj)/Tj + 1) × Cj ≤ L`. Time horizon capped at
60 seconds. Returns `pdFALSE` if demand exceeds available time at any point.

**`prvEDFAdmissionControl()`** — Dispatch function. If all tasks have D == T,
uses LL bound. If any task has D < T, uses processor demand analysis. Called at
the top of `xTaskCreateEDF()` before any memory allocation.

---

## `FreeRTOS/FreeRTOS/Source/include/task.h`

### `xTaskCreateEDF()` Prototype — NEW

Added inside `#if ( configUSE_EDF_SCHEDULER == 1 )`, before the existing
`xTaskCreate()` declaration in the Task Creation API section.

---

## `LedTest/FreeRTOSConfig.h`

### New Defines

- `configUSE_EDF_SCHEDULER` — set to 1 to enable EDF, 0 for stock FreeRTOS

### Modified Defines

- `configUSE_APPLICATION_TASK_TAG` — changed from 0 to 1 (required for GPIO
  trace hooks)

### Trace Hook Macros — NEW

- `traceTASK_SWITCHED_IN()` — sets the current task's tagged GPIO pin HIGH
- `traceTASK_SWITCHED_OUT()` — sets the current task's tagged GPIO pin LOW

These macros call `vTracePinHigh()` / `vTracePinLow()` (defined in `main_edf.c`),
which read the task's application tag to determine which GPIO to toggle. This
enables logic analyzer capture of the exact execution timeline including
preemption points.