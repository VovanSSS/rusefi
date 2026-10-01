package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.util.*;
import static com.rusefi.m749.M749Identification.bytes;
import static org.junit.jupiter.api.Assertions.*;

class M749UploaderTest {
    static class Ecu implements M749Uploader.Connection {
        final byte[] flash = new byte[0x24F000];
        final Map<Integer, byte[]> metadata = new LinkedHashMap<>();
        final List<Integer> erases = new ArrayList<>();
        final List<Integer> counters = new ArrayList<>();
        final List<String> messages = new ArrayList<>();
        int position, remaining, expectedCounter, resets, writes;
        int maxLength = 0x802;
        boolean corruptChecksum, badPostCrc, badFingerprint, emptyMetadata, zeroSeed, keySent;
        int failSid = -1;
        boolean app;

        Ecu() {
            Arrays.fill(flash, (byte) 0xA5);
            Arrays.fill(flash, 0x24E100, 0x24F000, (byte) 0xFF);
            putHex(0x0822DFFC, "94b8b6d7");
            putHex(0x08201E2C, "2de9f04184b004460d4617461e4601f0");
            putHex(0x08201D84, "70b506460d46144601f024fd012801d0");
            putHex(0x08204B7C, "08b50a4b1b68fff7e7ff012807d0fff7");
            System.arraycopy(M749Image.ACTIVATION_ABI, 0, flash, M749Image.ACTIVATION_ADDRESS - 0x08000000, 16);
            for (int did : new int[]{0xF188, 0xF189, 0xF194, 0xF195, 0xF198, 0xF199}) {
                metadata.put(did, bytes(1, 2, 3, 4, 5, 6, 7, 8));
            }
        }

        void putHex(int address, String hex) {
            for (int i = 0; i < hex.length(); i += 2) { flash[address - 0x08000000 + i / 2] = (byte) Integer.parseInt(hex.substring(i, i + 2), 16); }
        }
        int big(byte[] b, int offset) {
            return (b[offset] & 255) << 24 | (b[offset + 1] & 255) << 16 | (b[offset + 2] & 255) << 8 | b[offset + 3] & 255;
        }
        int little(int address) {
            int i = address - 0x08000000;
            return (flash[i] & 255) | (flash[i + 1] & 255) << 8 | (flash[i + 2] & 255) << 16 | flash[i + 3] << 24;
        }
        public void pause(long milliseconds) { }
        public byte[] exchange(byte[] q, byte[] prefix, long timeout) throws IOException {
            if ((q[0] & 255) == failSid) { throw new IOException("Injected service failure"); }
            byte[] result;
            switch (q[0] & 255) {
                case 0x10: result = bytes(0x50, 2, 0, 50, 1, 0xF4); break;
                case 0x27:
                    if (q[1] == 1) {
                        result = zeroSeed ? bytes(0x67, 1, 0, 0, 0, 0) : bytes(0x67, 1, 0x4F, 0xBE, 0x76, 0xC7);
                    } else {
                        assertArrayEquals(bytes(0x27, 2, 0x9B, 0xDE, 0x57, 0x52), q); // Expected loader key for this seed.
                        keySent = true;
                        result = bytes(0x67, 2);
                    }
                    break;
                case 0x31:
                    int address = big(q, 5), length = big(q, 9);
                    if (q[3] == 0) {
                        assertEquals(4096, length);
                        assertEquals(0, address & 4095);
                        assertTrue(address >= M749Image.START && address + length <= M749Image.END);
                        erases.add(address);
                        Arrays.fill(flash, address - 0x08000000, address - 0x08000000 + length, (byte) 0xFF);
                        result = bytes(0x71, 1, 0xFF, 0, 0);
                    } else {
                        int sum = 0;
                        for (int i = 0; i < length; i++) { sum = (sum + (flash[address - 0x08000000 + i] & 255)) & 65535; }
                        int want = (q[13] & 255) << 8 | q[14] & 255;
                        result = bytes(0x71, 1, 0xFF, 1, (badFingerprint ||
                                (corruptChecksum && address < M749Image.END && length > 1) || sum != want) ? 1 : 0);
                    }
                    break;
                case 0x34:
                    position = big(q, 3) - 0x08000000;
                    remaining = big(q, 7);
                    expectedCounter = 0;
                    result = bytes(0x74, 0x20, maxLength >>> 8, maxLength);
                    break;
                case 0x36:
                    assertEquals(expectedCounter, q[1] & 255);
                    assertTrue(q.length <= maxLength && q.length - 2 <= remaining);
                    counters.add(expectedCounter);
                    expectedCounter = (expectedCounter + 1) & 255;
                    System.arraycopy(q, 2, flash, position, q.length - 2);
                    position += q.length - 2;
                    remaining -= q.length - 2;
                    writes++;
                    result = bytes(0x76, q[1]);
                    break;
                case 0x37:
                    assertEquals(0, remaining);
                    result = bytes(0x77);
                    break;
                case 0x22:
                    int did = (q[1] & 255) << 8 | q[2] & 255;
                    if (app) {
                        int value = did == 0xF1A0 ? 0x4D740101 : did == 0xF1A1 ? little(0x080FFFFC) :
                                did == 0xF1A2 ? little(0x0807FFFC) : 0x43A0C212;
                        if (badPostCrc && did == 0xF1A1) { value ^= 1; }
                        result = bytes(0x62, q[1], q[2], value >>> 24, value >>> 16, value >>> 8, value);
                    } else {
                        byte[] data = emptyMetadata ? new byte[0] : metadata.get(did);
                        result = Arrays.copyOf(bytes(0x62, q[1], q[2]), data.length + 3);
                        System.arraycopy(data, 0, result, 3, data.length);
                    }
                    break;
                case 0x2E:
                    int writeDid = (q[1] & 255) << 8 | q[2] & 255;
                    byte[] newValue = Arrays.copyOfRange(q, 3, q.length);
                    if (flash[0x24E000] == (byte) 0xFF) {
                        metadata.put(writeDid, newValue);
                    } else {
                        assertArrayEquals(metadata.get(writeDid), newValue);
                    }
                    result = bytes(0x6E, q[1], q[2]);
                    break;
                case 0x11:
                    app = true;
                    resets++;
                    result = bytes(0x51, 1);
                    break;
                default: throw new AssertionError("Unexpected request " + Arrays.toString(q));
            }
            assertTrue(UdsClient.startsWith(result, prefix));
            return result;
        }
        void run(M749Image.Domain domain) throws Exception {
            new M749Uploader(this, messages::add).upload(M749Image.validate(M749ImageTest.records(domain), domain), false);
        }
    }

