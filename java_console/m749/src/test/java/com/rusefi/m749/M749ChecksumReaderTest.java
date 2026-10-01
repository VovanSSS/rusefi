package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.util.*;
import static com.rusefi.m749.M749Identification.bytes;
import static org.junit.jupiter.api.Assertions.*;

class M749ChecksumReaderTest {
    static class Ecu implements M749Uploader.Connection {
        final Map<Integer, Integer> memory = new HashMap<>();
        final List<byte[]> requests = new ArrayList<>();
        int failAddress = -1;
        boolean alwaysMatch, alwaysMiss, malformed;
        public void pause(long millis) { }
        public byte[] exchange(byte[] q, byte[] prefix, long timeout) throws IOException {
            requests.add(q.clone());
            assertArrayEquals(bytes(0x31, 1, 0xFF, 1, 0x44), Arrays.copyOf(q, 5));
            assertEquals(15, q.length);
            int address = word(q, 5), length = word(q, 9);
            if (address == failAddress) { throw new IOException("Lost reply"); }
            assertEquals(1, length);
            int sum = (q[13] & 255) << 8 | q[14] & 255;
            boolean match = alwaysMatch || (!alwaysMiss && sum == memory.getOrDefault(address, 0));
            return malformed ? bytes(0x71, 1, 0xFF, 1, 2) : bytes(0x71, 1, 0xFF, 1, match ? 0 : 1);
        }
        static int word(byte[] b, int i) {
            return (b[i] & 255) << 24 | (b[i + 1] & 255) << 16 | (b[i + 2] & 255) << 8 | b[i + 3] & 255;
        }
    }

    @Test void readsUnsignedByteAndChecksAWrongCandidate() throws Exception {
        for (int value : new int[]{0, 128, 255}) {
            Ecu ecu = new Ecu();
            ecu.memory.put(0x08274000, value);
            assertEquals(value, new M749ChecksumReader(ecu).readByte(0x08274000));
            assertEquals(value + 2, ecu.requests.size());
            assertEquals((value + 1) & 255, ecu.requests.get(ecu.requests.size() - 1)[14] & 255);
        }
    }

    @Test void refusesAlwaysMatchingAndAlwaysRejectingResponders() {
        Ecu ecu = new Ecu(); ecu.alwaysMatch = true;
        assertThrows(IOException.class, () -> new M749ChecksumReader(ecu).readByte(0x08274000));
        assertEquals(2, ecu.requests.size());
        Ecu missing = new Ecu(); missing.alwaysMiss = true;
        assertThrows(IOException.class, () -> new M749ChecksumReader(missing).readByte(0x08274000));
        assertEquals(256, missing.requests.size());
    }

    @Test void malformedOrLostReplyStopsWithoutTryingAnotherCandidate() {
        for (boolean malformed : new boolean[]{false, true}) {
            Ecu ecu = new Ecu(); ecu.malformed = malformed;
            if (!malformed) { ecu.failAddress = 0x08274000; }
            assertThrows(IOException.class, () -> new M749ChecksumReader(ecu).readByte(0x08274000));
            assertEquals(1, ecu.requests.size());
        }
    }

    @Test void invalidRangeAndCancellationSendNothing() {
        Ecu ecu = new Ecu(); M749ChecksumReader reader = new M749ChecksumReader(ecu);
        assertThrows(IllegalArgumentException.class, () -> reader.readByte(0x40000000));
        assertThrows(IllegalArgumentException.class, () -> reader.matches(0x083EFFFF, 2, 0));
        Thread.currentThread().interrupt();
        try { assertThrows(InterruptedException.class, () -> reader.readByte(0x08274000)); }
        finally { Thread.interrupted(); }
        assertTrue(ecu.requests.isEmpty());
    }

    @Test void assemblesLittleEndianWords() throws Exception {
        Ecu ecu = new Ecu();
        int[] data = {0, 1, 128, 255};
        for (int i = 0; i < 4; i++) { ecu.memory.put(0x080FFFFC + i, data[i]); }
        assertEquals(0xFF800100, new M749ChecksumReader(ecu).readWord(0x080FFFFC));
    }

    @Test void preparationUsesOnlyReadAndSecurityServicesWhenAlreadyInSessionTwo() throws Exception {
        List<Integer> services = new ArrayList<>();
        M749UploaderTest.Ecu ecu = new M749UploaderTest.Ecu() {
            public byte[] exchange(byte[] q, byte[] prefix, long timeout) throws IOException {
                services.add(q[0] & 255);
                if (q[0] == 0x22) { return bytes(0x62, 0xF1, 0x86, 2); }
                return super.exchange(q, prefix, timeout);
            }
        };
        new M749ChecksumReader(ecu).prepareRead();
        assertTrue(ecu.keySent);
        assertTrue(services.stream().allMatch(s -> s == 0x22 || s == 0x27 || s == 0x31));
        assertEquals(0, ecu.erases.size());
    }

    @Test void rejectedEntryStopsBeforeSecurityAndChecksums() {
        List<Integer> services = new ArrayList<>();
        M749Uploader.Connection app = new M749Uploader.Connection() {
            public void pause(long ms) { }
            public byte[] exchange(byte[] q, byte[] prefix, long timeout) throws IOException {
                services.add(q[0] & 255);
                if (q[0] == 0x10) { throw new UdsClient.NegativeResponse(0x10, 0x22); }
                assertArrayEquals(bytes(0x22, 0xF1, 0x86), q);
                return bytes(0x62, 0xF1, 0x86, 1);
            }
        };
        assertThrows(IOException.class, () -> new M749ChecksumReader(app).prepareRead());
        assertEquals(List.of(0x22, 0x10), services);
    }

    @Test void acceptedEntryWaitsForTheLoaderBeforeAuthentication() throws Exception {
        List<Integer> services = new ArrayList<>();
        long[] waited = {0};
        M749UploaderTest.Ecu ecu = new M749UploaderTest.Ecu() {
            public void pause(long ms) { waited[0] += ms; }
            public byte[] exchange(byte[] q, byte[] prefix, long timeout) throws IOException {
                services.add(q[0] & 255);
                if (q[0] == 0x22) { return bytes(0x62, 0xF1, 0x86, 1); }
                if (q[0] == 0x27) { assertEquals(1000, waited[0]); }
                return super.exchange(q, prefix, timeout);
            }
        };
        new M749ChecksumReader(ecu).prepareRead();
        assertEquals(List.of(0x22, 0x10, 0x27, 0x27), services.subList(0, 4));
        assertTrue(ecu.erases.isEmpty());
    }
}
