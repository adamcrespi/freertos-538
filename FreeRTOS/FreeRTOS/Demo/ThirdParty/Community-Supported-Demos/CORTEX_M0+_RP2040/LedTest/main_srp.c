/*
 * SRP (Stack Resource Policy) Test Program
 *
 * Adapted from textbook Fig 7.19 (Baker's SRP example).
 * Three EDF tasks share two binary semaphores under SRP.
 * τ3 (lowest preemption level) holds R2, then nested R1.
 * The system ceiling prevents τ1 and τ2 from preempting
 * until τ3 releases each resource.
 *
 * Task Set:
 *   τ1 (Red):    C=50ms   D=150ms  T=600ms   π=highest  uses R1
 *   τ2 (Yellow): C=100ms  D=300ms  T=800ms   π=middle   uses R2
 *   τ3 (Green):  C=250ms  D=500ms  T=1000ms  π=lowest   uses R2, R1 (nested)
 *
 * Resources:
 *   R1: ceiling = π(τ1) = configMAX_PREEMPTION_LEVEL - 150
 *   R2: ceiling = π(τ2) = configMAX_PREEMPTION_LEVEL - 300
 *
 * Expected execution (first hyperperiod):
 *   1. τ3 starts (only task ready), Πs = 0
 *   2. τ3 locks R2  → Πs = π(τ2)
 *   3. τ3 locks R1  → Πs = π(τ1)
 *      (τ1 and τ2 wake, but blocked by ceiling)
 *   4. τ3 releases R1 → Πs = π(τ2), τ1 preempts
 *   5. τ1 runs to completion (locks R1, uses it, releases — never blocks)
 *   6. τ3 resumes, releases R2 → Πs = 0, τ2 preempts
 *   7. τ2 runs to completion (locks R2, uses it, releases — never blocks)
 *   8. τ3 resumes and finishes
 *
 * GPIO assignments (for AD2 logic analyzer):
 *   GP16 → Red   (τ1) / AD2 D0
 *   GP17 → Yellow (τ2) / AD2 D1
 *   GP18 → Green  (τ3) / AD2 D2
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* ── GPIO Pins ──────────────────────────────────────────────── */
#define RED_PIN     16
#define YELLOW_PIN  17
#define GREEN_PIN   18

/* ── Task Parameters ────────────────────────────────────────── */
/* τ1 (Red) — highest preemption level, shortest D */
#define TASK1_WCET       pdMS_TO_TICKS(50)
#define TASK1_DEADLINE   pdMS_TO_TICKS(150)
#define TASK1_PERIOD     pdMS_TO_TICKS(600)

/* τ2 (Yellow) — middle preemption level */
#define TASK2_WCET       pdMS_TO_TICKS(100)
#define TASK2_DEADLINE   pdMS_TO_TICKS(300)
#define TASK2_PERIOD     pdMS_TO_TICKS(800)

/* τ3 (Green) — lowest preemption level, longest D */
#define TASK3_WCET       pdMS_TO_TICKS(600)
#define TASK3_DEADLINE   pdMS_TO_TICKS(2000)
#define TASK3_PERIOD     pdMS_TO_TICKS(3000)

/* ── Preemption Levels (π = MAX - D) ───────────────────────── */
#define PI_TAU1  ( configMAX_PREEMPTION_LEVEL - 150 )   /* highest */
#define PI_TAU2  ( configMAX_PREEMPTION_LEVEL - 300 )   /* middle  */
#define PI_TAU3  ( configMAX_PREEMPTION_LEVEL - 500 )   /* lowest  */

/* ── Resource Ceilings ──────────────────────────────────────── */
/* R1 used by τ1 and τ3 → ceiling = max(π1, π3) = π1 */
#define R1_CEILING  PI_TAU1
/* R2 used by τ2 and τ3 → ceiling = max(π2, π3) = π2 */
#define R2_CEILING  PI_TAU2

/* ── Worst-case critical section lengths (for admission ctrl) ─ */
#define R1_MAX_CS   pdMS_TO_TICKS(30)
#define R2_MAX_CS   pdMS_TO_TICKS(60)

/* ── Shared Semaphores ──────────────────────────────────────── */
static SemaphoreHandle_t xR1 = NULL;
static SemaphoreHandle_t xR2 = NULL;

/* ── Trace hooks (same as main_edf.c) ─────────────────────── */
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

