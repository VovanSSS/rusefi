package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.EnumMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;
import static com.rusefi.m749.M749Identification.bytes;
import static com.rusefi.m749.M749ImageTest.word;
import static org.junit.jupiter.api.Assertions.*;

class M749OemImageTest {
    @TempDir Path directory;
    private static final Map<M749TargetProfile, byte[]> FIXTURES = new EnumMap<>(M749TargetProfile.class);

    static synchronized byte[] backup(M749TargetProfile profile) {
        if (FIXTURES.containsKey(profile)) return FIXTURES.get(profile).clone();
        byte[] data = new byte[M749RamHelper.SIZE];
        Arrays.fill(data, (byte) 0xA5);
        word(data, 0x1000, profile == M749TargetProfile.I812 ? 0 : 0x20020000);
        word(data, 0x1004, 0x08080001);
        int split = profile.calibrationStart - 0x08000000;
        int software = crc(data, 0x1000, split, -1);
        word(data, 0xFFFFC, crc(data, 0x80000, 0xFFFFC, software));
        word(data, 0x7FFFC, crc(data, split, 0x7FFFC, -1));
        for (int n = 0; n < profile.addresses.length; n++) {
            for (int i = 0; i < profile.sentinels[n].length(); i += 2) {
                data[profile.addresses[n] - 0x08000000 + i / 2] = (byte) Integer.parseInt(profile.sentinels[n].substring(i, i + 2), 16);
            }
        }
        // Solve the final four boot-domain bytes for the profile's CRC. No OEM code fixture is needed.
        int prefix = crc(data, 0x201000, 0x22DFF8, crc(data, 0, 0x1000, -1));
        byte[] patch = new byte[4];
        int baseline = M749Image.crc32(patch, 4, prefix);
        int[] basis = new int[32], coefficients = new int[32];
        for (int bit = 0; bit < 32; bit++) {
            word(patch, 0, 1 << bit);
            int value = M749Image.crc32(patch, 4, prefix) ^ baseline, coefficient = 1 << bit;
            for (int n = 31; n >= 0; n--) {
                if ((value & (1 << n)) == 0) continue;
                if (basis[n] == 0) { basis[n] = value; coefficients[n] = coefficient; break; }
                value ^= basis[n]; coefficient ^= coefficients[n];
            }
        }
        int desired = profile.bootCrc ^ baseline, solution = 0;
        for (int n = 31; n >= 0; n--) {
            if ((desired & (1 << n)) != 0) { desired ^= basis[n]; solution ^= coefficients[n]; }
        }
        assertEquals(0, desired);
        word(data, 0x22DFF8, solution);
        word(data, 0x22DFFC, profile.bootCrc);
        FIXTURES.put(profile, data);
        return data.clone();
    }

    private static int crc(byte[] data, int start, int end, int initial) {
        return M749Image.crc32(Arrays.copyOfRange(data, start, end), end - start, initial);
    }

    @Test void validatesBothLayoutsAndSelectsOnlyApplicationAndCalibration() throws Exception {
        for (M749TargetProfile profile : M749TargetProfile.values()) {
            byte[] data = backup(profile);
            Path file = directory.resolve(profile + " full backup.BIN");
            Files.write(file, data);
            M749Image image = M749Image.load(file, M749Image.Domain.SOFTWARE);
            assertEquals(M749Image.Domain.OEM, image.domain);
            assertEquals(profile, image.oemProfile);
            image.requireTarget(profile);
            assertThrows(IOException.class, () -> image.requireTarget(profile == M749TargetProfile.I812 ? M749TargetProfile.I865 : M749TargetProfile.I812));
            assertEquals(1, image.ranges.size());
            assertEquals(0x08001000, image.ranges.get(0).address);
            assertArrayEquals(Arrays.copyOfRange(data, 0x1000, 0x100000), image.ranges.get(0).bytes());
            assertThrows(IOException.class, () -> M749Image.load(file, M749Image.Domain.CALIBRATION));
        }
    }

    @Test void rejectsPartialUnknownCorruptAndInvalidVectors() {
        assertThrows(IOException.class, () -> M749Image.oem(new byte[0x200000]));
        for (int offset : new int[]{0x1000, 0x60000, 0x80000, 0x7FFFC, 0xFFFFC, 0, 0x201000, 0x22DFFC}) {
            byte[] data = backup(M749TargetProfile.I865);
            data[offset] ^= 1;
            assertThrows(IOException.class, () -> M749Image.oem(data), "corruption at " + Integer.toHexString(offset));
        }
        byte[] badVectors = backup(M749TargetProfile.I812);
        word(badVectors, 0x1004, 0x08081001);
        word(badVectors, 0xFFFFC, crc(badVectors, 0x80000, 0xFFFFC, crc(badVectors, 0x1000, 0x69000, -1)));
        assertTrue(assertThrows(IOException.class, () -> M749Image.oem(badVectors)).getMessage().contains("vectors"));
    }

