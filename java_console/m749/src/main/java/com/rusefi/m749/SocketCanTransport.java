package com.rusefi.m749;

import com.rusefi.io.can.CanAddress;
import com.rusefi.io.can.ClassicCanFrame;
import com.rusefi.io.can.RawCanPort;
import com.rusefi.io.can.SocketCanRawPort;
import java.io.IOException;
import java.util.Locale;

/** Linux CAN interface configured by the host at 500 kbit/s. */
final class SocketCanTransport implements RawCanTransport {
    private final RawCanPort port;

    SocketCanTransport(RawCanPort port) {
        this.port = port;
    }

    static void requireInterface(String name) {
        if (name.isBlank() || name.startsWith("-") || name.equalsIgnoreCase("auto")) {
            throw new IllegalArgumentException("--socketcan requires an explicit interface name, e.g. can0");
        }
    }

    static SocketCanTransport open(String name) throws IOException {
        requireInterface(name);
        if (!System.getProperty("os.name").toLowerCase(Locale.ROOT).startsWith("linux")) {
            throw new IOException("SocketCAN requires Linux; use --slcan or native Windows --channel");
        }
        SocketCanRawPort port = new SocketCanRawPort(name);
        try {
            port.open(new CanAddress[]{new CanAddress(0x7E8, false),
                    new CanAddress(0x713, false), new CanAddress(0x714, false)});
            return new SocketCanTransport(port);
        } catch (IOException | RuntimeException | LinkageError e) {
            try { port.close(); } catch (IOException close) { e.addSuppressed(close); }
            throw new IOException("Cannot open SocketCAN " + name + ": " + e.getMessage() +
                    ". Bring the interface up at 500000 bit/s; JavaCAN native libraries must match the Linux JVM.", e);
        }
    }

    @Override
    public void sendCan(int id, byte[] data) throws IOException {
        if (id < 0 || id > 0x7FF || data.length > 8) {
            throw new IOException("Invalid standard CAN frame");
        }
        port.send(new ClassicCanFrame(new CanAddress(id, false), data));
    }

    @Override
    public Frame receiveCan() throws IOException {
        // A zero SO_RCVTIMEO means infinite blocking on Linux. Keep polls bounded.
        ClassicCanFrame frame = port.receive(1).orElse(null);
        if (frame == null || frame.getAddress().isExtended()) { return null; }
        return new Frame(frame.getAddress().getId(), frame.getPayload());
    }

    @Override
    public void close() throws IOException {
        port.close();
    }
}
