package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.io.IOException;
import java.nio.file.*;
import java.util.Arrays;
import java.util.Properties;
import static org.junit.jupiter.api.Assertions.*;

class M749FlashReaderTest {
    @TempDir Path directory;
    @Test void completeMainFlashHasExactBytesHashAndCoverageIncludingFinalShortBlock() throws Exception {
        Path out = directory.resolve("complete flash.bin");
        M749RamHelperTest.Ecu ecu = new M749RamHelperTest.Ecu();
        String checksum;
        try (FlashReadFile file = new FlashReadFile(out, M749RamHelper.BASE, M749RamHelper.SIZE, false)) {
            file.identify("test target");
            checksum = M749FlashReader.read(new M749RamHelper(ecu), file, 4080, s -> {});
        }
        byte[] expected = M749RamHelperTest.Ecu.content(M749RamHelper.BASE, M749RamHelper.SIZE);
        assertArrayEquals(expected, Files.readAllBytes(out));
        assertEquals(FlashReadFile.hash(expected), checksum);
        assertFalse(Files.exists(Path.of(out + ".part")));
        Properties state = new Properties();
        try (java.io.InputStream in = Files.newInputStream(Path.of(out + ".properties"))) { state.load(in); }
        assertEquals("true", state.getProperty("complete")); assertEquals(checksum, state.getProperty("sha256"));
        assertEquals(2028, ecu.requests.size()); // four probes plus two reads for each of 1012 blocks
        assertThrows(IOException.class, () -> new FlashReadFile(out, M749RamHelper.BASE, M749RamHelper.SIZE, false));
    }

    @Test void failedBlockNeverAdvancesCheckpointAndResumeVerifiesSavedPrefix() throws Exception {
        Path out = directory.resolve("resume.bin");
        M749RamHelperTest.Ecu first = new M749RamHelperTest.Ecu(); first.failAt = 8;
        try (FlashReadFile file = new FlashReadFile(out, M749RamHelper.BASE, 301, false)) {
            assertThrows(IOException.class, () -> M749FlashReader.read(new M749RamHelper(first), file, 128, s -> {}));
            assertEquals(128, file.completed());
        }
        assertFalse(Files.exists(out));
        Files.write(Path.of(out + ".part"), new byte[17], StandardOpenOption.APPEND);
        M749RamHelperTest.Ecu second = new M749RamHelperTest.Ecu();
        try (FlashReadFile file = new FlashReadFile(out, M749RamHelper.BASE, 301, true)) {
            assertEquals(128, Files.size(Path.of(out + ".part")));
            M749FlashReader.read(new M749RamHelper(second), file, 128, s -> {});
        }
        assertArrayEquals(M749RamHelperTest.Ecu.content(M749RamHelper.BASE, 301), Files.readAllBytes(out));
        assertEquals(128, ((second.requests.get(4)[5] & 255) << 8) | second.requests.get(4)[6] & 255);
    }

    @Test void rejectsChangedSavedBytesAndDifferentTargetWithoutExtendingBackup() throws Exception {
        Path out = directory.resolve("changed.bin");
        try (FlashReadFile file = new FlashReadFile(out, M749RamHelper.BASE, 512, false)) {
            file.identify("one"); file.append(M749RamHelperTest.Ecu.content(M749RamHelper.BASE, 128));
            assertThrows(IOException.class, () -> new FlashReadFile(out, M749RamHelper.BASE, 512, true));
        }
        try (FlashReadFile file = new FlashReadFile(out, M749RamHelper.BASE, 512, true)) {
            assertThrows(IOException.class, () -> file.identify("two"));
            M749RamHelperTest.Ecu ecu = new M749RamHelperTest.Ecu(); ecu.changeAt = M749RamHelper.BASE;
            assertThrows(IOException.class, () -> M749FlashReader.read(new M749RamHelper(ecu), file, 128, s -> {}));
            assertEquals(128, file.completed());
        }
        byte[] corrupt = Files.readAllBytes(Path.of(out + ".part")); corrupt[17] ^= 1;
        Files.write(Path.of(out + ".part"), corrupt);
        assertThrows(IOException.class, () -> new FlashReadFile(out, M749RamHelper.BASE, 512, true));
    }

    @Test void mismatchedRepeatAndInterruptionLeaveNoPublishedOutput() throws Exception {
        Path out = directory.resolve("mismatch.bin");
        M749RamHelperTest.Ecu ecu = new M749RamHelperTest.Ecu(); ecu.changeRead = 6;
        try (FlashReadFile file = new FlashReadFile(out, M749RamHelper.BASE, 128, false)) {
            assertThrows(IOException.class, () -> M749FlashReader.read(new M749RamHelper(ecu), file, 128, s -> {}));
            assertEquals(0, file.completed());
            Thread.currentThread().interrupt();
            try { assertThrows(InterruptedException.class, () -> M749FlashReader.read(new M749RamHelper(ecu), file, 128, s -> {})); }
            finally { Thread.interrupted(); }
        }
        assertFalse(Files.exists(out));
    }

    @Test void refusesMismatchedResumeGeometryAndNeverOverwritesNewFinalFile() throws Exception {
        Path out = directory.resolve("race.bin");
        try (FlashReadFile file = new FlashReadFile(out, M749RamHelper.BASE, 8, false)) {
            file.append(new byte[8]);
            Files.writeString(out, "human file");
            assertThrows(IOException.class, file::finish);
        }
        assertEquals("human file", Files.readString(out));
        Files.delete(out);
        assertThrows(IOException.class, () -> new FlashReadFile(out, M749RamHelper.BASE, 9, true));
        assertTrue(Files.exists(Path.of(out + ".part")));
    }
}
