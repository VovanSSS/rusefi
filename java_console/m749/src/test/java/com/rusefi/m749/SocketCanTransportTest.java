package com.rusefi.m749;

import com.rusefi.io.can.CanAddress;
import com.rusefi.io.can.ClassicCanFrame;
import com.rusefi.io.can.RawCanPort;
import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.Queue;
import static org.junit.jupiter.api.Assertions.*;

class SocketCanTransportTest {
    static class Port implements RawCanPort {
        final Queue<ClassicCanFrame> incoming = new ArrayDeque<>();
        final List<ClassicCanFrame> outgoing = new ArrayList<>();
        boolean closed;
        public void open(CanAddress address) { throw new AssertionError("Already open"); }
        public void send(ClassicCanFrame frame) { outgoing.add(frame); }
        public Optional<ClassicCanFrame> receive(int timeout) {
            assertTrue(timeout > 0 && timeout <= 10, "Polls must not block indefinitely");
            return Optional.ofNullable(incoming.poll());
        }
        public void close() { closed = true; }
        void reply(int id, byte... bytes) { incoming.add(new ClassicCanFrame(new CanAddress(id, false), bytes)); }
    }

    @Test void routesDiagnosticAndImmoTrafficWithoutLosingDlc() throws Exception {
        Port port = new Port();
        try (SocketCanTransport transport = new SocketCanTransport(port)) {
            transport.send(new byte[]{2, 0x10, 2});
            transport.sendCan(0x714, new byte[]{1, 2});
            assertEquals(0x7e0, port.outgoing.get(0).getAddress().getId());
            assertFalse(port.outgoing.get(0).getAddress().isExtended());
            assertArrayEquals(new byte[]{2, 0x10, 2}, port.outgoing.get(0).getPayload());
            assertEquals(0x714, port.outgoing.get(1).getAddress().getId());
            port.reply(0x713, (byte) 1);
            port.reply(0x714, (byte) 2);
            assertEquals(0x713, transport.receiveCan().id);
            assertEquals(0x714, transport.receiveCan().id);
            port.reply(0x713, (byte) 3);
            port.reply(0x7e8, (byte) 2, (byte) 0x50, (byte) 2);
            assertArrayEquals(new byte[]{2, 0x50, 2}, transport.receive());
            assertNull(transport.receiveCan());
            assertThrows(IOException.class, () -> transport.sendCan(0x800, new byte[1]));
            assertThrows(IOException.class, () -> transport.sendCan(-1, new byte[1]));
            assertThrows(IOException.class, () -> transport.sendCan(0x714, new byte[9]));
            assertEquals(2, port.outgoing.size());
        }
        assertTrue(port.closed);
    }
}
