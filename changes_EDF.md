# Changes — EDF

All files, functions changed, and new functions added to support EDF scheduling.

## Modified Files

### `FreeRTOS/FreeRTOS/Source/tasks.c`

1 _________________


#### `tskTCB` struct
Added the following fields inside `#if ( configUSE_EDF_SCHEDULER == 1 )`:
- `TickType_t xPeriod` — task period T in ticks
- `TickType_t xRelativeDeadline` — relative deadline D in ticks (D ≤ T)
- `TickType_t xWCET` — worst-case execution time C in ticks
- `TickType_t xAbsoluteDeadline` — current absolute deadline (release_time + D)
- `TickType_t xNextReleaseTime` — when the next job becomes ready
- `BaseType_t xIsEDFTask` — pdTRUE if this task uses EDF scheduling
- `UBaseType_t xDeadlineMissCount` — number of deadline misses detected

### `LedTest/FreeRTOSConfig.h`

Added:
- `configUSE_EDF_SCHEDULER` — set to 1 to enable EDF, 0 for stock FreeRTOS


2 _________________

### `xTaskCreateEDF()` — NEW FUNCTION in `tasks.c`
Creates an EDF-scheduled task. Wraps `prvCreateTask()` with priority 1, then sets
EDF-specific TCB fields (period, relative deadline, WCET, absolute deadline,
next release time). Sets `xStateListItem.xItemValue` to the absolute deadline
for sorted insertion into ready lists. Admission control is stubbed (TODO).

### `prvAddTaskToReadyList` macro — MODIFIED in `tasks.c`
Changed from always using `listINSERT_END()` (unsorted append) to conditionally
using `vListInsert()` (sorted by xItemValue) when the task is an EDF task
(`xIsEDFTask == pdTRUE`). Non-EDF tasks (idle, timer) still use `listINSERT_END()`.

### `task.h`
Added prototype for `xTaskCreateEDF()` inside `#if ( configUSE_EDF_SCHEDULER == 1 )`.


3 _________________

### `taskSELECT_HIGHEST_PRIORITY_TASK` macro — MODIFIED in `tasks.c`
Added EDF branch: when `configUSE_EDF_SCHEDULER == 1` and `uxTopPriority > 0`
(i.e., not the idle task), selects the head of the sorted ready list via
`listGET_HEAD_ENTRY()` instead of round-robin via `listGET_OWNER_OF_NEXT_ENTRY()`.
Since EDF tasks are inserted sorted by absolute deadline, the head is always the
task with the earliest deadline.

4.__________________
### `xTaskIncrementTick()` — MODIFIED in `tasks.c`

**Deadline update on wake-up (before `prvAddTaskToReadyList`):**
When a task is moved from the delayed list back to the ready list, if it is an
EDF task, its absolute deadline and next release time are updated before
re-insertion:
- `xAbsoluteDeadline = xNextReleaseTime + xRelativeDeadline`
- `xNextReleaseTime += xPeriod`
- `xStateListItem.xItemValue` is set to the new absolute deadline

This ensures the task enters the ready list sorted by its new (future) deadline
rather than the old (expired) one.

**Deadline-aware preemption check (after `prvAddTaskToReadyList`):**
The existing preemption check compared `uxPriority` to decide if a context switch
was needed. Added an EDF branch: if both the waking task and the currently running
task are EDF tasks, compare `xAbsoluteDeadline` instead of `uxPriority`. A context
switch is triggered if the waking task has an earlier deadline than the currently
running task.

### `FreeRTOSConfig.h` — MODIFIED
- Set `configUSE_APPLICATION_TASK_TAG` to `1` (was `0`)
- Added `traceTASK_SWITCHED_IN` and `traceTASK_SWITCHED_OUT` hook macros that
  toggle GPIO pins based on task tags, enabling logic analyzer capture of which
  task is executing at any moment.