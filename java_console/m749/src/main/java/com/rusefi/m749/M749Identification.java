package com.rusefi.m749;

import com.rusefi.uds.M74_9_SeedKeyCalculator;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;

/** Bounded session-03 authentication and individual identity reads. No programming services. */
final class M749Identification {
    // DIDs the real M74.9 rejects with NRC 31 are omitted:
    // F180, F183, F18D, F18E, F191, F197, F198, F199, FD09.
    static final int[] DIDS = {
            0xF186, 0xF192, 0xF193, 0xF194, 0xF195, 0xF188, 0xF189,
            0xF18A, 0xF18B, 0xF18C, 0xF190,
            0xFD00, 0xFD01, 0xFD02, 0xFD03, 0xFD04, 0xFD05
    };

    interface Timing {
        long now();
        void pause(long milliseconds) throws InterruptedException;
    }

    private final DiagnosticTransport transport;
    private final Consumer<String> messages;
    private final Timing timing;
    private long overallDeadline;

    M749Identification(DiagnosticTransport transport, Consumer<String> messages) {
        this(transport, messages, new Timing() {
            public long now() { return System.nanoTime() / 1_000_000; }
            public void pause(long milliseconds) throws InterruptedException { Thread.sleep(milliseconds); }
        });
    }

    M749Identification(DiagnosticTransport transport, Consumer<String> messages, Timing timing) {
        this.transport = transport;
        this.messages = messages;
        this.timing = timing;
    }

    Map<Integer, byte[]> run() throws IOException, InterruptedException {
        overallDeadline = timing.now() + 15_000;
        messages.accept("M74.9: starting extended diagnostic session 03");
        requireLength(exchange(bytes(0x10, 0x03)), 6);
        byte[] seedResponse = exchange(bytes(0x27, 0x01, 0));
        requireLength(seedResponse, 6);
        int seed = ((seedResponse[2] & 0xFF) << 24) | ((seedResponse[3] & 0xFF) << 16)
                | ((seedResponse[4] & 0xFF) << 8) | (seedResponse[5] & 0xFF);
        if (seed != 0) {
            int key = M74_9_SeedKeyCalculator.Uds_Security_CalcKey(M74_9_SeedKeyCalculator.SECRET, seed, 0);
            requireLength(exchange(bytes(0x27, 0x02, key >>> 24, key >>> 16, key >>> 8, key)), 2);
            messages.accept("Authentication accepted");
        } else {
            messages.accept("Already unlocked (zero seed)");
        }

        overallDeadline = timing.now() + 90_000;
        Map<Integer, byte[]> values = new LinkedHashMap<>();
        int unavailable = 0;
        for (int did : DIDS) {
            try {
                byte[] response = exchange(bytes(0x22, did >>> 8, did));
                byte[] value = Arrays.copyOfRange(response, 3, response.length);
                messages.accept(String.format("%s: %d bytes | hex=%s | ASCII=%s",
                        label(did), value.length, hex(value), ascii(value)));
                values.put(did, value);
            } catch (NegativeResponse e) {
                messages.accept(String.format("%s: unavailable (NRC %02X)", label(did), e.code));
                unavailable++;
            }
            timing.pause(50);
        }
        messages.accept("Identification complete: " + values.size() + " read, " + unavailable + " unavailable");
        return values;
    }

    /** Human-readable status from the most useful identity records, in display order. */
    static List<String> summarize(Map<Integer, byte[]> values) {
        List<String> summary = new ArrayList<>();
        String vin = text(values.get(0xF190));
        if (vin != null) summary.add("VIN: " + vin);
        append(summary, "Software: ", text(values.get(0xF189)), ", built ", text(values.get(0xF195)));
        append(summary, "Hardware: ", text(values.get(0xF193)), ", part ", text(values.get(0xF192)));
        String supplier = text(values.get(0xF18A));
        String serial = text(values.get(0xF18C));
        append(summary, "ECU: ", supplier == null ? serial : serial == null ? supplier : supplier + ", serial " + serial,
                ", manufactured ", text(values.get(0xF18B)));
        return summary;
    }

    private static void append(List<String> summary, String prefix, String main, String separator, String extra) {
        if (main != null) {
            summary.add(prefix + main + (extra == null ? "" : separator + extra));
        }
    }

    /** Printable text with trailing padding (NULs etc) removed, or null when nothing printable. */
    private static String text(byte[] value) {
        int end = value == null ? 0 : value.length;
        while (end > 0 && ((value[end - 1] & 0xFF) < 32 || (value[end - 1] & 0xFF) > 126)) {
            end--;
        }
        return end == 0 ? null : ascii(Arrays.copyOf(value, end));
    }