    @Test void uploadsBothRangesPreservingCalibrationAndProtectedFlashThenChecksTwoBoots() throws Exception {
        Ecu ecu = new Ecu();
        byte[] original = ecu.flash.clone();
        ecu.maxLength = 1026; // Force UDS counter wrap; not just ISO-TP sequence wrap.
        ecu.run(M749Image.Domain.SOFTWARE);
        assertEquals(223, ecu.erases.size());
        assertFalse(ecu.erases.stream().anyMatch(a -> a >= M749Image.CAL && a < M749Image.SECOND));
        assertEquals(2, ecu.resets);
        assertTrue(ecu.keySent);
        assertTrue(Collections.frequency(ecu.counters, 0) >= 4);
        for (com.rusefi.libopenblt.file.SrecParser.SRecord record : M749ImageTest.records(M749Image.Domain.SOFTWARE)) {
            assertArrayEquals(record.data, Arrays.copyOfRange(ecu.flash, record.address - 0x08000000, record.address - 0x08000000 + record.data.length));
        }
        assertArrayEquals(Arrays.copyOfRange(original, 0x60000, 0x80000), Arrays.copyOfRange(ecu.flash, 0x60000, 0x80000));
        assertArrayEquals(Arrays.copyOf(original, 0x1000), Arrays.copyOf(ecu.flash, 0x1000));
        assertArrayEquals(Arrays.copyOfRange(original, 0x100000, original.length), Arrays.copyOfRange(ecu.flash, 0x100000, ecu.flash.length));
    }

    @Test void rejectedProgrammingEntryStopsAfterOneRequestAndReportsNoFlashWrites() {
        List<byte[]> requests = new ArrayList<>();
        M749Uploader.Connection connection = new M749Uploader.Connection() {
            public byte[] exchange(byte[] request, byte[] prefix, long timeout) throws IOException {
                requests.add(request.clone());
                throw new UdsClient.NegativeResponse(0x10, 0x22);
            }
            public void pause(long milliseconds) { fail("Rejected entry must not continue"); }
        };
        IOException failure = assertThrows(IOException.class, () -> new M749Uploader(connection, s -> { })
                .upload(M749Image.validate(M749ImageTest.records(M749Image.Domain.SOFTWARE),
                        M749Image.Domain.SOFTWARE), false));
        assertEquals(1, requests.size());
        assertArrayEquals(bytes(0x10, 2), requests.get(0));
        assertTrue(failure.getMessage().contains("No flash erase or programming requests were sent"));
        assertTrue(failure.getMessage().contains("Programming entry conditions were not met"));
        assertFalse(failure.getMessage().contains("may be incomplete"));
        assertInstanceOf(UdsClient.NegativeResponse.class, failure.getCause());
    }

