package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.util.*;
import java.util.function.Consumer;
import static com.rusefi.m749.M749Identification.bytes;
import static org.junit.jupiter.api.Assertions.*;

class UdsClientTest {
    static class Bus implements DiagnosticTransport, M749Identification.Timing {
        Queue<byte[]> rx = new ArrayDeque<>();
        List<byte[]> tx = new ArrayList<>();
        List<Long> pauses = new ArrayList<>();
        Consumer<byte[]> respond = frame -> {};
        long now;
        public void send(byte[] frame) { tx.add(frame.clone()); respond.accept(frame); }
        public byte[] receive() { now++; return rx.poll(); }
        public void close() { }
        public long now() { return now; }
        public void pause(long ms) { now += ms; pauses.add(ms); }
        UdsClient client() { return new UdsClient(this, this); }
    }

    @Test void honorsFlowControlBlocksStminAndSequenceWrap() throws Exception {
        Bus bus = new Bus();
        byte[] payload = new byte[180];
        payload[0] = 0x36;
        for (int i = 2; i < payload.length; i++) { payload[i] = (byte) i; }
        List<Byte> received = new ArrayList<>();
        int[] frames = {0};
        bus.respond = frame -> {
            int first = frame[0] & 255;
            int start = first >= 0x20 ? 1 : 2;
            for (int i = start; i < 8 && received.size() < payload.length; i++) { received.add(frame[i]); }
            if (first < 0x20) {
                bus.rx.add(bytes(0x31, 0, 0));
                bus.rx.add(bytes(0x30, 4, 0xF1));
            } else {
                assertEquals(0x20 | ((++frames[0]) & 15), first);
                if (received.size() == payload.length) { bus.rx.add(bytes(2, 0x76, 0)); }
                else if (frames[0] % 4 == 0) { bus.rx.add(bytes(0x30, 4, 3)); }
            }
        };
        assertArrayEquals(bytes(0x76, 0), bus.client().exchange(payload, bytes(0x76, 0), 10_000));
        for (int i = 0; i < payload.length; i++) { assertEquals(payload[i], received.get(i)); }
        assertTrue(bus.pauses.contains(1L));
        assertTrue(bus.pauses.contains(3L));
        assertTrue(frames[0] > 16);
    }

    @Test void assemblesMultiframeResponseAndSendsFlowControl() throws Exception {
        Bus bus = new Bus();
        bus.rx.add(bytes(0x10, 10, 0x62, 0xF1, 0x98, 1, 2, 3));
        bus.rx.add(bytes(0x21, 4, 5, 6, 7));
        assertArrayEquals(bytes(0x62, 0xF1, 0x98, 1, 2, 3, 4, 5, 6, 7),
                bus.client().exchange(bytes(0x22, 0xF1, 0x98), bytes(0x62, 0xF1, 0x98), 10_000));
        assertEquals(0x30, bus.tx.get(1)[0]);
    }

    @Test void pendingAndUnrelatedMessagesCannotExtendOverallDeadline() {
        Bus bus = new Bus();
        for (int i = 0; i < 100; i++) { bus.rx.add(bytes(3, 0x7F, 0x31, 0x78)); }
        assertThrows(IOException.class, () -> bus.client().exchange(bytes(0x31, 1), bytes(0x71, 1), 20));
        assertEquals(1, bus.tx.size());
        assertTrue(bus.now <= 21);
    }

    @Test void rejectsOverflowWaitFloodReservedStminAndMissingFlowControlWithoutRetry() {
        for (byte[] fc : new byte[][]{bytes(0x32, 0, 0), bytes(0x30, 0, 0x80), bytes(0x30), bytes(2, 0x76, 0)}) {
            Bus bus = new Bus();
            bus.rx.add(fc);
            assertThrows(IOException.class, () -> bus.client().exchange(new byte[20], bytes(0x76, 0), 500));
            assertEquals(1, bus.tx.size());
        }
        Bus waits = new Bus();
        for (int i = 0; i < 4; i++) { waits.rx.add(bytes(0x31, 0, 0)); }
        assertThrows(IOException.class, () -> waits.client().exchange(new byte[20], bytes(0x76, 0), 500));
        assertEquals(1, waits.tx.size());
        Bus silent = new Bus();
        assertThrows(IOException.class, () -> silent.client().exchange(new byte[20], bytes(0x76, 0), 500));
        assertEquals(1, silent.tx.size());
    }

