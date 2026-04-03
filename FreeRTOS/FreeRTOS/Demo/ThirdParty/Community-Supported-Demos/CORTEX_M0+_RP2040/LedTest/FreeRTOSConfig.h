/*
 * FreeRTOS V202212.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ── EDF / MP extensions ─────────────────────────────────────── */
#define configUSE_EDF_SCHEDULER         1
#define configUSE_APPLICATION_TASK_TAG  1

/*
 * Multiprocessor scheduling mode — pick exactly one:
 *   GLOBAL_EDF_ENABLE      1 : single global EDF queue, tasks run on any core
 *   PARTITIONED_EDF_ENABLE 1 : tasks pinned to a core at creation, per-core EDF
 *
 * Default: Global EDF.
 */
#define GLOBAL_EDF_ENABLE               1
#define PARTITIONED_EDF_ENABLE          0

/* ── GPIO trace hooks for AD2 logic analyzer ─────────────────── */
extern void vTracePinHigh(void);
extern void vTracePinLow(void);
#define traceTASK_SWITCHED_IN()  vTracePinHigh()
#define traceTASK_SWITCHED_OUT() vTracePinLow()

/* ── Scheduler ───────────────────────────────────────────────── */
#define configUSE_PREEMPTION                    1
#define configUSE_TICKLESS_IDLE                 0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     1
#define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES                    32
#define configMINIMAL_STACK_SIZE                ( configSTACK_DEPTH_TYPE ) 256
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1

/* ── Synchronization ─────────────────────────────────────────── */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 5

/* ── System ──────────────────────────────────────────────────── */
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

/* ── Memory ──────────────────────────────────────────────────── */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (128*1024)
#define configAPPLICATION_ALLOCATED_HEAP        0

/* ── Hooks ───────────────────────────────────────────────────── */
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/* ── Stats ───────────────────────────────────────────────────── */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* ── Co-routines ─────────────────────────────────────────────── */
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/* ── Software timers ─────────────────────────────────────────── */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            1024

/* ── SMP — dual-core RP2040 ──────────────────────────────────── */
#define configNUMBER_OF_CORES                   2
#define configTICK_CORE                         0

/* configRUN_MULTIPLE_PRIORITIES = 1 is required so both cores can
 * simultaneously execute EDF tasks (all at priority 1).  With 0,
 * only one core would run user tasks, defeating dual-core EDF. */
#define configRUN_MULTIPLE_PRIORITIES           1

/* configUSE_CORE_AFFINITY = 1 enables uxCoreAffinityMask in the TCB.
 * Global EDF: mask = 0x3 (both cores).
 * Partitioned EDF: mask = 0x1 or 0x2 to pin the task to a core. */
#define configUSE_CORE_AFFINITY                 1

/* Default affinity = both cores.  xTaskCreateEDF overrides for partitioned. */
#define configTASK_DEFAULT_CORE_AFFINITY        ( ( UBaseType_t ) 0x3 )

/* ── RP2040 / SMP ────────────────────────────────────────────── */
#define configSUPPORT_PICO_SYNC_INTEROP         1
#define configSUPPORT_PICO_TIME_INTEROP         1
#define configUSE_PASSIVE_IDLE_HOOK             0
#define portSUPPORT_SMP                         1

#include <assert.h>
#define configASSERT(x)                         assert(x)

/* ── Optional API ────────────────────────────────────────────── */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xTaskResumeFromISR              1
#define INCLUDE_xQueueGetMutexHolder            1
#define INCLUDE_vTaskCoreAffinitySet            1
#define INCLUDE_vTaskCoreAffinityGet            1

#endif /* FREERTOS_CONFIG_H */