/* ── τ1 (Red) — highest π, uses R1 ─────────────────────────── */
static void vTask1_Red( void *pvParameters )
{
    TickType_t xLastWakeTime;
    ( void ) pvParameters;

    gpio_init( RED_PIN );
    gpio_set_dir( RED_PIN, GPIO_OUT );
    gpio_put( RED_PIN, 0 );
    vTaskSetApplicationTaskTag( NULL, ( TaskHookFunction_t )( uintptr_t ) RED_PIN );

    vTaskDelay( pdMS_TO_TICKS( 20 ) );
    xLastWakeTime = xTaskGetTickCount();

    printf( "[Red/τ1] Started: C=%lu D=%lu T=%lu π=%u\n",
            ( unsigned long ) TASK1_WCET, ( unsigned long ) TASK1_DEADLINE,
            ( unsigned long ) TASK1_PERIOD, ( unsigned ) PI_TAU1 );

    for( ;; )
    {
        /* Phase 1: compute without resource */
        printf("[Red] phase1\n");
        vBusyWait( pdMS_TO_TICKS( 10 ) );

        /* Phase 2: use R1 */
        printf("[Red] taking R1\n");
        xSemaphoreTake( xR1, portMAX_DELAY );
        printf("[Red] got R1\n");
        vBusyWait( pdMS_TO_TICKS( 30 ) );
        xSemaphoreGive( xR1 );
        printf("[Red] released R1\n");

        /* Phase 3: compute without resource */
        vBusyWait( pdMS_TO_TICKS( 10 ) );
        printf("[Red] sleeping\n");
        printf("[Red] Πs=%u sleeping\n", (unsigned)uxSRPGetSystemCeiling());
        xTaskDelayUntil( &xLastWakeTime, TASK1_PERIOD );
    }
}

/* ── τ2 (Yellow) — middle π, uses R2 ───────────────────────── */
static void vTask2_Yellow( void *pvParameters )
{
    TickType_t xLastWakeTime;
    ( void ) pvParameters;

    gpio_init( YELLOW_PIN );
    gpio_set_dir( YELLOW_PIN, GPIO_OUT );
    gpio_put( YELLOW_PIN, 0 );
    vTaskSetApplicationTaskTag( NULL, ( TaskHookFunction_t )( uintptr_t ) YELLOW_PIN );

    vTaskDelay( pdMS_TO_TICKS( 20 ) );
    xLastWakeTime = xTaskGetTickCount();

    printf( "[Yellow/τ2] Started: C=%lu D=%lu T=%lu π=%u\n",
            ( unsigned long ) TASK2_WCET, ( unsigned long ) TASK2_DEADLINE,
            ( unsigned long ) TASK2_PERIOD, ( unsigned ) PI_TAU2 );

    for( ;; )
    {
        /* Phase 1: compute without resource */
        printf("[Yellow] phase1\n");
        vBusyWait( pdMS_TO_TICKS( 20 ) );

        /* Phase 2: use R2 */
        printf("[Yellow] taking R2\n");
        xSemaphoreTake( xR2, portMAX_DELAY );
        printf("[Yellow] got R2\n");
        vBusyWait( pdMS_TO_TICKS( 60 ) );
        xSemaphoreGive( xR2 );
        printf("[Yellow] released R2\n");

        /* Phase 3: compute without resource */
        vBusyWait( pdMS_TO_TICKS( 20 ) );
        printf("[Yellow] sleeping\n");
        xTaskDelayUntil( &xLastWakeTime, TASK2_PERIOD );
    }
}

