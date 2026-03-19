# Design — Stack Resource Policy (SRP) for FreeRTOS

## Overview

This document describes the design of the Stack Resource Policy (SRP)
implemented on top of the EDF scheduler within the FreeRTOS kernel for the
RP2040 (Cortex-M0+). SRP replaces FreeRTOS's built-in Priority Inheritance
Protocol for binary semaphores, providing three guarantees: a task blocks at
most once, once started a task never blocks, and deadlocks are impossible.

All SRP modifications are wrapped in `#if ( configUSE_SRP == 1 )` guards
nested inside `#if ( configUSE_EDF_SCHEDULER == 1 )`, since SRP depends on
EDF scheduling. Setting `configUSE_SRP` to 0 restores pure EDF behavior.

---

## 1. Background: SRP vs Priority Inheritance

FreeRTOS uses the Priority Inheritance Protocol (PIP) for mutexes. Under PIP,
when task A blocks on a mutex held by task B, B inherits A's priority. This
can lead to chain blocking (multiple blocks per task) and unnecessary context
switches.

Under SRP, blocking happens at **preemption time**, not at resource access time.
A task is prevented from starting if it might later block on a resource. Once
a task starts, all resources it needs are guaranteed available. This is
enforced by comparing the task's preemption level against a global system
ceiling.

**Key SRP properties (Baker, 1991):**
- A task blocks at most once (Theorem 7.6)
- Once started, a task never blocks (Theorem 7.5)
- Deadlocks are impossible (Theorem 7.7)
- Runtime stack sharing is possible among tasks at the same preemption level

---

## 2. Core Concepts

### 2.1 Preemption Level (π)

A static value assigned to each task at creation, based on its relative
deadline. For EDF:

    πi > πj  ⟺  Di < Dj

Implementation: `π = configMAX_PREEMPTION_LEVEL - xRelativeDeadline`

Unlike dynamic EDF priority (based on absolute deadline), preemption level is
fixed and used to predict which tasks could preempt in the future. Tasks with
the same relative deadline get the same preemption level, enabling stack
sharing.

### 2.2 Resource Ceiling

Each SRP binary semaphore has a ceiling value — the maximum preemption level
of all tasks that use the resource. For a binary semaphore:

- **Available:** ceiling contributes 0 to the system ceiling
- **Locked:** ceiling = max{πi} of all tasks that use this resource

The ceiling is specified by the programmer at semaphore creation time via
`xSemaphoreCreateBinarySRP(ceiling, maxCSLength)`. The kernel does not
automatically compute ceilings.

### 2.3 System Ceiling (Πs)

The maximum ceiling of all currently held resources:

    Πs = max of all held resource ceilings (0 if nothing held)

Tracked with a **stack** (as recommended by Baker, section 7.8.6):
- Lock resource: push old Πs, set Πs = max(Πs, resource ceiling)
- Unlock resource: pop Πs from stack

The stack handles nested resource acquisition. If a task holds R2 (ceiling
65235) then locks R1 (ceiling 65385), Πs rises to 65385. When R1 is released,
Πs is restored to 65235 from the stack.

### 2.4 The SRP Preemption Test

A task τ may preempt only if both conditions hold:
1. τ has the highest EDF priority among ready tasks (earliest absolute deadline)
2. π(τ) > Πs (preemption level strictly greater than system ceiling)

When no resources are held (Πs = 0), condition 2 is always true for EDF tasks
(all have π > 0), so EDF behaves normally with zero overhead.

---

## 3. Architecture: Option A — Modify Existing Paths

SRP semaphores are created with a new creation function but accessed via the
standard `xSemaphoreTake` / `xSemaphoreGive` macros. The semaphore internally
knows which protocol to use based on `uxResourceCeiling > 0` in the queue
struct.

**Take path:** `xSemaphoreTake` → `xQueueSemaphoreTake` → SRP fast path
(if `uxResourceCeiling > 0`): assert resource available, set
`uxMessagesWaiting = 0`, push ceiling, return immediately. No blocking, no
waiting lists, no priority inheritance.

**Give path:** `xSemaphoreGive` → `xQueueGenericSend` → `prvCopyDataToQueue`
→ SRP path (if `uxResourceCeiling > 0`): pop ceiling from stack, check if any
ready task should now preempt. The `uxMessagesWaiting` increment at the end of
`prvCopyDataToQueue` restores the semaphore to available state.

Regular binary semaphores and mutexes (with `uxResourceCeiling = 0`) are
completely unaffected — the SRP check is skipped and existing behavior runs.

---

## 4. System Ceiling Stack

Global state in `tasks.c`:

```c
static UBaseType_t uxSystemCeiling = 0;
static UBaseType_t uxCeilingStack[ configMAX_SRP_NESTING ];
static UBaseType_t uxCeilingStackIndex = 0;
```

