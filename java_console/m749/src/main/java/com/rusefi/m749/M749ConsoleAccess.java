package com.rusefi.m749;

import com.rusefi.PortScanner;
import java.io.IOException;
import java.util.concurrent.TimeUnit;

/** Release console ownership and wait for discovery probes before opening a transfer adapter. */
final class M749ConsoleAccess implements M749Monitor.TransferAccess {
    private final PortScanner scanner;
    private final Runnable disconnect;

    M749ConsoleAccess(PortScanner scanner, Runnable disconnect) {
        this.scanner = scanner;
        this.disconnect = disconnect;
    }

    public int run(M749Monitor.TransferAction action) throws IOException, InterruptedException {
        try {
            if (!scanner.suspend().await(30, TimeUnit.SECONDS)) {
                throw new IOException("Timed out waiting for adapter discovery to stop");
            }
            disconnect.run();
            return action.run();
        } finally {
            // Reconnection is manual: a failed transfer may have left the ECU in its loader/helper.
            scanner.resume();
        }
    }
}
