package com.rusefi.m749;

import java.io.OutputStream;
import java.io.PrintStream;
import java.util.Locale;
import java.util.function.LongSupplier;

/** Prefix console lines, including library logs, without delaying partial output. */
final class ElapsedTimeOutputStream extends OutputStream {
    private final PrintStream target;
    private final LongSupplier seconds;
    private boolean lineStart = true;

    private ElapsedTimeOutputStream(PrintStream target, LongSupplier seconds) {
        this.target = target;
        this.seconds = seconds;
    }

    static void install() {
        long start = System.nanoTime();
        LongSupplier seconds = () -> (System.nanoTime() - start) / 1_000_000_000L;
        System.setOut(new PrintStream(new ElapsedTimeOutputStream(System.out, seconds), true));
        System.setErr(new PrintStream(new ElapsedTimeOutputStream(System.err, seconds), true));
    }

    private void prefix() {
        if (lineStart) {
            target.printf(Locale.ROOT, "[%4d] ", seconds.getAsLong());
            lineStart = false;
        }
    }

    @Override
    public synchronized void write(int value) {
        prefix();
        target.write(value);
        lineStart = (value & 0xFF) == '\n';
    }

    @Override
    public synchronized void write(byte[] bytes, int offset, int length) {
        java.util.Objects.checkFromIndexSize(offset, length, bytes.length);
        int end = offset + length;
        while (offset < end) {
            prefix();
            int next = offset;
            while (next < end && bytes[next++] != '\n') {
                // Forward a whole line or partial line without changing its encoding.
            }
            target.write(bytes, offset, next - offset);
            lineStart = bytes[next - 1] == '\n';
            offset = next;
        }
    }

    @Override
    public synchronized void flush() {
        target.flush();
    }
}
