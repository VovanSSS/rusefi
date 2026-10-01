package com.rusefi.m749;

import java.io.IOException;
import java.util.Arrays;

/** One outstanding physical request; classic CAN ISO-TP, no automatic retries. */
final class UdsClient implements M749Uploader.Connection {
    private static final long FRAME_TIMEOUT = 2_000;
    private final DiagnosticTransport transport;
    private final M749Identification.Timing clock;
    private final int receiveBlockSize;
    private final int receiveStmin;
    private int sid;
    private String phase = "before any request";

    /** Names the request and ISO-TP phase that expired; nothing was retried. */
    static final class Timeout extends IOException {
        final int sid;
        final String phase;

        Timeout(int sid, String phase) {
            super(String.format("ISO-TP/UDS timeout: SID %02X %s; request was not retried", sid, phase));
            this.sid = sid;
            this.phase = phase;
        }
    }

    static final class NegativeResponse extends IOException {
        final int code;

        NegativeResponse(int sid, int code) {
            super(String.format("UDS %02X rejected: NRC %02X", sid, code));
            this.code = code;
        }
    }

    UdsClient(DiagnosticTransport transport) {
        this(transport, 0, 1);
    }

    UdsClient(DiagnosticTransport transport, int receiveBlockSize, int receiveStmin) {
        this(transport, new M749Identification.Timing() {
            public long now() { return System.nanoTime() / 1_000_000; }
            public void pause(long milliseconds) throws InterruptedException { Thread.sleep(milliseconds); }
        }, receiveBlockSize, receiveStmin);
    }

    UdsClient(DiagnosticTransport transport, M749Identification.Timing clock) {
        this(transport, clock, 0, 1);
    }

    UdsClient(DiagnosticTransport transport, M749Identification.Timing clock, int receiveBlockSize, int receiveStmin) {
        if (receiveBlockSize < 0 || receiveBlockSize > 255 || receiveStmin < 0 || receiveStmin > 127) {
            throw new IllegalArgumentException("Invalid receive flow control");
        }
        this.transport = transport;
        this.clock = clock;
        this.receiveBlockSize = receiveBlockSize;
        this.receiveStmin = receiveStmin;
    }

    public void pause(long milliseconds) throws InterruptedException {
        clock.pause(milliseconds);
    }

    public byte[] exchange(byte[] request, byte[] prefix, long timeout) throws IOException, InterruptedException {
        if (request.length == 0 || request.length > 4095 || prefix.length == 0 || timeout <= 0) {
            throw new IllegalArgumentException("Invalid UDS request/prefix/timeout");
        }
        long deadline = clock.now() + timeout;
        sid = request[0] & 255;
        transmit(request, deadline);
        long responseDeadline = Math.min(deadline, clock.now() + FRAME_TIMEOUT);
        phase = "awaiting the response";
        while (true) {
            byte[] response = receive(prefix, responseDeadline, deadline);
            if (response == null) {
                continue; // Unrelated traffic does not extend the timeout.
            }
            if ((response[0] & 255) == 0x7F) {
                if (response.length != 3 || response[1] != request[0]) {
                    continue;
                }
                if ((response[2] & 255) != 0x78) {
                    throw new NegativeResponse(request[0] & 255, response[2] & 255);
                }
                responseDeadline = Math.min(deadline, clock.now() + 5_000);
                phase = "awaiting the response after a 7F 78 pending reply";
            } else if (startsWith(response, prefix)) {
                return response;
            }
        }
    }

    private void transmit(byte[] payload, long deadline) throws IOException, InterruptedException {
        phase = "before sending the request";
        check(deadline);
        byte[] frame = padded();
        if (payload.length <= 7) {
            frame[0] = (byte) payload.length;
            System.arraycopy(payload, 0, frame, 1, payload.length);
            transport.send(frame);
            return;
        }
        frame[0] = (byte) (0x10 | payload.length >>> 8);
        frame[1] = (byte) payload.length;
        System.arraycopy(payload, 0, frame, 2, 6);
        transport.send(frame);
        int position = 6;
        int sequence = 1;
        while (position < payload.length) {
            phase = String.format("awaiting flow control for a %d-byte request (%d bytes sent)", payload.length, position);
            byte[] fc = flowControl(deadline);
            phase = String.format("sending consecutive frames of a %d-byte request", payload.length);
            int blockSize = fc[1] & 255;
            int stMin = fc[2] & 255;
            // Millisecond sleep rounds the ISO-TP 100-900 us range upwards.
            int delay = stMin <= 0x7F ? stMin : stMin >= 0xF1 && stMin <= 0xF9 ? 1 : -1;
            if (delay < 0) {
                throw new IOException("Reserved ISO-TP STmin");
            }
            int sent = 0;
            while (position < payload.length && (blockSize == 0 || sent < blockSize)) {
                check(deadline);
                if (delay > 0) {
                    clock.pause(delay);
                }
                check(deadline);
                frame = padded();
                frame[0] = (byte) (0x20 | sequence);
                sequence = (sequence + 1) & 15;
                int length = Math.min(7, payload.length - position);
                System.arraycopy(payload, position, frame, 1, length);
                transport.send(frame);
                position += length;
                sent++;
            }
        }
    }

