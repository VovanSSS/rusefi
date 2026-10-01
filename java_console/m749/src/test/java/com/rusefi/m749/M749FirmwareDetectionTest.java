package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Queue;
import static com.rusefi.m749.M749Identification.bytes;
import static com.rusefi.m749.M749FirmwareDetection.Result.*;
import static org.junit.jupiter.api.Assertions.*;

class M749FirmwareDetectionTest {
    private static final class Bus implements DiagnosticTransport, M749Identification.Timing {
        final Queue<byte[]> incoming = new ArrayDeque<>();
        final List<Integer> requests = new ArrayList<>();
        byte[] identity, activation;
        IOException sendError;
        boolean reject, malformed;
        long time;

        public void send(byte[] frame) throws IOException {
            if (sendError != null) { throw sendError; }
            assertEquals(3, frame[0]);
            assertEquals(0x22, frame[1], "Detection must only read DIDs");
            int did = (frame[2] & 255) << 8 | frame[3] & 255;
            requests.add(did);
            byte[] payload = did == 0xF1A4 ? identity : activation;
            if (reject) { payload = bytes(0x7F, 0x22, 0x31); }
            if (malformed) { incoming.add(bytes(7, 0x62)); return; }
            if (payload != null) {
                byte[] response = new byte[8];
                response[0] = (byte) payload.length;
                System.arraycopy(payload, 0, response, 1, payload.length);
                incoming.add(response);
            }
        }
        public byte[] receive() { return incoming.poll(); }
        public void close() { }
        public long now() { return time; }
        public void pause(long ms) { time += ms; }
        M749FirmwareDetection.Result detect() throws Exception {
            return M749FirmwareDetection.detect(new UdsClient(this, this));
        }
    }

    @Test void dedicatedIdentityDoesNotPromiseOemLoader() throws Exception {
        Bus bus = new Bus();
        bus.identity = bytes(0x62, 0xF1, 0xA4, 'r', 'E', 'F', 'I');
        assertEquals(RUSEFI, bus.detect());
        assertFalse(RUSEFI.m749);
        assertEquals(List.of(0xF1A4, 0xF1A0), bus.requests);
    }

    @Test void installedFirmwareNeedsOnlyLegacyActivationReply() throws Exception {
        Bus bus = new Bus();
        bus.activation = bytes(0x62, 0xF1, 0xA0, 0x4D, 0x74, 1, 1);
        assertEquals(M749_READY, bus.detect());
        assertTrue(M749_READY.m749);
    }

    @Test void identityAndReadinessRemainSeparate() throws Exception {
        Bus bus = new Bus();
        bus.identity = bytes(0x62, 0xF1, 0xA4, 'r', 'E', 'F', 'I');
        bus.activation = bytes(0x62, 0xF1, 0xA0, 0x4D, 0x74, 1, 0);
        assertEquals(M749_NOT_READY, bus.detect());
    }

    @Test void silenceIsUnknownAndBounded() throws Exception {
        Bus bus = new Bus();
        assertEquals(UNKNOWN, bus.detect());
        assertEquals(4000, bus.time);
    }

    @Test void unsupportedDidsAreUnknown() throws Exception {
        Bus bus = new Bus();
        bus.reject = true;
        assertEquals(UNKNOWN, bus.detect());
    }

    @Test void partialSignatureAndUnknownAbiAreNotMatches() throws Exception {
        Bus bus = new Bus();
        bus.identity = bytes(0x62, 0xF1, 0xA4, 'r', 'E', 'F');
        bus.activation = bytes(0x62, 0xF1, 0xA0, 0x4D, 0x74, 2, 1);
        assertEquals(UNKNOWN, bus.detect());
    }

    @Test void transportFailuresAreNotHiddenAsUnknownFirmware() {
        Bus bus = new Bus();
        bus.sendError = new IOException("adapter disconnected");
        assertSame(bus.sendError, assertThrows(IOException.class, bus::detect));
    }

    @Test void malformedFramesAreErrors() {
        Bus bus = new Bus();
        bus.malformed = true;
        assertTrue(assertThrows(IOException.class, bus::detect).getMessage().contains("Malformed"));
    }
}
