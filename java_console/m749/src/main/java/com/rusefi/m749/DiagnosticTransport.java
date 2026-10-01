package com.rusefi.m749;

import java.io.IOException;

/** Classic CAN frames on the physical diagnostic request/response IDs. */
interface DiagnosticTransport extends AutoCloseable {
    void send(byte[] frame) throws IOException;

    /** Returns a standard 0x7E8 frame, truncated to its DLC, or null if the queue is empty. */
    byte[] receive() throws IOException;

    @Override
    void close() throws IOException;
}