    private byte[] exchange(byte[] request) throws IOException, InterruptedException {
        checkDeadline(overallDeadline);
        byte[] frame = new byte[8];
        Arrays.fill(frame, (byte) 0xCC);
        frame[0] = (byte) request.length;
        System.arraycopy(request, 0, frame, 1, request.length);
        transport.send(frame);
        long deadline = timing.now() + 2_000;
        byte[] assembly = null;
        int count = 0;
        int sequence = 1;
        while (true) {
            checkDeadline(Math.min(deadline, overallDeadline));
            byte[] incoming = transport.receive();
            if (incoming == null) {
                timing.pause(2);
                continue;
            }
            if (incoming.length < 2 || incoming.length > 8) {
                throw new IOException("Malformed ISO-TP frame");
            }
            int pci = (incoming[0] & 0xFF) >>> 4;
            byte[] payload;
            if (pci == 0) {
                int length = incoming[0] & 0xFF;
                if (assembly != null || length < 1 || length > 7 || length + 1 > incoming.length) {
                    throw new IOException("Malformed ISO-TP single frame");
                }
                payload = Arrays.copyOfRange(incoming, 1, length + 1);
            } else if (pci == 1) {
                int length = ((incoming[0] & 0x0F) << 8) | (incoming[1] & 0xFF);
                if (request[0] != 0x22 || assembly != null || incoming.length != 8 || length < 8 || length > 1024) {
                    throw new IOException("Malformed or oversized ISO-TP first frame");
                }
                if (!matches(request, Arrays.copyOfRange(incoming, 2, 8))) {
                    continue;
                }
                assembly = new byte[length];
                System.arraycopy(incoming, 2, assembly, 0, 6);
                count = 6;
                sequence = 1;
                transport.send(bytes(0x30, 0, 5, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC));
                deadline = timing.now() + 2_000;
                continue;
            } else if (pci == 2 && assembly != null) {
                int size = Math.min(7, assembly.length - count);
                if ((incoming[0] & 0x0F) != sequence || incoming.length < size + 1) {
                    throw new IOException("ISO-TP consecutive-frame sequence/length error");
                }
                System.arraycopy(incoming, 1, assembly, count, size);
                count += size;
                sequence = (sequence + 1) & 0x0F;
                deadline = timing.now() + 2_000;
                if (count < assembly.length) {
                    continue;
                }
                payload = assembly;
                assembly = null;
            } else {
                throw new IOException("Unexpected ISO-TP frame");
            }
            if ((payload[0] & 0xFF) == 0x7F) {
                if (payload.length != 3) {
                    throw new IOException("Malformed UDS negative response");
                }
                if (payload[1] != request[0]) {
                    continue;
                }
                int nrc = payload[2] & 0xFF;
                if (nrc == 0x78) {
                    deadline = timing.now() + 5_000;
                    continue;
                }
                throw new NegativeResponse(request[0] & 0xFF, nrc);
            }
            if (matches(request, payload)) {
                return payload;
            }
        }
    }

    private void checkDeadline(long deadline) throws IOException, InterruptedException {
        if (Thread.currentThread().isInterrupted()) {
            throw new InterruptedException("Identification cancelled");
        }
        if (timing.now() >= deadline) {
            throw new IOException("M74.9 response timeout; check ECU power, CAN wiring and 500 kbit/s bus");
        }
    }

    private static boolean matches(byte[] request, byte[] response) {
        int prefix = request[0] == 0x22 ? 3 : 2;
        if (response.length < prefix || (response[0] & 0xFF) != (request[0] & 0xFF) + 0x40) {
            return false;
        }
        for (int i = 1; i < prefix; i++) {
            if (response[i] != request[i]) {
                return false;
            }
        }
        return true;
    }

    private static void requireLength(byte[] response, int length) throws IOException {
        if (response.length != length) {
            throw new IOException("Unexpected UDS response length: " + response.length);
        }
    }

    private static String label(int did) {
        return String.format(did == 0xF190 ? "VIN (DID %04X)" : "DID %04X", did);
    }

    static String hex(byte[] value) {
        StringBuilder result = new StringBuilder();
        for (byte b : value) {
            if (result.length() > 0) result.append(' ');
            result.append(String.format("%02X", b & 0xFF));
        }
        return result.toString();
    }

    static String ascii(byte[] value) {
        StringBuilder result = new StringBuilder();
        for (byte b : value) {
            int c = b & 0xFF;
            result.append(c >= 32 && c <= 126 ? (char) c : '.');
        }
        return result.toString();
    }

    static byte[] bytes(int... values) {
        byte[] result = new byte[values.length];
        for (int i = 0; i < values.length; i++) result[i] = (byte) values[i];
        return result;
    }

    private static final class NegativeResponse extends IOException {
        final int code;

        NegativeResponse(int service, int code) {
            super(String.format("UDS service %02X rejected (NRC %02X)", service, code));
            this.code = code;
        }
    }
}
