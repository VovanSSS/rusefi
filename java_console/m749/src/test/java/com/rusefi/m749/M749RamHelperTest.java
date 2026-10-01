package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.util.*;
import static org.junit.jupiter.api.Assertions.*;
import static com.rusefi.m749.M749Identification.bytes;

class M749RamHelperTest {
    @Test void optionalPreparationMayBeUnsupportedButOtherFailuresStopBeforeRamWrites() throws Exception {
        for (int nrc : new int[]{0x11, 0x7f, 0x22, 0x33}) {
            Ecu ecu = new Ecu() {
                public byte[] exchange(byte[] q, byte[] prefix, long timeout) throws IOException {
                    if ((q[0] & 255) == 0x85 || q[0] == 0x28) {
                        requests.add(q.clone());
                        throw new UdsClient.NegativeResponse(q[0] & 255, nrc);
                    }
                    return super.exchange(q, prefix, timeout);
                }
            };
            M749RamHelper helper = new M749RamHelper(ecu);
            if (nrc == 0x11 || nrc == 0x7f) {
                List<String> out = new ArrayList<>();
                helper.start(M749RamHelper.load(), false, out::add);
                assertEquals(13, ecu.requests.stream().filter(q -> q[0] == 0x3d).count());
                assertTrue(out.contains(String.format(
                        "Optional preparation SID 85 unavailable (NRC %02X); continuing to RAM upload", nrc)), out.toString());
                assertTrue(out.contains(String.format(
                        "Optional preparation SID 28 unavailable (NRC %02X); continuing to RAM upload", nrc)), out.toString());
            } else {
                assertThrows(UdsClient.NegativeResponse.class, () -> helper.start(M749RamHelper.load(), false, s -> {}));
                assertFalse(ecu.requests.stream().anyMatch(q -> q[0] == 0x3d));
            }
        }
    }

    @Test void acceptedPreparationStepsAreReported() throws Exception {
        Ecu ecu = new Ecu();
        List<String> out = new ArrayList<>();
        new M749RamHelper(ecu).start(M749RamHelper.load(), false, out::add);
        assertTrue(out.contains("Preparation SID 85 (DTC control) accepted"), out.toString());
        assertTrue(out.contains("Preparation SID 28 (communication control) accepted"), out.toString());
        assertTrue(out.indexOf("Preparation SID 28 (communication control) accepted")
                < out.indexOf("RAM helper upload: 512/6656 bytes acknowledged"), out.toString());
    }

    static class Ecu implements M749Uploader.Connection {
        final List<byte[]> requests = new ArrayList<>();
        byte[] helper;
        boolean uniform, badAddress, shortResponse;
        int failAt = -1, changeAt = -1, changeRead = -1, reads;
        Ecu() throws IOException { helper = M749RamHelper.load(); }
        public void pause(long ms) { }
        static byte[] content(int address, int length) {
            byte[] data = new byte[length];
            for (int i = 0; i < length; i++) {
                int offset = address + i - M749RamHelper.BASE;
                data[i] = (byte) ((offset * 73) ^ (offset >>> 8) ^ (offset >>> 17));
            }
            return data;
        }
        public byte[] exchange(byte[] q, byte[] prefix, long timeout) throws IOException {
            requests.add(q.clone());
            if (requests.size() == failAt) { throw new IOException("Injected connection failure"); }
            switch (q[0] & 255) {
                case 0x10: assertArrayEquals(bytes(0x10, 0x60), q); return bytes(0x50, 0x60, 0, 50, 1, 0xf4);
                case 0x27:
                    return q[1] == 1 ? bytes(0x67, 1, 0x12, 0x34, 0x56, 0x78) : bytes(0x67, 2);
                case 0x85: return bytes(0xc5, 2);
                case 0x28: return bytes(0x68, 1);
                case 0x3d:
                    assertEquals(0x24, q[1]); assertEquals(520, q.length);
                    int address = integer(q, 2);
                    assertTrue(address >= 0x2001ba00 && address + 512 <= 0x2001d400);
                    assertArrayEquals(Arrays.copyOfRange(helper, address - 0x2001ba00, address - 0x2001ba00 + 512),
                            Arrays.copyOfRange(q, 8, q.length));
                    byte[] ack = Arrays.copyOf(q, 8); ack[0] = 0x7d; return ack;
                case 0x31:
                    assertArrayEquals(bytes(0x31, 1, 0xf0, 0, 0x20, 1, 0xba, 0), q);
                    return bytes(0x71, 1, 0xf0, 0, 0x7c, 0, 0);
                case 0x1a: return bytes(0x5a, 0xc1, 0x41, 0x0f, 0xc2, 0x41, 0x70, 8, 0x45, 0x40);
                case 0x3e: assertEquals(1, q.length); return bytes(0x7e);
                case 0x11: return bytes(0x51, 1);
                case 0x23:
                    assertEquals(7, q.length);
                    int start = integer(q, 1), count = (q[5] & 255) * 256 + (q[6] & 255);
                    byte[] data = start >= 0x2001ba00 && start + count <= 0x2001d400 ?
                            Arrays.copyOfRange(helper, start - 0x2001ba00, start - 0x2001ba00 + count) : content(start, count);
                    if (uniform) { Arrays.fill(data, (byte) 255); }
                    if (start == changeAt || ++reads == changeRead) { data[0] ^= 1; }
                    byte[] reply = Arrays.copyOf(q, 5 + count);
                    reply[0] = 0x63;
                    if (badAddress) { reply[4] ^= 1; }
                    System.arraycopy(data, 0, reply, 5, count);
                    return shortResponse ? Arrays.copyOf(reply, reply.length - 1) : reply;
                default: throw new AssertionError("Unexpected service " + q[0]);
            }
        }
        static int integer(byte[] q, int start) {
            return (q[start] & 255) << 24 | (q[start+1] & 255) << 16 | (q[start+2] & 255) << 8 | q[start+3] & 255;
        }
    }

