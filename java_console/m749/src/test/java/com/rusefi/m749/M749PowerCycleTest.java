package com.rusefi.m749;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

class M749PowerCycleTest {
    private static final long SILENCE = 750;
    private static final long TIMEOUT = 30_000;

    static final class Clock implements M749Identification.Timing {
        long millis;
        public long now() { return millis; }
        public void pause(long ms) { millis += ms; }
    }

    /**
     * ECU broadcasts a frame every {@code period} ms while it has power, and nothing while off.
     * It is alive for {@code now < offAt} and again for {@code now >= onAt}, silent in between.
     */
    static final class ScriptedBus implements RawCanTransport {
        final Clock clock;
        final long offAt, onAt, period;
        long lastEmit;

        ScriptedBus(Clock clock, long offAt, long onAt, long period) {
            this.clock = clock;
            this.offAt = offAt;
            this.onAt = onAt;
            this.period = period;
            this.lastEmit = -period; // Guarantees the first live poll emits without integer overflow.
        }

        public void sendCan(int id, byte[] data) {
            fail("waitForPowerCycle must not transmit while listening");
        }

        public Frame receiveCan() {
            long t = clock.now();
            boolean alive = t < offAt || t >= onAt;
            if (alive && t - lastEmit >= period) {
                lastEmit = t;
                return new Frame(0x600, new byte[]{(byte) t});
            }
            return null;
        }

        public void close() { }
    }

    private static boolean run(ScriptedBus bus, List<String> log) throws IOException, InterruptedException {
        return M749PowerCycle.waitForPowerCycle(bus, bus.clock, log::add, SILENCE, TIMEOUT);
    }

    @Test void confirmsWhenTrafficStopsThenResumes() throws Exception {
        Clock clock = new Clock();
        ScriptedBus bus = new ScriptedBus(clock, 2_000, 5_000, 20);
        List<String> log = new ArrayList<>();
        assertTrue(run(bus, log));
        // Silence must be detected after the ECU actually lost power and success after it came back.
        assertTrue(clock.now() >= 5_000, "confirmed before the ECU powered back on");
        assertTrue(log.stream().anyMatch(s -> s.contains("traffic stopped")));
        assertTrue(log.get(log.size() - 1).contains("power cycle confirmed"));
    }

    @Test void timesOutWhenEcuNeverBroadcasts() {
        Clock clock = new Clock();
        // Never alive: offAt at time 0 with no later restart.
        ScriptedBus bus = new ScriptedBus(clock, 0, Long.MAX_VALUE, 20);
        IOException e = assertThrows(IOException.class, () -> run(bus, new ArrayList<>()));
        assertTrue(e.getMessage().contains("No ECU CAN traffic"));
        assertTrue(clock.now() >= TIMEOUT);
    }

    @Test void timesOutWhenEcuNeverLosesPower() {
        Clock clock = new Clock();
        ScriptedBus bus = new ScriptedBus(clock, Long.MAX_VALUE, Long.MAX_VALUE, 20);
        IOException e = assertThrows(IOException.class, () -> run(bus, new ArrayList<>()));
        assertTrue(e.getMessage().contains("no power-off silence") || e.getMessage().contains("kept broadcasting"));
    }

    @Test void timesOutWhenEcuNeverReturns() {
        Clock clock = new Clock();
        ScriptedBus bus = new ScriptedBus(clock, 2_000, Long.MAX_VALUE, 20);
        List<String> log = new ArrayList<>();
        IOException e = assertThrows(IOException.class, () -> run(bus, log));
        assertTrue(e.getMessage().contains("did not power back on"));
        // It still observed the off transition before giving up on the return.
        assertTrue(log.stream().anyMatch(s -> s.contains("traffic stopped")));
    }

    @Test void ordinaryInterFrameGapsDoNotCountAsPowerOff() throws Exception {
        Clock clock = new Clock();
        // Period 700ms is under the 750ms silence window, so gaps between live frames must not trip it.
        ScriptedBus bus = new ScriptedBus(clock, 3_000, 5_000, 700);
        assertTrue(run(bus, new ArrayList<>()));
        assertTrue(clock.now() >= 5_000);
    }

    @Test void rejectsInvalidWindows() {
        Clock clock = new Clock();
        ScriptedBus bus = new ScriptedBus(clock, 1_000, 2_000, 20);
        assertThrows(IllegalArgumentException.class,
                () -> M749PowerCycle.waitForPowerCycle(bus, clock, s -> {}, 0, TIMEOUT));
        assertThrows(IllegalArgumentException.class,
                () -> M749PowerCycle.waitForPowerCycle(bus, clock, s -> {}, SILENCE, 0));
    }

    @Test void honoursThreadInterruption() {
        Clock clock = new Clock();
        ScriptedBus bus = new ScriptedBus(clock, 0, Long.MAX_VALUE, 20);
        Thread.currentThread().interrupt();
        try {
            assertThrows(InterruptedException.class, () -> run(bus, new ArrayList<>()));
        } finally {
            // Clear the flag so it cannot leak into other tests on this thread.
            Thread.interrupted();
        }
    }
}
