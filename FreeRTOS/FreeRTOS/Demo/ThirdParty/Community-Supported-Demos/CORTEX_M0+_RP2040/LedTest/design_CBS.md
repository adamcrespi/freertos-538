# Design — Constant Bandwidth Server (CBS) for FreeRTOS

## Overview

This document describes the design of the Constant Bandwidth Server (CBS)
implemented on top of the EDF scheduler within the FreeRTOS kernel for the
RP2040 (Cortex-M0+). CBS enables soft real-time aperiodic tasks to coexist
with hard real-time periodic tasks under EDF scheduling, with guaranteed
bandwidth isolation.

All CBS modifications are wrapped in `#if ( configUSE_CBS == 1 )` guards
nested inside `#if ( configUSE_EDF_SCHEDULER == 1 )`, since CBS depends on
EDF scheduling. Setting `configUSE_CBS` to 0 restores pure EDF behavior.

---

## 1. Background: The Aperiodic Task Problem

EDF handles periodic tasks with known WCETs and deadlines. Aperiodic tasks
(event-driven, irregular arrival, variable execution time) don't fit this
model. If aperiodic tasks run freely under EDF, they can steal CPU time from
periodic tasks and cause deadline misses. If they run at low priority, they
get poor response times.

CBS (Abeni and Buttazzo, 1998) solves this by giving each aperiodic task a
**CPU bandwidth reservation** defined by two parameters:
- **Qs** — maximum budget (computation time per server period)
- **Ts** — server period (replenishment interval)
- **Us = Qs/Ts** — reserved bandwidth fraction

The CBS guarantees that the aperiodic task can never consume more than its
reserved bandwidth, regardless of how much work it tries to do. From the
schedulability perspective, a CBS with bandwidth Us behaves exactly like a
periodic task with WCET=Qs and period=Ts.

---

## 2. CBS Rules

### Rule 1: Budget Exhaustion

When the CBS task's remaining budget qs reaches 0:
1. Server deadline is postponed: ds = ds + Ts
2. Budget is replenished: qs = Qs
3. The task remains in the ready queue with the new, later deadline
4. A context switch is triggered (a periodic task may now have earlier deadline)

This is the core mechanism. The task keeps running but with progressively
later deadlines, giving it lower EDF priority. It effectively gets pushed
behind periodic tasks that have earlier deadlines.

### Rule 2: Job Arrival When Server Is Idle

When a new aperiodic job arrives at time r and the server is idle, the system
checks whether the remaining budget is too large relative to the time until
the current deadline:

    if qs * Ts > (ds - r) * Qs:
        ds = r + Ts      (generate fresh deadline)
        qs = Qs           (full budget)

This prevents a situation where leftover budget from a previous job, combined
with an early deadline, would let the server exceed its reserved bandwidth.

The integer form avoids floating-point division on the Cortex-M0+ (no FPU).

### Rule 3: Scheduling

The CBS server is scheduled by EDF using ds as its deadline, alongside
periodic tasks. This requires no special treatment — the CBS task's
`xAbsoluteDeadline` field serves as both the EDF scheduling key and the
server deadline.

### Rule 4: Tie-Breaking

Per the assignment requirement, when a CBS task and a periodic task have the
same absolute deadline, the CBS task runs first. This improves aperiodic
response time without affecting periodic task schedulability.

### Rule 5: Admission Control

A set of periodic tasks and CBS servers is schedulable by EDF if:

    Σ(Ci/Ti) + Σ(Qs,k/Ts,k) ≤ 1

CBS tasks are registered in the EDF task registry with Qs as WCET and Ts as
period, so the existing admission control math handles them transparently.

---

## 3. Architecture

CBS reuses the existing EDF infrastructure with minimal additions. A CBS task
IS an EDF task — it has `xIsEDFTask = pdTRUE` and is scheduled from the same
ready list. The only differences are:

1. **Budget tracking:** Each tick the CBS task runs, its budget decrements
2. **Deadline postponement:** When budget hits 0, deadline moves forward by Ts
3. **Dynamic deadline:** Unlike periodic tasks (deadline = release + D), CBS
   tasks have deadlines that change at runtime based on budget exhaustion

### TCB Extensions

Added inside `#if ( configUSE_CBS == 1 )`, nested in the EDF block:

| Field | Type | Purpose |
|-------|------|---------|
| `xIsCBSTask` | `BaseType_t` | `pdTRUE` if managed by a CBS |
| `xCBSBudget` | `TickType_t` | qs — current remaining budget |
| `xCBSMaxBudget` | `TickType_t` | Qs — max budget per server period |
| `xCBSServerPeriod` | `TickType_t` | Ts — server replenishment period |

