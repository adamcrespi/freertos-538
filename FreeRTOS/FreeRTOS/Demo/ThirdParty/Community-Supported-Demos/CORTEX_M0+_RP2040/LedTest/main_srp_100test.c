/*
 * SRP Stack Sharing Quantitative Test
 *
 * Demonstrates that under SRP, tasks with the same preemption level
 * (same relative deadline D) can share runtime stack memory, since
 * they can never be active simultaneously (Baker's Theorem 7.5).
 *
 * Setup:
 *   100 tasks across 10 preemption levels (10 tasks per level)
 *   Each task needs 256 words (1024 bytes) of stack
 *
 * Without sharing: 100 separate stacks = 100 × 1024 = 102,400 bytes
 * With sharing:    10 shared stacks    =  10 × 1024 =  10,240 bytes
 * Savings:         92,160 bytes (90%)
 *
 * This matches the textbook example (section 7.8.5):
 *   "100 tasks distributed on 10 preemption levels, with 10 tasks
 *    for each level... using a stack per task, 1000 Kbytes would be
 *    required. On the contrary, using a single stack, only 100 Kbytes
 *    would be sufficient... we would save 900 Kbytes, that is, 90%."
 *
 * The test also demonstrates actual stack pool allocation — assigning
 * one stack buffer per preemption level and reusing it across tasks
 * at that level.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* ── Test Parameters ────────────────────────────────────────── */
#define NUM_TASKS           100
#define NUM_LEVELS          10
#define TASKS_PER_LEVEL     ( NUM_TASKS / NUM_LEVELS )
#define STACK_WORDS         256     /* 256 words = 1024 bytes per task */
#define STACK_BYTES         ( STACK_WORDS * sizeof( StackType_t ) )

/* ── Trace hooks (required by kernel) ─────────────────────── */
void vTracePinHigh( void ) {}
void vTracePinLow( void ) {}

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

/* ── Stack Pool (shared stacks indexed by preemption level) ── */
typedef struct {
    StackType_t *pxStack;               /* shared stack buffer */
    configSTACK_DEPTH_TYPE uxSize;      /* size in words */
    UBaseType_t uxPreemptionLevel;      /* which π level */
    UBaseType_t uxTaskCount;            /* tasks sharing this stack */
} SRPStackPoolEntry_t;

static SRPStackPoolEntry_t xStackPool[ NUM_LEVELS ];
static UBaseType_t uxPoolCount = 0;

/*
 * Find or create a stack pool entry for the given preemption level.
 * Returns the shared stack pointer.
 */
static StackType_t * pxGetSharedStack( UBaseType_t uxPL,
                                        configSTACK_DEPTH_TYPE uxNeeded )
{
    UBaseType_t i;

    /* Search for existing entry at this preemption level */
    for( i = 0; i < uxPoolCount; i++ )
    {
        if( xStackPool[ i ].uxPreemptionLevel == uxPL )
        {
            xStackPool[ i ].uxTaskCount++;
            /* If this task needs more stack, grow the allocation */
            if( uxNeeded > xStackPool[ i ].uxSize )
            {
                xStackPool[ i ].pxStack = pvPortMalloc( uxNeeded * sizeof( StackType_t ) );
                xStackPool[ i ].uxSize = uxNeeded;
            }
            return xStackPool[ i ].pxStack;
        }
    }

    /* No entry found — allocate new stack buffer */
    if( uxPoolCount < NUM_LEVELS )
    {
        xStackPool[ uxPoolCount ].pxStack = pvPortMalloc( uxNeeded * sizeof( StackType_t ) );
        xStackPool[ uxPoolCount ].uxSize = uxNeeded;
        xStackPool[ uxPoolCount ].uxPreemptionLevel = uxPL;
        xStackPool[ uxPoolCount ].uxTaskCount = 1;
        uxPoolCount++;
        return xStackPool[ uxPoolCount - 1 ].pxStack;
    }

    return NULL;
}

