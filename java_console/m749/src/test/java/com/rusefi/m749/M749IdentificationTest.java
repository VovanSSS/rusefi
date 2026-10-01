package com.rusefi.m749;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.function.Consumer;

import static com.rusefi.m749.M749Identification.bytes;
import static org.junit.jupiter.api.Assertions.*;

class M749IdentificationTest {
    private static final class Harness implements DiagnosticTransport, M749Identification.Timing {
        final Queue<byte[]> incoming = new ArrayDeque<>();
        final List<byte[]> sent = new ArrayList<>();
        final List<String> messages = new ArrayList<>();
        final List<Integer> dids = new ArrayList<>();
        Consumer<byte[]> responder = this::normalReply;
        long clock;
        boolean zeroSeed;
        int flowControls;

        Map<Integer, byte[]> run() throws IOException, InterruptedException {
            return new M749Identification(this, messages::add, this).run();
        }

        public void send(byte[] frame) {
            assertEquals(8, frame.length);
            sent.add(frame.clone());
            if (frame[0] == 0x30) {
                assertArrayEquals(bytes(0x30, 0, 5, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC), frame);
                flowControls++;
            } else {
                responder.accept(Arrays.copyOfRange(frame, 1, 1 + frame[0]));
            }
        }

        void normalReply(byte[] request) {
            if (request[0] == 0x10) {
                assertArrayEquals(bytes(0x10, 3), request);
                reply(bytes(0x50, 3, 0, 50, 1, 0xF4));
            } else if (request[0] == 0x27 && request[1] == 1) {
                assertArrayEquals(bytes(0x27, 1, 0), request);
                reply(zeroSeed ? bytes(0x67, 1, 0, 0, 0, 0) : bytes(0x67, 1, 0x4F, 0x95, 0xB9, 0x3A));
            } else if (request[0] == 0x27 && request[1] == 2) {
                reply(bytes(0x67, 2));
            } else if (request[0] == 0x22) {
                int did = ((request[1] & 0xFF) << 8) | (request[2] & 0xFF);
                dids.add(did);
                if (did == 0xF190) {
                    byte[] vin = "TESTVIN1234567890".getBytes(StandardCharsets.US_ASCII);
                    byte[] result = Arrays.copyOf(bytes(0x62, 0xF1, 0x90), 3 + vin.length);
                    System.arraycopy(vin, 0, result, 3, vin.length);
                    reply(result);
                } else {
                    reply(bytes(0x62, request[1], request[2], 3));
                }
            } else {
                fail("Unexpected request " + M749Identification.hex(request));
            }
        }

        void reply(byte[] payload) {
            byte[] frame = new byte[8];
            if (payload.length <= 7) {
                frame[0] = (byte) payload.length;
                System.arraycopy(payload, 0, frame, 1, payload.length);
                incoming.add(frame);
                return;
            }
            frame[0] = (byte) (0x10 | (payload.length >>> 8));
            frame[1] = (byte) payload.length;
            System.arraycopy(payload, 0, frame, 2, 6);
            incoming.add(frame);
            int sequence = 1;
            for (int offset = 6; offset < payload.length; offset += 7) {
                frame = new byte[8];
                frame[0] = (byte) (0x20 | sequence);
                sequence = (sequence + 1) & 15;
                System.arraycopy(payload, offset, frame, 1, Math.min(7, payload.length - offset));
                incoming.add(frame);
            }
        }

        public byte[] receive() { clock++; return incoming.poll(); }
        public void close() { }
        public long now() { return clock; }
        public void pause(long milliseconds) { clock += milliseconds; }
    }

    @Test
    void authenticatesAndReadsAllRecordsIncludingMultiFrameVin() throws Exception {
        Harness h = new Harness();
        Map<Integer, byte[]> values = h.run();
        // Independent selector-00 vector: 35 rounds with the application polynomial.
        assertArrayEquals(bytes(6, 0x27, 2, 0xD3, 0x50, 0xD7, 0xF8, 0xCC), h.sent.get(2));
        assertEquals(17, h.dids.size());
        assertEquals(17, new HashSet<>(h.dids).size());
        assertEquals(Integer.valueOf(0xFD05), h.dids.get(16));
        assertEquals(1, h.flowControls);
        assertTrue(h.messages.stream().anyMatch(s -> s.contains("VIN (DID F190)") && s.contains("TESTVIN1234567890")));
        assertEquals("Identification complete: 17 read, 0 unavailable", h.messages.get(h.messages.size() - 1));
        assertArrayEquals("TESTVIN1234567890".getBytes(StandardCharsets.US_ASCII), values.get(0xF190));
    }

    @Test
    void summaryPicksKeyRecordsAndTrimsPadding() {
        Map<Integer, byte[]> values = new HashMap<>();
        values.put(0xF190, "TESTVIN1234567890".getBytes(StandardCharsets.US_ASCII));
        values.put(0xF189, "I812TA01_w2243v21\0\0".getBytes(StandardCharsets.US_ASCII));
        values.put(0xF195, "20221027".getBytes(StandardCharsets.US_ASCII));
        values.put(0xF193, "2581_3765_320_R07".getBytes(StandardCharsets.US_ASCII));
        values.put(0xF192, "8450086874".getBytes(StandardCharsets.US_ASCII));
        values.put(0xF18A, "Itelma LLC".getBytes(StandardCharsets.US_ASCII));
        values.put(0xF18C, "0008354".getBytes(StandardCharsets.US_ASCII));
        values.put(0xF18B, "20230314".getBytes(StandardCharsets.US_ASCII));
        assertEquals(Arrays.asList(
                "VIN: TESTVIN1234567890",
                "Software: I812TA01_w2243v21, built 20221027",
                "Hardware: 2581_3765_320_R07, part 8450086874",
                "ECU: Itelma LLC, serial 0008354, manufactured 20230314"), M749Identification.summarize(values));
        values.remove(0xF18A);
        values.remove(0xF195);
        assertTrue(M749Identification.summarize(values).contains("Software: I812TA01_w2243v21"));
        assertTrue(M749Identification.summarize(values).contains("ECU: 0008354, manufactured 20230314"));
        assertTrue(M749Identification.summarize(new HashMap<>()).isEmpty());
        values.put(0xF190, bytes(0, 0, 0));
        assertFalse(M749Identification.summarize(values).toString().contains("VIN"));
    }