    @Test void timeoutNamesRequestAndIsoTpPhase() {
        Bus silent = new Bus();
        UdsClient.Timeout response = assertThrows(UdsClient.Timeout.class,
                () -> silent.client().exchange(bytes(0x28, 1, 1), bytes(0x68, 1), 500));
        assertEquals(0x28, response.sid);
        assertEquals("ISO-TP/UDS timeout: SID 28 awaiting the response; request was not retried", response.getMessage());

        Bus noFlowControl = new Bus();
        byte[] write = new byte[520]; write[0] = 0x3D;
        UdsClient.Timeout fc = assertThrows(UdsClient.Timeout.class,
                () -> noFlowControl.client().exchange(write, bytes(0x7D), 500));
        assertEquals(1, noFlowControl.tx.size());
        assertEquals("ISO-TP/UDS timeout: SID 3D awaiting flow control for a 520-byte request (6 bytes sent); request was not retried",
                fc.getMessage());

        Bus pending = new Bus();
        pending.rx.add(bytes(3, 0x7F, 0x31, 0x78));
        UdsClient.Timeout afterPending = assertThrows(UdsClient.Timeout.class,
                () -> pending.client().exchange(bytes(0x31, 1), bytes(0x71, 1), 500));
        assertEquals("ISO-TP/UDS timeout: SID 31 awaiting the response after a 7F 78 pending reply; request was not retried",
                afterPending.getMessage());

        Bus truncated = new Bus();
        truncated.rx.add(bytes(0x10, 10, 0x62, 0xF1, 0x98, 1, 2, 3));
        UdsClient.Timeout partial = assertThrows(UdsClient.Timeout.class,
                () -> truncated.client().exchange(bytes(0x22, 0xF1, 0x98), bytes(0x62, 0xF1, 0x98), 500));
        assertEquals("ISO-TP/UDS timeout: SID 22 receiving a 10-byte multi-frame response (6 bytes received); request was not retried",
                partial.getMessage());
    }

    @Test void rejectsOutOfOrderResponsesAndNegativeReply() {
        Bus bus = new Bus();
        bus.rx.add(bytes(0x10, 10, 0x62, 0xF1, 0x98, 1, 2, 3));
        bus.rx.add(bytes(0x22, 4, 5, 6, 7));
        assertThrows(IOException.class, () -> bus.client().exchange(bytes(0x22, 0xF1, 0x98), bytes(0x62, 0xF1, 0x98), 500));
        Bus negative = new Bus();
        negative.rx.add(bytes(3, 0x7F, 0x36, 0x72));
        UdsClient.NegativeResponse error = assertThrows(UdsClient.NegativeResponse.class,
                () -> negative.client().exchange(bytes(0x36, 0, 1), bytes(0x76, 0), 500));
        assertEquals(0x72, error.code);
    }

    @Test void receivesMaximumHelperBlockWithFiniteFlowControlAndWrapAndIgnoresPadding() throws Exception {
        for (int block : new int[]{0, 1, 16}) {
            Bus bus = new Bus();
            byte[] response = new byte[4085]; response[0] = 0x63;
            for (int i = 1; i < response.length; i++) { response[i] = (byte) (i * 17); }
            int[] position = {6}, sequence = {1}, controls = {0};
            bus.respond = frame -> {
                if (frame[0] != 0x30) {
                    byte[] first = bytes(0x1f, 0xf5, 0, 0, 0, 0, 0, 0);
                    System.arraycopy(response, 0, first, 2, 6); bus.rx.add(first); return;
                }
                controls[0]++;
                assertEquals(block, frame[1] & 255); assertEquals(3, frame[2]);
                for (int i = 0; (block == 0 || i < block) && position[0] < response.length; i++) {
                    byte[] cf = new byte[8]; Arrays.fill(cf, (byte) 0xA5);
                    cf[0] = (byte) (0x20 | sequence[0]); sequence[0] = (sequence[0] + 1) & 15;
                    int count = Math.min(7, response.length - position[0]);
                    System.arraycopy(response, position[0], cf, 1, count); position[0] += count; bus.rx.add(cf);
                }
            };
            assertArrayEquals(response, new UdsClient(bus, bus, block, 3).exchange(
                    bytes(0x23, 8, 0, 0, 0, 15, 240), bytes(0x63), 15000));
            assertEquals(block == 0 ? 1 : (583 + block - 1) / block, controls[0]);
        }
    }
}