Manipulated by `vSRPPushCeiling()` and `vSRPPopCeiling()`, which are called
from the take/give paths in `queue.c`. These functions do not use their own
critical sections because they are always called from within the caller's
existing critical section.

Maximum nesting depth `configMAX_SRP_NESTING` defaults to 32, which is
generous — the textbook example nests only 2 deep. An assert catches
overflow.

---

## 5. Preemption Test Integration

The SRP preemption test is checked in two places:

**A. `xTaskIncrementTick()` — task wakes from delay:**
The existing EDF check (`xAbsoluteDeadline < pxCurrentTCB->xAbsoluteDeadline`)
is extended with `&& pxTCB->uxPreemptionLevel > uxSystemCeiling`. A waking
task can only preempt if it passes both the EDF priority test and the SRP
ceiling test.

**B. `prvCopyDataToQueue()` — resource released:**
After popping the ceiling stack, `xSRPCheckPreemptionNeeded()` walks the
ready list to find the first task whose π > Πs and has an earlier absolute
deadline than the current task. If found, signals a context switch.

The `taskSELECT_HIGHEST_PRIORITY_TASK` macro is left unchanged — it always
selects the earliest-deadline task. The SRP ceiling only gates preemption
at wake time and resource release time, not during general task selection.
This ensures that a task holding a resource can always resume after being
preempted by a higher-π task.

---

## 6. Admission Control with Blocking

The EDF admission control from Task 1 is extended to include blocking factors.
From the textbook (equation 7.18), the maximum blocking time for task τi is:

    Bi = max{ δj,k | πj < πi AND ceiling(Sk) ≥ πi }

This is the longest critical section belonging to any task with lower
preemption level, guarding a resource with ceiling ≥ πi. Implemented in
`prvSRPComputeBlockingTime()`.

**LL bound with blocking (equation 7.20):**

    Σ(Ci/Ti) + max_i(Bi/Ti) ≤ 1

The maximum blocking ratio across all tasks is added to the total utilization.

**Processor demand with blocking (equation 7.25):**

At each testing point L, the maximum critical section length across all SRP
resources is added conservatively to the demand. This is an over-approximation
that ensures correctness.

**Resource registry:** Each call to `xSemaphoreCreateBinarySRP()` registers
the resource's ceiling and worst-case critical section length in
`xSRPResourceRegistry[]`. The admission control functions query this registry
to compute blocking times.

---

## 7. Stack Sharing

Under SRP, tasks at the same preemption level can never be active
simultaneously (Baker's Theorem 7.5). Once a task starts, it runs to
completion without blocking. Therefore, tasks with the same π can share
a single runtime stack.

**Stack pool:** A pool indexed by preemption level manages shared stack
buffers. When a task is created:
1. Compute π from relative deadline
2. Search pool for matching π
3. If found: reuse existing stack buffer (grow if needed)
4. If not found: allocate new buffer, add to pool

**Savings:** For N tasks distributed across K preemption levels with the same
stack size S, total stack memory drops from N × S to K × S. With 100 tasks
on 10 levels, this is a 90% reduction, matching the textbook example
(section 7.8.5).

---

## 8. Configuration

| Define | Default | Purpose |
|--------|---------|---------|
| `configUSE_SRP` | 1 | Enable/disable SRP (requires `configUSE_EDF_SCHEDULER == 1`) |
| `configMAX_SRP_NESTING` | 32 | Maximum depth of nested resource acquisitions |
| `configMAX_PREEMPTION_LEVEL` | 0xFFFF | Base value for preemption level computation |
| `configMAX_SRP_RESOURCES` | 16 | Maximum number of SRP semaphores for admission control |

---

## 9. Design Rationale

**Why modify existing take/give instead of new API?**
The assignment says "extend the semaphore implementation." Modifying the
existing path means `xSemaphoreTake` / `xSemaphoreGive` work transparently
for both SRP and regular semaphores. The semaphore itself knows which
protocol to use.

**Why user-specified ceilings?**
Auto-computing ceilings requires knowing all future tasks at semaphore
creation time. Since tasks are created individually, the system cannot predict
who will use which resource. The textbook assumes ceilings are known at design
time.

**Why a stack for the system ceiling?**
Nested resource acquisition requires restoring previous ceiling values.
A stack naturally handles this and is recommended by Baker (section 7.8.6).

**Why not modify `taskSELECT_HIGHEST_PRIORITY_TASK`?**
The task that holds a resource (and thus raised the ceiling) must always be
able to resume when higher-priority tasks finish. If the task selector
applied the SRP ceiling check, the resource holder could be blocked by its
own ceiling. The SRP test only gates new preemptions, not ongoing scheduling.
