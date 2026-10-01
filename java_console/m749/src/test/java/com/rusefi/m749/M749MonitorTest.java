package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import peak.can.basic.TPCANHandle;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.function.Consumer;

import static org.junit.jupiter.api.Assertions.*;

class M749MonitorTest {
    private static final class Harness implements M749Monitor.Backend, M749Monitor.View {
        final M749Monitor monitor = new M749Monitor(this, this);
        final List<String> messages = new ArrayList<>();
        final List<TPCANHandle> opened = new ArrayList<>();
        final List<List<String>> summaries = new ArrayList<>();
        List<PcanDevice.Channel> channels = Collections.emptyList();
        Set<TPCANHandle> dead = Collections.emptySet();
        boolean detected;
        boolean busy;
        boolean failQuery;
        boolean failScan;
        boolean missingLibrary;
        int queries;

        void connected(boolean available) {
            channels = Collections.singletonList(new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS2, available));
        }

        public List<PcanDevice.Channel> scan() throws IOException {
            if (missingLibrary) throw new UnsatisfiedLinkError("PCANBasic_JNI");
            if (failScan) throw new IOException("Driver failed");
            return channels;
        }

        public List<String> identify(PcanDevice.Channel channel, Consumer<String> messages) throws IOException {
            assertTrue(busy);
            opened.add(channel.handle);
            queries++;
            if (dead.contains(channel.handle)) throw new IOException("Open " + channel.handle + ": PCAN_ERROR_NODRIVER");
            if (failQuery) throw new IOException("ECU timeout");
            messages.accept("VIN result");
            return Collections.singletonList("VIN: TESTVIN1234567890");
        }

        public void detection(boolean detected, String detail) { this.detected = detected; }
        public void identification(List<String> summary) { summaries.add(summary); }
        public void message(String message) { messages.add(message); }
        public void busy(boolean busy) { this.busy = busy; }
    }

    @Test
    void detectsHotplugQueriesOnceAndQueriesAgainAfterReconnect() {
        Harness h = new Harness();
        h.monitor.poll(false);
        assertFalse(h.detected);
        assertEquals(0, h.queries);
        h.connected(true);
        h.monitor.poll(false);
        h.monitor.poll(false);
        assertTrue(h.detected);
        assertEquals(1, h.queries);
        assertTrue(h.messages.contains("VIN result"));
        assertEquals(Collections.singletonList("VIN: TESTVIN1234567890"), h.summaries.get(0));
        h.channels = Collections.emptyList();
        h.monitor.poll(false);
        assertFalse(h.detected);
        h.connected(true);
        h.monitor.poll(false);
        assertEquals(2, h.queries);
    }

    @Test
    void failedEcuQueryKeepsDetectedStatusAndRequiresExplicitRetry() {
        Harness h = new Harness();
        h.connected(true);
        h.failQuery = true;
        h.monitor.poll(false);
        h.monitor.poll(false);
        assertTrue(h.detected);
        assertFalse(h.busy);
        assertEquals(1, h.queries);
        assertTrue(h.messages.stream().anyMatch(s -> s.contains("ECU timeout")));
        assertTrue(h.summaries.isEmpty());
        h.monitor.poll(true);
        assertEquals(2, h.queries);
    }

    @Test
    void phantomChannelFailsOverToWorkingAdapterWithoutReattempting() {
        Harness h = new Harness();
        h.channels = Arrays.asList(
                new PcanDevice.Channel(TPCANHandle.PCAN_ISABUS1, true),
                new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS1, true));
        h.dead = Collections.singleton(TPCANHandle.PCAN_ISABUS1);
        h.monitor.poll(false);
        assertEquals(Arrays.asList(TPCANHandle.PCAN_ISABUS1, TPCANHandle.PCAN_USBBUS1), h.opened);
        assertTrue(h.messages.stream().anyMatch(s -> s.contains("PCAN_ERROR_NODRIVER")));
        assertTrue(h.messages.contains("VIN result"));
        assertFalse(h.busy);
        h.monitor.poll(false);
        assertEquals(2, h.queries);
    }

    @Test
    void occupiedAdapterIsDetectedButNeverOpened() {
        Harness h = new Harness();
        h.connected(false);
        h.monitor.poll(false);
        assertTrue(h.detected);
        assertEquals(0, h.queries);
        h.connected(true);
        h.monitor.poll(false);
        assertEquals(1, h.queries);
    }

    @Test
    void missingNativeLibraryIsReportedWithoutFloodingMessages() {
        Harness h = new Harness();
        h.missingLibrary = true;
        h.monitor.poll(false);
        h.monitor.poll(false);
        assertFalse(h.detected);
        assertEquals(0, h.queries);
        assertEquals(1, h.messages.size());
        assertTrue(h.messages.get(0).contains("PCAN native library unavailable"));
    }

    @Test
    void explicitSelectionNeverQueriesAnotherAdapter() {
        Harness h = new Harness();
        h.channels = Arrays.asList(
                new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS1, true),
                new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS2, true));
        h.dead = Collections.singleton(TPCANHandle.PCAN_USBBUS2);
        h.monitor.poll(true, "PCAN_USBBUS2");
        assertEquals(Collections.singletonList(TPCANHandle.PCAN_USBBUS2), h.opened);
        assertTrue(h.summaries.isEmpty());
        h.monitor.poll(true, "PCAN_USBBUS9");
        assertEquals(1, h.opened.size());
    }

    @Test
    void transientScanFailureDoesNotCauseAnotherAuthentication() {
        Harness h = new Harness();
        h.connected(true);
        h.monitor.poll(false);
        h.failScan = true;
        h.monitor.poll(false);
        h.failScan = false;
        h.monitor.poll(false);
        assertTrue(h.detected);
        assertEquals(1, h.queries);
    }
}
