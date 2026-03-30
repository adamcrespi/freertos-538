# Multiprocessor EDF — Code Changes

## Branch: `multiprocessor`
Created from `edf` branch.

---

## `FreeRTOS/Source/include/task.h`

- Added `BaseType_t xCorePreference` parameter to `xTaskCreateEDF` prototype
  (between `xWCET` and `pxCreatedTask`).
  - `-1` = global/auto
  - `0` or `1` = pin to specific core (partitioned mode)

---

## `FreeRTOS/Source/tasks.c`

### 0. Default-mode guard (inside `#if configUSE_EDF_SCHEDULER == 1`)
Added a compile-time default so that if neither `GLOBAL_EDF_ENABLE` nor
`PARTITIONED_EDF_ENABLE` is set to `1` by the user, the kernel automatically
enables Global EDF (as required by the assignment spec):
```c
#if ( configNUMBER_OF_CORES > 1 )
    #if !defined( GLOBAL_EDF_ENABLE ) && !defined( PARTITIONED_EDF_ENABLE )
        #define GLOBAL_EDF_ENABLE      1
        #define PARTITIONED_EDF_ENABLE 0
    #elif ( GLOBAL_EDF_ENABLE == 0 ) && ( PARTITIONED_EDF_ENABLE == 0 )
        #undef  GLOBAL_EDF_ENABLE
        #define GLOBAL_EDF_ENABLE 1   /* both-zero → default global */
    #endif
#endif
```

### 1. Forward declaration (near line 605)
```c
#if ( configNUMBER_OF_CORES > 1 ) && ( configUSE_EDF_SCHEDULER == 1 )
    static void prvYieldForEDFTask( const TCB_t * pxTCB );
#endif
```

### 2. `prvYieldForEDFTask` implementation (before `prvSelectHighestPriorityTask`)
New function that replaces `prvYieldForTask` in the tick interrupt path for
EDF tasks.  Iterates all cores and yields any core whose current task has a
later absolute deadline than the newly-ready task.  Respects affinity masks.

### 3. `prvSelectHighestPriorityTask` — insertion fix
Changed `vListInsertEnd` to `vListInsert` for EDF tasks when re-inserting the
current task before searching for the next one.  This preserves the
deadline-sorted order of the EDF ready list.

### 4. `xTaskIncrementTick` — SMP preemption path
Replaced generic `prvYieldForTask(pxTCB)` with `prvYieldForEDFTask(pxTCB)` in
the SMP (multi-core) code path when the unblocked task is an EDF task.

### 5. `xTaskCreateEDF` — new signature + partitioned support
New `xCorePreference` parameter.  Added:
- Per-core admission control (`ulCoreUtil[]`, `uxCoreTaskCount[]`)
- `prvPartitionedAssignCore()` — worst-fit heuristic
- `prvPartitionedRegister()` — records task on assigned core
- Affinity mask set to `0x3` (global) or `1<<core` (partitioned)

### 6. Static data (partitioned mode)
```c
#if ( PARTITIONED_EDF_ENABLE == 1 )
static UBaseType_t uxCoreTaskCount[ configNUMBER_OF_CORES ];
static uint32_t    ulCoreUtil[ configNUMBER_OF_CORES ];
#endif
```

---

## `LedTest/FreeRTOSConfig.h`

Full rewrite.  Key additions vs single-core EDF config:
- `configNUMBER_OF_CORES 2`
- `configRUN_MULTIPLE_PRIORITIES 1`
- `configUSE_CORE_AFFINITY 1`
- `configTASK_DEFAULT_CORE_AFFINITY 0x3`
- `portSUPPORT_SMP 1`
- `configUSE_PASSIVE_IDLE_HOOK 0`
- `GLOBAL_EDF_ENABLE 1` / `PARTITIONED_EDF_ENABLE 0`
- `INCLUDE_vTaskCoreAffinitySet/Get 1`

---

## New Test Programs

| File                     | Target               | Description                                      |
|--------------------------|----------------------|--------------------------------------------------|
| `main_mp_global.c`       | `mp_global_test`     | 3-task global EDF, implicit deadline, migration  |
| `main_mp_partitioned.c`  | `mp_partitioned_test`| 3-task partitioned EDF, implicit deadline, no migration |

GP16–GP18 connect to AD2 DIO0–DIO2 for logic analyzer capture.

Task parameters (implicit deadline, D=T):

**Global test:**
- τ1 GP16: C=300ms, T=500ms, U=0.600
- τ2 GP17: C=350ms, T=700ms, U=0.500
- τ3 GP18: C=360ms, T=900ms, U=0.400 — Total U=1.500

**Partitioned test:**
- Core 0: τ1 GP16 (C=250, T=500, U=0.500), τ2 GP17 (C=280, T=700, U=0.400) — Core 0 U=0.900
- Core 1: τ3 GP18 (C=360, T=900, U=0.400) — Core 1 U=0.400

---

## Updated Files

| File              | Change                                              |
|-------------------|-----------------------------------------------------|
| `main_edf.c`      | Added `xCorePreference = -1` to all `xTaskCreateEDF` calls |
| `CMakeLists.txt`  | Added `mp_global_test` and `mp_partitioned_test` targets |

---

## New Scripts

| File                  | Description                                        |
|-----------------------|----------------------------------------------------|
| `capture_gantt_mp.py` | Capture 4-channel AD2 data, render Gantt with SMP  |
|                       | parallel-execution highlighting (purple shading)   |

---

## Bug fixes applied during testing

### B6: Trace hooks — `xTaskGetApplicationTaskTagFromISR` (both test files)

`vTracePinHigh` and `vTracePinLow` originally called
`xTaskGetApplicationTaskTag(NULL)`.  That function enters a task-level critical
section (`vTaskEnterCritical`) which calls `prvCheckForRunStateChange`.  When
called from within `vTaskSwitchContext` (via `traceTASK_SWITCHED_OUT` /
`traceTASK_SWITCHED_IN`), the task state can be `taskTASK_SCHEDULED_TO_YIELD`,
causing `prvCheckForRunStateChange` to spin forever — core 1 never runs.

Fixed in both `main_mp_global.c` and `main_mp_partitioned.c`:
```c
// Before (broken):
uint32_t pin = (uint32_t)xTaskGetApplicationTaskTag( NULL );
// After (fixed):
uint32_t pin = (uint32_t)xTaskGetApplicationTaskTagFromISR( NULL );
```

The `FromISR` variant uses `vTaskEnterCriticalFromISR()` which acquires only
the ISR spinlock and never calls `prvCheckForRunStateChange`.
