# User take: TMR2 delays and the CAN/dash impact - m74_9, 2026-10-03

The user's hypothesis and observations from the 2026-10-03 16:10-16:17 car
session (recorded from the user's own words; not yet proven in code).

## Goal

Remove the delays in TMR2 (the angle clock). Today there may be 5-10 us
delays that can persist for some time; then certain events - errors in code,
interrupts, timer glitches - begin to misbehave and throw TMR2 up to
60-80 us.

## The user's refutation of print-based explanations (recorded verbatim-ish,
2026-10-03)

"If there were warnings, they would be in the log. The growth happens even
when there are no logs in the console. In the session above there was maybe
one error the whole time, but there is a lot of lockstats output."

Facts: exactly ONE warning in the whole 16:10-16:17 session (C9009 at
16:12:07, the benign fall+4 rpm-flap mode), while the xsec >=10us counts
are hundreds per window (w3: 1599 total) and the triggerIsr 100-250 us
bucket holds thousands of teeth. Printed diagnostics cannot be the driver
of the 60-80 us class. Recorded as a durable rule in CLAUDE.md.

## The user's causal chain (hypothesis)

1. TMR2 delay baseline: 5-10 us, can hold for a while.
2. Then some events / code errors / interrupts / timers start glitching and
   push TMR2 to 60-80 us.
3. The max spikes (~1188 us triggerIsr, ~960 us trgPostDecode, window 1)
   affect not only TMR2 but also CAN: the console connection to the ECU is
   interrupted briefly.
4. Today (2026-10-03) the dashboard CAN stream stalled: the check-engine
   light came on and the gauges showed nothing - i.e. CAN froze.

## What the lockstats data supports (facts from the same session)

- angclk maxLateUs reached 95 us in the two loaded windows (w3/w4) and
  24-45 us elsewhere; per-type xsec maxUs 81-95 us. So 60-80 us-class TMR2
  entry delays are real and measured.
- The xsec split shows the 60-80 us entries are mostly same-prio
  serialization on the shared TMR2 vector (ser >> handoff), i.e. a previous
  TMR2 handler still executing its actions - NOT the trigger decode.
- The single ~1.2 ms stall in w1 is in trgPostDecode (the trigger handoff),
  not in TMR2 itself; TMR2 only inherits its effects (fires waiting behind
  the handoff, counted as handoff= in xsec).
- CAN is at the bottom of the ISR ladder (prio 12) and the dash heartbeat
  (0x0189 at 10 ms) is a BCM-side timeout: a long-enough CPU-ISR stall chain
  can starve the dash stream without any CAN hardware fault.

## Open items (not yet verified)

- The exact mechanism by which a ~1.2 ms handoff stall or 60-80 us TMR2
  bursts break the console/dash link: CAN TX starvation, ISO-TP timers,
  or a longer wedge (the NT-clock wedge class). No CAN error counters
  (TER/TEC) were captured at the stall instants.
- Which callback bodies produce the TMR2 serialization tails (the ser= class):
  dwell/spark/open/close action-body durations are measured in cbmax only on
  the executor, not on TMR2.
- Whether the dash stall (check light) coincided with the 16:12-16:14
  close-busy storm or with the w1 stall instant.

## Next data points

- lockstats immediately after a dash/console stall + a CAN error-counter
  readout (TER/TEC, bus-off flags) at the same instant.
- Per-type TMR2 action-body duration counters (the ser= tail source).
- A console-log timestamp cross-check of the check-engine event against the
  trigger histograms.
