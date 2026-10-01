package com.rusefi.m749;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.time.LocalDate;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.function.Consumer;

import static com.rusefi.m749.M749Identification.bytes;

/** Supported resident-loader programmer with application-side persistent activation. */
final class M749Uploader {
    interface Connection {
        byte[] exchange(byte[] request, byte[] prefix, long timeout) throws IOException, InterruptedException;
        void pause(long milliseconds) throws InterruptedException;
    }

    private final Connection connection;
    private final Consumer<String> out;
    private final M749ChecksumReader reader;
    private String phase = "preflight";

    M749Uploader(Connection connection, Consumer<String> out) {
        this.connection = connection;
        this.out = out;
        reader = new M749ChecksumReader(connection);
    }

    void upload(M749Image image, boolean verifyBytes) throws IOException, InterruptedException {
        upload(image, verifyBytes, false);
    }

    void checkTarget(M749Image image) throws IOException, InterruptedException {
        upload(image, false, true);
    }

    private void upload(M749Image image, boolean verifyBytes, boolean preflightOnly) throws IOException, InterruptedException {
        image.requireActivationSupport();
        boolean eraseRequested = false;
        try {
            phase = "entering programming session";
            exact(request(bytes(0x10, 2), bytes(0x50, 2)), 6);
            // The application acknowledges before its deferred reset into the loader.
            connection.pause(1_000);
            phase = "loader authentication";
            reader.authenticate();
            byte[] response;
            phase = "checking the resident-loader profile";
            // Compare individual bytes via FF01, where the sum cannot collide.
            // These are compatibility sentinels, not an integrity check of the whole loader.
            M749TargetProfile profile = reader.checkProfile();
            out.accept(profile + " loader compatibility sentinels match; authentication accepted");
            image.requireTarget(profile);

            phase = "activation preflight";
            if (image.domain == M749Image.Domain.CALIBRATION) {
                boolean legacy = reader.matches(M749Image.ACTIVATION_ADDRESS + 7, 1, '1');
                byte[] descriptor = legacy ? M749Image.ACTIVATION_ABI : M749Image.ACTIVATION_ABI_V2;
                for (int i = 0; i < descriptor.length; i++) {
                    reader.verifyByte(M749Image.ACTIVATION_ADDRESS + i, descriptor[i] & 255);
                }
            }
            int journalSlot = checkJournalSpace();
            int softwareCrc = image.domain != M749Image.Domain.CALIBRATION ? image.crc : readWord(0x080FFFFC);
            int calibrationCrc = image.domain == M749Image.Domain.OEM ? image.calibrationCrc :
                    image.domain == M749Image.Domain.CALIBRATION ? image.crc : readWord(0x0807FFFC);
            Map<Integer, byte[]> metadata = journalSlot == 0 ? firstMetadata(softwareCrc, image.domain == M749Image.Domain.OEM) : readMetadata();
            out.accept(String.format("Target %s: calibration starts at 0x%08X, expected CRC %08X",
                    profile, profile.calibrationStart, calibrationCrc));
            if (preflightOnly) {
                out.accept("Target preflight passed; no erase, download, metadata write or reset sent. ECU remains in session 02.");
                return;
            }

            for (M749Image.Range range : image.ranges) {
                byte[] data = range.bytes();
                phase = String.format("erasing 0x%08X + 0x%X", range.address, data.length);
                out.accept(phase);
                // Bound each erase below the ECU's response/session/watchdog windows.
                for (int offset = 0; offset < data.length; offset += M749Image.PAGE) {
                    phase = String.format("erasing page 0x%08X", range.address + offset);
                    byte[] erase = addressed(bytes(0x31, 1, 0xFF, 0, 0x44), range.address + offset, M749Image.PAGE, 0);
                    // A lost reply cannot establish whether the ECU performed the erase.
                    eraseRequested = true;
                    response = request(erase, bytes(0x71, 1, 0xFF, 0));
                    routineSucceeded(response);
                }
                phase = String.format("opening download at 0x%08X", range.address);
                response = request(addressed(bytes(0x34, 0, 0x44), range.address, data.length, 0), bytes(0x74));
                exact(response, 4);
                if (response[1] != 0x20) {
                    throw new IOException("Unsupported RequestDownload length format");
                }
                int maximum = (response[2] & 255) << 8 | response[3] & 255;
                if (maximum < 6 || maximum > 0x802) {
                    throw new IOException("Invalid loader maximum transfer length: " + maximum);
                }
                int blockSize = (maximum - 2) & ~3;
                int counter = 0; // OEM loader starts at zero, not the usual UDS one.
                int nextProgress = 0;
                for (int offset = 0; offset < data.length; offset += blockSize) {
                    int count = Math.min(blockSize, data.length - offset);
                    phase = String.format("writing 0x%08X (counter %02X)", range.address + offset, counter);
                    byte[] block = new byte[count + 2];
                    block[0] = 0x36;
                    block[1] = (byte) counter;
                    System.arraycopy(data, offset, block, 2, count);
                    exact(request(block, bytes(0x76, counter)), 2);
                    counter = (counter + 1) & 255;
                    int percent = (offset + count) * 100 / data.length;
                    if (percent >= nextProgress) {
                        out.accept(String.format("Transfer 0x%08X: %d/%d bytes (%d%%)",
                                range.address, offset + count, data.length, percent));
                        nextProgress = percent + 10;
                    }
                }
                phase = "closing download";
                exact(request(bytes(0x37), bytes(0x77)), 1);
                // Never interpret TransferExit as validation; it only closes the transfer.
                int checkSize = verifyBytes ? 1 : 2048;
                for (int offset = 0; offset < data.length; offset += checkSize) {
                    int count = Math.min(checkSize, data.length - offset);
                    phase = String.format("verifying 0x%08X", range.address + offset);
                    int sum = 0;
                    for (int i = offset; i < offset + count; i++) {
                        sum = (sum + (data[i] & 255)) & 0xFFFF;
                    }
                    checksum(range.address + offset, count, sum);
                    if (offset % 16384 == 0) {
                        out.accept(String.format("Verify 0x%08X: %d/%d bytes", range.address, offset, data.length));
                    }
                }
                out.accept(String.format("Verified 0x%08X + 0x%X using %s", range.address, data.length,
                        verifyBytes ? "individual-byte comparisons" : "16-bit additive block checksums"));
            }
            phase = "appending programming metadata";
            // The OEM record transaction arms the loader's return-token reset.
            // Preserve opaque bytes; do not invent tester identity or overwrite ECU identity DIDs.
            for (Map.Entry<Integer, byte[]> entry : metadata.entrySet()) {
                int did = entry.getKey();
                byte[] value = entry.getValue();
                byte[] write = Arrays.copyOf(bytes(0x2E, did >>> 8, did), value.length + 3);
                System.arraycopy(value, 0, write, 3, value.length);
                exact(request(write, bytes(0x6E, did >>> 8, did)), 3);
            }
            Map<Integer, byte[]> restored = readMetadata();
            for (int did : metadata.keySet()) {
                if (!Arrays.equals(metadata.get(did), restored.get(did))) {
                    throw new IOException(String.format("Programming metadata %04X changed", did));
                }
            }
            phase = "activating and checking the application";
            exact(request(bytes(0x11, 1), bytes(0x51, 1)), 2);
            if (image.domain == M749Image.Domain.OEM) {
                awaitOemApplication();
                out.accept("Upload complete: OEM software/calibration transferred and verified; application session 01 confirmed after reset.");
                out.accept("OEM activation CRC/marker DIDs are unavailable. Cold power-cycle validation remains a separate check.");
                return;
            }
            awaitApplication();
            verifyApplication(softwareCrc, calibrationCrc);
            phase = "confirming boot without the SRAM return token";
            exact(request(bytes(0x11, 1), bytes(0x51, 1)), 2);
            awaitApplication();
            verifyApplication(softwareCrc, calibrationCrc);
            out.accept("Upload complete: application reports valid software/calibration/loader CRCs and normal boot marker after reset.");
        } catch (IOException e) {
            String hint = phase.equals("entering programming session") &&
                    e instanceof UdsClient.NegativeResponse && ((UdsClient.NegativeResponse) e).code == 0x22
                    ? " Programming entry conditions were not met; check the installed firmware and ECU operating state."
                    : "";
            throw new IOException("Stopped while " + phase + ": " + e.getMessage() +
                    (eraseRequested
                            ? ". An erase request was sent; flash/activation may be incomplete."
                            : ". No flash erase or programming requests were sent by this upload.") +
                    " No recovery reset was sent." + hint, e);
        }
    }

