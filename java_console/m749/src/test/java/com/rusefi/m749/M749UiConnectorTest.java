package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import peak.can.basic.TPCANHandle;
import javax.swing.*;
import java.awt.*;
import java.nio.file.Path;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import static org.junit.jupiter.api.Assertions.*;

class M749UiConnectorTest {
    @TempDir Path directory;
    private static class Backend implements M749Monitor.Backend {
        volatile int scans, queries;
        volatile M749ConnectionOptions identified, flashed;
        volatile M749FirmwareDetection.Result firmware = M749FirmwareDetection.Result.M749_READY;
        public List<PcanDevice.Channel> scan() {
            scans++;
            return List.of(new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS2, true));
        }
        public List<String> identify(PcanDevice.Channel channel, Consumer<String> out) { throw new AssertionError("Legacy identification"); }
        public M749Monitor.Identification inspect(M749ConnectionOptions options, Consumer<String> out) throws InterruptedException {
            assertFalse(SwingUtilities.isEventDispatchThread());
            queries++;
            if ("auto".equals(options.slcan)) options.slcan = "COM42";
            if ("auto".equals(options.channel)) options.channel = "PCAN_USBBUS2";
            identified = options.copy();
            return new M749Monitor.Identification(firmware, List.of(options.connector() + " identified"));
        }
        public void flash(M749ConnectionOptions options, M749Image image, M749Immo immo, Consumer<String> out) {
            assertFalse(SwingUtilities.isEventDispatchThread());
            assertEquals(M749Image.Domain.SOFTWARE, image.domain);
            assertNull(immo, "Recognized rusEFI must not load stale credentials");
            flashed = options.copy();
            out.accept("Upload complete");
        }
    }

    @Test void automaticIdentificationAndBundledUpdateUseSameConnectorAndOptions() throws Exception {
        Path image = M749UiFlashTest.writeSoftware(directory);
        for (int connector = 0; connector < 3; connector++) {
            Backend backend = new Backend();
            M749Panel panel = open(backend, image, connector);
            try {
                await(() -> find(panel, JLabel.class, "firmwareStatus").getText().contains("ready to update") && button(panel, "flash").isEnabled());
                SwingUtilities.invokeAndWait(() -> {
                    find(panel, JTextField.class, "credential").setText("missing credentials.pair");
                    button(panel, "flash").doClick();
                });
                await(() -> find(panel, JLabel.class, "activity").getText().equals("Upload complete"));
                assertNotNull(backend.flashed);
                assertEquals(backend.identified.key(), backend.flashed.key());
                assertEquals(8, backend.flashed.block);
                assertEquals(4, backend.flashed.stmin);
                if (connector == 0) assertEquals("PCAN_USBBUS2", backend.flashed.channel);
                else {
                    assertEquals(0, backend.scans, "Non-PCAN operations must not load PCAN");
                    if (connector == 1) assertEquals("COM42", backend.flashed.slcan);
                    else assertEquals("can0", backend.flashed.socketcan);
                }
            } finally { SwingUtilities.invokeAndWait(panel::removeNotify); }
        }
    }

    @Test void connectorChangeClearsOldFirmwareAndIgnoresLateIdentification() throws Exception {
        CountDownLatch started = new CountDownLatch(1), release = new CountDownLatch(1);
        Backend backend = new Backend() {
            @Override public M749Monitor.Identification inspect(M749ConnectionOptions options, Consumer<String> out) throws InterruptedException {
                if (options.slcan != null) {
                    started.countDown();
                    assertTrue(release.await(5, TimeUnit.SECONDS));
                    return new M749Monitor.Identification(M749FirmwareDetection.Result.M749_READY, List.of("old ECU"));
                }
                firmware = M749FirmwareDetection.Result.RUSEFI;
                return super.inspect(options, out);
            }
        };
        M749Panel panel = open(backend, M749UiFlashTest.writeSoftware(directory), 1);
        try {
            assertTrue(started.await(5, TimeUnit.SECONDS));
            SwingUtilities.invokeAndWait(() -> find(panel, JComboBox.class, "transferTransport").setSelectedIndex(2));
            release.countDown();
            await(() -> find(panel, JLabel.class, "firmwareStatus").getText().contains("update support not confirmed"));
            SwingUtilities.invokeAndWait(() -> {
                assertFalse(button(panel, "flash").isEnabled());
                assertFalse(button(panel, "writeFlash").isEnabled());
                assertFalse(find(panel, JTextArea.class, "status").getText().contains("old ECU"));
            });
            assertEquals("can0", backend.identified.socketcan);
        } finally { release.countDown(); SwingUtilities.invokeAndWait(panel::removeNotify); }
    }

    @Test void invalidTransportSettingsDisableWritesUntilCorrected() throws Exception {
        Backend backend = new Backend();
        M749Panel panel = open(backend, M749UiFlashTest.writeSoftware(directory), 1);
        try {
            await(() -> button(panel, "flash").isEnabled() && backend.identified != null);
            SwingUtilities.invokeAndWait(() -> {
                JTextField stmin = find(panel, JTextField.class, "stmin");
                stmin.setText("128");
                stmin.postActionEvent();
                assertFalse(button(panel, "flash").isEnabled());
                assertFalse(button(panel, "writeFlash").isEnabled());
                stmin.setText("7");
                stmin.postActionEvent();
            });
            await(() -> backend.identified.stmin == 7 && button(panel, "flash").isEnabled());
        } finally { SwingUtilities.invokeAndWait(panel::removeNotify); }
    }

    private static M749Panel open(Backend backend, Path image, int connector) throws Exception {
        AtomicReference<M749Panel> ref = new AtomicReference<>();
        SwingUtilities.invokeAndWait(() -> {
            M749Panel panel = new M749Panel(backend, () -> image);
            find(panel, JComboBox.class, "transferTransport").setSelectedIndex(connector);
            find(panel, JTextField.class, "blockSize").setText("8");
            JTextField stmin = find(panel, JTextField.class, "stmin");
            stmin.setText("4");
            stmin.postActionEvent();
            ref.set(panel);
            panel.addNotify();
        });
        return ref.get();
    }
    private static JButton button(Container panel, String name) { return find(panel, JButton.class, name); }
    private static void await(BooleanSupplier condition) throws Exception {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        AtomicReference<Boolean> done = new AtomicReference<>(false);
        while (System.nanoTime() < end) {
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
