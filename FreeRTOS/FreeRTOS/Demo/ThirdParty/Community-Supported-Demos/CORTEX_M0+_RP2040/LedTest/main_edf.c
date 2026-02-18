/*
 * EDF Scheduler Test Program
 *
 * Creates 3 periodic EDF tasks with known timing parameters.
 * Each task toggles a GPIO/LED when running, allowing verification
 * via logic analyzer (AD2) and the capture_gantt_edf.py script.
 *
 * Task Set (all times in ms, converted to ticks at 1ms/tick):
 *   τ1 (Red):    C=100, D=250, T=500   → U=0.20
 *   τ2 (Yellow): C=150, D=500, T=1000  → U=0.15
 *   τ3 (Green):  C=200, D=1000, T=2000 → U=0.10
 *   Total U = 0.45 (easily schedulable by EDF)
 *
 * Expected behavior: EDF scheduler always runs the task with the
 * earliest absolute deadline. No deadline misses should occur.
 *
 * GPIO assignments (directly for AD2 logic analyzer):
 *   GP16 → Red LED / AD2 D0
 *   GP17 → Yellow LED / AD2 D1
 *   GP18 → Green LED / AD2 D2
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

/* ── GPIO Pins ──────────────────────────────────────────────── */
#define RED_PIN     16
#define YELLOW_PIN  17
#define GREEN_PIN   18

/* ── Task Parameters (in ms, converted to ticks) ────────────── */
/* τ1: Red — short period, tight deadline */
#define TASK1_WCET       pdMS_TO_TICKS(80)
#define TASK1_DEADLINE   pdMS_TO_TICKS(200)
#define TASK1_PERIOD     pdMS_TO_TICKS(400)

/* τ2: Yellow */
#define TASK2_WCET       pdMS_TO_TICKS(150)
#define TASK2_DEADLINE   pdMS_TO_TICKS(400)
#define TASK2_PERIOD     pdMS_TO_TICKS(800)

/* τ3: Green — long execution, will get preempted */
#define TASK3_WCET       pdMS_TO_TICKS(400)
#define TASK3_DEADLINE   pdMS_TO_TICKS(1000)
#define TASK3_PERIOD     pdMS_TO_TICKS(1600)



void vTracePinHigh(void)
{
    TaskHandle_t xTask = xTaskGetCurrentTaskHandle();
    uint32_t pin = (uint32_t)xTaskGetApplicationTaskTag(xTask);
    if (pin != 0) gpio_put(pin, 1);
}

void vTracePinLow(void)
{
    TaskHandle_t xTask = xTaskGetCurrentTaskHandle();
    uint32_t pin = (uint32_t)xTaskGetApplicationTaskTag(xTask);
    if (pin != 0) gpio_put(pin, 0);
}

/* ── Busy-wait helper ───────────────────────────────────────── */
/* Simulates computation for exactly 'ticks' worth of time.
 * The GPIO stays HIGH while the task is "executing". */
static void vBusyWait( TickType_t xTicks )
{
    TickType_t xStart = xTaskGetTickCount();

    while( ( xTaskGetTickCount() - xStart ) < xTicks )
    {
        /* Spin — simulating real computation */
        __asm volatile ("nop");
    }
}

/* ── EDF Task Function ──────────────────────────────────────── */
typedef struct {
    uint     gpio;
    TickType_t xWCET;
    TickType_t xPeriod;
    const char *pcName;
} EDFTaskParams_t;

static void vEDFTask( void *pvParameters )
{
    EDFTaskParams_t *pxParams = ( EDFTaskParams_t * ) pvParameters;
    TickType_t xLastWakeTime;

    /* Initialize GPIO */
    gpio_init( pxParams->gpio );
    gpio_set_dir( pxParams->gpio, GPIO_OUT );
    gpio_put( pxParams->gpio, 0 );

    /* Set task tag to GPIO pin — trace hooks use this */
    vTaskSetApplicationTaskTag( NULL, ( TaskHookFunction_t )( uintptr_t )pxParams->gpio );

    xLastWakeTime = xTaskGetTickCount();

    printf( "[%s] Started: C=%lu T=%lu on GP%u\n",
            pxParams->pcName,
            ( unsigned long ) pxParams->xWCET,
            ( unsigned long ) pxParams->xPeriod,
            pxParams->gpio );

    for( ;; )
    {
        /* Just busy-wait — trace hooks handle GPIO toggling */
        vBusyWait( pxParams->xWCET );

        /* Sleep until next period */
        xTaskDelayUntil( &xLastWakeTime, pxParams->xPeriod );
    }
}

