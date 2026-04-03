# Multiprocessor EDF — Known Bugs & Issues

## B1: `vListInsertEnd` corrupts EDF ready list in SMP context

**Status:** Fixed

**Description:**
`prvSelectHighestPriorityTask` (SMP path, `configNUMBER_OF_CORES > 1`) moves
the currently-running task to the tail of the ready list using `vListInsertEnd`
before selecting the next task.  This is correct for round-robin scheduling
(stock FreeRTOS) but destroys the deadline-sorted order that EDF requires.

**Root cause:**
`vListInsert` inserts by `xItemValue` (absolute deadline); `vListInsertEnd`
always appends to the tail, ignoring sort key.

**Fix:**
Added conditional in `prvSelectHighestPriorityTask`:
```c
#if ( configUSE_EDF_SCHEDULER == 1 )
if( pxCurrentTCBs[ xCoreID ]->xIsEDFTask == pdTRUE )
    vListInsert( ... );    // sorted by deadline
else
#endif
    vListInsertEnd( ... ); // round-robin tail append
```

**Impact if unpatched:** EDF task selection degrades to FIFO/random order;
tasks do not run by earliest deadline; deadline misses occur immediately.

---

## B2: SMP preemption uses priority, not deadline

**Status:** Fixed

**Description:**
`taskYIELD_ANY_CORE_IF_USING_PREEMPTION` (used in `prvAddNewTaskToReadyList`,
task-unblock, and notification paths) maps to `prvYieldForTask(pxTCB)` under
SMP.  `prvYieldForTask` compares priorities — since all EDF tasks share
priority 1, it never triggers a cross-core deadline-based preemption.

**Fix:**
Added `prvYieldForEDFTask` which checks absolute deadlines and calls
`prvYieldCore(xCoreID)` for any core running a task with a later deadline.
This is called from `xTaskIncrementTick` when a sleeping EDF task wakes.

**Remaining limitation:** Dynamic task creation while the scheduler is running
(after `vTaskStartScheduler`) still uses the priority-based path.  For this
assignment all tasks are created before the scheduler starts, so this is not
an issue in practice.

---

## B3: `configRUN_MULTIPLE_PRIORITIES` must be 1

**Status:** Configuration requirement (documented)

**Description:**
With `configRUN_MULTIPLE_PRIORITIES = 0`, the FreeRTOS SMP kernel prevents a
lower-priority task from running on a core while a higher-priority task exists
on any core.  Since all EDF tasks are priority 1 and idle tasks are priority 0,
this would mean only one core can ever run an EDF task at a time — defeating
dual-core EDF entirely.

**Fix:** `configRUN_MULTIPLE_PRIORITIES = 1` is set in `FreeRTOSConfig.h`.

---

## B4: Partitioned EDF auto-assign may differ between compilations

**Status:** Known / acceptable

**Description:**
`prvPartitionedAssignCore` uses worst-fit bin packing (assign to the core with
most remaining capacity).  The assignment depends on task creation order.  If
tasks are created in a different order across builds, core assignments may
differ.  This is expected behaviour for worst-fit and does not affect
correctness — it only affects which tasks end up on which core.

---

## B5: No dynamic task admission in partitioned mode at runtime

**Status:** By design

**Description:**
The per-core registry (`ulCoreUtil`, `uxCoreTaskCount`) is initialized to zero
and never decremented (tasks are never deleted in these tests).  If tasks were
deleted at runtime, the registry would not be updated, and the freed capacity
would never become available for new tasks.

**Mitigation:** Task deletion is not used in the test programs.  A future
implementation would need `vTaskDeleteEDF` to decrement the registry.

---

## B6: Trace hooks call `xTaskGetApplicationTaskTag` inside `vTaskSwitchContext` — core deadlocks

**Status:** Fixed

**Description:**
`traceTASK_SWITCHED_OUT()` and `traceTASK_SWITCHED_IN()` are both defined to
call `xTaskGetApplicationTaskTag(NULL)`.  That function enters a task-level
critical section via `vTaskEnterCritical()`, which (for SMP, when nesting
count is 0) acquires both spinlocks and then calls `prvCheckForRunStateChange()`.

`prvCheckForRunStateChange` spins in a loop while
`pxCurrentTCBs[xCoreID]->xTaskRunState == taskTASK_SCHEDULED_TO_YIELD`.

The SMP time-slicing code in `xTaskIncrementTick` calls `prvYieldCore(xCoreID)`
for each core whose ready list at the running priority has more than one entry.
`prvYieldCore` for a remote core sets
`pxCurrentTCBs[xCoreID]->xTaskRunState = taskTASK_SCHEDULED_TO_YIELD` and
sends a FIFO interrupt.  When core 1 receives that interrupt and enters
`vTaskSwitchContext(1)`, `traceTASK_SWITCHED_OUT()` fires before
`prvSelectHighestPriorityTask(1)` has run.  At that point the task state is
still `taskTASK_SCHEDULED_TO_YIELD`, so `prvCheckForRunStateChange` enters its
spin loop.  Because `prvSelectHighestPriorityTask` (the only code that clears
the state to `taskTASK_NOT_RUNNING`) has not yet been called, the loop never
exits.  Core 1 is permanently stuck; only T1 on core 0 ever runs.

**Root cause:**
`xTaskGetApplicationTaskTag` is the *task-context* API.  Its internal critical
section calls `prvCheckForRunStateChange`, which is unsafe to call from within
`vTaskSwitchContext` because the scheduler state is mid-transition.