/* ── Main ───────────────────────────────────────────────────── */
int main( void )
{
    stdio_init_all();
    sleep_ms( 2000 );

    printf( "\n============================================================\n" );
    printf( "SRP Stack Sharing Quantitative Test\n" );
    printf( "============================================================\n\n" );

    printf( "Configuration:\n" );
    printf( "  Tasks:            %d\n", NUM_TASKS );
    printf( "  Preemption levels: %d\n", NUM_LEVELS );
    printf( "  Tasks per level:  %d\n", TASKS_PER_LEVEL );
    printf( "  Stack per task:   %lu bytes (%d words)\n",
            ( unsigned long ) STACK_BYTES, STACK_WORDS );
    printf( "\n" );

    /* ── Part 1: Compute theoretical savings ──────────────── */
    uint32_t ulWithoutSharing = ( uint32_t ) NUM_TASKS * STACK_BYTES;
    uint32_t ulWithSharing = ( uint32_t ) NUM_LEVELS * STACK_BYTES;
    uint32_t ulSavings = ulWithoutSharing - ulWithSharing;
    uint32_t ulPercent = ( ulSavings * 100 ) / ulWithoutSharing;

    printf( "── Theoretical Analysis ─────────────────────────────────\n" );
    printf( "  Without sharing: %lu stacks × %lu bytes = %lu bytes\n",
            ( unsigned long ) NUM_TASKS, ( unsigned long ) STACK_BYTES,
            ( unsigned long ) ulWithoutSharing );
    printf( "  With sharing:    %lu stacks × %lu bytes = %lu bytes\n",
            ( unsigned long ) NUM_LEVELS, ( unsigned long ) STACK_BYTES,
            ( unsigned long ) ulWithSharing );
    printf( "  Savings:         %lu bytes (%lu%%)\n",
            ( unsigned long ) ulSavings, ( unsigned long ) ulPercent );
    printf( "\n" );

    /* ── Part 2: Demonstrate actual stack pool allocation ──── */
    printf( "── Stack Pool Allocation Demo ───────────────────────────\n" );
    printf( "  Allocating stacks for %d tasks across %d levels...\n\n",
            NUM_TASKS, NUM_LEVELS );

    uint32_t ulTotalAllocNoShare = 0;
    uint32_t ulTotalAllocShared = 0;
    UBaseType_t uxLevel, uxTask;

    /* Relative deadlines: 100, 200, 300, ..., 1000 ms */
    for( uxLevel = 0; uxLevel < NUM_LEVELS; uxLevel++ )
    {
        TickType_t xDeadline = pdMS_TO_TICKS( ( uxLevel + 1 ) * 100 );
        UBaseType_t uxPL = configMAX_PREEMPTION_LEVEL - xDeadline;

        for( uxTask = 0; uxTask < TASKS_PER_LEVEL; uxTask++ )
        {
            /* Without sharing: each task gets its own stack */
            ulTotalAllocNoShare += STACK_BYTES;

            /* With sharing: get stack from pool (reuses existing) */
            StackType_t *pxStack = pxGetSharedStack( uxPL, STACK_WORDS );
            if( pxStack == NULL )
            {
                printf( "  ERROR: stack pool allocation failed at level %lu task %lu\n",
                        ( unsigned long ) uxLevel, ( unsigned long ) uxTask );
            }
        }
    }

    /* Count actual shared allocations */
    for( uxLevel = 0; uxLevel < uxPoolCount; uxLevel++ )
    {
        ulTotalAllocShared += xStackPool[ uxLevel ].uxSize * sizeof( StackType_t );
        printf( "  Level %lu: π=%u, D=%lu ms, %lu tasks sharing %lu bytes\n",
                ( unsigned long ) uxLevel,
                ( unsigned ) xStackPool[ uxLevel ].uxPreemptionLevel,
                ( unsigned long ) ( ( configMAX_PREEMPTION_LEVEL - xStackPool[ uxLevel ].uxPreemptionLevel ) ),
                ( unsigned long ) xStackPool[ uxLevel ].uxTaskCount,
                ( unsigned long ) ( xStackPool[ uxLevel ].uxSize * sizeof( StackType_t ) ) );
    }

    printf( "\n" );
    printf( "── Actual Allocation Results ────────────────────────────\n" );
    printf( "  Without sharing: %lu bytes (%lu separate stacks)\n",
            ( unsigned long ) ulTotalAllocNoShare, ( unsigned long ) NUM_TASKS );
    printf( "  With sharing:    %lu bytes (%lu shared stacks)\n",
            ( unsigned long ) ulTotalAllocShared, ( unsigned long ) uxPoolCount );

    ulSavings = ulTotalAllocNoShare - ulTotalAllocShared;
    ulPercent = ( ulSavings * 100 ) / ulTotalAllocNoShare;
    printf( "  Savings:         %lu bytes (%lu%%)\n",
            ( unsigned long ) ulSavings, ( unsigned long ) ulPercent );

    printf( "\n" );
    printf( "── SRP Stack Sharing Justification ─────────────────────\n" );
    printf( "  Under SRP (Baker's Theorem 7.5), once a task starts\n" );
    printf( "  executing, it can never be blocked. Therefore, tasks\n" );
    printf( "  at the same preemption level can never occupy stack\n" );
    printf( "  space simultaneously — they can share a single stack.\n" );
    printf( "\n" );
    printf( "  The space required equals the sum of the largest stack\n" );
    printf( "  request at each preemption level, rather than the sum\n" );
    printf( "  of all individual task stacks.\n" );

    printf( "\n============================================================\n" );
    printf( "Test complete.\n" );
    printf( "============================================================\n" );

    /* Don't start the scheduler — this is a computation-only test */
    for( ;; )
    {
        tight_loop_contents();
    }
}
