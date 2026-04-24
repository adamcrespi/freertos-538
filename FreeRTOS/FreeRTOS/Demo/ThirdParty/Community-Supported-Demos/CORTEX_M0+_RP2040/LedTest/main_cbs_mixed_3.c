/*
 * CBS Mixed Workload Test
 *
 * One periodic EDF tasks + two independent CBS servers.
 * Demonstrates multiple CBS servers coexisting with a periodic task,
 * each maintaining its own budget and deadline independently.
 *
 * Task Set:
 *   τ1 (Red):     Periodic  C=50ms   D=200ms  T=400ms   U=0.125
 *   S1 (Green):   CBS       Qs=80ms  Ts=400ms            Us=0.20
 *   S2 (Blue):    CBS       Qs=100ms Ts=500ms            Us=0.20
 *   Total U = 0.525 (schedulable)
 *
 * Both CBS tasks do continuous work. Expected behavior:
 *   - Each CBS server runs in bursts limited by its budget
 *   - Periodic tasks always meet deadlines
 *   - S1 gets ~20% bandwidth, S2 gets ~20% bandwidth
 *   - τ1 gets ~12.5%, τ2 gets ~12.5%
 *
 * GPIO assignments:
 *   GP16 → Red    (τ1 periodic)
 *   GP17 → Yellow (τ2 periodic)
 *   GP18 → Green  (S1 CBS)
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

/* ── GPIO Pins ──────────────────────────────────────────────── */
#define RED_PIN     16
#define YELLOW_PIN  17
#define GREEN_PIN   18

/* ── Periodic Task Parameters ───────────────────────────────── */
#define TASK1_WCET       pdMS_TO_TICKS(50)
#define TASK1_DEADLINE   pdMS_TO_TICKS(200)
#define TASK1_PERIOD     pdMS_TO_TICKS(400)

/* ── CBS Server Parameters ──────────────────────────────────── */
#define CBS1_BUDGET      pdMS_TO_TICKS(80)
#define CBS1_PERIOD      pdMS_TO_TICKS(400)

#define CBS2_BUDGET      pdMS_TO_TICKS(100)
#define CBS2_PERIOD      pdMS_TO_TICKS(500)

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

/* ── Periodic task function ─────────────────────────────────── */
typedef struct {
    uint32_t     ulPin;
    TickType_t   xWCET;
    TickType_t   xPeriod;
    const char  *pcName;
} PeriodicParams_t;

static void vPeriodicTask( void *pvParameters )
{
    PeriodicParams_t *pxP = ( PeriodicParams_t * ) pvParameters;
    TickType_t xLastWakeTime;
    uint32_t ulCount = 0;

    gpio_init( pxP->ulPin );
    gpio_set_dir( pxP->ulPin, GPIO_OUT );
    gpio_put( pxP->ulPin, 0 );
    vTaskSetApplicationTaskTag( NULL, ( TaskHookFunction_t )( uintptr_t ) pxP->ulPin );

    xLastWakeTime = xTaskGetTickCount();

    printf( "[%s] Started: C=%lu D=%lu T=%lu on GP%lu\n",
            pxP->pcName,
            ( unsigned long ) pxP->xWCET,
            ( unsigned long ) pxP->xPeriod,
            ( unsigned long ) pxP->xPeriod,
            ( unsigned long ) pxP->ulPin );

    for( ;; )
    {
        ulCount++;
        vBusyWait( pxP->xWCET );

        if( ( ulCount % 10 ) == 0 )
        {
            printf( "[%s] completed %lu jobs\n", pxP->pcName, ( unsigned long ) ulCount );
        }

        xTaskDelayUntil( &xLastWakeTime, pxP->xPeriod );
    }
}

/* ── CBS task function ──────────────────────────────────────── */
typedef struct {
    uint32_t     ulPin;
    TickType_t   xBudget;
    TickType_t   xServerPeriod;
    const char  *pcName;
} CBSParams_t;