    @Test void bundledResourceBootstrapUsesOnlyRamWritesAndCorrectEntry() throws Exception {
        Ecu ecu = new Ecu(); M749RamHelper reader = new M749RamHelper(ecu);
        reader.start(M749RamHelper.load(), false, s -> {});
        assertEquals(13, ecu.requests.stream().filter(q -> q[0] == 0x3d).count());
        assertEquals(20, ecu.requests.size());
        reader.probe();
        assertArrayEquals(Ecu.content(0x083ef011, 4079), reader.read(0x083ef011, 4079));
        reader.reset();
        assertArrayEquals(bytes(0x11, 1), ecu.requests.get(ecu.requests.size()-1));
    }

    @Test void failedAdmissionStopsBeforeUploadAndBadResourceStopsBeforeRequests() throws Exception {
        Ecu ecu = new Ecu(); ecu.failAt = 1;
        M749RamHelper reader = new M749RamHelper(ecu);
        assertThrows(IOException.class, () -> reader.start(M749RamHelper.load(), false, s -> {}));
        assertEquals(1, ecu.requests.size());
        ecu.requests.clear();
        byte[] bad = M749RamHelper.load(); bad[10] ^= 1;
        assertThrows(IOException.class, () -> reader.start(bad, false, s -> {}));
        assertTrue(ecu.requests.isEmpty());
    }

    @Test void runningModeVerifiesWholeHelperWithoutSessionOrWrite() throws Exception {
        Ecu ecu = new Ecu(); M749RamHelper reader = new M749RamHelper(ecu);
        reader.start(M749RamHelper.load(), true, s -> {});
        assertTrue(ecu.requests.stream().allMatch(q -> q[0] == 0x3e || q[0] == 0x23 || q[0] == 0x1a));
        assertEquals(13, ecu.requests.stream().filter(q -> q[0] == 0x23).count());
        ecu.helper[1024] ^= 1;
        assertThrows(IOException.class, () -> reader.start(M749RamHelper.load(), true, s -> {}));
    }

    @Test void rejectsBadReadAddressLengthAndUniformProbe() throws Exception {
        Ecu ecu = new Ecu(); M749RamHelper reader = new M749RamHelper(ecu);
        for (int[] range : new int[][]{{0x07ffffff, 1}, {0x083effff, 2}, {0x08000000, 0}, {0x08000000, 4081}}) {
            assertThrows(IllegalArgumentException.class, () -> reader.read(range[0], range[1]));
        }
        assertTrue(ecu.requests.isEmpty());
        ecu.badAddress = true;
        assertThrows(IOException.class, () -> reader.read(0x08000000, 128));
        ecu.badAddress = false; ecu.shortResponse = true;
        assertThrows(IOException.class, () -> reader.read(0x08000000, 128));
        ecu.shortResponse = false; ecu.uniform = true;
        assertThrows(IOException.class, reader::probe);
    }
}