/* ── τ3 (Green) — lowest π, uses R2 then nested R1 ─────────── */
static void vTask3_Green( void *pvParameters )
{
    TickType_t xLastWakeTime;
    ( void ) pvParameters;

    gpio_init( GREEN_PIN );
    gpio_set_dir( GREEN_PIN, GPIO_OUT );
    gpio_put( GREEN_PIN, 0 );
    vTaskSetApplicationTaskTag( NULL, ( TaskHookFunction_t )( uintptr_t ) GREEN_PIN );

    xLastWakeTime = xTaskGetTickCount();

    printf( "[Green/τ3] Started: C=%lu D=%lu T=%lu π=%u\n",
            ( unsigned long ) TASK3_WCET, ( unsigned long ) TASK3_DEADLINE,
            ( unsigned long ) TASK3_PERIOD, ( unsigned ) PI_TAU3 );

    for( ;; )
    {
        /* Phase 1: compute without resource (30ms) */
        printf("[Green] phase1\n");
        vBusyWait( pdMS_TO_TICKS( 20 ) );

        /* Phase 2: hold R2 — ceiling rises to π(τ2) */
        printf("[Green] taking R2\n");
        xSemaphoreTake( xR2, portMAX_DELAY );
        printf("[Green] got R2\n");
        vBusyWait( pdMS_TO_TICKS( 20 ) );

            /* Phase 3: nested — hold R1 inside R2 — ceiling rises to π(τ1) */
            printf("[Green] taking R1 inside R2\n");
            xSemaphoreTake( xR1, portMAX_DELAY );
            printf("[Green] got R1 inside R2\n");
            vBusyWait( pdMS_TO_TICKS( 300 ) );
            xSemaphoreGive( xR1 );
            printf("[Green] released R1, Πs=%u\n", (unsigned)uxSRPGetSystemCeiling());
            /* Ceiling drops to π(τ2). τ1 may preempt here. */

        /* Phase 4: still holding R2 */
        vBusyWait( pdMS_TO_TICKS( 200 ) );
        xSemaphoreGive( xR2 );
        printf("[Green] released R2, Πs=%u\n", (unsigned)uxSRPGetSystemCeiling());
        /* Ceiling drops to 0. τ2 may preempt here. */

        /* Phase 5: compute without resource (30ms) */
        vBusyWait( pdMS_TO_TICKS( 30 ) );
        printf("[Green] sleeping\n");
        xTaskDelayUntil( &xLastWakeTime, TASK3_PERIOD );
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
    printf( "SRP Test — Textbook Example (Baker Fig 7.19)\n" );
    printf( "============================================================\n" );
    printf( "Tasks:\n" );
    printf( "  τ1 (Red):    C=%3lu  D=%4lu  T=%4lu  π=%u  uses R1\n",
            ( unsigned long ) TASK1_WCET, ( unsigned long ) TASK1_DEADLINE,
            ( unsigned long ) TASK1_PERIOD, ( unsigned ) PI_TAU1 );
    printf( "  τ2 (Yellow): C=%3lu  D=%4lu  T=%4lu  π=%u  uses R2\n",
            ( unsigned long ) TASK2_WCET, ( unsigned long ) TASK2_DEADLINE,
            ( unsigned long ) TASK2_PERIOD, ( unsigned ) PI_TAU2 );
    printf( "  τ3 (Green):  C=%3lu  D=%4lu  T=%4lu  π=%u  uses R2, R1\n",
            ( unsigned long ) TASK3_WCET, ( unsigned long ) TASK3_DEADLINE,
            ( unsigned long ) TASK3_PERIOD, ( unsigned ) PI_TAU3 );
    printf( "Resources:\n" );
    printf( "  R1: ceiling=%u (τ1 highest user)\n", ( unsigned ) R1_CEILING );
    printf( "  R2: ceiling=%u (τ2 highest user)\n", ( unsigned ) R2_CEILING );
    printf( "============================================================\n\n" );

    /* Create SRP binary semaphores */
    xR1 = xSemaphoreCreateBinarySRP( R1_CEILING, R1_MAX_CS );
    xR2 = xSemaphoreCreateBinarySRP( R2_CEILING, R2_MAX_CS );
    printf( "R1 semaphore: %s\n", xR1 != NULL ? "OK" : "FAIL" );
    printf( "R2 semaphore: %s\n", xR2 != NULL ? "OK" : "FAIL" );

    /* Create EDF tasks — τ3 first so it starts running before τ1/τ2 wake */
    xResult = xTaskCreateEDF( vTask3_Green, "Green", 512, NULL,
                              TASK3_PERIOD, TASK3_DEADLINE, TASK3_WCET, NULL );
    printf( "Create Green (τ3):  %s\n", xResult == pdPASS ? "OK" : "FAIL" );

    xResult = xTaskCreateEDF( vTask2_Yellow, "Yellow", 512, NULL,
                              TASK2_PERIOD, TASK2_DEADLINE, TASK2_WCET, NULL );
    printf( "Create Yellow (τ2): %s\n", xResult == pdPASS ? "OK" : "FAIL" );

    xResult = xTaskCreateEDF( vTask1_Red, "Red", 512, NULL,
                              TASK1_PERIOD, TASK1_DEADLINE, TASK1_WCET, NULL );
    printf( "Create Red (τ1):    %s\n", xResult == pdPASS ? "OK" : "FAIL" );

    printf( "\nExpected sequence: τ3 → [τ3 locks R2] → [τ3 locks R1]\n" );
    printf( "  → [τ3 releases R1, τ1 preempts] → τ1 done\n" );
    printf( "  → [τ3 releases R2, τ2 preempts] → τ2 done → τ3 done\n" );
    printf( "\nStarting scheduler...\n" );

    vTaskStartScheduler();

    printf( "ERROR: Scheduler exited!\n" );
    for( ;; );
}
