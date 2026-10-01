package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.function.Consumer;
import static org.junit.jupiter.api.Assertions.*;

class M749CliReadTest {
    @TempDir Path directory;
    private final M749Monitor.Backend noDevices = new M749Monitor.Backend() {
        public List<PcanDevice.Channel> scan() { throw new AssertionError("Unexpected adapter access"); }
        public List<String> identify(PcanDevice.Channel c, Consumer<String> out) { throw new AssertionError("Unexpected identify"); }
    };
    private final M749Cli.UploadAction noUpload = (c, i, v, a, o) -> { throw new AssertionError("Unexpected upload"); };
    private final M749Cli.ReadAction noRead = (c, a, p, k, immo, o) -> { throw new AssertionError("Unexpected read"); };

    @Test void invalidReadModesNeverOpenAnAdapter() throws Exception {
        for (String[] args : new String[][]{
                {"--read-byte"},
                {"--read-byte", "nope", "--channel", "PCAN_USBBUS1"},
                {"--read-byte", "0x40000000", "--channel", "PCAN_USBBUS1"},
                 {"--export-pair", "ecu.pair"},
                {"--read-pair", "ecu.pair", "--channel", "PCAN_USBBUS1", "--dry-run"},
                {"--read-pair", "ecu.pair", "--read-byte", "08000000", "--channel", "PCAN_USBBUS1"},
                {"--pair-file", "ecu.pair"}, {"--list", "--read-pair", "ecu.pair"}}) {
            assertEquals(2, M749Cli.execute(args, noDevices, noUpload, noRead, s -> {}));
        }
    }

    @Test void byteReadPassesExactAddressAndChannel() throws Exception {
        boolean[] called = {false};
        assertEquals(0, M749Cli.execute(new String[]{"--read-byte", "0x0827400F", "--channel", "PCAN_USBBUS2"},
                noDevices, noUpload, (c, a, p, k, immo, o) -> {
                    called[0] = true;
                    assertEquals("PCAN_USBBUS2", c.channel); assertEquals(0x0827400F, a.intValue());
                    assertNull(p); assertNull(k);
                }, s -> {}));
        assertTrue(called[0]);
    }

    @Test void socketCanByteAndPairReadsUseExplicitInterface() throws Exception {
        for (String mode : new String[]{"--read-byte", "--read-pair"}) {
            String target = mode.equals("--read-byte") ? "0x08000004" : directory.resolve("socketcan.pair").toString();
            boolean[] called = {false};
            assertEquals(0, M749Cli.execute(new String[]{mode, target, "--socketcan", "can9"},
                    noDevices, noUpload, (channel, address, path, known, immo, out) -> {
                        called[0] = true;
                        assertEquals("can9", channel.socketcan);
                        if (address != null) { assertEquals(0x08000004, address.intValue()); }
                        else { assertEquals(Path.of(target), path); assertEquals(0, known.knownCount()); }
                    }, s -> {}));
            assertTrue(called[0]);
        }
    }

    @Test void pairReadPassesSparseStateAndRejectsBadFilesBeforeAdapterAccess() throws Exception {
        Path path = directory.resolve("ecu pair.pair");
        M749PairFile pair = new M749PairFile(); pair.put(5, 0); pair.save(path);
        boolean[] called = {false};
        assertEquals(0, M749Cli.execute(new String[]{"--read-pair", path.toString(), "--channel", "PCAN_USBBUS1"},
                noDevices, noUpload, (c, a, p, k, immo, o) -> {
                    called[0] = true;
                    assertNull(a); assertEquals(path, p); assertEquals(1, k.knownCount());
                    assertEquals(0, k.get(5)); assertEquals(-1, k.get(0));
                }, s -> {}));
        assertTrue(called[0]);
        Files.writeString(path, "not a pair file");
        assertThrows(IOException.class, () -> M749Cli.execute(
                new String[]{"--read-pair", path.toString(), "--channel", "PCAN_USBBUS1"},
                noDevices, noUpload, noRead, s -> {}));
    }
    @Test void readAuthorizationLoadsCompleteCredentialBeforeAdapterAccess() throws Exception {
        Path credential = directory.resolve("known.pair");
        M749PairFile pair = new M749PairFile();
        for (int i = 0; i < 24; i++) { pair.put(i, i); }
        pair.save(credential);
        for (String mode : new String[]{"--read-pair", "--read-byte"}) {
            String target = mode.equals("--read-pair") ? directory.resolve("capture.pair").toString() : "08000004";
            String[] args = {mode, target, "--channel", "PCAN_USBBUS1", "--pair-file", credential.toString()};
            boolean[] called = {false};
            assertEquals(0, M749Cli.execute(args, noDevices, noUpload, (c, a, p, k, immo, o) -> {
                called[0] = true;
                assertNotNull(immo);
                for (int i = 0; i < 24; i++) { assertEquals(i, immo.pairFile().get(i)); }
            }, s -> {}));
            assertTrue(called[0]);
        }
        Files.writeString(credential, "M749PAIR1 I865\n0=00\n");
        assertThrows(IOException.class, () -> M749Cli.execute(new String[]{"--read-pair",
                directory.resolve("capture.pair").toString(), "--channel", "PCAN_USBBUS1",
                "--pair-file", credential.toString()}, noDevices, noUpload, noRead, s -> {}));
        assertEquals(2, M749Cli.execute(new String[]{"--read-pair", "capture.pair",
                "--channel", "PCAN_USBBUS1", "--pair-file", credential.toString(),
                "--immo-backup", "backup.bin"}, noDevices, noUpload, noRead, s -> {}));
    }

}
