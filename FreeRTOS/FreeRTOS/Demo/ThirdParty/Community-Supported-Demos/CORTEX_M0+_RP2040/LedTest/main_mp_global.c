/*
 * Global EDF Multiprocessor Test
 *
 * Demonstrates Global EDF on the RP2040 dual-core Cortex-M0+.
 * All tasks share a single deadline-sorted ready queue; both cores
 * independently pick the earliest-deadline runnable task.
 *
 * Task Set — implicit deadline (D=T), times in ms = ticks at 1ms/tick:
 *   τ1 (GP16): C=300, D=500, T=500   U=0.600
 *   τ2 (GP17): C=350, D=700, T=700   U=0.500
 *   τ3 (GP18): C=360, D=900, T=900   U=0.400
 *   Total U = 1.500  (≤ 2.0 Global EDF bound — schedulable)
 *
 * AD2 Gantt chart proof of SMP:
 *   Connect GP16→D0, GP17→D1, GP18→D2.
 *   When two channels are HIGH simultaneously, two tasks are running
 *   in parallel — direct visual proof of dual-core execution.
 *   Use capture_gantt_mp.py to capture and render the Gantt chart.
 *
 * Serial output includes get_core_num() so you can confirm tasks
 * migrate freely between cores across jobs.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

/* ── GPIO Pins (match AD2 channels D0-D2) ───────────────────── */
#define PIN_T1   16
#define PIN_T2   17
#define PIN_T3   18

/* ── Task Parameters — implicit deadline (D=T), ms = ticks ─── */
#define T1_WCET      pdMS_TO_TICKS(300)
#define T1_DEADLINE  pdMS_TO_TICKS(500)
#define T1_PERIOD    pdMS_TO_TICKS(500)

#define T2_WCET      pdMS_TO_TICKS(350)
#define T2_DEADLINE  pdMS_TO_TICKS(700)
#define T2_PERIOD    pdMS_TO_TICKS(700)

#define T3_WCET      pdMS_TO_TICKS(360)
#define T3_DEADLINE  pdMS_TO_TICKS(900)
#define T3_PERIOD    pdMS_TO_TICKS(900)

/* ── Trace hooks — toggle the pin stored as the task tag ────── */
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

/* ── Per-task parameters ────────────────────────────────────── */
typedef struct {
    uint       gpio;
    TickType_t xWCET;
    TickType_t xPeriod;
    const char *pcName;
    uint32_t   ulJobCount;
} MPTaskParams_t;

static MPTaskParams_t xP1 = { PIN_T1, T1_WCET, T1_PERIOD, "T1", 0 };
static MPTaskParams_t xP2 = { PIN_T2, T2_WCET, T2_PERIOD, "T2", 0 };
static MPTaskParams_t xP3 = { PIN_T3, T3_WCET, T3_PERIOD, "T3", 0 };

/* ── Busy-wait helper ───────────────────────────────────────── */
static void vBusyWait( TickType_t xTicks )
{
    TickType_t xStart = xTaskGetTickCount();
    while( ( xTaskGetTickCount() - xStart ) < xTicks )
    {
        __asm volatile ("nop");
    }
}

/* ── EDF Task Function ──────────────────────────────────────── */
static void vMPTask( void *pvParameters )
{
    MPTaskParams_t *p = (MPTaskParams_t *)pvParameters;
    TickType_t xLastWake;

    gpio_init( p->gpio );
    gpio_set_dir( p->gpio, GPIO_OUT );
    gpio_put( p->gpio, 0 );

    /* Store GPIO pin in task tag for trace hooks */
    vTaskSetApplicationTaskTag( NULL, (TaskHookFunction_t)(uintptr_t)p->gpio );

    xLastWake = xTaskGetTickCount();

    printf( "[%s] start  core=%u  C=%lu T=%lu GP%u\n",
            p->pcName, (unsigned)get_core_num(),
            (unsigned long)p->xWCET, (unsigned long)p->xPeriod, p->gpio );

    for( ;; )
    {
        vBusyWait( p->xWCET );

        p->ulJobCount++;
        /* Every 10 jobs print which core we're on — proof of migration */
        if( ( p->ulJobCount % 10 ) == 0 )
        {
            printf( "[%s] job %lu  core=%u\n",
                    p->pcName, (unsigned long)p->ulJobCount,
                    (unsigned)get_core_num() );
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
    printf( " Global EDF  —  Dual-Core RP2040\n" );
    printf( "========================================\n" );
    printf( " Mode: GLOBAL_EDF (single shared queue)\n" );
    printf( " Tasks may migrate freely between cores\n" );
    printf( " AD2: simultaneous HIGH = SMP in action\n" );
    printf( "----------------------------------------\n" );
    printf( " τ1 GP%d  C=%3lu D=%3lu T=%4lu  U=%.3f\n",
            PIN_T1, (unsigned long)T1_WCET, (unsigned long)T1_DEADLINE,
            (unsigned long)T1_PERIOD, (float)T1_WCET/(float)T1_PERIOD );
    printf( " τ2 GP%d  C=%3lu D=%3lu T=%4lu  U=%.3f\n",
            PIN_T2, (unsigned long)T2_WCET, (unsigned long)T2_DEADLINE,
            (unsigned long)T2_PERIOD, (float)T2_WCET/(float)T2_PERIOD );
    printf( " τ3 GP%d  C=%3lu D=%3lu T=%4lu  U=%.3f\n",
            PIN_T3, (unsigned long)T3_WCET, (unsigned long)T3_DEADLINE,
            (unsigned long)T3_PERIOD, (float)T3_WCET/(float)T3_PERIOD );
    printf( " Total U = %.3f  (bound = %.1f)\n",
            (float)T1_WCET/(float)T1_PERIOD + (float)T2_WCET/(float)T2_PERIOD +
            (float)T3_WCET/(float)T3_PERIOD,
            (float)configNUMBER_OF_CORES );
    printf( "========================================\n\n" );

    /* xCorePreference = -1: global mode, scheduler picks any core */
    r = xTaskCreateEDF( vMPTask, "T1", 512, &xP1,
                        T1_PERIOD, T1_DEADLINE, T1_WCET, -1, NULL );
    printf( "Create T1: %s\n", r == pdPASS ? "OK" : "REJECTED" );

    r = xTaskCreateEDF( vMPTask, "T2", 512, &xP2,
                        T2_PERIOD, T2_DEADLINE, T2_WCET, -1, NULL );
    printf( "Create T2: %s\n", r == pdPASS ? "OK" : "REJECTED" );

    r = xTaskCreateEDF( vMPTask, "T3", 512, &xP3,
                        T3_PERIOD, T3_DEADLINE, T3_WCET, -1, NULL );
    printf( "Create T3: %s\n", r == pdPASS ? "OK" : "REJECTED" );

    /* Over-utilization rejection test: U+0.60 pushes total to 2.10 > 2.0 */
    r = xTaskCreateEDF( vMPTask, "Ovfl", 512, &xP1,
                        pdMS_TO_TICKS(1000), pdMS_TO_TICKS(1000), pdMS_TO_TICKS(600),
                        -1, NULL );
    printf( "Create Ovfl (U+0.60, expect REJECTED): %s\n",
            r == pdPASS ? "ACCEPTED" : "REJECTED" );

    printf( "\nStarting scheduler...\n" );
    vTaskStartScheduler();

    printf( "ERROR: scheduler returned\n" );
    for( ;; );
}
