package com.rusefi.m749;

import com.rusefi.libopenblt.file.SrecParser.SRecord;

import java.io.BufferedReader;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Strict upload input: checks metadata as well as data records. */
final class SRecordReader {
    private SRecordReader() {
    }

    static List<SRecord> read(Path file) throws IOException {
        List<SRecord> records = new ArrayList<>();
        boolean terminated = false;
        boolean counted = false;
        int dataType = 0;
        try (BufferedReader reader = Files.newBufferedReader(file, StandardCharsets.US_ASCII)) {
            String line;
            int number = 0;
            while ((line = reader.readLine()) != null) {
                number++;
                line = line.trim();
                if (line.isEmpty()) {
                    continue;
                }
                if (terminated || line.length() < 10 || line.length() > 514 ||
                        line.length() % 2 != 0 || line.charAt(0) != 'S') {
                    throw bad(number);
                }
                int type = line.charAt(1) - '0';
                int addressBytes;
                switch (type) {
                    case 0: case 1: case 5: case 9: addressBytes = 2; break;
                    case 2: case 6: case 8: addressBytes = 3; break;
                    case 3: case 7: addressBytes = 4; break;
                    default: throw bad(number);
                }
                byte[] bytes = new byte[(line.length() - 2) / 2];
                int sum = 0;
                for (int i = 0; i < bytes.length; i++) {
                    int hi = Character.digit(line.charAt(2 + 2 * i), 16);
                    int lo = Character.digit(line.charAt(3 + 2 * i), 16);
                    if (hi < 0 || lo < 0) {
                        throw bad(number);
                    }
                    bytes[i] = (byte) (hi * 16 + lo);
                    sum += bytes[i] & 255;
                }
                int count = bytes[0] & 255;
                if (count + 1 != bytes.length || count < addressBytes + 1 || (sum & 255) != 255) {
                    throw bad(number);
                }
                long address = 0;
                for (int i = 1; i <= addressBytes; i++) {
                    address = address << 8 | (bytes[i] & 255);
                }
                int length = count - addressBytes - 1;
                if (type >= 1 && type <= 3) {
                    if (counted || length == 0 || address + length > 0x1_0000_0000L ||
                            (dataType != 0 && dataType != type)) {
                        throw bad(number);
                    }
                    dataType = type;
                    records.add(new SRecord((int) address, Arrays.copyOfRange(bytes, addressBytes + 1, count)));
                } else if (type == 0) {
                    if (!records.isEmpty() || address != 0) {
                        throw bad(number);
                    }
                } else if (type == 5 || type == 6) {
                    if (counted || length != 0 || address != records.size()) {
                        throw bad(number);
                    }
                    counted = true;
                } else {
                    if (length != 0 || dataType == 0 || type != 10 - dataType) {
                        throw bad(number);
                    }
                    terminated = true;
                }
            }
        }
        if (!terminated) {
            throw new IOException("S-record termination record is missing");
        }
        return records;
    }

    private static IOException bad(int line) {
        return new IOException("Invalid S-record at line " + line);
    }
}
