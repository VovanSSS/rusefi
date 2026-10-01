package com.rusefi.m749;

import java.io.IOException;
import java.util.Arrays;
import java.util.function.Consumer;

/** Reads only through the RAM helper and advances progress after matching reads. */
final class M749FlashReader {
    static String read(M749RamHelper helper, FlashReadFile file, int block, Consumer<String> out)
            throws IOException, InterruptedException {
        if (block < 1 || block > M749RamHelper.MAX_BLOCK) { throw new IllegalArgumentException("Invalid read block size"); }
        helper.probe();
        if (file.completed() > 0) {
            out.accept("Checking all saved bytes against the connected ECU before resuming");
            for (int offset = 0; offset < file.completed(); offset += block) {
                int count = Math.min(block, file.completed() - offset);
                if (!Arrays.equals(file.saved(offset, count), helper.read(file.address + offset, count))) {
                    throw new IOException(String.format("Saved bytes differ at %08X; backup was not extended", file.address + offset));
                }
            }
        }
        long start = System.nanoTime();
        int initial = file.completed(), lastReport = initial;
        while (file.completed() < file.length) {
            if (Thread.currentThread().isInterrupted()) { throw new InterruptedException(); }
            int count = Math.min(block, file.length - file.completed());
            int address = file.address + file.completed();
            byte[] first = helper.read(address, count);
            if (!Arrays.equals(first, helper.read(address, count))) {
                throw new IOException(String.format("Repeated read differs at %08X; block was not saved", address));
            }
            file.append(first);
            if (file.completed() - lastReport >= 16384 || file.completed() == file.length) {
                double seconds = Math.max(0.001, (System.nanoTime() - start) / 1e9);
                out.accept(String.format("Verified %d/%d bytes (%.1f%%), %.0f bytes/s", file.completed(), file.length,
                        100.0 * file.completed() / file.length, (file.completed() - initial) / seconds));
                lastReport = file.completed();
            }
        }
        return file.finish();
    }
}
