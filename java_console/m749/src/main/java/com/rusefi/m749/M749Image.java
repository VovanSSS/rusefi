package com.rusefi.m749;

import com.rusefi.libopenblt.file.SrecParser;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import java.util.function.Consumer;

/** Immutable, complete CRC domain. Validation finishes before any device access. */
final class M749Image {
    enum Domain { SOFTWARE, CALIBRATION, OEM }
    static final int START = 0x08001000, CAL = 0x08060000, SECOND = 0x08080000, END = 0x08100000;
    static final int PAGE = 4096;
    static final int ACTIVATION_ADDRESS = 0x0805FFE0;
    static final byte[] ACTIVATION_ABI = new byte[]{0x4D, 0x37, 0x34, 0x39, 0x41, 0x43, 0x54, 0x31,
            (byte) 0x94, (byte) 0xB8, (byte) 0xB6, (byte) 0xD7, 1, 0, 0, 0};
    static final byte[] ACTIVATION_ABI_V2 = new byte[]{0x4D, 0x37, 0x34, 0x39, 0x41, 0x43, 0x54, 0x32,
            2, 0, 0, 0, 1, 0, 0, 0,
            (byte) 0x94, (byte) 0xB8, (byte) 0xB6, (byte) 0xD7, 0, 0, 6, 8,
            (byte) 0xD9, 0x6C, 0x25, 0x4F, 0, (byte) 0x90, 6, 8};
    static final class Range {
        final int address;
        private final byte[] data;
        Range(int address, byte[] data) { this.address = address; this.data = data.clone(); }
        int length() { return data.length; }
        byte[] bytes() { return data.clone(); }
    }
    final Domain domain;
    final List<Range> ranges;
    final int crc;
    final M749TargetProfile oemProfile;
    final int calibrationCrc;

    private M749Image(Domain domain, List<Range> ranges, int crc) {
        this(domain, ranges, crc, null, 0);
    }

    private M749Image(Domain domain, List<Range> ranges, int crc, M749TargetProfile profile, int calibrationCrc) {
        this.domain = domain;
        this.ranges = Collections.unmodifiableList(ranges);
        this.crc = crc;
        this.oemProfile = profile;
        this.calibrationCrc = calibrationCrc;
    }

    static M749Image load(Path file, Domain domain) throws IOException {
        if (Files.size(file) > 8 * 1024 * 1024) {
            throw new IOException("Image text exceeds 8 MiB");
        }
        String name = file.getFileName().toString().toLowerCase(Locale.ROOT);
        if (name.endsWith(".bin")) {
            if (domain != Domain.SOFTWARE) { throw new IOException("OEM BIN restores both software and calibration; omit --calibration"); }
            if (Files.size(file) != M749RamHelper.SIZE) { throw new IOException("OEM BIN must be a complete 4032 KiB main-flash backup"); }
            return oem(Files.readAllBytes(file));
        }
        List<SrecParser.SRecord> records;
        if (name.endsWith(".srec") || name.endsWith(".s19") || name.endsWith(".s28") || name.endsWith(".s37")) {
            records = SRecordReader.read(file);
        } else if (name.endsWith(".hex")) {
            records = IntelHexReader.read(file);
        } else {
            throw new IOException("Expected rusEFI .hex/.srec or a supported OEM full-flash .bin");
        }
        return validate(records, domain);
    }

