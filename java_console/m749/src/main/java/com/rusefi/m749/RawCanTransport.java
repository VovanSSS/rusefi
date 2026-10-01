package com.rusefi.m749;

import java.io.IOException;

/** One owned CAN channel, shared sequentially by IMMO and UDS. */
interface RawCanTransport extends DiagnosticTransport {
    final class Frame {
        final int id;
        final byte[] data;

        Frame(int id, byte[] data) {
            this.id = id;
            this.data = data.clone();
        }
    }

    void sendCan(int id, byte[] frame) throws IOException;
    Frame receiveCan() throws IOException;

    @Override
    default void send(byte[] frame) throws IOException {
        sendCan(0x7E0, frame);
    }

    @Override
    default byte[] receive() throws IOException {
        for (int i = 0; i < 64; i++) {
            Frame frame = receiveCan();
            if (frame == null) {
                return null;
            }
            if (frame.id == 0x7E8) {
                return frame.data;
            }
        }
        return null;
    }
}
