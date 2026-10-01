package com.rusefi.m749;

import com.rusefi.libopenblt.file.SrecParser;
import java.io.BufferedReader;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/** Intel HEX records retain absolute addresses; no gap filling or relocation. */
final class IntelHexReader {
    private IntelHexReader() { }
    static List<SrecParser.SRecord> read(Path file) throws IOException {
        List<SrecParser.SRecord> records = new ArrayList<>();
        long base = 0;
        boolean eof = false;
        try (BufferedReader reader = Files.newBufferedReader(file, StandardCharsets.US_ASCII)) {
            String line;
            int number = 0;
            while ((line = reader.readLine()) != null) {
                number++;
                line = line.trim();
                if (line.isEmpty()) { continue; }
                if (eof || !line.startsWith(":") || line.length() < 11 || line.length() > 521 || line.length() % 2 != 1) {
                    throw bad(number);
                }
                byte[] bytes = new byte[(line.length() - 1) / 2];
                int sum = 0;
                for (int i = 0; i < bytes.length; i++) {
                    int hi = Character.digit(line.charAt(1 + i * 2), 16);
                    int lo = Character.digit(line.charAt(2 + i * 2), 16);
                    if (hi < 0 || lo < 0) { throw bad(number); }
                    bytes[i] = (byte) (hi * 16 + lo);
                    sum += bytes[i] & 255;
                }
                int length = bytes[0] & 255;
                int offset = (bytes[1] & 255) << 8 | bytes[2] & 255;
                int type = bytes[3] & 255;
                if (bytes.length != length + 5 || (sum & 255) != 0) { throw bad(number); }
                if (type == 0) {
                    long address = base + offset;
                    if (length == 0 || offset + length > 65536 || address + length > 0x1_0000_0000L) { throw bad(number); }
                    byte[] data = new byte[length];
                    System.arraycopy(bytes, 4, data, 0, length);
                    records.add(new SrecParser.SRecord((int) address, data));
                } else if (type == 1) {
                    if (length != 0 || offset != 0) { throw bad(number); }
                    eof = true;
                } else if (type == 2 || type == 4) {
                    if (length != 2 || offset != 0) { throw bad(number); }
                    base = ((bytes[4] & 255L) << 8 | bytes[5] & 255L) << (type == 2 ? 4 : 16);
                } else if (type == 3 || type == 5) {
                    // Start metadata is not a command to execute or relocate the payload.
                    if (length != 4 || offset != 0) { throw bad(number); }
                } else {
                    throw bad(number);
                }
            }
        }
        if (!eof) { throw new IOException("Intel HEX EOF record is missing"); }
        return records;
    }
    private static IOException bad(int line) { return new IOException("Invalid Intel HEX record at line " + line); }
}
