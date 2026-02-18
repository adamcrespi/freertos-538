# Design — EDF


1.Extend the TCB
    add fields for C, D, T, and the current absolute deadline to each task's control block




2.New task creation WRAPPER
    -something like xTaskCreateEDF(function, name, stack, params, C, D, T, &handle) so tasks can declare their timing parameters
    -normal creation logic
    -fill out edf fields in TCB
    -run admission control to check if adding keeps system scheduable
    -return error code if it fails
    -set all EDF tasks to same priority in TCB

    ## Task Creation
    EDF tasks are created via `xTaskCreateEDF()`, which accepts period (T), relative
    deadline (D), and WCET (C) instead of a fixed priority. All EDF tasks are assigned
    priority 1 internally, with priority 0 reserved for the idle task. This ensures
    any EDF task preempts idle, while EDF-to-EDF scheduling is handled by deadline
    ordering rather than the priority field.

    On creation, the absolute deadline is computed as `currentTick + D` and the next
    release time as `currentTick + T`. This supports runtime task creation — tasks
    added after the scheduler starts get deadlines relative to their actual creation
    time, not tick 0.

    ## Ready List Insertion — Sorted by Deadline
    FreeRTOS maintains an array of ready lists, one per priority level. Tasks are
    placed into `pxReadyTasksLists[uxPriority]`. Stock FreeRTOS uses `listINSERT_END()`
    which appends to the tail — ordering doesn't matter because all tasks at the
    same priority are round-robined.

    For EDF, ordering within the ready list matters — the scheduler must always pick
    the task with the earliest absolute deadline. We modified the `prvAddTaskToReadyList`
    macro to check `xIsEDFTask`:
    - **EDF tasks**: use `vListInsert()`, which inserts sorted by `xItemValue`. Since
    we set `xItemValue` to the absolute deadline, the task with the earliest deadline
    ends up at the head of the list.
    - **Non-EDF tasks**: use `listINSERT_END()` as before (no behavior change).

    This means in `vTaskSwitchContext()`, selecting the earliest-deadline task is O(1) —
    just grab the head of the priority-1 ready list. The O(n) cost is paid once at
    insertion time, which happens far less frequently than context switches.

    ### Why not a single global sorted list?
    An alternative would be to collapse all EDF tasks into one sorted list regardless
    of priority. We chose to keep the existing priority array structure because:
    1. It preserves stock FreeRTOS behavior when `configUSE_EDF_SCHEDULER == 0`
    2. Non-EDF tasks (idle, timer) naturally stay separate at priority 0
    3. The macro change is minimal — one conditional in `prvAddTaskToReadyList`










3.Replace the scheduler's task selection: MAIN CHANGE
    in vTaskSwitchContext(), instead of "pick highest priority ready task," scan all ready tasks and pick the one with the earliest absolute deadline
    
    ## Scheduler Task Selection
    The `taskSELECT_HIGHEST_PRIORITY_TASK` macro is called on every context switch.

    **Stock FreeRTOS:** Finds the highest non-empty priority list, then calls
    `listGET_OWNER_OF_NEXT_ENTRY()` which cycles through tasks round-robin.

    **EDF modification:** When `uxTopPriority > 0` (EDF priority level), we call
    `listGET_HEAD_ENTRY()` instead, which returns the first item in the list.
    Because EDF tasks are inserted via `vListInsert()` sorted by absolute deadline
    (stored in `xItemValue`), the head of the list is always the task with the
    earliest absolute deadline. This gives O(1) task selection.

    The `uxTopPriority > 0` check ensures idle tasks at priority 0 still use the
    original round-robin selection. This cleanly separates EDF scheduling from
    the system tasks that should not participate in deadline-based scheduling.



4.Manage periodic releases
    Each EDF task is periodic, it runs, finishes its job, then sleeps until its next period
    when a task's period expires, update its absolute deadline to the next one (release_time + D) and put it back in the ready queue

5.Admission control (mandatory part of assignment)
    before accepting a new task, check whether the task set is schedulable. Two methods:
        Liu & Layland bound
            simple check, U = ΣC/T ≤ 1.0 (exact for EDF when D = T)
        
        Processor demand analysis
            exact test for when D ≤ T but D ≠ T



6.Deadline miss handling
    -increment counter in TCB.... maybe


EXTRA SMALL THINGS

Config flag
    -inside FreeRTOSConfig.h add, #define configUSE_EDF_SCHEDULER  1 and then wrap every kernel change in 
        #if (configUSE_EDF_SCHEDULER == 1)
            // EDF Logic
        #else
            // original FreeRTOS logic