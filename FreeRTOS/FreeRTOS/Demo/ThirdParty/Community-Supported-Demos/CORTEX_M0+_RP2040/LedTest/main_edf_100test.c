/*
 * EDF Admission Control: 100-Task Comparison
 *
 * Demonstrates the difference between Liu & Layland utilization bound
 * and Processor Demand Analysis for EDF admission control.
 *
 * Creates 100 identical tasks with D < T:
 *   C = 3 ticks (3ms), D = 50 ticks (50ms), T = 250 ticks (250ms)
 *   U per task = 3/250 = 0.012
 *
 * LL bound rejects when total U > 1.0 (around task 84).
 * Processor demand analysis exploits the slack (D << T) and accepts more.
 *
 * No actual FreeRTOS tasks are created — only admission control math runs.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

/* Task parameters — all 100 tasks are identical */
/* Stagger deadlines across tasks to spread demand */
#define TEST_WCET       pdMS_TO_TICKS(5)
#define TEST_PERIOD     pdMS_TO_TICKS(250)
#define BASE_DEADLINE   pdMS_TO_TICKS(30)
#define NUM_TASKS       100

/* Hook functions required by FreeRTOS */
void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName )
{
    (void)xTask;
    printf( "!!! STACK OVERFLOW: %s !!!\n", pcTaskName );
    for( ;; );
}

void vApplicationTickHook( void ) {}

void vApplicationMallocFailedHook( void )
{
    printf( "!!! MALLOC FAILED !!!\n" );
    for( ;; );
}

/* Trace hooks (required since we enabled task tags) */
void vTracePinHigh( void ) {}
void vTracePinLow( void ) {}

int main( void )
{
    BaseType_t xLLResult, xPDResult;
    int iLLAccepted = 0;
    int iPDAccepted = 0;
    int iFirstLLReject = -1;
    int iFirstPDReject = -1;
    int i;

    stdio_init_all();
    sleep_ms( 2000 );

    printf( "\n" );
    printf( "============================================================\n" );
    printf( "EDF Admission Control: 100-Task Comparison\n" );
    printf( "============================================================\n" );
    printf( "Task parameters: C=%lu ms, D=100..298 ms, T=%lu ms\n",
            ( unsigned long )TEST_WCET,
            ( unsigned long )TEST_PERIOD );
    printf( "U per task = %lu/%lu = 0.012\n",
            ( unsigned long )TEST_WCET,
            ( unsigned long )TEST_PERIOD );
    printf( "============================================================\n" );
    printf( "\n" );
    printf( "Task   C    D    T    U_total   LL     PD\n" );
    printf( "-------------------------------------------\n" );

    for( i = 1; i <= NUM_TASKS; i++ )
    {
        /* Stagger deadlines: D = 100 + (i-1)*2, ranges from 100 to 298 */
        TickType_t xDeadline = BASE_DEADLINE + ( ( i - 1 ) * 5 );

        xEDFTestAdmission( TEST_WCET, TEST_PERIOD, xDeadline,
                           &xLLResult, &xPDResult );
        /* Compute total utilization as fixed-point (x1000 for display) */
        uint32_t ulUtilX1000 = ( ( uint32_t )TEST_WCET * 1000 * i ) / ( uint32_t )TEST_PERIOD;

        if( xLLResult == pdTRUE )
        {
            iLLAccepted = i;
        }
        else if( iFirstLLReject < 0 )
        {
            iFirstLLReject = i;
        }

        if( xPDResult == pdTRUE )
        {
            iPDAccepted = i;
        }
        else if( iFirstPDReject < 0 )
        {
            iFirstPDReject = i;
        }

        /* Print every task, highlight divergence */
        const char *pcMarker = "";
        if( xLLResult != xPDResult )
        {
            pcMarker = " <-- DIVERGENCE";
        }

        printf( "%3d  %3lu  %3lu  %3lu    %lu.%03lu   %s   %s%s\n",
                i,
                ( unsigned long )TEST_WCET,
                ( unsigned long )xDeadline,
                ( unsigned long )TEST_PERIOD,
                ( unsigned long )( ulUtilX1000 / 1000 ),
                ( unsigned long )( ulUtilX1000 % 1000 ),
                xLLResult == pdTRUE ? "PASS" : "FAIL",
                xPDResult == pdTRUE ? "PASS" : "FAIL",
                pcMarker );
    }

    printf( "\n" );
    printf( "============================================================\n" );
    printf( "RESULTS\n" );
    printf( "============================================================\n" );
    printf( "LL bound accepted:          %d / %d", iLLAccepted, NUM_TASKS );
    if( iFirstLLReject > 0 )
    {
        printf( "  (rejected at task %d)", iFirstLLReject );
    }
    printf( "\n" );

    printf( "Processor demand accepted:  %d / %d", iPDAccepted, NUM_TASKS );
    if( iFirstPDReject > 0 )
    {
        printf( "  (rejected at task %d)", iFirstPDReject );
    }
    printf( "\n" );

    if( iPDAccepted > iLLAccepted )
    {
        printf( "Difference:                 %d more tasks accepted by PD\n",
                iPDAccepted - iLLAccepted );
    }
    else if( iPDAccepted == iLLAccepted )
    {
        printf( "No difference (try a task set with D < T)\n" );
    }
    printf( "============================================================\n" );

    /* Start scheduler so FreeRTOS doesn't complain — only idle runs */
    vTaskStartScheduler();

    for( ;; );
}