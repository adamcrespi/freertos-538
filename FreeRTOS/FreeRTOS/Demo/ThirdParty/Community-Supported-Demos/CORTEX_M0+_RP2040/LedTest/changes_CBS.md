# Changes — Constant Bandwidth Server (CBS)

All kernel modifications to support CBS. Every change is wrapped in
`#if ( configUSE_CBS == 1 )` nested inside `#if ( configUSE_EDF_SCHEDULER == 1 )`
unless otherwise noted.

---

## Modified Files Summary

| File | What Changed |
|------|-------------|
| `tasks.c` | TCB struct (added CBS fields), `xTaskCreateCBS()`, budget tracking in `xTaskIncrementTick()`, job arrival Rule 2 check, `xTaskCreateEDF()` CBS field initialization, `taskSELECT_HIGHEST_PRIORITY_TASK` tie-breaking |
| `task.h` | `xTaskCreateCBS()` prototype |
| `FreeRTOSConfig.h` | `configUSE_CBS`, `configMAX_CBS_SERVERS` |

---

## `FreeRTOS/FreeRTOS/Source/tasks.c`

### 1. TCB Extension — `tskTCB` struct MODIFIED

Added fields inside `#if ( configUSE_CBS == 1 )`, nested within the existing
`#if ( configUSE_EDF_SCHEDULER == 1 )` block:

| Field | Type | Purpose |
|-------|------|---------|
| `xIsCBSTask` | `BaseType_t` | `pdTRUE` if task is managed by a CBS |
| `xCBSBudget` | `TickType_t` | qs — current remaining budget (decrements each tick) |
| `xCBSMaxBudget` | `TickType_t` | Qs — maximum budget per server period |
| `xCBSServerPeriod` | `TickType_t` | Ts — server replenishment period |

### 2. CBS Field Initialization — `xTaskCreateEDF()` MODIFIED

After setting `xIsEDFTask = pdTRUE` and `xDeadlineMissCount = 0`, added
initialization of CBS fields for regular periodic tasks:

```c
#if ( configUSE_CBS == 1 )
    pxNewTCB->xIsCBSTask = pdFALSE;
    pxNewTCB->xCBSBudget = 0;
    pxNewTCB->xCBSMaxBudget = 0;
    pxNewTCB->xCBSServerPeriod = 0;
#endif
```

Ensures CBS code paths are never triggered for regular EDF tasks.

### 3. Task Creation — `xTaskCreateCBS()` NEW FUNCTION

Creates a CBS-managed task. Signature:

```c
BaseType_t xTaskCreateCBS( TaskFunction_t pxTaskCode,
                            const char * const pcName,
                            const configSTACK_DEPTH_TYPE uxStackDepth,
                            void * const pvParameters,
                            TickType_t xServerBudget,
                            TickType_t xServerPeriod,
                            TaskHandle_t * const pxCreatedTask );
```

Implementation:
1. Runs `prvEDFAdmissionControl(Qs, Ts, Ts)` — CBS bandwidth = Qs/Ts
2. Creates task via `prvCreateTask()` with priority 1
3. Sets EDF fields: `xAbsoluteDeadline = now + Ts`, `xPeriod = Ts`,
   `xRelativeDeadline = Ts`, `xWCET = Qs`, `xIsEDFTask = pdTRUE`
4. Sets CBS fields: `xIsCBSTask = pdTRUE`, `xCBSBudget = Qs`,
   `xCBSMaxBudget = Qs`, `xCBSServerPeriod = Ts`
5. Registers `(Qs, Ts, Ts)` in `xEDFTaskRegistry[]`
6. Sets list item value to deadline and adds to ready list

### 4. Budget Tracking — `xTaskIncrementTick()` MODIFIED

Added after the tick count increment, before delayed task processing:

