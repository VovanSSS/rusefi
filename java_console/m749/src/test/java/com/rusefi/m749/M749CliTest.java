package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import peak.can.basic.TPCANHandle;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.function.Consumer;

import static org.junit.jupiter.api.Assertions.*;

class M749CliTest {
    private static final class Harness implements M749Monitor.Backend {
        final List<String> messages = new ArrayList<>();
        final List<TPCANHandle> opened = new ArrayList<>();
        List<PcanDevice.Channel> channels = Collections.emptyList();
        TPCANHandle working;

        public List<PcanDevice.Channel> scan() {
            return channels;
        }

        public List<String> identify(PcanDevice.Channel channel, Consumer<String> messages) throws IOException {
            opened.add(channel.handle);
            if (channel.handle != working) throw new IOException("Open " + channel.handle + ": PCAN_ERROR_NODRIVER");
            messages.accept("VIN result");
            return Collections.singletonList("VIN: TESTVIN1234567890");
        }

        int run(String requested, boolean listOnly) throws IOException, InterruptedException {
            return M749Cli.run(requested, listOnly, this, messages::add);
        }
    }

    @Test
    void noAdapterFails() throws Exception {
        Harness h = new Harness();
        assertEquals(1, h.run(null, false));
        assertTrue(h.messages.get(0).contains("PCAN not detected"));
    }

    @Test
    void skipsPhantomChannelsUntilOneAnswers() throws Exception {
        Harness h = new Harness();
        h.channels = Arrays.asList(
                new PcanDevice.Channel(TPCANHandle.PCAN_ISABUS1, true),
                new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS1, true));
        h.working = TPCANHandle.PCAN_USBBUS1;
        assertEquals(0, h.run(null, false));
        assertEquals(Arrays.asList(TPCANHandle.PCAN_ISABUS1, TPCANHandle.PCAN_USBBUS1), h.opened);
        assertTrue(h.messages.stream().anyMatch(s -> s.contains("PCAN_ERROR_NODRIVER")));
        assertTrue(h.messages.contains("VIN result"));
        assertTrue(h.messages.contains("VIN: TESTVIN1234567890"));
    }

    @Test
    void explicitChannelIsUsedDirectlyEvenWhenOccupied() throws Exception {
        Harness h = new Harness();
        h.channels = Arrays.asList(
                new PcanDevice.Channel(TPCANHandle.PCAN_ISABUS1, true),
                new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS1, false));
        h.working = TPCANHandle.PCAN_USBBUS1;
        assertEquals(0, h.run("pcan_usbbus1", false));
        assertEquals(Collections.singletonList(TPCANHandle.PCAN_USBBUS1), h.opened);
    }

    @Test
    void unknownChannelIsUsageError() throws Exception {
        Harness h = new Harness();
        h.channels = Collections.singletonList(new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS1, true));
        assertEquals(2, h.run("PCAN_USBBUS9", false));
        assertTrue(h.opened.isEmpty());
    }

    @Test
    void allChannelsFailingFails() throws Exception {
        Harness h = new Harness();
        h.channels = Collections.singletonList(new PcanDevice.Channel(TPCANHandle.PCAN_ISABUS1, true));
        assertEquals(1, h.run(null, false));
        assertTrue(h.messages.stream().anyMatch(s -> s.contains("No channel produced")));
    }

    @Test
    void listOnlyScansWithoutQuerying() throws Exception {
        Harness h = new Harness();
        h.channels = Collections.singletonList(new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS1, true));
        assertEquals(0, h.run(null, true));
        assertTrue(h.opened.isEmpty());
        assertTrue(h.messages.get(0).contains("PCAN_USBBUS1"));
    }

    @Test
    void occupiedChannelsAreNotAutoSelected() throws Exception {
        Harness h = new Harness();
        h.channels = Collections.singletonList(new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS1, false));
        assertEquals(1, h.run(null, false));
        assertTrue(h.opened.isEmpty());
        assertTrue(h.messages.stream().anyMatch(s -> s.contains("in use")));
    }
}