    private void checksum(int address, int length, int sum) throws IOException, InterruptedException {
        if (!checksumMatches(address, length, sum)) {
            throw new IOException(String.format("Checksum mismatch at 0x%08X + 0x%X", address, length));
        }
    }

    private boolean checksumMatches(int address, int length, int sum) throws IOException, InterruptedException {
        return reader.matches(address, length, sum);
    }

    private int readWord(int address) throws IOException, InterruptedException {
        return reader.readWord(address);
    }

    private Map<Integer, byte[]> readMetadata() throws IOException, InterruptedException {
        int[][] limits = {{0xF188, 1, 48}, {0xF189, 1, 48}, {0xF194, 1, 40},
                {0xF195, 8, 8}, {0xF198, 1, 100}, {0xF199, 8, 8}};
        Map<Integer, byte[]> result = new LinkedHashMap<>();
        for (int[] limit : limits) {
            byte[] response = request(bytes(0x22, limit[0] >>> 8, limit[0]), bytes(0x62, limit[0] >>> 8, limit[0]));
            int length = response.length - 3;
            if (length < limit[1] || length > limit[2]) {
                throw new IOException(String.format("Cannot preserve loader metadata %04X: length %d", limit[0], length));
            }
            result.put(limit[0], Arrays.copyOfRange(response, 3, response.length));
        }
        return result;
    }