    @Test void cliDryRunAndDefaultTransportValidateBeforeHardware() throws Exception {
        M749Monitor.Backend noDevices = new M749Monitor.Backend() {
            public List<PcanDevice.Channel> scan() { throw new AssertionError("Unexpected device access"); }
            public List<String> identify(PcanDevice.Channel c, Consumer<String> out) { throw new AssertionError(); }
        };
        Path file = directory.resolve("OEM image with spaces.bin");
        Files.write(file, backup(M749TargetProfile.I865));
        assertEquals(0, M749Cli.execute(new String[]{"--write-flash", file.toString(), "--dry-run"}, noDevices,
                (c, i, v, a, o) -> fail("Dry run opened uploader"), s -> {}));
        boolean[] called = {false};
        assertEquals(0, M749Cli.execute(new String[]{"--write-flash", file.toString()}, noDevices,
                (c, i, v, a, o) -> { called[0] = true; assertEquals("auto", c.slcan); assertEquals(M749Image.Domain.OEM, i.domain); }, s -> {}));
        assertTrue(called[0]);
        Files.write(file, new byte[100]);
        assertThrows(IOException.class, () -> M749Cli.execute(new String[]{"--write-flash", file.toString()}, noDevices,
                (c, i, v, a, o) -> fail("Invalid image opened uploader"), s -> {}));
    }

    private static class OemEcu extends M749UploaderTest.Ecu {
        boolean rejectApplication;
        OemEcu(M749TargetProfile profile) {
            for (int n = 0; n < profile.addresses.length; n++) putHex(profile.addresses[n], profile.sentinels[n]);
            word(flash, 0x22DFFC, profile.bootCrc);
        }
        @Override public byte[] exchange(byte[] q, byte[] prefix, long timeout) throws IOException {
            if (app && q[0] == 0x22) {
                assertArrayEquals(bytes(0x22, 0xF1, 0x86), q, "OEM must not require rusEFI activation DIDs");
                return bytes(0x62, 0xF1, 0x86, rejectApplication ? 2 : 1);
            }
            return super.exchange(q, prefix, timeout);
        }
    }

    @Test void restoresBothProfilesPreservesProtectedFlashAndChecksOemReturn() throws Exception {
        for (M749TargetProfile profile : M749TargetProfile.values()) {
            OemEcu ecu = new OemEcu(profile);
            byte[] original = ecu.flash.clone(), data = backup(profile);
            new M749Uploader(ecu, ecu.messages::add).upload(M749Image.oem(data), false);
            assertEquals(255, ecu.erases.size());
            assertEquals(1, ecu.resets);
            assertArrayEquals(Arrays.copyOfRange(data, 0x1000, 0x100000), Arrays.copyOfRange(ecu.flash, 0x1000, 0x100000));
            assertArrayEquals(Arrays.copyOf(original, 0x1000), Arrays.copyOf(ecu.flash, 0x1000));
            assertArrayEquals(Arrays.copyOfRange(original, 0x100000, original.length), Arrays.copyOfRange(ecu.flash, 0x100000, ecu.flash.length));
            assertTrue(ecu.messages.stream().anyMatch(s -> s.startsWith("Upload complete: OEM")));
        }
    }

    @Test void profileMismatchAndPreflightSendNoEraseAndFailedReturnDoesNotReportSuccess() throws Exception {
        OemEcu mismatch = new OemEcu(M749TargetProfile.I865);
        assertThrows(IOException.class, () -> new M749Uploader(mismatch, s -> {}).upload(M749Image.oem(backup(M749TargetProfile.I812)), false));
        assertTrue(mismatch.erases.isEmpty());
        OemEcu check = new OemEcu(M749TargetProfile.I812);
        new M749Uploader(check, s -> {}).checkTarget(M749Image.oem(backup(M749TargetProfile.I812)));
        assertTrue(check.erases.isEmpty());
        assertEquals(0, check.resets);
        OemEcu failure = new OemEcu(M749TargetProfile.I865);
        failure.rejectApplication = true;
        assertThrows(IOException.class, () -> new M749Uploader(failure, failure.messages::add).upload(M749Image.oem(backup(M749TargetProfile.I865)), false));
        assertEquals(1, failure.resets);
        assertFalse(failure.messages.stream().anyMatch(s -> s.startsWith("Upload complete")));
    }
}
