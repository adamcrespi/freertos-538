/*
 * Partitioned EDF Multiprocessor Test
 *
 * Demonstrates Partitioned EDF on the RP2040 dual-core Cortex-M0+.
 * Tasks are pinned to a specific core at creation via xCorePreference.
 * Each core runs its own independent EDF scheduler.  Tasks NEVER migrate.
 *
 * Task Set — implicit deadline (D=T), times in ms = ticks at 1ms/tick:
 *
 *   Core 0:
 *     τ1 (GP16): C=250, D=500,  T=500   U=0.500
 *     τ2 (GP17): C=280, D=700,  T=700   U=0.400
 *     Core 0 total U = 0.900  (≤ 1.0)
 *
 *   Core 1:
 *     τ3 (GP18): C=360, D=900,  T=900   U=0.400
 *     Core 1 total U = 0.400  (≤ 1.0)
 *
 * AD2 Gantt chart:
 *   GP16/GP17 (D0/D1) show core 0 activity.
 *   GP18 (D2) shows core 1 activity.
 *   GP16/GP17 HIGH simultaneously with GP18 = SMP proof.
 *
 * Serial output always shows the same core number per task (no migration).
 *
 * Rejection test demonstrates per-core capacity enforcement:
 *   Adding a task on core 0 with U=0.60 pushes core 0 to 1.50 > 1.0 —
 *   rejected even though total system U would only be 1.30 < 2.0.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

/* ── GPIO Pins ──────────────────────────────────────────────── */
#define PIN_T1   16   /* Core 0 */
#define PIN_T2   17   /* Core 0 */
#define PIN_T3   18   /* Core 1 */

/* ── Task Parameters — implicit deadline (D=T), ms = ticks ─── */
#define T1_WCET      pdMS_TO_TICKS(250)
#define T1_DEADLINE  pdMS_TO_TICKS(500)
#define T1_PERIOD    pdMS_TO_TICKS(500)

#define T2_WCET      pdMS_TO_TICKS(280)
#define T2_DEADLINE  pdMS_TO_TICKS(700)
#define T2_PERIOD    pdMS_TO_TICKS(700)

#define T3_WCET      pdMS_TO_TICKS(360)
#define T3_DEADLINE  pdMS_TO_TICKS(900)
#define T3_PERIOD    pdMS_TO_TICKS(900)

/* ── Trace hooks ────────────────────────────────────────────── */
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

/* ── Task parameters ────────────────────────────────────────── */
typedef struct {
    uint       gpio;
    TickType_t xWCET;
    TickType_t xPeriod;
    const char *pcName;
    uint32_t   ulJobCount;
    BaseType_t xExpectedCore;
} PartTaskParams_t;

static PartTaskParams_t xP1 = { PIN_T1, T1_WCET, T1_PERIOD, "T1", 0, 0 };
static PartTaskParams_t xP2 = { PIN_T2, T2_WCET, T2_PERIOD, "T2", 0, 0 };
static PartTaskParams_t xP3 = { PIN_T3, T3_WCET, T3_PERIOD, "T3", 0, 1 };

static volatile uint32_t ulMigrationErrors = 0;

/* ── Busy-wait ──────────────────────────────────────────────── */
static void vBusyWait( TickType_t xTicks )
{
    TickType_t xStart = xTaskGetTickCount();
    while( ( xTaskGetTickCount() - xStart ) < xTicks )
    {
        __asm volatile ("nop");
    }
}

/* ── Task Function ──────────────────────────────────────────── */
static void vPartTask( void *pvParameters )
{
    PartTaskParams_t *p = (PartTaskParams_t *)pvParameters;
    TickType_t xLastWake;
    uint coreNow;

    gpio_init( p->gpio );
    gpio_set_dir( p->gpio, GPIO_OUT );
    gpio_put( p->gpio, 0 );

    vTaskSetApplicationTaskTag( NULL, (TaskHookFunction_t)(uintptr_t)p->gpio );

    xLastWake = xTaskGetTickCount();
    coreNow = get_core_num();

    printf( "[%s] start  core=%u (expected %d)  GP%u\n",
            p->pcName, coreNow, (int)p->xExpectedCore, p->gpio );

    if( (BaseType_t)coreNow != p->xExpectedCore )
    {
        printf( "[%s] *** WRONG CORE at start! ***\n", p->pcName );
        ulMigrationErrors++;
    }

    for( ;; )
    {
        vBusyWait( p->xWCET );

        coreNow = get_core_num();
        p->ulJobCount++;

        /* Verify no migration */
        if( (BaseType_t)coreNow != p->xExpectedCore )
        {
            printf( "[%s] *** MIGRATION DETECTED: job %lu on core %u (expected %d)! ***\n",
                    p->pcName, (unsigned long)p->ulJobCount,
                    coreNow, (int)p->xExpectedCore );
            ulMigrationErrors++;
        }

        if( ( p->ulJobCount % 10 ) == 0 )
        {
            printf( "[%s] job %lu  core=%u  errors=%lu\n",
                    p->pcName, (unsigned long)p->ulJobCount,
                    coreNow, (unsigned long)ulMigrationErrors );
        }

        xTaskDelayUntil( &xLastWake, p->xPeriod );
    }
}

