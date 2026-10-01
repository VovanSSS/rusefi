package com.rusefi.m749;

import com.rusefi.PortScanner;
import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import static org.junit.jupiter.api.Assertions.*;

class M749ConsoleAccessTest {
    private static PortScanner scanner(List<String> events, CountDownLatch stopped) {
        return (PortScanner) Proxy.newProxyInstance(PortScanner.class.getClassLoader(), new Class<?>[]{PortScanner.class},
                (proxy, method, args) -> {
                    events.add(method.getName());
                    if (method.getName().equals("suspend")) return stopped;
                    if (method.getName().equals("resume")) return null;
                    throw new AssertionError("Unexpected scanner operation " + method.getName());
                });
    }

    @Test void discoveryStopsAndConsoleDisconnectsBeforeTransferAndResumesAfterFailure() throws Exception {
        for (boolean fail : new boolean[]{false, true}) {
            List<String> events = new ArrayList<>();
            M749ConsoleAccess access = new M749ConsoleAccess(scanner(events, new CountDownLatch(0)), () -> events.add("disconnect"));
            M749Monitor.TransferAction action = () -> {
                events.add("transfer");
                if (fail) throw new IOException("failure");
                return 0;
            };
            if (fail) assertThrows(IOException.class, () -> access.run(action));
            else assertEquals(0, access.run(action));
            assertEquals(List.of("suspend", "disconnect", "transfer", "resume"), events);
        }
    }

    @Test void timeoutOrInterruptionCannotOpenAdapter() {
        for (boolean interrupt : new boolean[]{false, true}) {
            List<String> events = new ArrayList<>();
            CountDownLatch stopped = new CountDownLatch(1) {
                @Override public boolean await(long timeout, TimeUnit unit) throws InterruptedException {
                    if (interrupt) throw new InterruptedException();
                    return false;
                }
            };
            M749ConsoleAccess access = new M749ConsoleAccess(scanner(events, stopped), () -> fail("Disconnected before discovery stopped"));
            if (interrupt) assertThrows(InterruptedException.class, () -> access.run(() -> { fail("Opened adapter"); return 0; }));
            else assertThrows(IOException.class, () -> access.run(() -> { fail("Opened adapter"); return 0; }));
            assertEquals(List.of("suspend", "resume"), events);
        }
    }
}