    private int checkJournalSpace() throws IOException, InterruptedException {
        // Supported loaders select the first record with bytes 0 and 8 FF.
        // Region 6 is an append-only 16-record page; the loader does not erase it.
        for (int slot = 0; slot < 16; slot++) {
            int address = 0x0824E000 + slot * 256;
            if (checksumMatches(address, 1, 255) && checksumMatches(address + 8, 1, 255)) {
                // 256 * 255 = 65280: maximum sum, with no modulo wrap, proves all FF.
                if (!checksumMatches(address, 256, 65280)) {
                    throw new IOException("Next programming-history slot is partially written");
                }
                out.accept("Programming-history slot " + slot + "/15 is erased");
                return slot;
            }
        }
        throw new IOException("OEM programming-history page is full; refusing to erase application or compact protected NVM");
    }

    private Map<Integer, byte[]> firstMetadata(int softwareCrc, boolean oem) {
        String date = LocalDate.now(ZoneOffset.UTC).format(DateTimeFormatter.BASIC_ISO_DATE);
        Map<Integer, byte[]> records = new LinkedHashMap<>();
        records.put(0xF188, (oem ? "M74.9 OEM restore" : "rusEFI m74_9").getBytes(StandardCharsets.US_ASCII));
        records.put(0xF189, String.format("M749-%08X", softwareCrc).getBytes(StandardCharsets.US_ASCII));
        records.put(0xF194, (oem ? "M74.9 OEM restore" : "rusEFI m74_9").getBytes(StandardCharsets.US_ASCII));
        records.put(0xF195, date.getBytes(StandardCharsets.US_ASCII));
        records.put(0xF198, "fw-m74.9 CLI".getBytes(StandardCharsets.US_ASCII));
        records.put(0xF199, date.getBytes(StandardCharsets.US_ASCII));
        out.accept("Programming history is empty: prepared a tool record with the software CRC and today's UTC date");
        return records;
    }

    private void awaitApplication() throws IOException, InterruptedException {
        IOException last = null;
        connection.pause(1_000);
        for (int attempt = 0; attempt < 10; attempt++) {
            try {
                if (applicationWord(0xF1A0) == 0x4D740101) {
                    return;
                }
            } catch (IOException e) {
                last = e;
            }
            connection.pause(250);
        }
        throw new IOException("Application activation status did not become ready", last);
    }

    private void awaitOemApplication() throws IOException, InterruptedException {
        IOException last = null;
        connection.pause(1_000);
        for (int attempt = 0; attempt < 10; attempt++) {
            try {
                byte[] reply = connection.exchange(bytes(0x22, 0xF1, 0x86), bytes(0x62, 0xF1, 0x86), 3_000);
                exact(reply, 4);
                if (reply[3] == 1) { return; }
            } catch (IOException e) { last = e; }
            connection.pause(250);
        }
        throw new IOException("OEM application session 01 was not confirmed after reset", last);
    }

    private int applicationWord(int did) throws IOException, InterruptedException {
        byte[] response = connection.exchange(bytes(0x22, did >>> 8, did), bytes(0x62, did >>> 8, did), 3_000);
        exact(response, 7);
        return (response[3] & 255) << 24 | (response[4] & 255) << 16 | (response[5] & 255) << 8 | response[6] & 255;
    }

    private void verifyApplication(int software, int calibration) throws IOException, InterruptedException {
        if (applicationWord(0xF1A1) != software || applicationWord(0xF1A2) != calibration ||
                applicationWord(0xF1A3) != 0x43A0C212) {
            throw new IOException("Application CRC or persistent boot marker mismatch");
        }
    }

    private byte[] request(byte[] request, byte[] prefix) throws IOException, InterruptedException {
        return connection.exchange(request, prefix, 60_000);
    }

    private static void routineSucceeded(byte[] response) throws IOException {
        exact(response, 5);
        if (response[4] != 0) {
            throw new IOException("Loader routine failed (result " + (response[4] & 255) + ")");
        }
    }

    private static void exact(byte[] response, int length) throws IOException {
        if (response.length != length) {
            throw new IOException("Unexpected UDS response length: " + response.length);
        }
    }

    private static byte[] addressed(byte[] prefix, int address, int length, int tail) {
        byte[] result = Arrays.copyOf(prefix, prefix.length + 8 + tail);
        for (int i = 0; i < 4; i++) {
            result[prefix.length + i] = (byte) (address >>> (24 - i * 8));
            result[prefix.length + 4 + i] = (byte) (length >>> (24 - i * 8));
        }
        return result;
    }
}