**Fix:**
Replace `xTaskGetApplicationTaskTag(NULL)` with
`xTaskGetApplicationTaskTagFromISR(NULL)` in both `vTracePinHigh` and
`vTracePinLow`.  The `FromISR` variant uses `vTaskEnterCriticalFromISR()`,
which acquires only the ISR spinlock and never calls
`prvCheckForRunStateChange`.

```c
void vTracePinHigh( void )
{
    uint32_t pin = (uint32_t)xTaskGetApplicationTaskTagFromISR( NULL );
    if( pin != 0 ) gpio_put( (uint)pin, 1 );
}
void vTracePinLow( void )
{
    uint32_t pin = (uint32_t)xTaskGetApplicationTaskTagFromISR( NULL );
    if( pin != 0 ) gpio_put( (uint)pin, 0 );
}
```

**Impact if unpatched:** Core 1 deadlocks on the first time-slice interrupt
after the scheduler starts.  Only tasks assigned to core 0 ever execute.

---

## B8: Partitioned EDF — tasks migrate to wrong core when multiple tasks are pinned to the same core

**Status:** Known / open

**Description:**
In partitioned EDF mode, tasks pinned to the same core (e.g., τ1 and τ2 both
assigned to core 0 via `uxCoreAffinityMask = 0x1`) intermittently execute on
the wrong core.  Serial output confirms the migration:
```
[T1] *** MIGRATION DETECTED: job 267 on core 1 (expected 0)! ***
```
The migration begins after several hundred jobs and increases in frequency over
time.  Admission control, task creation, and initial scheduling all appear
correct; the violation starts mid-run.

**Root cause:**
The stock FreeRTOS SMP kernel contains an affinity optimisation in
`prvSelectHighestPriorityTask` (`tasks.c`, around the `pxPreviousTCB` block):

```c
/* Strip cores where the new task can run from the evicted task's search map. */
uxCoreMap &= ~( pxCurrentTCBs[ xCoreID ]->uxCoreAffinityMask );
```

When task τ2 (mask `0x1`) preempts task τ1 (mask `0x1`) on core 0, this
optimisation computes:
```
uxCoreMap = τ1->mask = 0x1
uxCoreMap &= ~(τ2->mask) = ~0x1 = 0xFE
uxCoreMap &= ((1<<2)-1) = 0xFE & 0x3 = 0x2   ← core 1 only
```
The optimisation concludes "search core 1 for τ1" and issues a spurious
`prvYieldCore(1)`.  The affinity guard inside `prvSelectHighestPriorityTask`
(`uxCoreAffinityMask & (1 << xCoreID)`) *should* then prevent core 1 from
actually scheduling τ1, but under SMP race conditions between the two cores'
context-switch paths this protection fails intermittently, resulting in τ1
running on core 1.

**Why the optimisation is wrong here:**
The optimisation was designed for tasks with flexible affinity masks (e.g.,
`0x3`), where evicting task B from core 0 with task A means B can legitimately
migrate to core 1.  It does not handle the case where both tasks share an
identical single-core mask — there is no alternative core to migrate to, but
the optimisation strips the only valid core from the search map.

**Impact:**
- Tasks execute on the wrong core, violating the partitioned EDF invariant.
- GPIO trace hooks toggle on the wrong core, producing incorrect Gantt charts.
- Migration frequency grows over time as scheduling events accumulate.
- τ1 and τ2 deadline miss counts are inflated by the spurious context switches.

**Workaround / mitigation:**
Assign at most one EDF task per core to avoid the triggering condition entirely.
With one task per core, no two tasks share the same affinity mask, so the
optimisation's result always points to a valid alternative core (or zero cores,
in which case no spurious yield is sent).

A kernel-level fix would require either:
(a) Skipping the optimisation when `uxCoreMap` becomes 0 after the mask
    operation (i.e., the evicted task has nowhere else to go), or
(b) Replacing the optimisation with a full per-core affinity scan that respects
    single-core pinning.

---

## B7: Global and partitioned builds share one `FreeRTOSConfig.h`

**Status:** By design / workflow requirement

**Description:**
`mp_global_test` and `mp_partitioned_test` link against the same
`FreeRTOS-Kernel` CMake target, which is compiled from a single
`FreeRTOSConfig.h`.  The scheduling mode flags
(`GLOBAL_EDF_ENABLE` / `PARTITIONED_EDF_ENABLE`) are compile-time `#define`s
in that file, so only one mode can be active in a given build.

If `GLOBAL_EDF_ENABLE = 1` is left set and `mp_partitioned_test` is
(re-)built, the affinity-mask assignment inside `xTaskCreateEDF` takes the
global path (`uxCoreAffinityMask = 0x3` for all tasks).  Tasks then migrate
freely despite `xCorePreference` being set to 0 or 1.

The admission-control rejection test in `mp_partitioned_test` can still
produce a spurious "REJECTED" when both flags are 0, because
`prvEDFAdmissionControl` falls back to the single-core bound (U ≤ 1.0) and
the combined task set exceeds that bound — even though the per-core bound
would have accepted it in true partitioned mode.

**Workaround:** Before building `mp_partitioned_test`, set:
```c
#define GLOBAL_EDF_ENABLE      0
#define PARTITIONED_EDF_ENABLE 1
```
Restore `GLOBAL_EDF_ENABLE 1 / PARTITIONED_EDF_ENABLE 0` before rebuilding
`mp_global_test`.  Only one binary needs to be on the device at a time.