```c
#if ( configUSE_CBS == 1 )
if( pxCurrentTCB->xIsCBSTask == pdTRUE && pxCurrentTCB->xCBSBudget > 0 )
{
    pxCurrentTCB->xCBSBudget--;
    if( pxCurrentTCB->xCBSBudget == 0 )
    {
        /* Budget exhausted — postpone deadline, replenish */
        pxCurrentTCB->xAbsoluteDeadline += pxCurrentTCB->xCBSServerPeriod;
        pxCurrentTCB->xCBSBudget = pxCurrentTCB->xCBSMaxBudget;

        /* Update list item and re-sort ready list */
        listSET_LIST_ITEM_VALUE( &( pxCurrentTCB->xStateListItem ),
                                  pxCurrentTCB->xAbsoluteDeadline );
        uxListRemove( &( pxCurrentTCB->xStateListItem ) );
        prvAddTaskToReadyList( pxCurrentTCB );

        /* Force context switch */
        xSwitchRequired = pdTRUE;
    }
}
#endif
```

Budget only decrements when the CBS task is actually running (`pxCurrentTCB`).
Preempted CBS tasks preserve their remaining budget.

### 5. Job Arrival Check — `xTaskIncrementTick()` MODIFIED

In the delayed task processing loop, where tasks are moved from the delayed
list to the ready list, modified the EDF deadline update block to handle CBS
tasks differently:

```c
#if ( configUSE_EDF_SCHEDULER == 1 )
{
    if( pxTCB->xIsEDFTask == pdTRUE )
    {
        #if ( configUSE_CBS == 1 )
        if( pxTCB->xIsCBSTask == pdTRUE )
        {
            /* CBS Rule 2: check if budget is too large for remaining time */
            TickType_t xTimeToDeadline = pxTCB->xAbsoluteDeadline - xNow;
            if( (pxTCB->xCBSBudget * pxTCB->xCBSServerPeriod) >
                (xTimeToDeadline * pxTCB->xCBSMaxBudget) )
            {
                pxTCB->xAbsoluteDeadline = xNow + pxTCB->xCBSServerPeriod;
                pxTCB->xCBSBudget = pxTCB->xCBSMaxBudget;
            }
            listSET_LIST_ITEM_VALUE( &( pxTCB->xStateListItem ),
                                      pxTCB->xAbsoluteDeadline );
        }
        else
        #endif
        {
            /* Regular EDF periodic task */
            pxTCB->xAbsoluteDeadline = pxTCB->xNextReleaseTime + pxTCB->xRelativeDeadline;
            pxTCB->xNextReleaseTime += pxTCB->xPeriod;
            listSET_LIST_ITEM_VALUE( &( pxTCB->xStateListItem ),
                                      pxTCB->xAbsoluteDeadline );
        }
    }
}
#endif
```

CBS tasks skip the normal periodic deadline update and instead apply Rule 2.
Regular EDF tasks are unaffected.

### 6. Tie-Breaking — `taskSELECT_HIGHEST_PRIORITY_TASK` macro MODIFIED

After selecting the head of the EDF ready list (earliest deadline), added a
check for CBS tie-breaking. If the head task is not a CBS task, scans
subsequent tasks with the same deadline for a CBS task and prefers it:

```c
if( ( configUSE_CBS == 1 ) && ( pxCurrentTCB->xIsCBSTask != pdTRUE ) )
{
    /* Check if a CBS task at the same deadline exists */
    ListItem_t * pxNext = listGET_NEXT( pxFirstItem );
    while( pxNext != pxEnd )
    {
        if( listGET_LIST_ITEM_VALUE( pxNext ) != xHeadDeadline )
            break;
        TCB_t * pxNextTCB = listGET_LIST_ITEM_OWNER( pxNext );
        if( pxNextTCB->xIsCBSTask == pdTRUE )
        {
            pxCurrentTCB = pxNextTCB;
            break;
        }
        pxNext = listGET_NEXT( pxNext );
    }
}
```

Only scans tasks with identical deadlines. Overhead is zero when no ties
exist.

---

## `FreeRTOS/FreeRTOS/Source/include/task.h`

### `xTaskCreateCBS()` Prototype — NEW

Added inside `#if ( configUSE_CBS == 1 )`, nested within the existing
`#if ( configUSE_EDF_SCHEDULER == 1 )` block.

---

## `LedTest/FreeRTOSConfig.h`

### New Defines

| Define | Value | Purpose |
|--------|-------|---------|
| `configUSE_CBS` | 1 | Enable CBS support (requires `configUSE_EDF_SCHEDULER == 1`) |
| `configMAX_CBS_SERVERS` | 16 | Maximum number of CBS servers (informational limit) |
