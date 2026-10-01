package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Queue;

import static org.junit.jupiter.api.Assertions.*;

class M749ImmoTest {
    @TempDir Path directory;
    // Fixed test inputs and expected proof, permission and stream values.
    private static final byte[] KEY = hex("000102030405060708090a0b0c0d0e0f");
    private static final byte[] NONCE = hex("08090a0b0c0d0e0f");
    private static final byte[] PEER = hex("1011121314151617");
    private static final byte[] REFERENCE = hex("a0a1a2a3a4a5a6a7");
    private static final byte[] PROOF = hex("731824de52721557");
    private static final byte[] ECU_PROOF = hex("4a388f0028eabd93");
    private static final byte[] PERMISSION = hex("062b5ad6d6d4f202");
    private static final byte[] ACK = hex("9c01182f93662828");

    static byte[] hex(String s) {
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) {
            b[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        }
        return b;
    }

    static final class Clock implements M749Identification.Timing {
        long millis;
        public long now() { return millis; }
        public void pause(long ms) { millis += ms; }
    }

    static final class Bus implements RawCanTransport {
        final Queue<Frame> incoming = new ArrayDeque<>();
        final List<Frame> sent = new ArrayList<>();
        final Clock clock = new Clock();
        boolean loader, rusefi, genericRusefi, missingNonce, competingPeer, badProof, badAck, missingProof, badLength;
        int immoFrames;
        long proofAt;

        public void sendCan(int id, byte[] data) {
            sent.add(new Frame(id, data));
            if (id == 0x7E0) {
                if (rusefi || genericRusefi) {
                    if (genericRusefi && Arrays.equals(hex("0322f1a4cccccccc"), data)) {
                        incoming.add(new Frame(0x7E8, hex("0762f1a472454649")));
                    } else if (rusefi && Arrays.equals(hex("0322f1a0cccccccc"), data)) {
                        incoming.add(new Frame(0x7E8, hex("0762f1a04d740101")));
                    }
                    return;
                }
                if (data[3] == (byte) 0xA4 || data[3] == (byte) 0xA0) {
                    incoming.add(new Frame(0x7E8, hex("037f223100000000")));
                    return;
                }
                assertArrayEquals(hex("0322f186cccccccc"), data);
                incoming.add(new Frame(0x7E8, hex(loader ? "0462f18602cccccc" : "0462f18601cccccc")));
                if (competingPeer) {
                    incoming.add(new Frame(0x714, new byte[8]));
                }
                if (!loader && !missingNonce) {
                    incoming.add(new Frame(0x713, badLength ? new byte[7] : NONCE));
                }
            } else {
                assertEquals(0x714, id, "No erase, download or reset traffic during authentication");
                switch (++immoFrames) {
                    case 1: assertArrayEquals(PROOF, data); break;
                    case 2:
                        assertArrayEquals(PEER, data);
                        if (!missingProof) {
                            byte[] proof = ECU_PROOF.clone();
                            if (badProof) { proof[0] ^= 1; }
                            incoming.add(new Frame(0x713, proof));
                            proofAt = clock.now();
                        }
                        break;
                    case 3:
                        assertTrue(clock.now() - proofAt >= 180);
                        assertArrayEquals(PERMISSION, data);
                        byte[] ack = ACK.clone();
                        if (badAck) { ack[0] ^= 1; }
                        incoming.add(new Frame(0x713, ack));
                        break;
                    default: fail("Unexpected authentication retry");
                }
            }
        }

        public Frame receiveCan() { return incoming.poll(); }
        public void close() { }
    }

    private void authorize(Bus bus) throws IOException, InterruptedException {
        new M749Immo(KEY, REFERENCE).authorize(bus, s -> {}, bus.clock, PEER, 1000);
    }

    @Test void installedRusefiNeedsNoUnsupportedSessionDidOrPowerCycle() throws Exception {
        Bus bus = new Bus();
        bus.rusefi = true;
        authorize(bus);
        assertEquals(2, bus.sent.size());
        assertEquals(0, bus.immoFrames);
    }

