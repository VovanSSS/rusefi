package com.rusefi.m749;

import com.rusefi.ui.plugins.ConsoleTabProvider;
import org.junit.jupiter.api.Test;
import peak.can.basic.TPCANHandle;

import javax.swing.*;
import java.awt.*;
import java.util.Collections;
import java.util.List;
import java.util.ServiceLoader;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;

import static org.junit.jupiter.api.Assertions.*;

class M749PanelTest {
    @Test
    void providerIsDiscoverableWithoutNativeLibraries() throws Exception {
        ConsoleTabProvider provider = ServiceLoader.load(ConsoleTabProvider.class).iterator().next();
        assertEquals("M74.9", provider.getTitle());
        SwingUtilities.invokeAndWait(() -> {
            JComponent panel = provider.createTab(null);
            JLabel label = find(panel, JLabel.class, "SLCAN not detected");
            assertEquals(M749Panel.MISSING_COLOR, label.getForeground());
            JTabbedPane tabs = find(panel, JTabbedPane.class, null);
            assertEquals("Messages", tabs.getTitleAt(0));
            assertFalse(find(panel, JTextArea.class, "messages").isEditable());
            assertFalse(find(panel, JTextArea.class, "status").isEditable());
        });
    }

    @Test
    void queryRunsOffEdtDisplaysResultsAndIsCancelledOnRemoval() throws Exception {
        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch released = new CountDownLatch(1);
        AtomicReference<M749Panel> panel = new AtomicReference<>();
        M749Monitor.Backend backend = new M749Monitor.Backend() {
            public List<PcanDevice.Channel> scan() {
                assertFalse(SwingUtilities.isEventDispatchThread());
                return Collections.singletonList(new PcanDevice.Channel(TPCANHandle.PCAN_USBBUS1, true));
            }

            public List<String> identify(PcanDevice.Channel channel, Consumer<String> messages) throws InterruptedException {
                assertFalse(SwingUtilities.isEventDispatchThread());
                messages.accept("VIN (DID F190): TESTVIN1234567890");
                started.countDown();
                try {
                    new CountDownLatch(1).await();
                } finally {
                    released.countDown();
                }
                return Collections.emptyList();
            }
        };
        SwingUtilities.invokeAndWait(() -> {
            panel.set(new M749Panel(backend, () -> { throw new java.io.IOException("No test firmware"); }));
            find(panel.get(), JComboBox.class, "transferTransport").setSelectedIndex(0);
            panel.get().addNotify();
        });
        try {
            assertTrue(started.await(5, TimeUnit.SECONDS));
            SwingUtilities.invokeAndWait(() -> {
                JLabel label = find(panel.get(), JLabel.class, "PCAN detected");
                assertEquals(M749Panel.DETECTED_COLOR, label.getForeground());
                assertTrue(find(panel.get(), JTextArea.class, "messages").getText().contains("TESTVIN1234567890"));
                assertFalse(find(panel.get(), JButton.class, "Scan / query again").isEnabled());
            });
        } finally {
            SwingUtilities.invokeAndWait(() -> panel.get().removeNotify());
        }
        assertTrue(released.await(5, TimeUnit.SECONDS));
    }

    private static <T extends Component> T find(Container root, Class<T> type, String text) {
        for (Component child : root.getComponents()) {
            String caption = child instanceof JLabel ? ((JLabel) child).getText()
                    : child instanceof JButton ? ((JButton) child).getText() : child.getName();
            if (type.isInstance(child) && (text == null || text.equals(caption))) return type.cast(child);
            if (child instanceof Container) {
                T found = find((Container) child, type, text);
                if (found != null) return found;
            }
        }
        return null;
    }
}
