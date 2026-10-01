package com.rusefi.m749;

import com.rusefi.uds.M74_9_SeedKeyCalculator;
import java.io.IOException;
import java.io.InputStream;
import java.util.Arrays;
import java.util.function.Consumer;
import static com.rusefi.m749.M749Identification.bytes;

/** Session-60 RAM bootstrap and the bundled helper's block-read protocol. */
final class M749RamHelper {
    static final int BASE = 0x08000000;
    static final int SIZE = 0x003F0000;
    static final int MAX_BLOCK = 4080;
    static final String SHA256 = "21604ac8be0a6ca27714f6bb49c9eadcc30b1e210fd13120c88e664df26e1275";
    private final M749Uploader.Connection connection;
    private final int receiveStmin;

    M749RamHelper(M749Uploader.Connection connection) { this(connection, 127); }

    M749RamHelper(M749Uploader.Connection connection, int receiveStmin) {
        if (receiveStmin < 0 || receiveStmin > 127) { throw new IllegalArgumentException("Invalid STmin"); }
        this.connection = connection;
        this.receiveStmin = receiveStmin;
    }

    static byte[] load() throws IOException {
        try (InputStream in = M749RamHelper.class.getResourceAsStream("/uds_helper_2001ba00.bin")) {
            if (in == null) { throw new IOException("Bundled RAM helper is missing"); }
            byte[] data = in.readAllBytes();
            if (data.length != 6656 || !FlashReadFile.hash(data).equals(SHA256)) {
                throw new IOException("Bundled RAM helper size/SHA-256 mismatch");
            }
            return data;
        }
    }

    String start(byte[] helper, boolean alreadyRunning, Consumer<String> out) throws IOException, InterruptedException {
        if (helper.length != 6656 || !FlashReadFile.hash(helper).equals(SHA256)) {
            throw new IOException("RAM helper integrity check failed");
        }
        if (!alreadyRunning) {
            out.accept("Entering application session 60 for RAM helper upload");
            exact(bytes(0x10, 0x60), bytes(0x50, 0x60), 6);
            out.accept("Application session 60 accepted");
            byte[] seedReply = exact(bytes(0x27, 1, 0), bytes(0x67, 1), 6);
            int seed = (seedReply[2] & 255) << 24 | (seedReply[3] & 255) << 16 |
                    (seedReply[4] & 255) << 8 | seedReply[5] & 255;
            if (seed != 0) {
                int key = M74_9_SeedKeyCalculator.Uds_Security_CalcKey(M74_9_SeedKeyCalculator.SECRET, seed, 0);
                exact(bytes(0x27, 2, key >>> 24, key >>> 16, key >>> 8, key), bytes(0x67, 2), 2);
            }
            out.accept("Application security accepted");
            prepare(bytes(0x85, 2), bytes(0xC5, 2), "DTC control", out);
            prepare(bytes(0x28, 1, 1), bytes(0x68, 1), "communication control", out);
            for (int offset = 0; offset < helper.length; offset += 512) {
                int address = 0x2001BA00 + offset;
                byte[] request = Arrays.copyOf(bytes(0x3D, 0x24, address >>> 24, address >>> 16,
                        address >>> 8, address, 2, 0), 520);
                System.arraycopy(helper, offset, request, 8, 512);
                byte[] expected = Arrays.copyOf(request, 8);
                expected[0] = 0x7D;
                exact(request, expected, expected.length);
                out.accept(String.format("RAM helper upload: %d/%d bytes acknowledged", offset + 512, helper.length));
            }
            byte[] ready = bytes(0x71, 1, 0xF0, 0, 0x7C, 0, 0);
            exact(bytes(0x31, 1, 0xF0, 0, 0x20, 1, 0xBA, 0), ready, ready.length);
            out.accept("RAM helper running; flash erase/program commands are not used");
        } else {
            exact(bytes(0x3E), bytes(0x7E), 1);
            // Verify the actual running code before trusting a pre-existing helper.
            for (int offset = 0; offset < helper.length; offset += 512) {
                int size = Math.min(512, helper.length - offset);
                if (!Arrays.equals(Arrays.copyOfRange(helper, offset, offset + size),
                        readAddress(0x2001BA00 + offset, size))) {
                    throw new IOException("Running RAM helper differs from the bundled helper");
                }
            }
        }
        byte[] identity = exact(bytes(0x1A, 0xC1), bytes(0x5A, 0xC1), 10);
        String id = M749Identification.hex(Arrays.copyOfRange(identity, 2, 10));
        out.accept("Helper CPU identification: " + id);
        return id;
    }

