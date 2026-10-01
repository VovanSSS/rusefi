package com.rusefi.m749;

import com.rusefi.io.can.slcan.SlcanPortScanner;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import static org.junit.jupiter.api.Assertions.*;

class M749ReadFlashCliTest {
    @TempDir Path directory;
    M749ReadFlashCli.TransportFactory noAdapter = o -> { throw new AssertionError("Unexpected adapter access"); };

    @Test void identifyRejectsReadAndWriteOptionsBeforeOpeningAdapter() throws Exception {
        for (String option : new String[]{"--read-flash", "--upload", "--helper-running", "--reset-after", "--resume", "--identify"}) {
            assertEquals(2, M749ReadFlashCli.identify(new String[]{"--identify", option}, noAdapter, s -> {}));
        }
        assertEquals(0, M749ReadFlashCli.identify(new String[]{"--identify", "--help"}, noAdapter, s -> {}));
    }

    @Test void validatesAllOptionsBeforeAdapterAccess() throws Exception {
        for (String[] args : new String[][]{
                {"--read-flash", "--resume"}, {"--read-flash", "x", "--slcan"},
                {"--read-flash", "--serial-baud", "9600"},
                {"--read-flash", "x", "--slcan", "a", "--channel", "PCAN_USBBUS1"},
                {"--read-flash", "x", "--socketcan", "can0", "--slcan", "a"},
                {"--read-flash", "x", "--socketcan", "can0", "--channel", "auto"},
                {"--read-flash", "x", "--socketcan", "can0", "--serial-baud", "115200"},
                {"--read-flash", "x", "--socketcan", "can0", "--slcan-bus", "1"},
                {"--read-flash", "x", "--socketcan", "can0", "--socketcan", "can1"},
                {"--read-flash", "x", "--socketcan"},
                {"--read-flash", "x", "--socketcan", "auto"},
                {"--read-flash", "x", "--socketcan", ""},
                {"--read-flash", "x", "--slcan", "a", "--chunk-size", "4096"},
                {"--read-flash", "x", "--slcan", "a", "--start", "0x40000000"},
                {"--read-flash", "x", "--slcan", "a", "--length", "-1"},
                {"--read-flash", "x", "--slcan", "a", "--length", "0x400000"},
                {"--read-flash", "x", "--slcan", "a", "--stmin", "128"},
                {"--read-flash", "x", "--slcan", "a", "--slcan", "b"},
                {"--read-flash", "x", "--slcan", "a", "--upload", "file"},
                {"--read-flash", "x", "--channel", "PCAN_USBBUS1", "--serial-baud", "115200"},
                {"--read-flash", "x", "--slcan", "a", "--helper-running", "--pair-file", "file"}}) {
            assertEquals(2, M749ReadFlashCli.execute(args, noAdapter, s -> {}));
        }
        assertEquals(0, M749ReadFlashCli.execute(new String[]{"--read-flash", "--help"}, noAdapter, s -> {}));
    }

    @Test void acceptsWindowsAndUnixSerialNamesAndConservativeDefaults() {
        for (String port : new String[]{"COM12", "/dev/ttyACM0", "/dev/cu.usbmodem1"}) {
            M749ReadFlashCli.Options o = M749ReadFlashCli.parse(new String[]{"--read-flash", "backup file.bin", "--slcan", port});
            assertEquals(port, o.slcan); assertEquals(3, o.stmin); assertEquals(1024, o.chunk);
            assertEquals(0x3f0000, o.length); assertEquals(16, o.block);
        }
        assertEquals(1, M749ReadFlashCli.parse(new String[]{"--read-flash", "x", "--channel", "PCAN_USBBUS1"}).stmin);
    }

    @Test void noArgumentReadSelectsTimestampedOutputAndSharedScanner() {
        M749ReadFlashCli.Options o = M749ReadFlashCli.parse(new String[]{"--read-flash"});
        assertEquals("auto", o.slcan);
        assertEquals(3, o.stmin);
        assertTrue(o.output.toString().matches("m749-full-[0-9]{8}T[0-9]{9}Z\\.bin"));
        assertEquals("auto", M749ReadFlashCli.parse(new String[]{"--read-flash", "--channel", "auto"}).channel);
        assertEquals(Path.of("backup with spaces.bin"), M749ReadFlashCli.parse(new String[]{"--read-flash", "backup with spaces.bin"}).output);
    }