/* ── Static task parameter structs (must outlive the tasks) ─── */
static EDFTaskParams_t xTask1Params = { RED_PIN,    TASK1_WCET, TASK1_PERIOD, "Red" };
static EDFTaskParams_t xTask2Params = { YELLOW_PIN, TASK2_WCET, TASK2_PERIOD, "Yellow" };
static EDFTaskParams_t xTask3Params = { GREEN_PIN,  TASK3_WCET, TASK3_PERIOD, "Green" };

/* ── Hook Functions (required by FreeRTOS config) ───────────── */
void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName )
{
    printf( "!!! STACK OVERFLOW: %s !!!\n", pcTaskName );
    for( ;; );
}

void vApplicationTickHook( void )
{
}

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
    sleep_ms( 2000 );  /* Give serial terminal time to connect */

    printf( "\n========================================\n" );
    printf( "EDF Scheduler Test\n" );
    printf( "========================================\n" );
    printf( "Task Set:\n" );
    printf( "  t1 (Red):    C=%3lu  D=%4lu  T=%4lu  U=%.2f\n",
            (unsigned long)TASK1_WCET, (unsigned long)TASK1_DEADLINE,
            (unsigned long)TASK1_PERIOD,
            (float)TASK1_WCET / (float)TASK1_PERIOD );
    printf( "  t2 (Yellow): C=%3lu  D=%4lu  T=%4lu  U=%.2f\n",
            (unsigned long)TASK2_WCET, (unsigned long)TASK2_DEADLINE,
            (unsigned long)TASK2_PERIOD,
            (float)TASK2_WCET / (float)TASK2_PERIOD );
    printf( "  t3 (Green):  C=%3lu  D=%4lu  T=%4lu  U=%.2f\n",
            (unsigned long)TASK3_WCET, (unsigned long)TASK3_DEADLINE,
            (unsigned long)TASK3_PERIOD,
            (float)TASK3_WCET / (float)TASK3_PERIOD );
    printf( "  Total U = %.2f\n",
            (float)TASK1_WCET / (float)TASK1_PERIOD +
            (float)TASK2_WCET / (float)TASK2_PERIOD +
            (float)TASK3_WCET / (float)TASK3_PERIOD );
    printf( "========================================\n\n" );

    /* Create EDF tasks */
    xResult = xTaskCreateEDF( vEDFTask, "Red", 512, &xTask1Params,
                              TASK1_PERIOD, TASK1_DEADLINE, TASK1_WCET, NULL );
    printf( "Create Red:    %s\n", xResult == pdPASS ? "OK" : "FAIL" );

    xResult = xTaskCreateEDF( vEDFTask, "Yellow", 512, &xTask2Params,
                              TASK2_PERIOD, TASK2_DEADLINE, TASK2_WCET, NULL );
    printf( "Create Yellow: %s\n", xResult == pdPASS ? "OK" : "FAIL" );

    xResult = xTaskCreateEDF( vEDFTask, "Green", 512, &xTask3Params,
                              TASK3_PERIOD, TASK3_DEADLINE, TASK3_WCET, NULL );
    printf( "Create Green:  %s\n", xResult == pdPASS ? "OK" : "FAIL" );
    
    // rejection test.
    xResult = xTaskCreateEDF( vEDFTask, "Reject", 512, &xTask1Params,
                              pdMS_TO_TICKS(200), pdMS_TO_TICKS(200), pdMS_TO_TICKS(150), NULL );
    printf( "Create Reject: %s (expected FAIL)\n", xResult == pdPASS ? "OK" : "FAIL" );

    printf( "\nStarting scheduler...\n" );
    vTaskStartScheduler();

    /* Should never reach here */
    printf( "ERROR: Scheduler exited!\n" );
    for( ;; );
}
