package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import java.util.function.Consumer;
import static org.junit.jupiter.api.Assertions.*;

class M749ConnectionOptionsTest {
    @TempDir Path directory;
    private final M749Monitor.Backend noDevice = new M749Monitor.Backend() {
        public List<PcanDevice.Channel> scan() { throw new AssertionError("Opened PCAN"); }
        public List<String> identify(PcanDevice.Channel c, Consumer<String> out) { throw new AssertionError("Opened PCAN"); }
    };
    private static final class Captured extends IOException { }

    @Test void helpWorksWithoutAnActionOperandOnEveryCommand() throws Exception {
        for (String action : List.of("--identify", "--read-flash", "--check-target", "--upload", "--write-flash", "--read-byte", "--read-pair")) {
            List<String> out = new ArrayList<>();
            assertEquals(0, M749Cli.execute(new String[]{action, "--help"}, noDevice,
                    (c, i, v, a, o) -> fail("Upload"), (c, a, p, k, immo, o) -> fail("Read"), out::add));
            assertTrue(out.stream().anyMatch(s -> s.contains("--serial-baud")), action);
            assertTrue(out.stream().anyMatch(s -> s.contains("--block-size")), action);
        }
    }

    @Test void allCommandsShareDefaultsExplicitOptionsAndAutoSelectors() throws Exception {
        Path firmware = directory.resolve("OEM with spaces.bin");
        Files.write(firmware, M749OemImageTest.backup(M749TargetProfile.I865));
        for (String[] selection : new String[][]{
                {}, {"--channel", "auto"}, {"--channel", "PCAN_USBBUS2"}, {"--socketcan", "can7"},
                {"--slcan", "COM with spaces", "--serial-baud", "57600", "--slcan-bus", "3"},
                {"--slcan", "auto", "--block-size", "8", "--stmin", "7"},
                {"--socketcan", "can0", "--block-size", "0", "--stmin", "0"}}) {
            M749ConnectionOptions expected = new M749ConnectionOptions();
            for (int i = 0; i < selection.length; i += 2) expected.accept(selection[i], selection[i + 1]);
            expected.validate();
            for (String action : List.of("--upload", "--write-flash", "--read-byte", "--read-pair", "--read-flash", "--check-target", "--identify")) {
                List<String> args = new ArrayList<>(List.of(action));
                if (action.equals("--upload") || action.equals("--write-flash") || action.equals("--check-target")) args.add(firmware.toString());
                else if (action.equals("--read-byte")) args.add("0x08000004");
                else if (!action.equals("--identify")) args.add(directory.resolve(UUID.randomUUID() + ".bin").toString());
                args.addAll(List.of(selection));
                Consumer<M749ConnectionOptions> check = actual -> assertEquals(expected.key(), actual.key(), action);
                M749ReadFlashCli.TransportFactory factory = actual -> { check.accept(actual); throw new Captured(); };
                String[] command = args.toArray(new String[0]);
                if (action.equals("--read-flash")) assertThrows(Captured.class, () -> M749ReadFlashCli.execute(command, factory, s -> {}));
                else if (action.equals("--identify")) assertThrows(Captured.class, () -> M749ReadFlashCli.identify(command, factory, s -> {}));
                else if (action.equals("--check-target")) assertThrows(Captured.class, () -> M749TargetCli.execute(command, factory, s -> {}));
                else {
                    boolean[] called = {false};
                    assertEquals(0, M749Cli.execute(command, noDevice,
                            (c, image, verify, immo, out) -> { check.accept(c); called[0] = true; },
                            (c, address, path, known, immo, out) -> { check.accept(c); called[0] = true; }, s -> {}));
                    assertTrue(called[0], action);
                }
            }
        }
    }

    @Test void invalidCommonOptionsAreRejectedByEveryCommandBeforeFileOrAdapterAccess() throws Exception {
        for (String[] selection : new String[][]{
                {"--channel", "auto", "--slcan", "auto"}, {"--channel", "auto", "--serial-baud", "115200"},
                {"--socketcan", "auto"}, {"--channel", "invalid"}, {"--slcan", ""},
                {"--slcan", "auto", "--serial-baud", "9600"}, {"--slcan-bus", "4"},
                {"--block-size", "256"}, {"--stmin", "-1"}, {"--stmin", "128"},
                {"--stmin", "1", "--stmin", "2"}, {"--serial-baud"}, {"--channel", "--dry-run"}}) {
            for (String action : List.of("--upload", "--write-flash", "--read-byte", "--read-pair", "--read-flash", "--check-target", "--identify")) {
                List<String> args = new ArrayList<>(List.of(action));
                if (!action.equals("--identify")) args.add(action.equals("--read-byte") ? "08000004" : "missing.bin");
                args.addAll(List.of(selection));
                String[] command = args.toArray(new String[0]);
                M749ReadFlashCli.TransportFactory noOpen = o -> { throw new AssertionError("Opened adapter"); };
                int exit;
                if (action.equals("--read-flash")) exit = M749ReadFlashCli.execute(command, noOpen, s -> {});
                else if (action.equals("--identify")) exit = M749ReadFlashCli.identify(command, noOpen, s -> {});
                else if (action.equals("--check-target")) exit = M749TargetCli.execute(command, noOpen, s -> {});
                else exit = M749Cli.execute(command, noDevice, (c, i, v, a, o) -> fail("Upload"),
                        (c, a, p, k, immo, o) -> fail("Read"), s -> {});
                assertEquals(2, exit, action + Arrays.toString(selection));
            }
        }
    }

    @Test void defaultAndPositionalIdentificationUseSameBackendOptions() throws Exception {
        List<String> selected = new ArrayList<>();
        M749Monitor.Backend backend = new M749Monitor.Backend() {
            public List<PcanDevice.Channel> scan() { throw new AssertionError(); }
            public List<String> identify(PcanDevice.Channel c, Consumer<String> out) { throw new AssertionError(); }
            public M749Monitor.Identification inspect(M749ConnectionOptions options, Consumer<String> out) {
                selected.add(options.connector() + ":" + options.endpoint());
                return new M749Monitor.Identification(M749FirmwareDetection.Result.OEM, List.of("ECU present"));
            }
        };
        for (String[] args : new String[][]{{}, {"PCAN_USBBUS2"}, {"--socketcan", "can0"}, {"--slcan", "COM1"}}) {
            assertEquals(0, M749Cli.execute(args, backend, (c, i, v, a, o) -> fail("Upload"), s -> {}));
        }
        assertEquals(List.of("SLCAN:auto", "PCAN:PCAN_USBBUS2", "SocketCAN:can0", "SLCAN:COM1"), selected);
    }
}