    void probe() throws IOException, InterruptedException {
        // Populated boot/application samples distinguish a working read path
        // from uniform protected/invalid reads before creating a trusted backup.
        for (int address : new int[]{BASE, BASE + 0x80000}) {
            byte[] data = read(address, 32);
            if (!Arrays.equals(data, read(address, 32))) { throw new IOException("Read probe is unstable"); }
            boolean zero = true, erased = true;
            for (byte value : data) { zero &= value == 0; erased &= value == (byte) 255; }
            if (zero || erased) {
                throw new IOException(String.format("Read probe at %08X is uniformly %s; flash access is not established",
                        address, zero ? "00" : "FF"));
            }
        }
    }

    byte[] read(int address, int length) throws IOException, InterruptedException {
        requireRange(address, length);
        if (length > MAX_BLOCK) { throw new IllegalArgumentException("Maximum read block is 4080 bytes"); }
        return readAddress(address, length);
    }

    private byte[] readAddress(int address, int length) throws IOException, InterruptedException {
        byte[] request = bytes(0x23, address >>> 24, address >>> 16, address >>> 8, address, length >>> 8, length);
        // Match SID first, then reject a wrong address rather than ignoring it.
        long timeout = 5_000L + ((length + 5 + 6) / 7) * receiveStmin;
        byte[] reply = exact(request, bytes(0x63), length + 5, timeout);
        if (!Arrays.equals(Arrays.copyOfRange(request, 1, 5), Arrays.copyOfRange(reply, 1, 5))) {
            throw new IOException("Read response address does not match the request");
        }
        return Arrays.copyOfRange(reply, 5, reply.length);
    }

    void reset() throws IOException, InterruptedException { exact(bytes(0x11, 1), bytes(0x51, 1), 2); }

    private void prepare(byte[] request, byte[] expected, String name, Consumer<String> out)
            throws IOException, InterruptedException {
        try {
            exact(request, expected, expected.length);
            out.accept(String.format("Preparation SID %02X (%s) accepted", request[0] & 255, name));
        } catch (UdsClient.NegativeResponse e) {
            // Some applications omit DTC/communication control in session 60.
            // Only explicit service-unavailable responses permit the RAM upload
            // to be attempted; admission, security and every write still must pass.
            if (e.code != 0x11 && e.code != 0x7F) { throw e; }
            out.accept(String.format("Optional preparation SID %02X unavailable (NRC %02X); continuing to RAM upload",
                    request[0] & 255, e.code));
        }
    }

    private byte[] exact(byte[] request, byte[] prefix, int length) throws IOException, InterruptedException {
        return exact(request, prefix, length, 15_000);
    }

    private byte[] exact(byte[] request, byte[] prefix, int length, long timeout) throws IOException, InterruptedException {
        if (Thread.currentThread().isInterrupted()) { throw new InterruptedException(); }
        byte[] reply = connection.exchange(request, prefix, timeout);
        if (reply.length != length || !UdsClient.startsWith(reply, prefix)) {
            throw new IOException(String.format("Unexpected response to SID %02X", request[0] & 255));
        }
        return reply;
    }

    static void requireRange(int address, int length) {
        if (address < BASE || length <= 0 || (long) address + length > (long) BASE + SIZE) {
            throw new IllegalArgumentException("Read range must be within 0x08000000..0x083EFFFF");
        }
    }
}
