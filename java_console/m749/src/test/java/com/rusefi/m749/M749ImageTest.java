package com.rusefi.m749;

import com.rusefi.libopenblt.file.SrecParser.SRecord;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

class M749ImageTest {
    @TempDir Path directory;

    static List<SRecord> records(M749Image.Domain domain) {
        return records(domain, M749Image.ACTIVATION_ABI);
    }

    static List<SRecord> records(M749Image.Domain domain, byte[] descriptor) {
        List<SRecord> records = new ArrayList<>();
        if (domain == M749Image.Domain.SOFTWARE) {
            byte[] first = new byte[M749Image.CAL - M749Image.START];
            Arrays.fill(first, (byte) 0xFF);
            word(first, 0, 0x20020000);
            word(first, 4, 0x08080001);
            System.arraycopy(descriptor, 0, first, M749Image.ACTIVATION_ADDRESS - M749Image.START, descriptor.length);
            records.add(new SRecord(M749Image.START, first));
            records.add(new SRecord(M749Image.SECOND, new byte[M749Image.END - M749Image.SECOND]));
        } else {
            records.add(new SRecord(M749Image.CAL, new byte[M749Image.SECOND - M749Image.CAL]));
        }
        int crc = -1;
        for (int i = 0; i < records.size(); i++) {
            byte[] data = records.get(i).data;
            crc = M749Image.crc32(data, data.length - (i + 1 == records.size() ? 4 : 0), crc);
        }
        byte[] tail = records.get(records.size() - 1).data;
        word(tail, tail.length - 4, crc);
        return records;
    }

    static void word(byte[] data, int offset, int value) {
        for (int i = 0; i < 4; i++) {
            data[offset + i] = (byte) (value >>> (i * 8));
        }
    }

    @Test void crcVectorRangesAndDefensiveCopies() throws Exception {
        assertEquals(0x0376E6E7, M749Image.crc32("123456789".getBytes(java.nio.charset.StandardCharsets.US_ASCII), 9, -1));
        List<SRecord> input = records(M749Image.Domain.SOFTWARE);
        M749Image image = M749Image.validate(input, M749Image.Domain.SOFTWARE);
        image.requireActivationSupport();
        input.get(0).data[0] = 1;
        byte[] copy = image.ranges.get(0).bytes();
        assertEquals(0, copy[0]);
        copy[0] = 2;
        assertEquals(0, image.ranges.get(0).bytes()[0]);
        assertEquals(2, image.ranges.size());
        assertEquals(M749Image.SECOND, image.ranges.get(1).address);
    }

    @Test void rejectsCorruptionHolesOverlapsAndProtectedOrMixedRanges() {
        List<SRecord> input = records(M749Image.Domain.SOFTWARE);
        input.get(1).data[100] ^= 1;
        assertThrows(IOException.class, () -> M749Image.validate(input, M749Image.Domain.SOFTWARE));
        for (int address : new int[]{0x08000000, M749Image.CAL, 0x08200000, 0x20000000, -1}) {
            assertThrows(IOException.class, () -> M749Image.validate(Arrays.asList(new SRecord(address, new byte[]{1})), M749Image.Domain.SOFTWARE));
        }
        List<SRecord> cal = records(M749Image.Domain.CALIBRATION);
        assertDoesNotThrow(() -> M749Image.validate(cal, M749Image.Domain.CALIBRATION));
        assertThrows(IOException.class, () -> M749Image.validate(cal, M749Image.Domain.SOFTWARE));
        cal.add(new SRecord(M749Image.CAL + 1, new byte[]{0}));
        assertThrows(IOException.class, () -> M749Image.validate(cal, M749Image.Domain.CALIBRATION));
        assertThrows(IOException.class, () -> M749Image.validate(List.of(new SRecord(M749Image.CAL, new byte[4096])), M749Image.Domain.CALIBRATION));
    }

    @Test void oldImageCanBeParsedButCannotBeActivated() throws Exception {
        List<SRecord> input = records(M749Image.Domain.SOFTWARE);
        Arrays.fill(input.get(0).data, M749Image.ACTIVATION_ADDRESS - M749Image.START,
                M749Image.ACTIVATION_ADDRESS - M749Image.START + 16, (byte) 0xFF);
        byte[] tail = input.get(1).data;
        word(tail, tail.length - 4, M749Image.crc32(tail, tail.length - 4,
                M749Image.crc32(input.get(0).data, input.get(0).data.length, -1)));
        M749Image image = M749Image.validate(input, M749Image.Domain.SOFTWARE);
        assertThrows(IOException.class, image::requireActivationSupport);
    }

    @Test void intelHexChecksMetadataChecksumAndEof() throws Exception {
        Path file = directory.resolve("image with spaces.hex");
        String valid = ":020000040806EC\n:0400000001020304F2\n:00000001FF\n";
        Files.writeString(file, valid);
        List<SRecord> result = IntelHexReader.read(file);
        assertEquals(0x08060000, result.get(0).address);
        assertArrayEquals(new byte[]{1, 2, 3, 4}, result.get(0).data);
        for (String bad : new String[]{valid.replace("F2", "F3"), valid.replace(":00000001FF\n", ""),
                valid + ":00000001FF\n", ":00000001GG\n", ":00000101FE\n"}) {
            Files.writeString(file, bad);
            assertThrows(IOException.class, () -> IntelHexReader.read(file));
        }
    }

    @Test void srecordsRequireCorrectCountTypeAndTermination() throws Exception {
        Path file = directory.resolve("file.srec");
        String valid = "S3090806000001020304DE\nS5030001FB\nS70500000000FA\n";
        Files.writeString(file, valid);
        assertEquals(0x08060000, SRecordReader.read(file).get(0).address);
        for (String bad : new String[]{valid.replace("DE", "DF"), valid.replace("0001FB", "0002FA"),
                valid.replace("S70500000000FA\n", ""), valid + "S70500000000FA\n",
                valid.replace("S70500000000FA", "S9030000FC")}) {
            Files.writeString(file, bad);
            assertThrows(IOException.class, () -> SRecordReader.read(file));
        }
    }
}