    @Test
    void zeroSeedSkipsKey() throws Exception {
        Harness h = new Harness();
        h.zeroSeed = true;
        h.run();
        assertFalse(h.sent.stream().anyMatch(f -> f[1] == 0x27 && f[2] == 2));
    }

    @Test
    void unavailableDidDoesNotDiscardOtherRecords() throws Exception {
        Harness h = new Harness();
        h.responder = request -> {
            if (Arrays.equals(request, bytes(0x22, 0xF1, 0x92))) h.reply(bytes(0x7F, 0x22, 0x31));
            else h.normalReply(request);
        };
        h.run();
        assertTrue(h.messages.contains("DID F192: unavailable (NRC 31)"));
        assertEquals("Identification complete: 16 read, 1 unavailable", h.messages.get(h.messages.size() - 1));
    }

    @Test
    void pendingAndUnrelatedResponsesDoNotAdvanceWrongRequest() throws Exception {
        Harness h = new Harness();
        h.responder = request -> {
            h.reply(bytes(0x7F, 0x11, 0x31));
            h.reply(bytes(0x7F, request[0], 0x78));
            h.reply(bytes(0x62, 0x12, 0x34, 0));
            h.normalReply(request);
        };
        h.run();
        assertEquals(17, h.dids.size());
    }

    @Test
    void authenticationRejectionStopsWithoutRetryOrReads() {
        Harness h = new Harness();
        h.responder = request -> {
            if (request[0] == 0x27) h.reply(bytes(0x7F, 0x27, 0x35));
            else h.normalReply(request);
        };
        assertTrue(assertThrows(IOException.class, h::run).getMessage().contains("NRC 35"));
        assertEquals(2, h.sent.size());
        assertTrue(h.dids.isEmpty());
    }

    @Test
    void timeoutStopsInsteadOfMovingToNextDid() {
        Harness h = new Harness();
        h.responder = request -> {
            if (request[0] != 0x22) h.normalReply(request);
        };
        assertTrue(assertThrows(IOException.class, h::run).getMessage().contains("timeout"));
        assertEquals(4, h.sent.size());
        assertTrue(h.clock < 3_000);
    }

    @Test
    void endlessPendingIsBoundedByOverallDeadline() {
        Harness h = new Harness();
        h.responder = request -> {
            for (int i = 0; i < 20_000; i++) h.reply(bytes(0x7F, request[0], 0x78));
        };
        assertThrows(IOException.class, h::run);
        assertEquals(15_000, h.clock);
        assertEquals(1, h.sent.size());
    }

    @Test
    void malformedAndOversizedFramesAreRejected() {
        for (byte[] bad : Arrays.asList(bytes(0), bytes(8, 0x50, 3), bytes(0x10, 7, 0x62, 0xF1, 0x86, 0, 0, 0),
                bytes(0x14, 1, 0x62, 0xF1, 0x86, 0, 0, 0), bytes(0x21, 0, 0, 0))) {
            Harness h = new Harness();
            h.responder = request -> {
                if (request[0] == 0x22) h.incoming.add(bad);
                else h.normalReply(request);
            };
            assertThrows(IOException.class, h::run);
            assertEquals(4, h.sent.size());
        }
    }

    @Test
    void wrongConsecutiveSequenceStopsQuery() {
        Harness h = new Harness();
        h.responder = request -> {
            if (request[0] == 0x22) {
                h.incoming.add(bytes(0x10, 10, 0x62, 0xF1, 0x86, 0, 0, 0));
                h.incoming.add(bytes(0x22, 1, 2, 3, 4));
            } else h.normalReply(request);
        };
        assertTrue(assertThrows(IOException.class, h::run).getMessage().contains("sequence"));
    }

    @Test
    void longResponseWrapsSequenceAndKeepsBinaryDataPrintable() throws Exception {
        Harness h = new Harness();
        h.responder = request -> {
            if (Arrays.equals(request, bytes(0x22, 0xFD, 0x02))) {
                byte[] payload = new byte[150];
                payload[0] = 0x62;
                payload[1] = (byte) 0xFD;
                payload[2] = 2;
                h.reply(payload);
            } else h.normalReply(request);
        };
        h.run();
        assertEquals(2, h.flowControls);
        assertTrue(h.messages.stream().anyMatch(s -> s.startsWith("DID FD02: 147 bytes")));
        assertEquals(".A..", M749Identification.ascii(bytes(0, 65, 127, 255)));
    }

    @Test
    void cancellationDoesNotTransmit() {
        Harness h = new Harness();
        Thread.currentThread().interrupt();
        try {
            assertThrows(InterruptedException.class, h::run);
            assertTrue(h.sent.isEmpty());
        } finally {
            Thread.interrupted();
        }
    }
}