    @Test void socketCanUsesNativePacingAndReadOnlyIdentification() throws Exception {
        M749ReadFlashCli.Options options = M749ReadFlashCli.parse(new String[]{"--read-flash", "--socketcan", "can2"});
        assertEquals("can2", options.socketcan);
        assertNull(options.slcan);
        assertNull(options.channel);
        assertEquals(1, options.stmin);
        SocketCanTransportTest.Port port = new SocketCanTransportTest.Port() {
            public void send(com.rusefi.io.can.ClassicCanFrame frame) {
                super.send(frame);
                byte[] q = frame.getPayload();
                assertEquals(0x22, q[1]);
                reply(0x7e8, (byte) 4, (byte) 0x62, q[2], q[3], (byte) 1);
            }
        };
        assertEquals(0, M749ReadFlashCli.identify(new String[]{"--identify", "--socketcan", "can2"}, o -> {
            assertEquals("can2", o.socketcan);
            return new SocketCanTransport(port);
        }, s -> {}));
        assertFalse(port.outgoing.isEmpty());
        assertTrue(port.closed);
    }

    @Test void socketCanBackupUsesIsoTpAndPublishesVerifiedBytes() throws Exception {
        Path path = directory.resolve("socketcan backup.bin");
        WireEcu peer = new WireEcu();
        assertEquals(0, M749ReadFlashCli.execute(new String[]{"--read-flash", path.toString(),
                "--socketcan", "can0", "--length", "128"}, options -> {
            assertEquals("can0", options.socketcan);
            assertNull(options.slcan);
            return peer;
        }, s -> {}));
        assertArrayEquals(M749RamHelperTest.Ecu.content(M749RamHelper.BASE, 128), Files.readAllBytes(path));
        assertTrue(peer.closed);
    }

    @Test void discoveryExcludesConsoleAndUnknownPortsAndRejectsAmbiguity() throws Exception {
        SlcanPortScanner.Result console = new SlcanPortScanner.Result("COM1", SlcanPortScanner.Type.TS_CONSOLE, "rusEFI");
        SlcanPortScanner.Result unknown = new SlcanPortScanner.Result("COM2", SlcanPortScanner.Type.UNKNOWN, null);
        SlcanPortScanner.Result slcan = new SlcanPortScanner.Result("COM3", SlcanPortScanner.Type.SLCAN, "V1220");
        assertEquals("COM3", M749ReadFlashCli.selectSlcan(Arrays.asList(console, unknown, slcan), s -> {}));
        assertThrows(IOException.class, () -> M749ReadFlashCli.selectSlcan(Arrays.asList(console, unknown), s -> {}));
        assertThrows(IOException.class, () -> M749ReadFlashCli.selectSlcan(Arrays.asList(slcan,
                new SlcanPortScanner.Result("COM4", SlcanPortScanner.Type.SLCAN, "V1220")), s -> {}));
        assertThrows(IOException.class, () -> M749ReadFlashCli.selectOnly(Collections.emptyList(), "PCAN", "--channel"));
        assertThrows(IOException.class, () -> M749ReadFlashCli.selectOnly(Arrays.asList("PCAN_USBBUS1", "PCAN_USBBUS2"), "PCAN", "--channel"));
        assertEquals("PCAN_USBBUS1", M749ReadFlashCli.selectOnly(Collections.singletonList("PCAN_USBBUS1"), "PCAN", "--channel"));
    }

    @Test void badOutputAndResumeNeverOpenAdapter() throws Exception {
        Path out = directory.resolve("exists.bin"); Files.writeString(out, "existing");
        assertThrows(IOException.class, () -> M749ReadFlashCli.execute(new String[]{"--read-flash", out.toString(), "--slcan", "port"}, noAdapter, s -> {}));
        assertThrows(IOException.class, () -> M749ReadFlashCli.execute(new String[]{"--read-flash", directory.resolve("missing.bin").toString(),
                "--slcan", "port", "--resume"}, noAdapter, s -> {}));
    }