    private byte[] flowControl(long deadline) throws IOException, InterruptedException {
        long limit = Math.min(deadline, clock.now() + FRAME_TIMEOUT);
        int waits = 0;
        while (true) {
            byte[] frame = next(limit);
            if (frame.length < 3 || (frame[0] & 0xF0) != 0x30) {
                throw new IOException("Expected ISO-TP flow control");
            }
            int status = frame[0] & 15;
            if (status == 0) {
                return frame;
            }
            if (status != 1 || ++waits > 3) {
                throw new IOException("ISO-TP flow control rejected or excessive WAIT");
            }
            limit = Math.min(deadline, clock.now() + FRAME_TIMEOUT);
        }
    }

    private byte[] receive(byte[] prefix, long responseDeadline, long deadline)
            throws IOException, InterruptedException {
        byte[] frame = next(responseDeadline);
        int type = (frame[0] & 255) >>> 4;
        if (type == 0) {
            int length = frame[0] & 15;
            if (length == 0 || length > 7 || frame.length < length + 1) {
                throw new IOException("Malformed ISO-TP single frame");
            }
            return Arrays.copyOfRange(frame, 1, length + 1);
        }
        if (type != 1) {
            return null;
        }
        if (frame.length != 8) {
            throw new IOException("Truncated ISO-TP first frame");
        }
        int length = (frame[0] & 15) << 8 | frame[1] & 255;
        if (length < 8) {
            throw new IOException("Invalid ISO-TP first-frame length");
        }
        // Only six payload bytes are present in the first frame. Match the
        // remaining prefix after reassembly (RAM-write acknowledgements use 8).
        if (!startsWith(Arrays.copyOfRange(frame, 2, 8), Arrays.copyOf(prefix, Math.min(prefix.length, 6)))) {
            return null;
        }
        byte[] payload = new byte[length];
        System.arraycopy(frame, 2, payload, 0, 6);
        phase = String.format("receiving a %d-byte multi-frame response", length);
        byte[] fc = padded();
        fc[0] = 0x30;
        fc[1] = (byte) receiveBlockSize;
        fc[2] = (byte) receiveStmin;
        transport.send(fc);
        int position = 6;
        int sequence = 1;
        int receivedInBlock = 0;
        while (position < length) {
            phase = String.format("receiving a %d-byte multi-frame response (%d bytes received)", length, position);
            frame = next(Math.min(deadline, clock.now() + FRAME_TIMEOUT));
            int count = Math.min(7, length - position);
            if ((frame[0] & 255) != (0x20 | sequence) || frame.length < count + 1) {
                throw new IOException("ISO-TP consecutive-frame sequence/length mismatch");
            }
            System.arraycopy(frame, 1, payload, position, count);
            sequence = (sequence + 1) & 15;
            position += count;
            if (++receivedInBlock == receiveBlockSize && position < length) {
                transport.send(fc);
                receivedInBlock = 0;
            }
        }
        return payload;
    }

    private byte[] next(long deadline) throws IOException, InterruptedException {
        while (true) {
            check(deadline);
            byte[] frame = transport.receive();
            if (frame != null) {
                if (frame.length == 0 || frame.length > 8) {
                    throw new IOException("Invalid CAN frame length");
                }
                return frame;
            }
            clock.pause(1);
        }
    }

    private void check(long deadline) throws IOException, InterruptedException {
        if (Thread.currentThread().isInterrupted()) {
            throw new InterruptedException("Diagnostic operation interrupted");
        }
        if (clock.now() >= deadline) {
            throw new Timeout(sid, phase);
        }
    }

    private static byte[] padded() {
        byte[] frame = new byte[8];
        Arrays.fill(frame, (byte) 0xCC);
        return frame;
    }

    static boolean startsWith(byte[] value, byte[] prefix) {
        if (value.length < prefix.length) {
            return false;
        }
        for (int i = 0; i < prefix.length; i++) {
            if (value[i] != prefix[i]) {
                return false;
            }
        }
        return true;
    }
}