/* ── FreeRTOS Hooks ─────────────────────────────────────────── */
void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName )
{
    (void)xTask;
    printf( "STACK OVERFLOW: %s\n", pcTaskName );
    for( ;; );
}

void vApplicationTickHook( void ) {}

void vApplicationMallocFailedHook( void )
{
    printf( "MALLOC FAILED\n" );
    for( ;; );
}

/* ── Main ───────────────────────────────────────────────────── */
int main( void )
{
    BaseType_t r;

    stdio_init_all();
    sleep_ms( 2000 );

    printf( "\n========================================\n" );
    printf( " Partitioned EDF  —  Dual-Core RP2040\n" );
    printf( "========================================\n" );
    printf( " Mode: PARTITIONED_EDF (tasks pinned)\n" );
    printf( " Tasks NEVER migrate between cores\n" );
    printf( "----------------------------------------\n" );
    printf( " Core 0:\n" );
    printf( "   τ1 GP%d  C=%3lu D=%3lu T=%4lu  U=%.3f\n",
            PIN_T1, (unsigned long)T1_WCET, (unsigned long)T1_DEADLINE,
            (unsigned long)T1_PERIOD, (float)T1_WCET/(float)T1_PERIOD );
    printf( "   τ2 GP%d  C=%3lu D=%3lu T=%4lu  U=%.3f\n",
            PIN_T2, (unsigned long)T2_WCET, (unsigned long)T2_DEADLINE,
            (unsigned long)T2_PERIOD, (float)T2_WCET/(float)T2_PERIOD );
    printf( "   Core 0 total U = %.3f\n",
            (float)T1_WCET/(float)T1_PERIOD + (float)T2_WCET/(float)T2_PERIOD );
    printf( " Core 1:\n" );
    printf( "   τ3 GP%d  C=%3lu D=%3lu T=%4lu  U=%.3f\n",
            PIN_T3, (unsigned long)T3_WCET, (unsigned long)T3_DEADLINE,
            (unsigned long)T3_PERIOD, (float)T3_WCET/(float)T3_PERIOD );
    printf( "   Core 1 total U = %.3f\n",
            (float)T3_WCET/(float)T3_PERIOD );
    printf( "========================================\n\n" );

    /* Pin τ1, τ2 to core 0 (xCorePreference=0) */
    r = xTaskCreateEDF( vPartTask, "T1", 512, &xP1,
                        T1_PERIOD, T1_DEADLINE, T1_WCET, 0, NULL );
    printf( "Create T1 (core 0): %s\n", r == pdPASS ? "OK" : "REJECTED" );

    r = xTaskCreateEDF( vPartTask, "T2", 512, &xP2,
                        T2_PERIOD, T2_DEADLINE, T2_WCET, 0, NULL );
    printf( "Create T2 (core 0): %s\n", r == pdPASS ? "OK" : "REJECTED" );

    /* Pin τ3, τ4 to core 1 (xCorePreference=1) */
    r = xTaskCreateEDF( vPartTask, "T3", 512, &xP3,
                        T3_PERIOD, T3_DEADLINE, T3_WCET, 1, NULL );
    printf( "Create T3 (core 1): %s\n", r == pdPASS ? "OK" : "REJECTED" );

    /*
     * Per-core rejection test:
     * Try to add a task on core 0 with U=0.60 (C=300, T=500).
     * Core 0 would be 0.900 + 0.600 = 1.500 > 1.0 → REJECTED.
     * Total system U would only be 1.300 < 2.0, but the per-core
     * partitioned bound still catches this.
     */
    r = xTaskCreateEDF( vPartTask, "Over0", 512, &xP1,
                        pdMS_TO_TICKS(500), pdMS_TO_TICKS(500), pdMS_TO_TICKS(300),
                        0, NULL );
    printf( "Create Over0 (U+0.60 on core 0, expect REJECTED): %s\n",
            r == pdPASS ? "ACCEPTED" : "REJECTED" );

    printf( "\nStarting scheduler...\n" );
    vTaskStartScheduler();

    printf( "ERROR: scheduler returned\n" );
    for( ;; );
}