The existing EDF fields are reused:
- `xAbsoluteDeadline` = ds (server deadline, used for EDF scheduling)
- `xPeriod` = Ts (used for periodic release if CBS task sleeps)
- `xRelativeDeadline` = Ts (CBS deadline equals period)
- `xWCET` = Qs (for admission control registry)

---

## 4. Budget Tracking

Budget decrement occurs in `xTaskIncrementTick()`, immediately after the tick
count is incremented and before delayed task processing. On each tick:

```
if current task is CBS and budget > 0:
    budget--
    if budget == 0:
        deadline += Ts          (postpone)
        budget = Qs             (replenish)
        update list item value  (re-sort ready list)
        remove and re-insert into ready list
        trigger context switch
```

The re-insertion into the ready list is necessary because the deadline
changed. `prvAddTaskToReadyList` uses `vListInsert` which sorts by deadline,
so the task moves to its correct position.

Budget only decrements when the CBS task is actually the running task
(`pxCurrentTCB`). If the CBS task is preempted, its budget is preserved.
This is correct — budget represents actual CPU consumption.

---

## 5. Job Arrival Handling

When a CBS task wakes from the delayed list (new aperiodic job arrives), the
Rule 2 check replaces the normal EDF deadline update. Inside the
`xTaskIncrementTick()` delayed task processing:

```
if task is CBS:
    apply Rule 2 (check if fresh deadline needed)
    set list item value to current deadline
else if task is EDF:
    normal EDF deadline update (release + D)
```

CBS tasks do not use `xNextReleaseTime` for deadline computation. Their
deadline is managed entirely by the CBS rules (budget exhaustion and job
arrival).

---

## 6. Tie-Breaking

In `taskSELECT_HIGHEST_PRIORITY_TASK`, after selecting the head of the ready
list (earliest deadline), the macro checks if a CBS task exists at the same
deadline. If the head is a non-CBS task and a CBS task has an equal deadline,
the CBS task is selected instead.

This only scans tasks with identical deadlines (same `xItemValue`), so the
overhead is minimal — typically zero or one additional comparison.

---

## 7. Task Creation

`xTaskCreateCBS()` is similar to `xTaskCreateEDF()`:

1. Admission control: checks if adding Us = Qs/Ts keeps total U ≤ 1
2. Creates task via `prvCreateTask()` with priority 1 (same as all EDF tasks)
3. Sets EDF fields: deadline = currentTick + Ts, period = Ts
4. Sets CBS fields: budget = Qs, maxBudget = Qs, serverPeriod = Ts
5. Registers (Qs, Ts, Ts) in the EDF task registry for future admission checks
6. Adds to ready list

Regular EDF tasks created via `xTaskCreateEDF()` have `xIsCBSTask = pdFALSE`
and budget fields initialized to 0, so CBS code paths are never triggered for
periodic tasks.

---

## 8. Configuration

| Define | Default | Purpose |
|--------|---------|---------|
| `configUSE_CBS` | 1 | Enable/disable CBS (requires `configUSE_EDF_SCHEDULER == 1`) |
| `configMAX_CBS_SERVERS` | 16 | Maximum number of CBS servers (informational) |

CBS tasks share the `configMAX_EDF_TASKS` registry with periodic tasks, so no
separate limit is needed for admission control.

---

## 9. Design Rationale

**Why reuse EDF infrastructure instead of a separate scheduler?**
CBS tasks are scheduled by EDF using their server deadline. They belong in the
same ready list as periodic tasks. No separate scheduling mechanism is needed.

**Why decrement budget in the tick handler?**
The tick handler runs every millisecond and already handles task state changes.
Decrementing the budget here gives tick-accurate accounting. The alternative
(decrementing in context switch hooks) would lose precision for tasks that run
across multiple ticks without preemption.

**Why remove and re-insert on deadline postponement?**
FreeRTOS lists are sorted by item value. Changing `xItemValue` without
re-inserting would leave the task in the wrong position, breaking EDF
ordering. The remove + insert operation is O(n) but happens at most once per
server period per CBS task.

**Why is admission control transparent?**
A CBS with (Qs, Ts) has the same utilization as a periodic task with (C=Qs,
T=Ts). Registering CBS tasks with these values in the EDF task registry
means the existing LL bound and processor demand tests work without
modification.
