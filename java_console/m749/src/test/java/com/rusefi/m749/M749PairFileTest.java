package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import static org.junit.jupiter.api.Assertions.*;

class M749PairFileTest {
    @TempDir Path directory;

    @Test void sparseFileKeepsUnknownSeparateFromZeroAndFF() throws Exception {
        Path path = directory.resolve("ecu with spaces.pair");
        M749PairFile file = new M749PairFile();
        file.put(0, 0); file.put(23, 255); file.save(path);
        M749PairFile loaded = M749PairFile.load(path);
        assertEquals(2, loaded.knownCount());
        assertEquals(0, loaded.get(0)); assertEquals(255, loaded.get(23));
        assertEquals(-1, loaded.get(1));
        assertFalse(Files.readString(path).contains("1="));
        assertThrows(IOException.class, loaded::credential);
        assertThrows(IOException.class, () -> loaded.put(0, 1));
    }

    @Test void rejectsMalformedDuplicateOutOfRangeAndWrongProfileFiles() throws Exception {
        Path path = directory.resolve("invalid.pair");
        for (String text : new String[]{"", "OTHER\n0=00\n", "M749PAIR1 I865\n0=00\n0=00\n",
                "M749PAIR1 I865\n24=01\n", "M749PAIR1 I865\n-1=00\n",
                "M749PAIR1 I865\n0=100\n", "M749PAIR1 I865\n0=??\n"}) {
            Files.writeString(path, text);
            assertThrows(IOException.class, () -> M749PairFile.load(path));
        }
    }

    @Test void interruptedReadPersistsOnlyVerifiedBytesAndResumeSkipsTheirSearch() throws Exception {
        Path path = directory.resolve("partial.pair");
        M749ChecksumReaderTest.Ecu ecu = new M749ChecksumReaderTest.Ecu();
        for (int i = 0; i < 24; i++) { ecu.memory.put(M749PairFile.address(i), i); }
        ecu.failAddress = M749PairFile.address(3);
        M749PairFile first = new M749PairFile();
        assertThrows(IOException.class, () -> first.readMissing(new M749ChecksumReader(ecu), path, s -> {}));
        M749PairFile saved = M749PairFile.load(path);
        assertEquals(3, saved.knownCount()); assertEquals(-1, saved.get(3));
        ecu.requests.clear(); ecu.failAddress = -1;
        saved.readMissing(new M749ChecksumReader(ecu), path, s -> {});
        for (int i = 0; i < 3; i++) {
            final int address = M749PairFile.address(i);
            assertEquals(2, ecu.requests.stream().filter(q -> M749ChecksumReaderTest.Ecu.word(q, 5) == address).count());
        }
        assertEquals(24, M749PairFile.load(path).knownCount());
        assertNotNull(M749PairFile.load(path).credential());
        try (java.util.stream.Stream<Path> files = Files.list(directory)) {
            assertEquals(1, files.count(), "No temporary files left after checkpointing");
        }
    }

    @Test void staleSavedValuePreventsExtendingOrOverwritingTheFile() throws Exception {
        Path path = directory.resolve("other-ecu.pair");
        M749PairFile file = new M749PairFile(); file.put(4, 17); file.save(path);
        byte[] before = Files.readAllBytes(path);
        M749ChecksumReaderTest.Ecu ecu = new M749ChecksumReaderTest.Ecu();
        assertThrows(IOException.class, () -> file.readMissing(new M749ChecksumReader(ecu), path, s -> {}));
        assertArrayEquals(before, Files.readAllBytes(path));
        assertEquals(1, ecu.requests.size());
    }

    @Test void anIncorrectCandidateMatchIsNotSaved() throws Exception {
        Path path = directory.resolve("untrusted.pair");
        M749ChecksumReaderTest.Ecu ecu = new M749ChecksumReaderTest.Ecu(); ecu.alwaysMatch = true;
        assertThrows(IOException.class, () -> new M749PairFile().readMissing(new M749ChecksumReader(ecu), path, s -> {}));
        assertEquals(0, M749PairFile.load(path).knownCount());
    }

    @Test void completeCredentialRoundTripsExactlyWithoutLoggingValues() throws Exception {
        byte[] key = new byte[16], reference = new byte[8];
        for (int i = 0; i < 16; i++) { key[i] = (byte) (i + 200); }
        for (int i = 0; i < 8; i++) { reference[i] = (byte) (i + 100); }
        M749PairFile pair = new M749Immo(key, reference).pairFile();
        Path path = directory.resolve("complete.pair"); pair.save(path);
        M749PairFile again = M749PairFile.load(path).credential().pairFile();
        for (int i = 0; i < 24; i++) { assertEquals(pair.get(i), again.get(i)); }
        M749ChecksumReaderTest.Ecu ecu = new M749ChecksumReaderTest.Ecu();
        for (int i = 0; i < 24; i++) { ecu.memory.put(M749PairFile.address(i), again.get(i)); }
        List<String> messages = new ArrayList<>();
        again.readMissing(new M749ChecksumReader(ecu), path, messages::add);
        assertEquals(List.of("Pair file checked: 24/24 known bytes"), messages);
        assertEquals(48, ecu.requests.size());
        assertEquals(0x0827400F, M749PairFile.address(15));
        assertEquals(0x0804C2B4, M749PairFile.address(16));
        assertEquals(0x0804C2BB, M749PairFile.address(23));
    }
}
