package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.*;
import static org.junit.jupiter.api.Assertions.*;
import static com.rusefi.m749.M749Identification.bytes;

class SlcanTransportTest {
    @Test void closeSynchronizesPastStaleFragmentsButActiveParsingStaysStrict() throws Exception {
        Port port = new Port(); port.acknowledge = true;
        port.offer("1000000\rt".repeat(70) + "\r");
        SlcanTransport transport = new SlcanTransport(port, 1);
        transport.initialize();
        assertEquals(Arrays.asList("C\r", "S6\r", "O\r"), port.writes);
        port.offer("1000000\r");
        assertThrows(IOException.class, transport::receiveCan);
        Port error = new Port(); error.offer("F01\r");
        assertThrows(IOException.class, () -> new SlcanTransport(error, 1).initialize());
        assertEquals(List.of("C\r"), error.writes);
    }

    @Test void canableWithoutAcknowledgementsRequiresVersionReplies() throws Exception {
        Port port = new Port() {
            public void write(byte[] data) {
                super.write(data);
                if (writes.get(writes.size() - 1).equals("V\r")) {
                    offer("16e7497-dirty github.com/normaldotcom/canable2.git\r");
                }
            }
        };
        try (SlcanTransport t = new SlcanTransport(port, 1)) {
            t.initialize();
            assertEquals(Arrays.asList("C\r", "V\r", "S6\r", "V\r", "O\r", "V\r"), port.writes);
            port.offer("t7E840362F186\r");
            assertArrayEquals(bytes(3, 0x62, 0xf1, 0x86), t.receiveCan().data);
        }
    }

    @Test void weActWithoutAcknowledgementsIsSetUpLikeCanable() throws Exception {
        Port port = new Port() {
            public void write(byte[] data) {
                super.write(data);
                if (writes.get(writes.size() - 1).equals("V\r")) {
                    offer("WeAct Studio V1.0.0.3_bb264e71\r");
                }
            }
        };
        List<String> log = new ArrayList<>();
        try (SlcanTransport t = new SlcanTransport(port, 1, log::add)) {
            t.initialize();
            assertEquals(Arrays.asList("C\r", "V\r", "S6\r", "V\r", "O\r", "V\r"), port.writes);
            assertTrue(log.contains("CANable family version: WeAct Studio V1.0.0.3_bb264e71"), log.toString());
            port.offer("t7E840362F186\r");
            assertArrayEquals(bytes(3, 0x62, 0xf1, 0x86), t.receiveCan().data);
        }
    }

    @Test void unrelatedVersionBannerWithoutAcknowledgementsDoesNotOpenCan() {
        Port port = new Port() {
            public void write(byte[] data) {
                super.write(data);
                if (writes.get(writes.size() - 1).equals("V\r")) { offer("WeAct Studio\r"); }
            }
        };
        assertThrows(IOException.class, () -> new SlcanTransport(port, 1).initialize());
        assertEquals(Arrays.asList("C\r", "V\r"), port.writes);
    }

    @Test void silentUnknownAdapterDoesNotOpenCan() {
        Port port = new Port();
        assertThrows(IOException.class, () -> new SlcanTransport(port, 1).initialize());
        assertEquals(Arrays.asList("C\r", "V\r"), port.writes);
    }

    @Test void canableSetupRequiresFreshVersionAndDoesNotTreatBlankAsSuccess() {
        Port port = new Port() {
            public void write(byte[] data) {
                super.write(data);
                if (writes.size() == 2) {
                    offer("0123456789abcdef0123456789abcdef01234567 github.com/normaldotcom/canable2.git\r");
                } else if (writes.size() > 2) { offer("\r"); }
            }
        };
        assertThrows(IOException.class, () -> new SlcanTransport(port, 1).initialize());
        assertEquals(Arrays.asList("C\r", "V\r", "S6\r", "V\r"), port.writes);
    }

    static class Port implements SlcanTransport.Port {
        Queue<byte[]> incoming = new ArrayDeque<>();
        List<String> writes = new ArrayList<>();
        boolean acknowledge, closed;
        void offer(String value) { incoming.add(value.getBytes(StandardCharsets.US_ASCII)); }
        public int read(byte[] buffer) {
            byte[] next = incoming.poll();
            if (next == null) { return 0; }
            System.arraycopy(next, 0, buffer, 0, next.length); return next.length;
        }
        public void write(byte[] data) {
            writes.add(new String(data, StandardCharsets.US_ASCII));
            if (acknowledge) { offer("\r"); }
        }
        public void close() { closed = true; }
    }

    @Test void initializesBitrateAndEncodesTaggedFramesAndCloses() throws Exception {
        Port port = new Port(); port.acknowledge = true;
        try (SlcanTransport t = new SlcanTransport(port, 2)) {
            t.initialize(); t.sendCan(0x7e0, bytes(7, 0x23, 8, 0, 0, 4, 0, 4));
            assertEquals(Arrays.asList("C\r", "S6\r", "O\r", "&t7E080723080000040004\r"), port.writes);
        }
        assertTrue(port.closed); assertEquals("C\r", port.writes.get(4));
    }

    @Test void preservesPartialLinesAndIgnoresEchoOtherBusRtrAndExtendedFrames() throws Exception {
        Port port = new Port(); SlcanTransport t = new SlcanTransport(port, 1);
        port.offer("\rz\rV1234\rF00\rt7E8205");
        assertNull(t.receiveCan()); assertNull(t.receiveCan());
        port.offer("630ABC\r&t7E8101\rr7E88\rT000007E8101\rt7E0101\rt123101\rt7E8110\r");
        RawCanTransport.Frame first = t.receiveCan();
        assertEquals(0x7e8, first.id); assertArrayEquals(bytes(5, 0x63), first.data);
        assertArrayEquals(bytes(0x10), t.receiveCan().data); assertNull(t.receiveCan());
    }

    @Test void rejectsBellStatusMalformedFramesAndOverlongPartialLine() {
        for (String input : new String[]{"\u0007", "F01\r", "t7E8900\r", "t7E8200\r", "t7E81GG\r",
                "tFFF100\r", "t7E810012\r", "garbage\r", "t".repeat(65)}) {
            Port port = new Port(); port.offer(input);
            assertThrows(IOException.class, () -> new SlcanTransport(port, 1).receiveCan(), input);
        }
    }

    @Test void busThreeOnlyAcceptsDollarFrames() throws Exception {
        Port port = new Port(); port.offer("t7E8101\r&t7E8102\r$t7E8103\r");
        SlcanTransport t = new SlcanTransport(port, 3);
        assertArrayEquals(bytes(3), t.receiveCan().data);
        assertNull(t.receiveCan());
    }

    @Test void alreadyClosedBellIsAcceptedButBitrateRejectionIsNot() throws Exception {
        Port closed = new Port() {
            public void write(byte[] data) { super.write(data); offer(writes.size() == 1 ? "\u0007" : "\r"); }
        };
        new SlcanTransport(closed, 1).initialize();
        assertEquals(Arrays.asList("C\r", "S6\r", "O\r"), closed.writes);
        Port badSpeed = new Port() {
            public void write(byte[] data) { super.write(data); offer(writes.size() == 1 ? "\r" : "\u0007"); }
        };
        assertThrows(IOException.class, () -> new SlcanTransport(badSpeed, 1).initialize());
        assertEquals(2, badSpeed.writes.size());
    }
}
