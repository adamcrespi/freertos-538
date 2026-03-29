/*
 * CBS (Constant Bandwidth Server) Test Program
 *
 * One periodic EDF task + one CBS task.
 * The CBS task does continuous work (never voluntarily sleeps).
 * CBS budget enforcement limits it to its reserved bandwidth.
 *
 * Task Set:
 *   τ1 (Red):    Periodic  C=100ms  D=300ms  T=500ms   U=0.20
 *   S1 (Green):  CBS       Qs=100ms Ts=400ms            Us=0.25
 *   Total U = 0.45 (easily schedulable)
 *
 * Expected behavior:
 *   - S1 runs for 100ms, then budget exhausts
 *   - S1's deadline is postponed by Ts=400ms
 *   - S1's budget is replenished to Qs=100ms
 *   - If τ1 has an earlier deadline, τ1 preempts
 *   - S1 continues with its new (later) deadline
 *   - τ1 never misses a deadline
 *
 * GPIO assignments (for AD2 logic analyzer):
 *   GP16 → Red   (τ1 periodic)
 *   GP18 → Green (S1 CBS)
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

/* ── GPIO Pins ──────────────────────────────────────────────── */
#define RED_PIN     16
#define GREEN_PIN   18

/* ── Periodic Task Parameters ───────────────────────────────── */
#define TASK1_WCET       pdMS_TO_TICKS(100)
#define TASK1_DEADLINE   pdMS_TO_TICKS(300)
#define TASK1_PERIOD     pdMS_TO_TICKS(500)

/* ── CBS Server Parameters ──────────────────────────────────── */
#define CBS1_BUDGET      pdMS_TO_TICKS(100)   /* Qs */
#define CBS1_PERIOD      pdMS_TO_TICKS(400)   /* Ts */

/* ── Trace hooks ──────────────────────────────────────────── */
void vTracePinHigh( void )
{
    TaskHandle_t xTask = xTaskGetCurrentTaskHandle();
    uint32_t pin = ( uint32_t ) xTaskGetApplicationTaskTag( xTask );
    if( pin != 0 ) gpio_put( pin, 1 );
}

void vTracePinLow( void )
{
    TaskHandle_t xTask = xTaskGetCurrentTaskHandle();
    uint32_t pin = ( uint32_t ) xTaskGetApplicationTaskTag( xTask );
    if( pin != 0 ) gpio_put( pin, 0 );
}

/* ── Busy-wait helper ───────────────────────────────────────── */
static void vBusyWait( TickType_t xTicks )
{
    TickType_t xStart = xTaskGetTickCount();
    while( ( xTaskGetTickCount() - xStart ) < xTicks )
    {
        __asm volatile ( "nop" );
    }
}

/* ── τ1 (Red) — periodic EDF task ───────────────────────────── */
static void vTask1_Periodic( void *pvParameters )
{
    TickType_t xLastWakeTime;
    uint32_t ulCount = 0;
    ( void ) pvParameters;

    gpio_init( RED_PIN );
    gpio_set_dir( RED_PIN, GPIO_OUT );
    gpio_put( RED_PIN, 0 );
    vTaskSetApplicationTaskTag( NULL, ( TaskHookFunction_t )( uintptr_t ) RED_PIN );

    xLastWakeTime = xTaskGetTickCount();

    printf( "[Red/τ1] Started: C=%lu D=%lu T=%lu\n",
            ( unsigned long ) TASK1_WCET, ( unsigned long ) TASK1_DEADLINE,
            ( unsigned long ) TASK1_PERIOD );

    for( ;; )
    {
        ulCount++;
        vBusyWait( TASK1_WCET );

        if( ( ulCount % 5 ) == 0 )
        {
            printf( "[Red] completed %lu jobs\n", ( unsigned long ) ulCount );
        }

        xTaskDelayUntil( &xLastWakeTime, TASK1_PERIOD );
    }
}