    /** Classic CAN peer with independent request assembly and finite-block replies. */
    static class WireEcu implements RawCanTransport {
        final M749RamHelperTest.Ecu ecu = new M749RamHelperTest.Ecu();
        final Queue<Frame> replies = new ArrayDeque<>();
        byte[] request, response;
        int requestAt, requestSequence, responseAt, responseSequence;
        boolean closed;
        WireEcu() throws IOException { }
        public void sendCan(int id, byte[] frame) throws IOException {
            assertEquals(0x7e0, id);
            int type = (frame[0] & 255) >>> 4;
            if (type == 3) {
                assertNotNull(response);
                int block = frame[1] & 255;
                for (int i = 0; responseAt < response.length && (block == 0 || i < block); i++) {
                    byte[] cf = new byte[8]; Arrays.fill(cf, (byte) 0xa5);
                    cf[0] = (byte) (0x20 | responseSequence); responseSequence = (responseSequence + 1) & 15;
                    int size = Math.min(7, response.length - responseAt);
                    System.arraycopy(response, responseAt, cf, 1, size); responseAt += size;
                    replies.add(new Frame(0x7e8, cf));
                }
                return;
            }
            if (type == 0) {
                request = Arrays.copyOfRange(frame, 1, 1 + frame[0]); requestAt = request.length;
            } else if (type == 1) {
                request = new byte[(frame[0] & 15) * 256 + (frame[1] & 255)];
                System.arraycopy(frame, 2, request, 0, 6); requestAt = 6; requestSequence = 1;
                replies.add(new Frame(0x7e8, new byte[]{0x30, 0, 0})); return;
            } else {
                assertEquals(2, type); assertEquals(0x20 | requestSequence, frame[0] & 255);
                requestSequence = (requestSequence + 1) & 15;
                int count = Math.min(7, request.length - requestAt);
                System.arraycopy(frame, 1, request, requestAt, count); requestAt += count;
            }
            if (requestAt < request.length) { return; }
            response = ecu.exchange(request, new byte[]{1}, 15000);
            byte[] first = new byte[8]; Arrays.fill(first, (byte) 0xcc);
            if (response.length <= 7) {
                first[0] = (byte) response.length; System.arraycopy(response, 0, first, 1, response.length);
            } else {
                first[0] = (byte) (0x10 | response.length >>> 8); first[1] = (byte) response.length;
                System.arraycopy(response, 0, first, 2, 6); responseAt = 6; responseSequence = 1;
            }
            replies.add(new Frame(0x7e8, first));
        }
        public Frame receiveCan() { return replies.poll(); }
        public void close() { closed = true; }
    }

    @Test void cliRunsBootstrapReadPublicationAndOptionalResetOverActualIsoTpClient() throws Exception {
        Path path = directory.resolve("cli backup.bin");
        WireEcu peer = new WireEcu(); List<String> log = new ArrayList<>();
        assertEquals(0, M749ReadFlashCli.execute(new String[]{"--read-flash", path.toString(),
                "--slcan", "/dev/test", "--length", "0x301", "--chunk-size", "128", "--reset-after"},
                options -> { assertEquals("/dev/test", options.slcan); return peer; }, log::add));
        assertArrayEquals(M749RamHelperTest.Ecu.content(M749RamHelper.BASE, 0x301), Files.readAllBytes(path));
        assertTrue(peer.closed);
        assertEquals(0x11, peer.ecu.requests.get(peer.ecu.requests.size() - 1)[0]);
        assertTrue(log.stream().anyMatch(line -> line.startsWith("Backup complete:")));
    }

    @Test void resetFailurePreservesPublishedBackupAndReportsItsCompletion() throws Exception {
        Path path = directory.resolve("reset failure.bin");
        WireEcu peer = new WireEcu() {
            public void sendCan(int id, byte[] frame) throws IOException {
                if (frame[0] == 2 && frame[1] == 0x11) { throw new IOException("Reset failed"); }
                super.sendCan(id, frame);
            }
        };
        List<String> log = new ArrayList<>();
        assertThrows(IOException.class, () -> M749ReadFlashCli.execute(new String[]{"--read-flash", path.toString(),
                "--slcan", "/dev/test", "--length", "32", "--reset-after"}, options -> peer, log::add));
        assertArrayEquals(M749RamHelperTest.Ecu.content(M749RamHelper.BASE, 32), Files.readAllBytes(path));
        assertTrue(log.stream().anyMatch(line -> line.startsWith("Backup is complete.")));
        assertFalse(log.stream().anyMatch(line -> line.contains("Use --resume")));
        assertTrue(peer.closed);
    }
}
