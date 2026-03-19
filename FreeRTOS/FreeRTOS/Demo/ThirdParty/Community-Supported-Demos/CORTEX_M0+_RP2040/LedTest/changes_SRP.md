# Changes — Stack Resource Policy (SRP)

All kernel modifications to support SRP. Every change is wrapped in
`#if ( configUSE_SRP == 1 )` nested inside `#if ( configUSE_EDF_SCHEDULER == 1 )`
unless otherwise noted.

---

## Modified Files Summary

| File | What Changed |
|------|-------------|
| `tasks.c` | TCB struct (added `uxPreemptionLevel`), system ceiling stack globals, ceiling push/pop/get functions, preemption check helper, preemption test in tick handler, admission control with blocking, SRP resource registry |
| `task.h` | Prototypes for `vSRPPushCeiling()`, `vSRPPopCeiling()`, `uxSRPGetSystemCeiling()`, `xSRPCheckPreemptionNeeded()`, `vSRPRegisterResource()` |
| `queue.c` | `xQUEUE` struct (added `uxResourceCeiling`, `xMaxCSLength`), init in `prvInitialiseNewQueue()`, new `xQueueCreateBinarySemaphoreSRP()`, SRP fast path in `xQueueSemaphoreTake()`, SRP ceiling pop in `prvCopyDataToQueue()` |
| `queue.h` | Prototype for `xQueueCreateBinarySemaphoreSRP()` |
| `semphr.h` | `xSemaphoreCreateBinarySRP()` macro |
| `FreeRTOSConfig.h` | `configUSE_SRP`, `configMAX_SRP_NESTING`, `configMAX_PREEMPTION_LEVEL`, `configMAX_SRP_RESOURCES` |

---

## `FreeRTOS/FreeRTOS/Source/tasks.c`

### 1. TCB Extension — `tskTCB` struct MODIFIED

Added field inside `#if ( configUSE_SRP == 1 )`, nested inside the existing
`#if ( configUSE_EDF_SCHEDULER == 1 )` block:

| Field | Type | Purpose |
|-------|------|---------|
| `uxPreemptionLevel` | `UBaseType_t` | π — static preemption level, shorter D = higher value |

### 2. System Ceiling Stack — NEW GLOBALS

Added inside `#if ( configUSE_SRP == 1 )`:

| Variable | Type | Purpose |
|----------|------|---------|
| `uxSystemCeiling` | `UBaseType_t` | Current system ceiling (Πs), 0 when no resources held |
| `uxCeilingStack[]` | `UBaseType_t[32]` | Stack of previous ceiling values for nested acquisitions |
| `uxCeilingStackIndex` | `UBaseType_t` | Current stack top index |

### 3. SRP Resource Registry — NEW GLOBALS

Added inside `#if ( configUSE_SRP == 1 )`:

| Variable | Type | Purpose |
|----------|------|---------|
| `xSRPResourceRegistry[]` | struct array | Registry of SRP semaphores: ceiling + max CS length |
| `uxSRPResourceCount` | `UBaseType_t` | Number of registered SRP resources |

Used by admission control to compute blocking factors.

### 4. Preemption Level Computation — `xTaskCreateEDF()` MODIFIED

After setting `xDeadlineMissCount = 0`, added:

```c
#if ( configUSE_SRP == 1 )
    pxNewTCB->uxPreemptionLevel = configMAX_PREEMPTION_LEVEL - xRelativeDeadline;
#endif
```

Shorter deadline → higher preemption level. Tasks with the same D get the
same π.

### 5. Ceiling Manipulation — NEW FUNCTIONS

**`vSRPPushCeiling( UBaseType_t uxResourceCeiling )`**
Saves current system ceiling on the stack, then raises Πs to the resource
ceiling if it is higher. Called from `xQueueSemaphoreTake()` SRP path.

**`vSRPPopCeiling( void )`**
Restores the previous system ceiling from the stack. Called from
`prvCopyDataToQueue()` SRP path.

**`uxSRPGetSystemCeiling( void )`**
Returns the current system ceiling. Used by test programs for debug output.

**`xSRPCheckPreemptionNeeded( void )`**
Walks the priority-1 ready list (EDF tasks sorted by absolute deadline).
For each task, checks if `uxPreemptionLevel > uxSystemCeiling`. The first
task that passes and has an earlier absolute deadline than `pxCurrentTCB`
triggers a context switch (returns `pdTRUE`). Called after releasing a
resource to check if the lowered ceiling enables a waiting task to preempt.

**`vSRPRegisterResource( UBaseType_t uxCeiling, TickType_t xMaxCSLength )`**
Adds a resource to `xSRPResourceRegistry[]` for admission control. Called
from `xQueueCreateBinarySemaphoreSRP()`.

### 6. Blocking Time Computation — NEW FUNCTION

**`prvSRPComputeBlockingTime( UBaseType_t uxPreemptionLevel )`**
Scans `xSRPResourceRegistry[]` and returns the longest critical section
length among resources whose ceiling ≥ the given preemption level. Implements
equation 7.18 from the textbook.

### 7. Admission Control — `prvEDFCheckLLBound()` MODIFIED