/* ── S1 (Green) — CBS aperiodic task (runs continuously) ───── */
static void vTask_CBS( void *pvParameters )
{
    uint32_t ulBursts = 0;
    TickType_t xLastPrint = 0;
    ( void ) pvParameters;

    gpio_init( GREEN_PIN );
    gpio_set_dir( GREEN_PIN, GPIO_OUT );
    gpio_put( GREEN_PIN, 0 );
    vTaskSetApplicationTaskTag( NULL, ( TaskHookFunction_t )( uintptr_t ) GREEN_PIN );

    printf( "[Green/S1] Started: Qs=%lu Ts=%lu Us=%.2f\n",
            ( unsigned long ) CBS1_BUDGET, ( unsigned long ) CBS1_PERIOD,
            ( float ) CBS1_BUDGET / ( float ) CBS1_PERIOD );

    for( ;; )
    {
        /* Do a burst of work equal to budget, then check time */
        vBusyWait( CBS1_BUDGET );
        ulBursts++;

        TickType_t xNow = xTaskGetTickCount();
        if( ( xNow - xLastPrint ) >= pdMS_TO_TICKS( 2000 ) )
        {
            printf( "[Green/CBS] %lu bursts completed, t=%lu\n",
                    ( unsigned long ) ulBursts, ( unsigned long ) xNow );
            xLastPrint = xNow;
        }

        /* CBS task keeps running — never calls vTaskDelayUntil.
         * The CBS budget mechanism will postpone its deadline
         * and let periodic tasks preempt when needed. */
    }
}

/* ── Hook Functions ─────────────────────────────────────────── */
void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName )
{
    printf( "!!! STACK OVERFLOW: %s !!!\n", pcTaskName );
    for( ;; );
}

void vApplicationTickHook( void ) {}

void vApplicationMallocFailedHook( void )
{
    printf( "!!! MALLOC FAILED !!!\n" );
    for( ;; );
}

/* ── Main ───────────────────────────────────────────────────── */
int main( void )
{
    BaseType_t xResult;

    stdio_init_all();
    sleep_ms( 2000 );

    printf( "\n============================================================\n" );
    printf( "CBS Test — Budget Exhaustion and Deadline Postponement\n" );
    printf( "============================================================\n" );
    printf( "Tasks:\n" );
    printf( "  τ1 (Red):   Periodic  C=%3lu  D=%4lu  T=%4lu  U=%.2f\n",
            ( unsigned long ) TASK1_WCET, ( unsigned long ) TASK1_DEADLINE,
            ( unsigned long ) TASK1_PERIOD,
            ( float ) TASK1_WCET / ( float ) TASK1_PERIOD );
    printf( "  S1 (Green): CBS       Qs=%3lu Ts=%4lu         Us=%.2f\n",
            ( unsigned long ) CBS1_BUDGET, ( unsigned long ) CBS1_PERIOD,
            ( float ) CBS1_BUDGET / ( float ) CBS1_PERIOD );
    printf( "  Total U = %.2f\n",
            ( float ) TASK1_WCET / ( float ) TASK1_PERIOD +
            ( float ) CBS1_BUDGET / ( float ) CBS1_PERIOD );
    printf( "============================================================\n\n" );

    /* Create periodic task */
    xResult = xTaskCreateEDF( vTask1_Periodic, "Red", 512, NULL,
                              TASK1_PERIOD, TASK1_DEADLINE, TASK1_WCET, NULL );
    printf( "Create Red (periodic): %s\n", xResult == pdPASS ? "OK" : "FAIL" );

    /* Create CBS task */
    xResult = xTaskCreateCBS( vTask_CBS, "Green", 512, NULL,
                              CBS1_BUDGET, CBS1_PERIOD, NULL );
    printf( "Create Green (CBS):    %s\n", xResult == pdPASS ? "OK" : "FAIL" );

    printf( "\nExpected: Green runs in %lums bursts, Red preempts periodically\n",
            ( unsigned long ) CBS1_BUDGET );
    printf( "Starting scheduler...\n\n" );

    vTaskStartScheduler();

    printf( "ERROR: Scheduler exited!\n" );
    for( ;; );
}
