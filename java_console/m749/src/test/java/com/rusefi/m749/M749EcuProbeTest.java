package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.util.*;
import static org.junit.jupiter.api.Assertions.*;
import static com.rusefi.m749.M749Identification.bytes;

class M749EcuProbeTest {
    @Test void productionBackendIdentifiesAllConnectorsThroughOwnedReadOnlyTransport() throws Exception {
        for (String[] selection : new String[][]{{"--channel", "PCAN_USBBUS2"}, {"--slcan", "COM42"}, {"--socketcan", "can7"}}) {
            M749ConnectionOptions options = new M749ConnectionOptions();
            options.accept(selection[0], selection[1]);
            options.validate();
            SocketCanTransportTest.Port port = new SocketCanTransportTest.Port() {
                @Override public void send(com.rusefi.io.can.ClassicCanFrame frame) {
                    super.send(frame);
                    byte[] q = frame.getPayload();
                    assertEquals(3, q[0]);
                    assertEquals(0x22, q[1], "Automatic identification must not change session or authenticate");
                    if ((q[3] & 255) == 0xA4) reply(0x7E8, bytes(7, 0x62, 0xF1, 0xA4, 'r', 'E', 'F', 'I'));
                    else if ((q[3] & 255) == 0xA0) reply(0x7E8, bytes(7, 0x62, 0xF1, 0xA0, 0x4D, 0x74, 1, 1));
                    else reply(0x7E8, bytes(3, 0x7F, 0x22, 0x31));
                }
            };
            boolean[] owned = {false};
            M749Monitor.Backend backend = M749Monitor.canBackend(action -> {
                owned[0] = true;
                try { return action.run(); }
                finally { owned[0] = false; }
            }, (selected, out) -> {
                assertTrue(owned[0]);
                assertEquals(options.key(), selected.key());
                return new SocketCanTransport(port);
            });
            assertEquals(M749FirmwareDetection.Result.M749_READY, backend.inspect(options, s -> {}).firmware);
            assertEquals(5, port.outgoing.size());
            assertTrue(port.closed);
            assertFalse(owned[0]);
        }
    }

    @Test void confirmsReplacementFirmwareEvenWhenOemDidsTimeOut() throws Exception {
        List<byte[]> sent = new ArrayList<>();
        List<String> log = new ArrayList<>();
        M749EcuProbe.identify(new M749Uploader.Connection() {
            public void pause(long ms) { }
            public byte[] exchange(byte[] request, byte[] prefix, long timeout) throws IOException {
                sent.add(request);
                if ((request[2] & 255) == 0xa4) { return bytes(0x62, 0xf1, 0xa4, 'r', 'E', 'F', 'I'); }
                throw new UdsClient.Timeout(0x22, "awaiting the response");
            }
        }, log::add);
        assertEquals(5, sent.size());
        assertTrue(sent.stream().allMatch(q -> q.length == 3 && q[0] == 0x22));
        assertTrue(log.stream().anyMatch(s -> s.contains("ECU presence confirmed")));
    }

    @Test void negativeResponsesAloneDoNotCertifyIdentity() {
        assertThrows(IOException.class, () -> M749EcuProbe.identify(new M749Uploader.Connection() {
            public void pause(long ms) { }
            public byte[] exchange(byte[] request, byte[] prefix, long timeout) throws IOException {
                throw new UdsClient.NegativeResponse(0x22, 0x31);
            }
        }, s -> {}));
    }
}