    @Test void lostFirstEraseReplyStillReportsPossibleIncompleteFlash() {
        Ecu ecu = new Ecu() {
            public byte[] exchange(byte[] request, byte[] prefix, long timeout) throws IOException {
                if (request[0] == 0x31 && request[3] == 0) {
                    super.exchange(request, prefix, timeout); // Erase succeeds but its reply is lost.
                    throw new IOException("Response timeout");
                }
                return super.exchange(request, prefix, timeout);
            }
        };
        IOException failure = assertThrows(IOException.class, () -> ecu.run(M749Image.Domain.SOFTWARE));
        assertEquals(1, ecu.erases.size());
        assertEquals(0, ecu.writes);
        assertEquals(0, ecu.resets);
        assertTrue(failure.getMessage().contains("An erase request was sent; flash/activation may be incomplete"));
        assertFalse(failure.getMessage().contains("No flash erase"));
    }

    @Test void calibrationIsSeparateAndZeroSeedSkipsKey() throws Exception {
        Ecu ecu = new Ecu();
        byte[] original = ecu.flash.clone();
        ecu.zeroSeed = true;
        ecu.run(M749Image.Domain.CALIBRATION);
        assertEquals(32, ecu.erases.size());
        assertTrue(ecu.erases.stream().allMatch(a -> a >= M749Image.CAL && a < M749Image.SECOND));
        assertFalse(ecu.keySent);
        assertArrayEquals(Arrays.copyOf(original, 0x60000), Arrays.copyOf(ecu.flash, 0x60000));
        assertArrayEquals(Arrays.copyOfRange(original, 0x80000, original.length), Arrays.copyOfRange(ecu.flash, 0x80000, ecu.flash.length));
    }

    @Test void compatibilityAndMetadataFailuresStopBeforeErase() {
        for (int failure = 0; failure < 2; failure++) {
            Ecu ecu = new Ecu();
            ecu.badFingerprint = failure == 0;
            ecu.emptyMetadata = failure == 1;
            assertThrows(IOException.class, () -> ecu.run(M749Image.Domain.SOFTWARE));
            assertTrue(ecu.erases.isEmpty());
            assertEquals(0, ecu.resets);
        }
        Ecu full = new Ecu();
        Arrays.fill(full.flash, 0x24E000, 0x24F000, (byte) 0xA5);
        assertThrows(IOException.class, () -> full.run(M749Image.Domain.SOFTWARE));
        assertTrue(full.erases.isEmpty());
        Ecu partial = new Ecu();
        partial.flash[0x24E101] = 1;
        assertThrows(IOException.class, () -> partial.run(M749Image.Domain.SOFTWARE));
        assertTrue(partial.erases.isEmpty());
    }

    @Test void blankProgrammingHistoryGetsANewRecordWithoutTouchingEcuIdentity() throws Exception {
        Ecu ecu = new Ecu();
        Arrays.fill(ecu.flash, 0x24E000, 0x24F000, (byte) 0xFF);
        ecu.run(M749Image.Domain.SOFTWARE);
        assertEquals(2, ecu.resets);
        assertEquals("fw-m74.9 CLI", new String(ecu.metadata.get(0xF198), java.nio.charset.StandardCharsets.US_ASCII));
        assertEquals(8, ecu.metadata.get(0xF199).length);
        assertEquals(6, ecu.metadata.size());
    }

    @Test void anyWriteOrVerificationFailureStopsWithoutResetOrSuccess() {
        for (int sid : new int[]{0x31, 0x34, 0x36, 0x37, 0x2E}) {
            Ecu ecu = new Ecu();
            ecu.failSid = sid;
            assertThrows(IOException.class, () -> ecu.run(M749Image.Domain.SOFTWARE));
            assertEquals(0, ecu.resets);
            assertFalse(ecu.messages.stream().anyMatch(s -> s.startsWith("Upload complete")));
        }
        Ecu badSum = new Ecu();
        badSum.corruptChecksum = true;
        assertThrows(IOException.class, () -> badSum.run(M749Image.Domain.SOFTWARE));
        assertEquals(95, badSum.erases.size());
        assertEquals(0, badSum.resets);
        Ecu badCrc = new Ecu();
        badCrc.badPostCrc = true;
        assertThrows(IOException.class, () -> badCrc.run(M749Image.Domain.SOFTWARE));
        assertEquals(1, badCrc.resets);
        assertFalse(badCrc.messages.stream().anyMatch(s -> s.startsWith("Upload complete")));
    }
}
