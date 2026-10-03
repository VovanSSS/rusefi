# Lockstats delay analysis - m74_9 car session 2026-10-03 16:10-16:17

8 lockstats windows, ~850 rpm idle, EFI_ANGLE_CLOCK build (single TMR2,
4 channels by event type). Each window is a delta since the previous
lockstats call (lockstats prints then resets). Units: us unless noted;
tick-based maxima converted at 4 MHz (1 tick = 0.25 us).

## Per-window averages

| window (local) | triggerIsr avg | trgPreDecode | trgDecode | trgPostDecode | schedulerIsr avg | sched exec n | sched maxLateUs | exec 64-256us % | angclk maxLateUs | immediate | open tickPast | close lateArm (all busy) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 16:10:19 | 65.9 | 1.55 | 14.4 | 48.6 | 75.0 | 4694 | 178 | 3.3 | 45 | 88 | 4 | 8 |
| 16:10:29 | 68.7 | 1.55 | 15.0 | 50.8 | 72.5 | 348 | 33 | 0 | 33 | 1 | 0 | 0 |
| 16:13:07 | 56.9 | 1.56 | 12.5 | 41.5 | 80.4 | 13458 | 249 | 8.5 | 95 | 284 | 67 | 1016 |
| 16:14:08 | 52.1 | 1.56 | 11.5 | 37.7 | 86.0 | 7276 | 248 | 9.7 | 95 | 246 | 42 | 1134 |
| 16:14:18 | 63.8 | 1.57 | 13.7 | 47.1 | 72.8 | 697 | 189 | 3.7 | 28 | 8 | 2 | 2 |
| 16:16:46 | 62.0 | 1.56 | 13.4 | 45.8 | 77.4 | 10493 | 189 | 7.9 | 83 | 146 | 36 | 117 |
| 16:17:40 | 67.9 | 1.55 | 14.8 | 50.2 | 70.8 | 2110 | 107 | 1.4 | 28 | 2 | 0 | 0 |
| 16:17:44 | 66.7 | 1.56 | 14.8 | 49.0 | 70.3 | 123 | 101 | 1.6 | 24 | 0 | 0 | 0 |

Global averages (all 8 windows, ~894k trigger teeth):

| phase | global avg, us | share of triggerIsr |
| --- | --- | --- |
| trgPreDecode | 1.56 | 3% |
| trgDecode | 13.0 | 22% |
| trgPostDecode | 43.5 | 73% |
| triggerIsr (total) | 59.3 | 100% |
| schedulerIsr (TIM5) | 79.2 | - |
| sched exec late mode | ~10 (4-16 us bucket) | - |

## Maxima

| metric | max | where |
| --- | --- | --- |
| triggerIsr | 1247 us (4986 ticks), 1 event; otherwise 158-214 us (634-856 ticks) | w1 / rest |
| trgDecode | 100-250 us, 38 events of 894k (0.004%) | w1/w3/w4/w6 |
| trgPostDecode | >1000 us, 1 event; 100-250 us, 3877 events (0.43%) | w1 |
| schedulerIsr | 250-500 us, 7 events | w1/w3/w4 |
| sched exec maxLateUs | 249 us | w3 |
| sched cbmax | 250 us (fuel fallback) | w4 |
| angclk maxLateUs | 95 us | w3/w4 |
| angclk xsec maxUs | close 95 / dwell 88 / spark 86 / open 81 | w3 |

## Average vs maximum (difference)

| metric | avg | max | max-avg | max/avg |
| --- | --- | --- | --- | --- |
| triggerIsr | 59.3 | 1247 | ~1188 us | 21x |
| triggerIsr (no outlier) | 59.3 | 214 | ~155 us | 3.6x |
| trgPostDecode | 43.5 | >1000 | ~960 us | 23x |
| trgPostDecode (no outlier) | 43.5 | 250 | ~206 us | 5.7x |
| trgDecode | 13.0 | 250 | ~237 us | 19x (0.004% of events) |
| schedulerIsr | 79.2 | 500 | ~420 us | 6.3x |
| sched exec | ~10 (mode) | 249 (w3) / 33 (w2) | ~240 / ~25 | ~25x / ~3x |
| angclk ISR entry | <10 (typical) | 95 | ~85 us | ~10x |

## Interpretation

- Averages are stable across all windows (triggerIsr 52-69 us); the variation
  is the scheduling/non-scheduling tooth mix, not degradation. postDecode is
  the dominant phase (73%).
- Large max/avg = rare tails, not a distribution shift: 100-250 us is 2.5% of
  triggerIsr teeth, 0.43% of postDecode, 0.004% of decode. The single >1000 us
  stall (w1) is 1 event per ~100k teeth and shows up in both triggerIsr and
  trgPostDecode histograms (the same event).
- The triggerIsr 50-100 us modal bucket is bin-crossing: the sum of three
  <50 us phases crosses 50 us, it is not a slow phase.
- schedulerIsr modal bucket 50-100 us = the executor batch; sched exec
  late>=10us ~95-100% is a threshold artifact cutting the 4-16 us bucket.
  The 64-256 us exec bucket grows to 8.5-9.7% only in w3/w4 (the C9009 storm
  windows) and falls back to 1.4-3.7% elsewhere.
- angclk xsec (ISR-entry >=10 us per type): in loaded windows ser >> handoff
  (w3: spark ser=179 vs handoff=14) - the entry delay is same-prio
  serialization on the shared TMR2 vector, not waiting for the trigger decode.
  Clean windows (w2/w5/w7/w8): handoff=0 ser=0, maxLateUs 24-33 us.

## Accounting (exact in every window)

- sched fuel n == open lateArm + close lateArm:
  w3 1083 = 67 + 1016, w4 1176 = 42 + 1134, w6 153 = 36 + 117.
- close fired + lateArm == open total (every busy close fell back to TIM5 1:1,
  injector still closed, precision loss only).
- C9009 at 16:12:07: counter-fall = 4 (10436-10432) = cylinderCount,
  charged=0 bail=no -> benign rpm-flap prevSparkName desync, NOT a
  double-charge.
- close-busy storm (w3/w4: 1016/1134 busy) is the injection analog of the
  double-schedule under the flap; self-cleared by w5 (busy=2).

## Health checks (all windows)

- nvic exti=0 handoff=4 tim5=5 canTx=12 canRx0=12 adc=7 (want 0/4/5/12/12/7).
- tim2clk: EN=1 LPEN=1 CEN=1 PSC=71 ARR=FFFFFFFF, sleep T2d~NTd~400k,
  busy T2d~NTd~400k - TMR2 clock exact (PMEN 32-bit holds).
- PLL: SWS=PLL, PLLMS=1 PLLNS=144 PLLFR=4.
- initRate 40001/40000, psc=71 - angle clock PSC rounding correct.

## Session verdict

Healthy at the averages; maxima are rare tails. The only real load was the
w3/w4 close-busy storm around the (benign) C9009, and it self-recovered.
The single ~1.2 ms postDecode stall in w1 is the known rare-tail class.
See docs/tmr2_can_stall_user_take_2026-10-03.md for the user's read of these
spikes (TMR2 5-10 -> 60-80 us degradation, CAN/dash impact).
