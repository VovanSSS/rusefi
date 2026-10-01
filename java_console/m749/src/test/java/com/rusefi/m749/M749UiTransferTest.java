package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import peak.can.basic.TPCANHandle;
import javax.swing.*;
import java.awt.*;
import java.io.IOException;
import java.nio.file.Path;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import static org.junit.jupiter.api.Assertions.*;

class M749UiTransferTest {
    private static class Backend implements M749Monitor.Backend {
        final CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1), exited = new CountDownLatch(1);
        volatile List<String> args;
        volatile boolean active, fail;
        public List<PcanDevice.Channel> scan() {
            assertFalse(active, "Discovery raced a file transfer");
            return List.of(new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS2, true));
        }
        public List<String> identify(PcanDevice.Channel c, Consumer<String> out) { return List.of("OEM ECU"); }
        public int transfer(String[] args, Consumer<String> out) throws IOException, InterruptedException {
            assertFalse(SwingUtilities.isEventDispatchThread());
            this.args = List.of(args);
            active = true;
            out.accept("Verified 16384/4128768 bytes (0.4%)");
            entered.countDown();
            try {
                assertTrue(release.await(10, TimeUnit.SECONDS));
                if (fail) throw new IOException("Transfer failure");
                return 0;
            } finally { active = false; exited.countDown(); }
        }
    }

    @Test void readUsesSaveSelectionProgressAndResumeOptionsWithoutBlockingEdt() throws Exception {
        Backend backend = new Backend();
        M749Panel panel = open(backend, (parent, read) -> {
            assertTrue(read);
            return new M749Panel.Selection(Path.of("backup with spaces.bin"), true, true);
        });
        try {
            await(() -> button(panel, "readFlash").isEnabled());
            SwingUtilities.invokeAndWait(() -> button(panel, "readFlash").doClick());
            assertTrue(backend.entered.await(5, TimeUnit.SECONDS));
            assertEquals(List.of("--read-flash", "backup with spaces.bin", "--channel", "PCAN_USBBUS2",
                    "--block-size", "16", "--stmin", "1", "--reset-after", "--resume", "--helper-running"), backend.args);
            SwingUtilities.invokeAndWait(() -> {
                assertFalse(button(panel, "readFlash").isEnabled());
                assertFalse(button(panel, "writeFlash").isEnabled());
                assertFalse(find(panel, JComboBox.class, "channels").isEnabled());
                assertTrue(find(panel, JTextArea.class, "messages").getText().contains("0.4%"));
            });
            backend.release.countDown();
            await(() -> label(panel).startsWith("Read complete") && button(panel, "readFlash").isEnabled());
        } finally { close(panel, backend); }
    }

    @Test void selectedWriteUsesEachTransportWithoutABundledSrec() throws Exception {
        for (int transport = 0; transport < 3; transport++) {
            Backend backend = new Backend();
            M749Panel panel = open(backend, (parent, read) -> {
                assertFalse(read);
                return new M749Panel.Selection(Path.of("selected firmware.bin"), false, false);
            });
            try {
                await(() -> button(panel, "writeFlash").isEnabled());
                int selected = transport;
                SwingUtilities.invokeAndWait(() -> {
                    find(panel, JComboBox.class, "transferTransport").setSelectedIndex(selected);
                    find(panel, JTextField.class, "credential").setText("paired key.pair");
                });
                await(() -> button(panel, "writeFlash").isEnabled());
                SwingUtilities.invokeAndWait(() -> button(panel, "writeFlash").doClick());
                assertTrue(backend.entered.await(5, TimeUnit.SECONDS));
                String option = transport == 0 ? "--channel" : transport == 1 ? "--slcan" : "--socketcan";
                String value = transport == 0 ? "PCAN_USBBUS2" : transport == 1 ? "auto" : "can0";
                java.util.ArrayList<String> expected = new java.util.ArrayList<>(List.of("--write-flash", "selected firmware.bin", option, value));
                if (transport == 1) expected.addAll(List.of("--serial-baud", "115200", "--slcan-bus", "1"));
                expected.addAll(List.of("--block-size", "16", "--stmin", transport == 1 ? "3" : "1", "--pair-file", "paired key.pair"));
                assertEquals(expected, backend.args);
                backend.release.countDown();
                await(() -> label(panel).startsWith("Write complete"));
                SwingUtilities.invokeAndWait(() -> assertEquals("Installed firmware: unknown",
                        find(panel, JLabel.class, "firmwareStatus").getText()));
            } finally { close(panel, backend); }
        }
    }

    @Test void cancelledDialogDoesNothingAndFailureReenablesControls() throws Exception {
        Backend cancelled = new Backend();
        M749Panel panel = open(cancelled, (parent, read) -> null);
        try {
            await(() -> button(panel, "readFlash").isEnabled());
            SwingUtilities.invokeAndWait(() -> button(panel, "readFlash").doClick());
            assertNull(cancelled.args);
        } finally { close(panel, cancelled); }
        Backend failed = new Backend();
        failed.fail = true;
        M749Panel failurePanel = open(failed, (parent, read) -> new M749Panel.Selection(Path.of("out.bin"), false, false));
        try {
            await(() -> button(failurePanel, "readFlash").isEnabled());
            SwingUtilities.invokeAndWait(() -> button(failurePanel, "readFlash").doClick());
            assertTrue(failed.entered.await(5, TimeUnit.SECONDS));
            failed.release.countDown();
            await(() -> label(failurePanel).startsWith("Read failed") && button(failurePanel, "writeFlash").isEnabled());
        } finally { close(failurePanel, failed); }
    }

    @Test void removalInterruptsTransfer() throws Exception {
        Backend backend = new Backend();
        M749Panel panel = open(backend, (parent, read) -> new M749Panel.Selection(Path.of("out.bin"), false, false));
        try {
            await(() -> button(panel, "readFlash").isEnabled());
            SwingUtilities.invokeAndWait(() -> button(panel, "readFlash").doClick());
            assertTrue(backend.entered.await(5, TimeUnit.SECONDS));
        } finally { SwingUtilities.invokeAndWait(panel::removeNotify); }
        assertTrue(backend.exited.await(5, TimeUnit.SECONDS));
    }

    private static M749Panel open(Backend backend, M749Panel.TransferChooser chooser) throws Exception {
        AtomicReference<M749Panel> panel = new AtomicReference<>();
        SwingUtilities.invokeAndWait(() -> {
            panel.set(new M749Panel(backend, () -> { throw new IOException("No bundle"); }, chooser));
            find(panel.get(), JComboBox.class, "transferTransport").setSelectedIndex(0);
            panel.get().addNotify();
        });
        return panel.get();
    }
    private static void close(M749Panel panel, Backend backend) throws Exception {
        SwingUtilities.invokeAndWait(panel::removeNotify);
        backend.release.countDown();
    }
    private static JButton button(M749Panel panel, String name) { return find(panel, JButton.class, name); }
    private static String label(M749Panel panel) { return find(panel, JLabel.class, "activity").getText(); }
    private static void await(BooleanSupplier condition) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        AtomicReference<Boolean> done = new AtomicReference<>(false);
        while (System.nanoTime() < deadline) {
            SwingUtilities.invokeAndWait(() -> done.set(condition.getAsBoolean()));
            if (done.get()) return;
            Thread.sleep(10);
        }
        fail("UI condition timed out");
    }
    private static <T extends Component> T find(Container root, Class<T> type, String name) {
        for (Component child : root.getComponents()) {
            if (type.isInstance(child) && name.equals(child.getName())) return type.cast(child);
            if (child instanceof Container) {
                T found = find((Container) child, type, name);
                if (found != null) return found;
            }
        }
        return null;
    }
}