Added SRP blocking factor computation after the utilization sum. For each
registered task, computes Bi via `prvSRPComputeBlockingTime()` and adds the
maximum `Bi/Ti` ratio to the total utilization. Implements equation 7.20:

    Σ(Ci/Ti) + max_i(Bi/Ti) ≤ 1

### 8. Admission Control — `prvEDFCheckProcessorDemand()` MODIFIED

At each testing point L, adds the maximum critical section length across all
SRP resources to the processor demand. Conservative over-approximation that
ensures no false acceptance.

### 9. Preemption Test — `xTaskIncrementTick()` MODIFIED

The existing EDF preemption check was extended. Original:

```c
if( pxTCB->xAbsoluteDeadline < pxCurrentTCB->xAbsoluteDeadline )
```

With SRP:

```c
if( ( pxTCB->xAbsoluteDeadline < pxCurrentTCB->xAbsoluteDeadline ) &&
    ( pxTCB->uxPreemptionLevel > uxSystemCeiling ) )
```

This prevents a waking task from preempting if its preemption level does not
exceed the current system ceiling.

---

## `FreeRTOS/FreeRTOS/Source/include/task.h`

### SRP Function Prototypes — NEW

Added inside `#if ( configUSE_SRP == 1 )`, nested within the existing
`#if ( configUSE_EDF_SCHEDULER == 1 )` block:

- `vSRPPushCeiling()`
- `vSRPPopCeiling()`
- `uxSRPGetSystemCeiling()`
- `xSRPCheckPreemptionNeeded()`
- `vSRPRegisterResource()`

---

## `FreeRTOS/FreeRTOS/Source/queue.c`

### 1. Queue Struct — `xQUEUE` MODIFIED

Added inside `#if ( configUSE_SRP == 1 )`:

| Field | Type | Purpose |
|-------|------|---------|
| `uxResourceCeiling` | `UBaseType_t` | SRP resource ceiling (0 = not SRP semaphore) |
| `xMaxCSLength` | `TickType_t` | Worst-case critical section length for admission control |

### 2. Queue Initialization — `prvInitialiseNewQueue()` MODIFIED

After the queue sets initialization block, added:

```c
#if ( configUSE_SRP == 1 )
    pxNewQueue->uxResourceCeiling = 0;
    pxNewQueue->xMaxCSLength = 0;
#endif
```

Ensures all non-SRP semaphores have `uxResourceCeiling = 0` and are treated
as regular semaphores.

### 3. SRP Semaphore Creation — `xQueueCreateBinarySemaphoreSRP()` NEW

Creates a standard binary semaphore via `xQueueGenericCreate()`, gives it
once (so it starts available), then sets `uxResourceCeiling` and
`xMaxCSLength`. Also registers the resource for admission control via
`vSRPRegisterResource()`.

### 4. SRP Take Path — `xQueueSemaphoreTake()` MODIFIED

Added SRP fast path before the `for( ;; )` blocking loop:

```c
#if ( configUSE_SRP == 1 )
if( pxQueue->uxResourceCeiling > 0 )
{
    configASSERT( pxQueue->uxMessagesWaiting > 0 );
    pxQueue->uxMessagesWaiting = 0;
    vSRPPushCeiling( pxQueue->uxResourceCeiling );
    return pdPASS;
}
#endif
```

The assert verifies the core SRP guarantee — if it ever fires, the protocol
is broken. Under correct SRP operation, the resource is always available when
a task accesses it.

### 5. SRP Give Path — `prvCopyDataToQueue()` MODIFIED

In the `uxItemSize == 0` branch (semaphore path), added SRP check before the
existing mutex priority disinheritance code:

```c
#if ( configUSE_SRP == 1 )
if( pxQueue->uxResourceCeiling > 0 )
{
    vSRPPopCeiling();
    xReturn = xSRPCheckPreemptionNeeded();
}
else
#endif
```

Pops the ceiling stack and checks if any ready task should now preempt. The
`uxMessagesWaiting` increment at the end of `prvCopyDataToQueue` restores the
semaphore to available state.

---

## `FreeRTOS/FreeRTOS/Source/include/queue.h`

### `xQueueCreateBinarySemaphoreSRP()` Prototype — NEW

Added inside `#if ( configUSE_SRP == 1 )`.

---

## `FreeRTOS/FreeRTOS/Source/include/semphr.h`

### `xSemaphoreCreateBinarySRP()` Macro — NEW

```c
#define xSemaphoreCreateBinarySRP( uxCeiling, xMaxCSLen ) \
    xQueueCreateBinarySemaphoreSRP( ( uxCeiling ), ( xMaxCSLen ) )
```

Added inside `#if ( configUSE_SRP == 1 )`.

---

## `LedTest/FreeRTOSConfig.h`

### New Defines

| Define | Value | Purpose |
|--------|-------|---------|
| `configUSE_SRP` | 1 | Enable SRP (requires `configUSE_EDF_SCHEDULER == 1`) |
| `configMAX_SRP_NESTING` | 32 | Maximum nested resource acquisitions |
| `configMAX_PREEMPTION_LEVEL` | 0xFFFF | Base for preemption level computation |
| `configMAX_SRP_RESOURCES` | 16 | Maximum SRP semaphores for admission control |