    static M749Image validate(List<SrecParser.SRecord> input, Domain domain) throws IOException {
        if (domain == Domain.OEM) { throw new IOException("OEM restore requires a full BIN backup"); }
        List<SrecParser.SRecord> sorted = new ArrayList<>(input);
        sorted.sort(Comparator.comparingLong(r -> Integer.toUnsignedLong(r.address)));
        List<Range> ranges = new ArrayList<>();
        ByteArrayOutputStream run = new ByteArrayOutputStream();
        int start = 0;
        long end = -1;
        for (SrecParser.SRecord record : sorted) {
            long address = Integer.toUnsignedLong(record.address);
            long last = address + record.data.length;
            if (record.data.length == 0 || address < START || last > END ||
                    (domain == Domain.CALIBRATION ? address < CAL || last > SECOND :
                            !(last <= CAL || address >= SECOND))) {
                throw new IOException(String.format("Forbidden or mixed-domain range at 0x%08X", address));
            }
            if (address < end) {
                throw new IOException("Overlapping image records");
            }
            if (address != end) {
                if (run.size() > 0) { ranges.add(new Range(start, run.toByteArray())); }
                run.reset();
                start = record.address;
            }
            run.write(record.data, 0, record.data.length);
            end = last;
        }
        if (run.size() > 0) { ranges.add(new Range(start, run.toByteArray())); }
        int[][] expected = domain == Domain.SOFTWARE ?
                new int[][]{{START, CAL - START}, {SECOND, END - SECOND}} :
                new int[][]{{CAL, SECOND - CAL}};
        if (ranges.size() != expected.length) { throw new IOException("Incomplete CRC domain: missing ranges or bytes"); }
        for (int i = 0; i < expected.length; i++) {
            Range range = ranges.get(i);
            if (range.address != expected[i][0] || range.length() != expected[i][1]) {
                throw new IOException("Incomplete CRC domain: every declared page byte, including padding, is required");
            }
        }
        if (domain == Domain.SOFTWARE && (littleEndian(ranges.get(0).data, 0) != 0x20020000 ||
                littleEndian(ranges.get(0).data, 4) != SECOND + 1)) {
            throw new IOException("Invalid M74.9 initial stack/reset vectors");
        }
        int crc = -1;
        for (int i = 0; i < ranges.size(); i++) {
            byte[] data = ranges.get(i).data;
            crc = crc32(data, i == ranges.size() - 1 ? data.length - 4 : data.length, crc);
        }
        byte[] tail = ranges.get(ranges.size() - 1).data;
        if (crc != littleEndian(tail, tail.length - 4)) {
            throw new IOException("CRC-32/MPEG-2 mismatch (including erased padding)");
        }
        return new M749Image(domain, ranges, crc);
    }

    static M749Image oem(byte[] data) throws IOException {
        if (data.length != M749RamHelper.SIZE) { throw new IOException("OEM BIN must be a complete 4032 KiB main-flash backup"); }
        int boot = littleEndian(data, 0x22DFFC);
        M749TargetProfile profile = null;
        for (M749TargetProfile candidate : M749TargetProfile.values()) {
            if (candidate.bootCrc == boot) { profile = candidate; }
        }
        if (profile == null) { throw new IOException("Unsupported OEM BIN loader profile; only I812/I865 full backups are supported"); }
        int bootCrc = crc32(Arrays.copyOfRange(data, 0, 0x1000), 0x1000, -1);
        bootCrc = crc32(Arrays.copyOfRange(data, 0x201000, 0x22DFFC), 0x2CFFC, bootCrc);
        if (bootCrc != boot) { throw new IOException("OEM BIN loader CRC mismatch"); }
        int split = profile.calibrationStart - M749RamHelper.BASE;
        int software = crc32(Arrays.copyOfRange(data, 0x1000, split), split - 0x1000, -1);
        software = crc32(Arrays.copyOfRange(data, 0x80000, 0xFFFFC), 0x7FFFC, software);
        int calibration = crc32(Arrays.copyOfRange(data, split, 0x7FFFC), 0x7FFFC - split, -1);
        if (software != littleEndian(data, 0xFFFFC) || calibration != littleEndian(data, 0x7FFFC)) {
            throw new IOException("OEM BIN software/calibration CRC mismatch");
        }
        // The I812 OEM application vector stores zero in its first word.
        int expectedStack = profile == M749TargetProfile.I812 ? 0 : 0x20020000;
        if (littleEndian(data, 0x1000) != expectedStack || littleEndian(data, 0x1004) != SECOND + 1) {
            throw new IOException("Invalid OEM application vectors");
        }
        if (descriptorMatches(data, ACTIVATION_ADDRESS - M749RamHelper.BASE, ACTIVATION_ABI) ||
                descriptorMatches(data, ACTIVATION_ADDRESS - M749RamHelper.BASE, ACTIVATION_ABI_V2)) {
            throw new IOException("BIN contains rusEFI; use its addressed HEX/SREC update instead");
        }
        return new M749Image(Domain.OEM, List.of(new Range(START, Arrays.copyOfRange(data, 0x1000, 0x100000))),
                software, profile, calibration);
    }

