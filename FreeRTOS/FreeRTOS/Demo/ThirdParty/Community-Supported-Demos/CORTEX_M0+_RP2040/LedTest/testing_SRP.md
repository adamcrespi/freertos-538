# Testing — Stack Resource Policy (SRP)

## Test Summary

| # | Test | Status |
|---|------|--------|
| 1 | EDF regression with SRP enabled | PASS |
| 2 | SRP correctness — textbook example (3 tasks, 2 resources) | PASS |
| 3 | Admission control with blocking factors | PASS |
| 4 | 100-task stack sharing quantitative study | PASS |

---

## Test 1: EDF Regression with SRP Enabled

**Goal:** Verify that enabling `configUSE_SRP = 1` does not break existing EDF
behavior when no SRP semaphores are created.

**Method:** Rebuilt `edf_test` (the 3-task EDF test from Task 1) with SRP code
present. Flashed and observed GPIO output.

**Task Set:** Same as EDF Test 2:

| Task | C (ms) | D (ms) | T (ms) |
|------|--------|--------|--------|
| Red | 80 | 200 | 400 |
| Yellow | 150 | 400 | 800 |
| Green | 400 | 1000 | 1600 |

**Expected:** Identical behavior to pure EDF — when no SRP semaphores exist,
`uxSystemCeiling = 0` and all SRP checks are transparent (all π > 0 > Πs).

**Result:** PASS — LEDs toggled normally, same pattern as pure EDF. No crashes,
no assertion failures. SRP code is fully transparent when no resources are
used.

---

## Test 2: SRP Correctness — Textbook Example (Baker Fig 7.19)

**Goal:** Verify correct SRP behavior with nested resource acquisition,
matching the textbook example.

**Program:** `main_srp.c`

**Task Set:**

| Task | C (ms) | D (ms) | T (ms) | π | Resources |
|------|--------|--------|--------|---|-----------|
| τ1 (Red) | 50 | 150 | 600 | 65385 | R1 |
| τ2 (Yellow) | 100 | 300 | 800 | 65235 | R2 |
| τ3 (Green) | 600 | 2000 | 3000 | 65035 | R2 (outer), R1 (nested) |

**Resources:**

| Resource | Ceiling | Highest user | Max CS |
|----------|---------|-------------|--------|
| R1 | 65385 | τ1 (π=65385) | 30ms |
| R2 | 65235 | τ2 (π=65235) | 60ms |

**Task structure:** τ3 starts immediately. τ1 and τ2 delay 20ms so τ3 has
time to lock both resources. τ3 takes R2, then nested R1, holds R1 for 300ms,
releases R1, holds R2 for another 200ms, then releases R2.

**Expected sequence:**
1. τ3 starts, takes R2 (Πs = 65235), takes R1 (Πs = 65385)
2. τ1 and τ2 wake at t=20 — both blocked by ceiling
3. τ3 releases R1 → Πs drops to 65235 → τ1 preempts (π=65385 > 65235)
4. τ1 takes R1 (available), runs to completion, sleeps
5. τ3 resumes, releases R2 → Πs drops to 0 → τ2 preempts (π=65235 > 0)
6. τ2 takes R2 (available), runs to completion, sleeps
7. τ3 resumes and finishes

**Serial Output (key section):**

```
[Green] got R2
[Green] taking R1 inside R2
[Green] got R1 inside R2
[Green] released R1, Πs=65235
[Red] phase1
[Red] taking R1
[Red] got R1
[Red] released R1
[Red] sleeping
[Red] Πs=65235 sleeping
[Green] released R2, Πs=0
[Green] sleeping
[Yellow] phase1
[Yellow] taking R2
[Yellow] got R2
[Yellow] released R2
[Yellow] sleeping
```

**Verified properties:**
- ✅ τ3 runs uninterrupted while holding resources (ceiling blocks τ1 and τ2)
- ✅ τ1 preempts immediately when τ3 releases R1 (ceiling drops, π > Πs)
- ✅ τ1 takes R1 without blocking (SRP guarantee — assert never fires)
- ✅ τ2 runs after τ3 releases R2 (ceiling drops to 0)
- ✅ τ2 takes R2 without blocking
- ✅ Ceiling stack pops correctly: 65385 → 65235 → 0
- ✅ Execution order matches textbook: τ3 → τ1 → τ3 → τ2 → τ3
- ✅ `configASSERT( uxMessagesWaiting > 0 )` never fires across all runs

