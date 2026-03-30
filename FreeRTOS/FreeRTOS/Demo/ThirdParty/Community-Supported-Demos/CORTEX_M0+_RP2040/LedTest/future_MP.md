# Multiprocessor EDF — Future Work

## F1: EDF-aware `taskYIELD_ANY_CORE_IF_USING_PREEMPTION`

Currently only `xTaskIncrementTick` (the tick ISR path) calls
`prvYieldForEDFTask`.  Other paths that make tasks ready — task notifications,
semaphore gives, `vTaskResume`, and dynamic task creation at runtime — still
use priority-based `prvYieldForTask`.

A more complete fix would replace the macro definition:
```c
#define taskYIELD_ANY_CORE_IF_USING_PREEMPTION( pxTCB ) \
    ( ( pxTCB )->xIsEDFTask ? prvYieldForEDFTask(pxTCB) : prvYieldForTask(pxTCB) )
```
This would give correct EDF preemption for all scheduling events, not just
periodic wakeups.

---

## F2: `vTaskDeleteEDF` — runtime capacity reclaim

`xTaskCreateEDF` registers task parameters in the admission control data
structures (`ulCoreUtil`, `uxCoreTaskCount`, `xEDFTaskRegistry`).  Deleting an
EDF task does not update these, so the capacity is permanently lost.

A future `vTaskDeleteEDF(TaskHandle_t xTask)` wrapper should:
1. Find the task in the registry by handle.
2. Decrement `ulCoreUtil[core]` and `uxCoreTaskCount[core]`.
3. Remove from `xEDFTaskRegistry`.
4. Call the standard `vTaskDelete`.

---

## F3: EDF-RM hybrid scheduling

Allow real-time tasks to mix EDF (dynamic priority) and RM (static priority)
within the same system.  EDF tasks would occupy a dedicated priority band;
RM tasks above or below.  Requires careful interaction with the ready-list
structure and the SMP preemption logic.

---

## F4: Pfair / LLREF multiprocessor scheduling

For tighter global multiprocessor utilization bounds (approaching 100% of m
cores), implement Pfair or LLREF.  These require sub-job splitting and finer-
grained scheduling decisions, which would require more significant FreeRTOS
changes.

---

## F5: Core migration cost model

Global EDF migrates tasks freely, which has real cache-flush costs on
hardware with per-core caches.  A future extension could track migration count
and optionally prefer the last-used core as a tie-breaker when two tasks have
equal deadlines.

---

## F6: Three or more cores

The implementation uses `configNUMBER_OF_CORES` throughout and is not
hard-coded to 2 cores.  It should generalize to RP2350 (2 cores) or other
multi-core Cortex-M platforms.  The partitioned worst-fit heuristic already
iterates all cores.  The global utilization bound is
`EDF_UTIL_SCALE * configNUMBER_OF_CORES`.

---

## F7: Per-target CMake compile definitions to eliminate config swap

Both test binaries currently share one `FreeRTOS-Kernel` CMake target compiled
from a single `FreeRTOSConfig.h`, so `GLOBAL_EDF_ENABLE` and
`PARTITIONED_EDF_ENABLE` must be manually toggled between builds.

A cleaner solution is to add per-target compile definitions in
`CMakeLists.txt` and remove those two `#define`s from `FreeRTOSConfig.h`:

```cmake
target_compile_definitions(mp_global_test PRIVATE
    GLOBAL_EDF_ENABLE=1 PARTITIONED_EDF_ENABLE=0)
target_compile_definitions(mp_partitioned_test PRIVATE
    GLOBAL_EDF_ENABLE=0 PARTITIONED_EDF_ENABLE=1)
```

This requires splitting the shared `FreeRTOS-Kernel` library into two separate
CMake `OBJECT` or `INTERFACE` targets (one per mode) so each binary gets its
own compiled copy of `tasks.c` with the correct defines.  Not strictly
necessary for the assignment but eliminates the manual swap step.