    @Test void genericRusefiStopsBeforeOemAuthentication() {
        Bus bus = new Bus();
        bus.genericRusefi = true;
        assertTrue(assertThrows(IOException.class, () -> authorize(bus)).getMessage().contains("not confirmed"));
        assertEquals(2, bus.sent.size());
        assertEquals(0, bus.immoFrames);
    }

    @Test void normalExchangeMatchesExpectedProofsAndPermission() throws Exception {
        Bus bus = new Bus();
        authorize(bus);
        assertEquals(6, bus.sent.size());
        assertEquals(3, bus.immoFrames);
        assertTrue(bus.clock.now() >= 280);
    }

    @Test void loaderSessionNeedsNoImmoTraffic() throws Exception {
        Bus bus = new Bus();
        bus.loader = true;
        authorize(bus);
        assertEquals(3, bus.sent.size());
        assertEquals(0, bus.immoFrames);
    }

    @Test void wrongProofStopsBeforePermission() {
        Bus bus = new Bus();
        bus.badProof = true;
        assertTrue(assertThrows(IOException.class, () -> authorize(bus)).getMessage().contains("proof mismatch"));
        assertEquals(2, bus.immoFrames);
    }

    @Test void wrongAcknowledgementFailsClosed() {
        Bus bus = new Bus();
        bus.badAck = true;
        assertTrue(assertThrows(IOException.class, () -> authorize(bus)).getMessage().contains("acknowledgement mismatch"));
        assertEquals(3, bus.immoFrames);
    }

    @Test void existingPeerAndMalformedNonceStopWithoutImmoWrites() {
        for (boolean peer : new boolean[]{false, true}) {
            Bus bus = new Bus();
            bus.competingPeer = peer;
            bus.badLength = !peer;
            assertThrows(IOException.class, () -> authorize(bus));
            assertEquals(0, bus.immoFrames);
        }
    }

    @Test void missingNonceOrProofTimesOutWithoutRetry() {
        for (boolean nonce : new boolean[]{false, true}) {
            Bus bus = new Bus();
            bus.missingNonce = nonce;
            bus.missingProof = !nonce;
            assertTrue(assertThrows(IOException.class, () -> authorize(bus)).getMessage().contains("timed out"));
            assertEquals(nonce ? 0 : 2, bus.immoFrames);
        }
    }

    @Test void invalidBackupRejectedBeforeAnyTransportIsNeeded() throws Exception {
        Path file = directory.resolve("backup with spaces.bin");
        Files.write(file, new byte[32]);
        assertThrows(IOException.class, () -> M749Immo.load(file));
        Files.write(file, new byte[0x3F0000]);
        assertTrue(assertThrows(IOException.class, () -> M749Immo.load(file)).getMessage().contains("does not match"));
    }

    @Test void splitStreamsCrossTwoBlocksAndMatchExpectedBytes() {
        String[] expected = {
                "d3b8847ef2d2b5f7383046f3c1e6b94ddbfe3d9b391428590d22575212bd22fd6e9a7c5129e0e34f3cecf4962cefb19b68232cc61273bba89228cebfda85d9836d75483b5178b2908dff8758fdbac9716b986107959f463918bf6c35d5bd33f965b852ee1b1631b85d13c740624cb5299cfd7a9f1398897c13cb6e394f9b05004445",
                "c85db8f2951febcaaaa557cb1af4b27f24c184b6c03deda703513de82311a40d24bf049ba1095fdf2e72f3a2e6bed0f66989d34077c30e6089a1fcde0d7f7fd20f65960da22b67869832664208815252033c19ed813f9e0b28f72e42178f658a91f4711bfcb72a87fb146bb6b77900ad627891cc071f303e1342994b7641a51325e3"};
        byte[] plain = new byte[130];
        for (int i = 0; i < plain.length; i++) { plain[i] = (byte) i; }
        for (int n = 0; n < 2; n++) {
            M749Immo.ChaCha stream = new M749Immo.ChaCha(KEY, NONCE, n == 0 ? 12 : 20);
            byte[] ciphertext = hex(expected[n]);
            int offset = 0;
            for (int size : new int[]{7, 57, 1, 65}) {
                assertArrayEquals(Arrays.copyOfRange(ciphertext, offset, offset + size),
                        stream.crypt(Arrays.copyOfRange(plain, offset, offset + size)));
                offset += size;
            }
        }
    }
}