static void vCBSTask( void *pvParameters )
{
    CBSParams_t *pxP = ( CBSParams_t * ) pvParameters;
    uint32_t ulBursts = 0;
    TickType_t xLastPrint = 0;

    gpio_init( pxP->ulPin );
    gpio_set_dir( pxP->ulPin, GPIO_OUT );
    gpio_put( pxP->ulPin, 0 );
    vTaskSetApplicationTaskTag( NULL, ( TaskHookFunction_t )( uintptr_t ) pxP->ulPin );

    printf( "[%s] Started: Qs=%lu Ts=%lu Us=%.2f on GP%lu\n",
            pxP->pcName,
            ( unsigned long ) pxP->xBudget,
            ( unsigned long ) pxP->xServerPeriod,
            ( float ) pxP->xBudget / ( float ) pxP->xServerPeriod,
            ( unsigned long ) pxP->ulPin );

    for( ;; )
    {
        /* Continuous work — CBS budget mechanism limits execution */
        vBusyWait( pxP->xBudget );
        ulBursts++;

        TickType_t xNow = xTaskGetTickCount();
        if( ( xNow - xLastPrint ) >= pdMS_TO_TICKS( 5000 ) )
        {
            printf( "[%s] %lu bursts, t=%lu\n",
                    pxP->pcName, ( unsigned long ) ulBursts,
                    ( unsigned long ) xNow );
            xLastPrint = xNow;
        }
    }
}

/* ── Static parameter structs ───────────────────────────────── */
static PeriodicParams_t xTask1Params = { RED_PIN,    TASK1_WCET, TASK1_PERIOD, "Red" };
static CBSParams_t      xCBS1Params  = { YELLOW_PIN, CBS1_BUDGET, CBS1_PERIOD, "Yellow/S1" };
static CBSParams_t      xCBS2Params  = { GREEN_PIN,  CBS2_BUDGET, CBS2_PERIOD, "Green/S2" };

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
    printf( "CBS Mixed Workload Test\n" );
    printf( "============================================================\n" );
    printf( "Tasks:\n" );
    printf( "  τ1 (Red):      Periodic  C=%3lu  D=%4lu  T=%4lu  U=%.3f\n",
            ( unsigned long ) TASK1_WCET, ( unsigned long ) TASK1_DEADLINE,
            ( unsigned long ) TASK1_PERIOD,
            ( float ) TASK1_WCET / ( float ) TASK1_PERIOD );
    printf( "  S1 (Yellow):    CBS       Qs=%3lu Ts=%4lu         Us=%.3f\n",
            ( unsigned long ) CBS1_BUDGET, ( unsigned long ) CBS1_PERIOD,
            ( float ) CBS1_BUDGET / ( float ) CBS1_PERIOD );
    printf( "  S2 (Green):     CBS       Qs=%3lu Ts=%4lu         Us=%.3f\n",
            ( unsigned long ) CBS2_BUDGET, ( unsigned long ) CBS2_PERIOD,
            ( float ) CBS2_BUDGET / ( float ) CBS2_PERIOD );
    printf( "  Total U = %.3f\n",
            ( float ) TASK1_WCET / ( float ) TASK1_PERIOD +
            ( float ) CBS1_BUDGET / ( float ) CBS1_PERIOD +
            ( float ) CBS2_BUDGET / ( float ) CBS2_PERIOD );
    printf( "============================================================\n\n" );

    /* Create periodic task */
    xResult = xTaskCreateEDF( vPeriodicTask, "Red", 512, &xTask1Params,
                              TASK1_PERIOD, TASK1_DEADLINE, TASK1_WCET, NULL );
    printf( "Create Red (periodic):     %s\n", xResult == pdPASS ? "OK" : "FAIL" );

    /* Create CBS servers */
    xResult = xTaskCreateCBS( vCBSTask, "YellowCBS", 512, &xCBS1Params,
                              CBS1_BUDGET, CBS1_PERIOD, NULL );
    printf( "Create Yellow/S1 (CBS):    %s\n", xResult == pdPASS ? "OK" : "FAIL" );

    xResult = xTaskCreateCBS( vCBSTask, "GreenCBS", 512, &xCBS2Params,
                              CBS2_BUDGET, CBS2_PERIOD, NULL );
    printf( "Create Green/S2 (CBS):     %s\n", xResult == pdPASS ? "OK" : "FAIL" );

    printf( "\nExpected: periodic tasks always meet deadlines,\n" );
    printf( "each CBS gets its reserved bandwidth independently.\n" );
    printf( "Starting scheduler...\n\n" );

    vTaskStartScheduler();

    printf( "ERROR: Scheduler exited!\n" );
    for( ;; );
}
