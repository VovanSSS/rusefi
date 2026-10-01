package com.rusefi.m749;

import java.io.IOException;
import java.util.function.Consumer;

/**
 * Confirms a physical bench power cycle by watching normal ECU CAN traffic.
 *
 * <p>The ECU broadcasts periodic frames while it runs. This helper first proves
 * the ECU is alive (frames arriving), then waits for a silence gap that only a
 * loss of power can produce, then returns once broadcasting resumes. It sends
 * nothing, so any received frame is treated as a sign of life.
 */
final class M749PowerCycle {
    private M749PowerCycle() {
    }

    /**
     * Waits for the ECU to go quiet and then resume broadcasting, proving it was power-cycled.
     *
     * @param silenceMillis minimum no-traffic gap that counts as the ECU being off; must exceed the
     *                       ECU's normal broadcast period so ordinary inter-frame gaps do not trigger it
     * @param timeoutMillis  overall budget for the complete alive -> off -> on transition
     * @return {@code true} once traffic resumes after a silence; the method throws rather than
     *         returning {@code false}
     */
    static boolean waitForPowerCycle(RawCanTransport transport, M749Identification.Timing clock,
                                     Consumer<String> out, long silenceMillis, long timeoutMillis)
            throws IOException, InterruptedException {
        if (silenceMillis <= 0 || timeoutMillis <= 0) {
            throw new IllegalArgumentException("Invalid power-cycle windows");
        }
        long deadline = clock.now() + timeoutMillis;
        awaitTraffic(transport, clock, deadline, "No ECU CAN traffic seen; cannot detect a power cycle");
        out.accept("ECU is broadcasting. Switch bench power off, then back on; PCAN can stay connected.");
        awaitSilence(transport, clock, deadline, silenceMillis, out);
        awaitTraffic(transport, clock, deadline, "ECU stayed silent; it did not power back on in time");
        out.accept("ECU CAN traffic resumed; power cycle confirmed.");
        return true;
    }

    /** Drains any pending frames and returns once no frame has arrived for {@code silenceMillis}. */
    private static void awaitSilence(RawCanTransport transport, M749Identification.Timing clock, long deadline,
                                     long silenceMillis, Consumer<String> out)
            throws IOException, InterruptedException {
        long lastSeen = clock.now();
        while (true) {
            check(deadline, clock, "ECU kept broadcasting; no power-off silence was observed");
            if (transport.receiveCan() != null) {
                lastSeen = clock.now();
            } else if (clock.now() - lastSeen >= silenceMillis) {
                out.accept("ECU CAN traffic stopped; waiting for it to power back on.");
                return;
            } else {
                clock.pause(1);
            }
        }
    }

    /** Returns as soon as a single frame arrives, i.e. the ECU is (still or again) alive. */
    private static void awaitTraffic(RawCanTransport transport, M749Identification.Timing clock,
                                     long deadline, String timeoutMessage)
            throws IOException, InterruptedException {
        while (true) {
            check(deadline, clock, timeoutMessage);
            if (transport.receiveCan() != null) {
                return;
            }
            clock.pause(1);
        }
    }

    private static void check(long deadline, M749Identification.Timing clock, String timeoutMessage)
            throws IOException, InterruptedException {
        if (Thread.currentThread().isInterrupted()) {
            throw new InterruptedException("Power-cycle wait interrupted");
        }
        if (clock.now() >= deadline) {
            throw new IOException(timeoutMessage);
        }
    }
}
