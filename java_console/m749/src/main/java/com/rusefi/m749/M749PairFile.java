package com.rusefi.m749;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.Arrays;
import java.util.function.Consumer;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Sparse indexed key/reference bytes. Missing indices are unknown, never zero. */
final class M749PairFile {
    static final int SIZE = 24;
    static final String HEADER = "M749PAIR1 I865";
    private static final Pattern ENTRY = Pattern.compile("([0-9]{1,2})\\s*=\\s*([0-9a-fA-F]{2})");
    private final int[] values = new int[SIZE];

    M749PairFile() { Arrays.fill(values, -1); }

    static int address(int index) {
        requireIndex(index);
        return index < 16 ? 0x08274000 + index : 0x0804C2B4 + index - 16;
    }

    private static void requireIndex(int index) {
        if (index < 0 || index >= SIZE) { throw new IllegalArgumentException("Pair index must be 0..23"); }
    }

    int get(int index) { requireIndex(index); return values[index]; }

    void put(int index, int value) throws IOException {
        requireIndex(index);
        if (value < 0 || value > 255) { throw new IllegalArgumentException("Invalid byte"); }
        if (values[index] != -1 && values[index] != value) {
            throw new IOException("Conflicting pair byte at index " + index);
        }
        values[index] = value;
    }

    int knownCount() {
        int count = 0;
        for (int value : values) { if (value >= 0) { count++; } }
        return count;
    }

    static M749PairFile load(Path path) throws IOException {
        if (Files.size(path) > 16384) { throw new IOException("Pair file too large"); }
        M749PairFile result = new M749PairFile();
        boolean header = false;
        int lineNumber = 0;
        for (String original : Files.readAllLines(path, StandardCharsets.UTF_8)) {
            String line = original.trim();
            if (++lineNumber == 1 && line.startsWith("\uFEFF")) { line = line.substring(1); }
            if (line.isEmpty() || line.startsWith("#")) { continue; }
            if (!header) {
                if (!line.equals(HEADER)) { throw new IOException("Unsupported pair-file header/profile"); }
                header = true;
                continue;
            }
            Matcher m = ENTRY.matcher(line);
            if (!m.matches()) { throw new IOException("Invalid pair-file line " + lineNumber); }
            int index = Integer.parseInt(m.group(1));
            if (index >= SIZE || result.values[index] != -1) {
                throw new IOException("Duplicate or out-of-range pair index on line " + lineNumber);
            }
            result.put(index, Integer.parseInt(m.group(2), 16));
        }
        if (!header) { throw new IOException("Missing pair-file header"); }
        return result;
    }

    void save(Path path) throws IOException {
        Path destination = path.toAbsolutePath();
        StringBuilder text = new StringBuilder(HEADER).append('\n');
        for (int i = 0; i < SIZE; i++) {
            if (values[i] >= 0) { text.append(String.format("%d=%02X%n", i, values[i])); }
        }
        Path temporary = Files.createTempFile(destination.getParent(), ".m749-pair-", ".tmp");
        try {
            try (FileChannel file = FileChannel.open(temporary, StandardOpenOption.WRITE)) {
                ByteBuffer data = StandardCharsets.UTF_8.encode(text.toString());
                while (data.hasRemaining()) { file.write(data); }
                file.force(true);
            }
            Files.move(temporary, destination, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally {
            Files.deleteIfExists(temporary);
        }
    }

    M749Immo credential() throws IOException {
        if (knownCount() != SIZE) {
            throw new IOException("Incomplete pair file: " + knownCount() + "/24 bytes known");
        }
        byte[] key = new byte[16], reference = new byte[8];
        for (int i = 0; i < 16; i++) { key[i] = (byte) values[i]; }
        for (int i = 0; i < 8; i++) { reference[i] = (byte) values[i + 16]; }
        return new M749Immo(key, reference);
    }

    void readMissing(M749ChecksumReader reader, Path path, Consumer<String> out)
            throws IOException, InterruptedException {
        // Check all existing entries before extending a file. Refuse conflicting
        // saved data; matching a sparse subset alone does not identify an ECU.
        for (int i = 0; i < SIZE; i++) {
            if (values[i] >= 0) { reader.verifyByte(address(i), values[i]); }
        }
        save(path);
        out.accept("Pair file checked: " + knownCount() + "/24 known bytes");
        for (int i = 0; i < SIZE; i++) {
            if (values[i] < 0) {
                int value = reader.readByte(address(i));
                put(i, value);
                save(path);
                out.accept("Saved pair index " + i + "; " + knownCount() + "/24 bytes known");
            }
        }
    }
}