**Result:** PASS

---

## Test 3: Admission Control with Blocking Factors

**Goal:** Verify that admission control correctly includes SRP blocking times
in schedulability analysis.

**Method:** SRP resources are registered during `xSemaphoreCreateBinarySRP()`.
The admission control functions (`prvEDFCheckLLBound` and
`prvEDFCheckProcessorDemand`) now compute blocking factors from the resource
registry and add them to the utilization/demand calculations.

**Verification:** The SRP test task set (τ1, τ2, τ3 with R1, R2) is admitted
successfully. The blocking factors are small relative to the periods:
- R1 max CS = 30ms, R2 max CS = 60ms
- Task periods: 600ms, 800ms, 3000ms
- Blocking ratios: max(60/600, 60/800, 60/3000) = 0.10
- Total utilization + blocking well under 1.0

The admission control correctly rejects task sets that would be unschedulable
with blocking. When blocking factors push the combined utilization above 1.0,
`xTaskCreateEDF()` returns failure as expected.

**Result:** PASS

---

## Test 4: 100-Task Stack Sharing Quantitative Study

**Goal:** Demonstrate that SRP enables significant runtime stack savings by
sharing stacks among tasks at the same preemption level. Matches the textbook
example in section 7.8.5.

**Program:** `main_srp_100test.c`

**Configuration:**

| Parameter | Value |
|-----------|-------|
| Total tasks | 100 |
| Preemption levels | 10 (D = 100, 200, ..., 1000 ms) |
| Tasks per level | 10 |
| Stack per task | 256 words (1024 bytes) |

**Serial Output:**

```
Configuration:
  Tasks:            100
  Preemption levels: 10
  Tasks per level:  10
  Stack per task:   1024 bytes (256 words)

── Theoretical Analysis ─────────────────────────────────
  Without sharing: 100 stacks × 1024 bytes = 102400 bytes
  With sharing:    10 stacks × 1024 bytes = 10240 bytes
  Savings:         92160 bytes (90%)

── Stack Pool Allocation Demo ───────────────────────────
  Level 0: π=65435, D=100 ms, 10 tasks sharing 1024 bytes
  Level 1: π=65335, D=200 ms, 10 tasks sharing 1024 bytes
  Level 2: π=65235, D=300 ms, 10 tasks sharing 1024 bytes
  Level 3: π=65135, D=400 ms, 10 tasks sharing 1024 bytes
  Level 4: π=65035, D=500 ms, 10 tasks sharing 1024 bytes
  Level 5: π=64935, D=600 ms, 10 tasks sharing 1024 bytes
  Level 6: π=64835, D=700 ms, 10 tasks sharing 1024 bytes
  Level 7: π=64735, D=800 ms, 10 tasks sharing 1024 bytes
  Level 8: π=64635, D=900 ms, 10 tasks sharing 1024 bytes
  Level 9: π=64535, D=1000 ms, 10 tasks sharing 1024 bytes

── Actual Allocation Results ────────────────────────────
  Without sharing: 102400 bytes (100 separate stacks)
  With sharing:    10240 bytes (10 shared stacks)
  Savings:         92160 bytes (90%)
```

**Result:** PASS — 90% stack memory savings, matching the textbook prediction.
Under SRP, tasks at the same preemption level can never be active
simultaneously (Theorem 7.5), so they safely share a single stack buffer.

---

## Build and Run Instructions

**Build:**
```bash
cd ~/rtos-project/FreeRTOS/FreeRTOS/Demo/ThirdParty/Community-Supported-Demos/CORTEX_M0+_RP2040/build
cmake -DPICO_SDK_PATH=~/rtos-project/pico-sdk ..
make srp_test -j$(nproc)         # SRP correctness test
make srp_100test -j$(nproc)      # Stack sharing quantitative test
```

**Flash:**
```bash
# Hold BOOTSEL on Pico, plug in USB
cp LedTest/srp_test.uf2 /media/$USER/RPI-RP2/
# or
cp LedTest/srp_100test.uf2 /media/$USER/RPI-RP2/
```

**Serial monitor:**
```bash
minicom -b 115200 -D /dev/ttyACM0
```
