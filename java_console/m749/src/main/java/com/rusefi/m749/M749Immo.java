package com.rusefi.m749;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.function.Consumer;

import static com.rusefi.m749.M749Identification.bytes;

/** Normal paired I865 authentication, independent of UDS seed/key. No key learning. */
final class M749Immo {
    private final byte[] key;
    private final byte[] reference;
    private static final byte[] ACK = bytes(0x11, 0x91, 0, 0, 0, 0, 0x8F, 0x6C);
    private static final String BACKUP_HASH = "ac052cd5cacf0385b4c2de794f6b1ad476e3f9f1badab5b8c54619854e428b39";

    M749Immo(byte[] key, byte[] reference) {
        if (key.length != 16 || reference.length != 8) {
            throw new IllegalArgumentException("Invalid I865 credential sizes");
        }
        this.key = key.clone();
        this.reference = reference.clone();
    }

    static M749Immo load(Path backup) throws IOException {
        if (Files.size(backup) != 0x3F0000) {
            throw new IOException("IMMO requires the validated I865 full backup (0x3F0000 bytes)");
        }
        byte[] data = Files.readAllBytes(backup);
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            StringBuilder hex = new StringBuilder();
            for (byte value : digest) {
                hex.append(String.format("%02x", value & 255));
            }
            if (!BACKUP_HASH.equals(hex.toString())) {
                throw new IOException("IMMO backup does not match the validated paired I865 image");
            }
            return new M749Immo(Arrays.copyOfRange(data, 0x274000, 0x274010),
                    Arrays.copyOfRange(data, 0x4C2B4, 0x4C2BC));
        } catch (NoSuchAlgorithmException e) {
            throw new AssertionError(e);
        } finally {
            Arrays.fill(data, (byte) 0);
        }
    }

    M749PairFile pairFile() throws IOException {
        M749PairFile file = new M749PairFile();
        for (int i = 0; i < key.length; i++) { file.put(i, key[i] & 255); }
        for (int i = 0; i < reference.length; i++) { file.put(i + 16, reference[i] & 255); }
        return file;
    }

    void authorize(RawCanTransport transport, Consumer<String> out) throws IOException, InterruptedException {
        M749Identification.Timing clock = new M749Identification.Timing() {
            public long now() { return System.nanoTime() / 1_000_000; }
            public void pause(long ms) throws InterruptedException { Thread.sleep(ms); }
        };
        byte[] nonce = new byte[8];
        new SecureRandom().nextBytes(nonce);
        authorize(transport, out, clock, nonce, 60_000);
    }

    void authorize(RawCanTransport transport, Consumer<String> out, M749Identification.Timing clock,
                   byte[] peerNonce, long timeout) throws IOException, InterruptedException {
        if (peerNonce.length != 8 || timeout <= 0) {
            throw new IllegalArgumentException("Invalid IMMO nonce/deadline");
        }
        M749FirmwareDetection.Result firmware = M749FirmwareDetection.detect(new UdsClient(transport, clock));
        if (firmware.m749) {
            out.accept(firmware.description + "; no OEM startup authentication or power cycle needed");
            return;
        }
        if (firmware == M749FirmwareDetection.Result.RUSEFI) {
            throw new IOException("rusEFI detected without the M74.9 activation interface; OEM programming entry is not confirmed");
        }
        // Already in the OEM loader: no startup IMMO exchange is needed.
        byte[] session = new UdsClient(transport, clock).exchange(bytes(0x22, 0xF1, 0x86),
                bytes(0x62, 0xF1, 0x86), 2_000);
        if (session.length != 4) {
            throw new IOException("Invalid active-session response before IMMO authentication");
        }
        if (session[3] == 2) {
            out.accept("ECU already reports programming session 02; continuing to loader checks");
            return;
        }
        out.accept("IMMO listener ready. Cycle ECU bench power now; waiting up to 60 seconds for its startup challenge.");
        long deadline = clock.now() + timeout;
        long permissionAt = Long.MAX_VALUE;
        int state = 0;
        byte[] expectedProof = null;
        ChaCha stream = null;
        while (clock.now() < deadline) {
            if (Thread.currentThread().isInterrupted()) {
                throw new InterruptedException();
            }
            if (state == 2 && clock.now() >= permissionAt) {
                transport.sendCan(0x714, stream.crypt(bytes(0x99, 0x11, 0, 0, 0, 0, 0x3C, 0x0F)));
                state = 3;
            }
            RawCanTransport.Frame frame = transport.receiveCan();
            if (frame == null) {
                clock.pause(1);
                continue;
            }
            if (frame.id == 0x714) {
                throw new IOException("Existing IMMO peer traffic on 714; no competing authentication attempted");
            }
            if (frame.id != 0x713) {
                continue;
            }
            if (frame.data.length != 8) {
                throw new IOException("Invalid IMMO frame length");
            }
            if (state == 0) {
                byte[] nonce = new byte[8];
                for (int i = 0; i < 8; i++) {
                    nonce[i] = (byte) (frame.data[i] ^ peerNonce[i]);
                }
                expectedProof = new ChaCha(key, peerNonce, 20).crypt(reference);
                stream = new ChaCha(key, nonce, 20);
                transport.sendCan(0x714, new ChaCha(key, frame.data, 12).crypt(reference));
                transport.sendCan(0x714, peerNonce);
                state = 1;
            } else if (state == 1) {
                if (!MessageDigest.isEqual(expectedProof, frame.data)) {
                    throw new IOException("IMMO ECU proof mismatch; no programming permission sent");
                }
                out.accept("IMMO ECU proof verified");
                permissionAt = clock.now() + 180;
                state = 2;
            } else if (state == 3) {
                if (!MessageDigest.isEqual(ACK, stream.crypt(frame.data))) {
                    throw new IOException("IMMO programming permission acknowledgement mismatch");
                }
                clock.pause(100); // Original application status propagation, validated on the bench.
                out.accept("IMMO programming permission acknowledged; entering the OEM loader");
                return;
            } else {
                throw new IOException("Unexpected IMMO frame before permission request");
            }
        }
        throw new IOException("IMMO authentication timed out; no erase/download requests sent");
    }

    /** Original ChaCha with a 128-bit key, 64-bit nonce/counter and shared RX/TX position. */
    static final class ChaCha {
        private final int[] state = new int[16];
        private final int rounds;
        private final byte[] block = new byte[64];
        private int position = 64;

        ChaCha(byte[] key, byte[] nonce, int rounds) {
            if (key.length != 16 || nonce.length != 8 || (rounds != 12 && rounds != 20)) {
                throw new IllegalArgumentException("Invalid OEM ChaCha parameters");
            }
            this.rounds = rounds;
            byte[] constant = "expand 16-byte k".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
            for (int i = 0; i < 4; i++) {
                state[i] = word(constant, i * 4);
                state[4 + i] = state[8 + i] = word(key, i * 4);
            }
            state[14] = word(nonce, 0);
            state[15] = word(nonce, 4);
        }

        private static int word(byte[] data, int i) {
            return (data[i] & 255) | (data[i + 1] & 255) << 8 |
                    (data[i + 2] & 255) << 16 | (data[i + 3] & 255) << 24;
        }

        byte[] crypt(byte[] input) {
            byte[] output = new byte[input.length];
            for (int i = 0; i < input.length; i++) {
                if (position == 64) {
                    generate();
                }
                output[i] = (byte) (input[i] ^ block[position++]);
            }
            return output;
        }

        private void generate() {
            int[] x = state.clone();
            for (int i = 0; i < rounds; i += 2) {
                quarter(x, 0, 4, 8, 12); quarter(x, 1, 5, 9, 13);
                quarter(x, 2, 6, 10, 14); quarter(x, 3, 7, 11, 15);
                quarter(x, 0, 5, 10, 15); quarter(x, 1, 6, 11, 12);
                quarter(x, 2, 7, 8, 13); quarter(x, 3, 4, 9, 14);
            }
            for (int i = 0; i < 16; i++) {
                int value = x[i] + state[i];
                for (int j = 0; j < 4; j++) {
                    block[4 * i + j] = (byte) (value >>> (8 * j));
                }
            }
            if (++state[12] == 0) {
                state[13]++;
            }
            position = 0;
        }

        private static void quarter(int[] x, int a, int b, int c, int d) {
            x[a] += x[b]; x[d] = Integer.rotateLeft(x[d] ^ x[a], 16);
            x[c] += x[d]; x[b] = Integer.rotateLeft(x[b] ^ x[c], 12);
            x[a] += x[b]; x[d] = Integer.rotateLeft(x[d] ^ x[a], 8);
            x[c] += x[d]; x[b] = Integer.rotateLeft(x[b] ^ x[c], 7);
        }
    }
}