    static int crc32(byte[] data, int length, int crc) {
        for (int i = 0; i < length; i++) {
            crc ^= (data[i] & 255) << 24;
            for (int bit = 0; bit < 8; bit++) {
                crc = (crc << 1) ^ (crc < 0 ? 0x04C11DB7 : 0);
            }
        }
        return crc;
    }

    void requireActivationSupport() throws IOException {
        if (domain == Domain.SOFTWARE) {
            byte[] first = ranges.get(0).data;
            int offset = ACTIVATION_ADDRESS - START;
            if (!descriptorMatches(first, offset, ACTIVATION_ABI) && !descriptorMatches(first, offset, ACTIVATION_ABI_V2)) {
                throw new IOException("Software lacks a supported M749ACT1/M749ACT2 persistent-activation ABI; rebuild first");
            }
        }
    }

    private static boolean descriptorMatches(byte[] data, int offset, byte[] descriptor) {
        return Arrays.equals(descriptor, Arrays.copyOfRange(data, offset, offset + descriptor.length));
    }

    void requireTarget(M749TargetProfile profile) throws IOException {
        if (domain == Domain.OEM) {
            if (oemProfile != profile) { throw new IOException("OEM BIN profile " + oemProfile + " does not match connected " + profile + " loader"); }
            return;
        }
        requireActivationSupport();
        if (domain == Domain.CALIBRATION && profile.calibrationStart != CAL) {
            throw new IOException("This calibration payload uses the I865 layout; I812 software updates preserve its calibration");
        }
        if (domain == Domain.SOFTWARE && profile == M749TargetProfile.I812 &&
                !descriptorMatches(ranges.get(0).data, ACTIVATION_ADDRESS - START, ACTIVATION_ABI_V2)) {
            throw new IOException("I812 requires M749ACT2 software; the M749ACT1 image supports only I865");
        }
    }

    private static int littleEndian(byte[] data, int i) {
        return (data[i] & 255) | (data[i + 1] & 255) << 8 | (data[i + 2] & 255) << 16 | data[i + 3] << 24;
    }

    void describe(Consumer<String> out) {
        if (domain == Domain.OEM) {
            out.accept(String.format("OEM %s restore: software CRC %08X, calibration CRC %08X", oemProfile, crc, calibrationCrc));
            out.accept("Writes application and calibration at 08001000..080FFFFF only; preserves loader, identity, pairing and storage.");
            out.accept("OEM completion checks application session after reset; custom rusEFI activation DIDs are unavailable.");
        }
        out.accept(String.format("Validated %s CRC domain: CRC32/MPEG-2 %08X", domain, crc));
        for (Range range : ranges) {
            out.accept(String.format("0x%08X-0x%08X: %d bytes, %d pages, %d blocks at maximum 2048 bytes",
                    range.address, range.address + range.length() - 1, range.length(),
                    range.length() / PAGE, (range.length() + 2047) / 2048));
        }
        out.accept("Plan: programming session -> loader security -> erase each range -> 34/36/37 -> verify -> activation.");
        out.accept(domain == Domain.OEM ? "Activation: preserve loader metadata -> reset -> OEM application session check. Cold boot still requires validation."
                : "Activation: preserve loader metadata -> reset -> application CRC/marker checks -> reset and recheck.");
    }
}
