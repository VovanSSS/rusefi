package com.rusefi.m749;

import javax.swing.*;
import javax.swing.filechooser.FileNameExtensionFilter;
import javax.swing.text.BadLocationException;
import java.awt.*;
import java.nio.file.Path;
import java.util.List;
import java.util.function.Consumer;
import java.time.LocalTime;
import java.time.format.DateTimeFormatter;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public final class M749Panel extends JPanel {
    static final Color DETECTED_COLOR = new Color(0, 140, 45);
    static final Color MISSING_COLOR = new Color(190, 35, 35);
    private static final DateTimeFormatter TIME = DateTimeFormatter.ofPattern("HH:mm:ss");
    private final JLabel detection = new JLabel("SLCAN not detected");
    private final JLabel detail = new JLabel("Select a connector and endpoint; auto requires one adapter.");
    private final JLabel activity = new JLabel(" ");
    private final JButton retry = new JButton("Scan / query again");
    private final JLabel firmwareStatus = new JLabel("Installed firmware: unknown");
    private final JLabel imageLabel = new JLabel("SREC: searching...");
    private final JLabel uploadHint = new JLabel();
    private final JComboBox<PcanDevice.Channel> channels = new JComboBox<>();
    private final JTextField credential = new JTextField(28);
    private final JButton browseCredential = new JButton("Choose pair file / backup...");
    private final JButton flash = new JButton("Flash rusEFI");
    private final JButton readFlash = new JButton("Read flash...");
    private final JButton writeFlash = new JButton("Write firmware...");
    private final JComboBox<String> transferTransport = new JComboBox<>(new String[]{"PCAN", "SLCAN", "SocketCAN"});
    private final JTextField transferEndpoint = new JTextField("auto", 18);
    private final JTextField serialBaud = new JTextField("115200", 7);
    private final JTextField slcanBus = new JTextField("1", 2);
    private final JTextField blockSize = new JTextField("16", 3);
    private final JTextField stmin = new JTextField("auto", 4);
    private volatile M749ConnectionOptions selectedConnection;
    private volatile int selectionRevision;
    private int activeSelection;
    private boolean changingOptions;
    private final TransferChooser transferChooser;
    private final JTextArea status = new JTextArea();
    private final JTextArea messages = new JTextArea();
    private final M749Monitor.Backend backend;
    private final M749FirmwareFile.Locator firmwareLocator;
    private Path imagePath;
    private M749FirmwareDetection.Result installed = M749FirmwareDetection.Result.UNKNOWN;
    private boolean changingChannels;
    private boolean querying;
    private volatile boolean uploading;
    private volatile boolean autoSelect = true;
    private volatile String selectedChannel;
    private ScheduledExecutorService worker;
    private M749Monitor monitor;
    private volatile int generation;

    public M749Panel() {
        this(M749Monitor.canBackend());
    }

    M749Panel(M749Monitor.Backend backend) {
        this(backend, M749FirmwareFile::locate);
    }

    M749Panel(M749Monitor.Backend backend, M749FirmwareFile.Locator firmwareLocator) {
        this(backend, firmwareLocator, M749Panel::chooseTransfer);
    }

    interface TransferChooser {
        Selection choose(Component parent, boolean read);
    }

    static final class Selection {
        final Path path;
        final boolean resume, helperRunning;
        Selection(Path path, boolean resume, boolean helperRunning) {
            this.path = path;
            this.resume = resume;
            this.helperRunning = helperRunning;
        }
    }

    M749Panel(M749Monitor.Backend backend, M749FirmwareFile.Locator firmwareLocator, TransferChooser transferChooser) {
        super(new BorderLayout(8, 8));
        this.backend = backend;
        this.firmwareLocator = firmwareLocator;
        this.transferChooser = transferChooser;
        setBorder(BorderFactory.createEmptyBorder(12, 12, 12, 12));
        detection.setForeground(MISSING_COLOR);
        detection.setFont(detection.getFont().deriveFont(Font.BOLD, 20f));
        JPanel top = new JPanel();
        top.setLayout(new BoxLayout(top, BoxLayout.Y_AXIS));
        top.add(detection);
        top.add(Box.createVerticalStrut(8));
        top.add(detail);
        top.add(Box.createVerticalStrut(12));
        top.add(new JLabel("M74.9 identification and firmware update"));
        top.add(new JLabel("CAN: 500 kbit/s   Request: 0x7E0   Response: 0x7E8"));
        top.add(Box.createVerticalStrut(12));
        JPanel adapterRow = new JPanel(new FlowLayout(FlowLayout.LEFT, 0, 0));
        adapterRow.add(new JLabel("Connector: "));
        transferTransport.setName("transferTransport");
        transferTransport.setSelectedIndex(1);
        adapterRow.add(transferTransport);
        transferEndpoint.setName("transferEndpoint");
        transferEndpoint.setToolTipText("SLCAN serial port or auto; SocketCAN interface already up at 500 kbit/s. Press Enter to query.");
        adapterRow.add(transferEndpoint);
        channels.setName("channels");
        adapterRow.add(channels);
        adapterRow.add(Box.createHorizontalStrut(8));
        adapterRow.add(retry);
        adapterRow.setMaximumSize(new Dimension(Integer.MAX_VALUE, adapterRow.getPreferredSize().height));
        top.add(adapterRow);
        JPanel settings = new JPanel(new FlowLayout(FlowLayout.LEFT, 4, 0));
        JTextField[] fields = {serialBaud, slcanBus, blockSize, stmin};
        String[] names = {"serialBaud", "slcanBus", "blockSize", "stmin"};
        String[] labels = {"Serial baud:", "SLCAN bus:", "Receive block:", "STmin ms:"};
        for (int i = 0; i < fields.length; i++) {
            fields[i].setName(names[i]);
            settings.add(new JLabel(labels[i]));
            settings.add(fields[i]);
        }
        settings.setMaximumSize(new Dimension(Integer.MAX_VALUE, settings.getPreferredSize().height));
        top.add(settings);
        firmwareStatus.setName("firmwareStatus");
        firmwareStatus.setFont(detail.getFont().deriveFont(Font.BOLD));
        top.add(firmwareStatus);
        imageLabel.setName("firmwareImage");
        top.add(imageLabel);
        JPanel credentialRow = new JPanel(new FlowLayout(FlowLayout.LEFT, 0, 4));
        credentialRow.add(new JLabel("OEM credentials (optional): "));
        credential.setName("credential");
        credential.setToolTipText("Pair file or original paired backup for OEM installation. Not needed for a rusEFI update.");
        credentialRow.add(credential);
        credentialRow.add(Box.createHorizontalStrut(8));
        credentialRow.add(browseCredential);
        credentialRow.setMaximumSize(new Dimension(Integer.MAX_VALUE, credentialRow.getPreferredSize().height));
        top.add(credentialRow);
        top.add(uploadHint);
        flash.setName("flash");
        flash.setEnabled(false);
        top.add(flash);
        JPanel transferRow = new JPanel(new FlowLayout(FlowLayout.LEFT, 0, 4));
        readFlash.setName("readFlash");
        writeFlash.setName("writeFlash");
        transferRow.add(readFlash);
        transferRow.add(writeFlash);
        transferRow.setMaximumSize(new Dimension(Integer.MAX_VALUE, transferRow.getPreferredSize().height));
        top.add(transferRow);
        top.add(new JLabel("Read: OEM full backup. Write: rusEFI HEX/SREC or OEM BIN software + calibration."));
        top.add(Box.createVerticalStrut(8));
        activity.setName("activity");
        top.add(activity);
        top.add(Box.createVerticalStrut(8));
        status.setName("status");
        status.setEditable(false);
        status.setOpaque(false);
        status.setFocusable(false);
        status.setFont(detail.getFont().deriveFont(Font.BOLD));
        top.add(status);
        for (Component component : top.getComponents()) {
            if (component instanceof JComponent) {
                ((JComponent) component).setAlignmentX(Component.LEFT_ALIGNMENT);
            }
        }

        messages.setName("messages");
        messages.setEditable(false);
        messages.setFont(new Font(Font.MONOSPACED, Font.PLAIN, 12));
        messages.setRows(14);
        JTabbedPane lower = new JTabbedPane();
        lower.addTab("Messages", new JScrollPane(messages));
        JSplitPane split = new JSplitPane(JSplitPane.VERTICAL_SPLIT, top, lower);
        split.setResizeWeight(0.35);
        split.setBorder(null);
        add(split, BorderLayout.CENTER);
        retry.addActionListener(event -> { selectionChanged(); queryAgain(); });
        channels.addActionListener(event -> {
            if (changingChannels) return;
            PcanDevice.Channel channel = (PcanDevice.Channel) channels.getSelectedItem();
            selectedChannel = channel == null ? null : channel.handle.name();
            autoSelect = false;
            selectionChanged();
            queryAgain();
        });
        browseCredential.addActionListener(event -> {
            JFileChooser chooser = new JFileChooser();
            chooser.setFileFilter(new FileNameExtensionFilter("ECU pair file or original backup", "pair", "bin"));
            if (chooser.showOpenDialog(this) == JFileChooser.APPROVE_OPTION) {
                credential.setText(chooser.getSelectedFile().getAbsolutePath());
            }
        });
        flash.addActionListener(event -> flashFirmware());
        readFlash.addActionListener(event -> transferFile(true));
        writeFlash.addActionListener(event -> transferFile(false));
        transferTransport.addActionListener(event -> {
            transferEndpoint.setText(transferTransport.getSelectedIndex() == 2 ? "can0" : "auto");
            selectionChanged();
            queryAgain();
        });
        for (JTextField field : new JTextField[]{transferEndpoint, serialBaud, slcanBus, blockSize, stmin}) {
            field.addActionListener(event -> { selectionChanged(); queryAgain(); });
            field.addFocusListener(new java.awt.event.FocusAdapter() {
                @Override public void focusLost(java.awt.event.FocusEvent event) {
                    if (selectedConnection == null) { selectionChanged(); queryAgain(); }
                }
            });
            field.getDocument().addDocumentListener(new javax.swing.event.DocumentListener() {
                public void insertUpdate(javax.swing.event.DocumentEvent e) { edited(); }
                public void removeUpdate(javax.swing.event.DocumentEvent e) { edited(); }
                public void changedUpdate(javax.swing.event.DocumentEvent e) { edited(); }
                private void edited() {
                    if (changingOptions) return;
                    selectedConnection = null;
                    selectionRevision++;
                    querying = false;
                    setFirmware(M749FirmwareDetection.Result.UNKNOWN);
                    status.setText("");
                }
            });
        }
        selectionChanged();
    }

    private M749ConnectionOptions connectionOptions() {
        M749ConnectionOptions options = new M749ConnectionOptions();
        int connector = transferTransport.getSelectedIndex();
        options.accept(connector == 0 ? "--channel" : connector == 1 ? "--slcan" : "--socketcan",
                connector == 0 ? selectedChannel == null ? "auto" : selectedChannel : transferEndpoint.getText().trim());
        if (connector == 1) {
            options.accept("--serial-baud", serialBaud.getText().trim());
            options.accept("--slcan-bus", slcanBus.getText().trim());
        }
        options.accept("--block-size", blockSize.getText().trim());
        if (!stmin.getText().trim().equalsIgnoreCase("auto")) options.accept("--stmin", stmin.getText().trim());
        options.validate();
        return options;
    }

    private void selectionChanged() {
        if (changingOptions) return;
        M749ConnectionOptions next;
        try { next = connectionOptions(); }
        catch (IllegalArgumentException e) {
            selectedConnection = null;
            detail.setText(e.getMessage());
            setFirmware(M749FirmwareDetection.Result.UNKNOWN);
            return;
        }
        if (selectedConnection == null || !selectedConnection.key().equals(next.key())) {
            selectionRevision++;
            querying = false;
            setFirmware(M749FirmwareDetection.Result.UNKNOWN);
            status.setText("");
            detection.setText(next.connector() + " not detected");
            detection.setForeground(MISSING_COLOR);
        }
        selectedConnection = next;
        updateControls();
    }

    private void queryAgain() {
        if (worker == null || uploading || querying || selectedConnection == null) return;
        querying = true;
        updateControls();
        int current = generation;
        M749Monitor activeMonitor = monitor;
        worker.execute(() -> {
            refreshFirmware(current);
            poll(activeMonitor, current, true);
            onEdt(current, () -> { querying = false; updateControls(); });
        });
    }

    private void refreshFirmware(int current) {
        try {
            Path path = firmwareLocator.locate();
            onEdt(current, () -> {
                imagePath = path;
                imageLabel.setText("SREC: " + path.getFileName());
                imageLabel.setToolTipText(path.toString());
                appendMessage("Firmware image: " + path);
                updateControls();
            });
        } catch (Exception e) {
            onEdt(current, () -> {
                imagePath = null;
                imageLabel.setText("SREC unavailable: " + e.getMessage());
                imageLabel.setToolTipText(null);
                updateControls();
            });
        }
    }

    private void showChannels(List<PcanDevice.Channel> available) {
        boolean unchanged = channels.getItemCount() == available.size();
        for (int i = 0; unchanged && i < available.size(); i++) {
            PcanDevice.Channel previous = channels.getItemAt(i);
            PcanDevice.Channel next = available.get(i);
            unchanged = previous.handle == next.handle && previous.available == next.available;
        }
        if (unchanged) return;
        changingChannels = true;
        try {
            String previous = selectedChannel;
            channels.removeAllItems();
            PcanDevice.Channel selection = null;
            for (PcanDevice.Channel channel : available) {
                channels.addItem(channel);
                if (channel.handle.name().equals(previous)) selection = channel;
            }
            // Do not switch a chosen upload target to a different adapter on disconnect.
            if (selection == null && autoSelect) {
                List<PcanDevice.Channel> usable = new java.util.ArrayList<>();
                for (PcanDevice.Channel candidate : available) if (candidate.available) usable.add(candidate);
                if (usable.size() == 1) selection = usable.get(0);
            }
            channels.setSelectedItem(selection);
            selectedChannel = selection == null ? previous : selection.handle.name();
            if (selection == null || !selection.available || !selection.handle.name().equals(previous)) {
                setFirmware(M749FirmwareDetection.Result.UNKNOWN);
                status.setText("");
            }
        } finally {
            changingChannels = false;
        }
        updateControls();
    }

    private void setFirmware(M749FirmwareDetection.Result result) {
        installed = result;
        uploadHint.setText(result.m749 ? "rusEFI updates use the resident loader; no pair file or startup power cycle is needed."
                : "For OEM authorization, choose your pair file and cycle ECU power when Messages asks.");
        String label = result == M749FirmwareDetection.Result.UNKNOWN ? "Installed firmware: unknown"
                : result == M749FirmwareDetection.Result.OEM ? "OEM firmware installed"
                : "rusEFI installed - " + (result == M749FirmwareDetection.Result.M749_READY ? "ready to update"
                : result == M749FirmwareDetection.Result.M749_NOT_READY ? "activation not ready"
                : "M74.9 update support not confirmed");
        firmwareStatus.setText(label);
        firmwareStatus.setForeground(result.m749 ? DETECTED_COLOR : getForeground());
        flash.setText(result.m749 ? "Update rusEFI" : "Flash rusEFI");
        updateControls();
    }

    private void updateControls() {
        boolean idle = worker != null && !uploading && !querying;
        retry.setEnabled(idle);
        boolean pcan = transferTransport.getSelectedIndex() == 0;
        boolean slcan = transferTransport.getSelectedIndex() == 1;
        channels.setVisible(pcan);
        channels.setEnabled(idle && pcan);
        transferEndpoint.setVisible(!pcan);
        serialBaud.setEnabled(idle && slcan);
        slcanBus.setEnabled(idle && slcan);
        blockSize.setEnabled(idle);
        stmin.setEnabled(idle);
        boolean needsCredential = !installed.m749;
        credential.setEnabled(idle && needsCredential);
        browseCredential.setEnabled(idle && needsCredential);
        PcanDevice.Channel channel = (PcanDevice.Channel) channels.getSelectedItem();
        flash.setEnabled(idle && selectedConnection != null && imagePath != null && (!pcan || channel != null && channel.available)
                && installed != M749FirmwareDetection.Result.RUSEFI);
        transferTransport.setEnabled(idle);
        transferEndpoint.setEnabled(idle && transferTransport.getSelectedIndex() != 0);
        boolean canTransfer = idle && selectedConnection != null && (!pcan || channel != null && channel.available);
        readFlash.setEnabled(canTransfer);
        writeFlash.setEnabled(canTransfer && installed != M749FirmwareDetection.Result.RUSEFI);
    }

    private static Selection chooseTransfer(Component parent, boolean read) {
        JFileChooser chooser = new JFileChooser();
        chooser.setDialogTitle(read ? "Save full OEM flash backup" : "Write firmware - select HEX, SREC or OEM BIN");
        chooser.setFileFilter(read ? new FileNameExtensionFilter("Flash backup (*.bin)", "bin") :
                new FileNameExtensionFilter("Firmware (*.hex, *.srec, *.bin)", "hex", "srec", "s19", "s28", "s37", "bin"));
        JCheckBox resume = new JCheckBox("Resume saved partial backup");
        JCheckBox running = new JCheckBox("RAM helper is already running");
        if (read) {
            chooser.setSelectedFile(new java.io.File("m749-full-" + java.time.Instant.now().toString().replaceAll("[:.]", "-") + ".bin"));
            JPanel options = new JPanel(new GridLayout(0, 1));
            options.add(resume);
            options.add(running);
            chooser.setAccessory(options);
        }
        int result = read ? chooser.showSaveDialog(parent) : chooser.showOpenDialog(parent);
        if (result != JFileChooser.APPROVE_OPTION) return null;
        Path path = chooser.getSelectedFile().toPath();
        if (read && !path.getFileName().toString().toLowerCase(java.util.Locale.ROOT).endsWith(".bin")) {
            path = path.resolveSibling(path.getFileName() + ".bin");
        }
        return new Selection(path, resume.isSelected(), running.isSelected());
    }

    private void transferFile(boolean read) {
        if (!(read ? readFlash : writeFlash).isEnabled()) return;
        Selection selection = transferChooser.choose(this, read);
        if (selection == null || worker == null) return;
        java.util.ArrayList<String> args = new java.util.ArrayList<>();
        args.add(read ? "--read-flash" : "--write-flash");
        args.add(selection.path.toString());
        M749ConnectionOptions options = selectedConnection;
        if (options == null) return;
        args.addAll(options.arguments());
        if (read) {
            args.add("--reset-after");
            if (selection.resume) args.add("--resume");
            if (selection.helperRunning) args.add("--helper-running");
        }
        String credentialText = credential.isEnabled() ? credential.getText().trim() : "";
        if (!credentialText.isEmpty() && !(read && selection.helperRunning)) {
            args.add(credentialText.toLowerCase(java.util.Locale.ROOT).endsWith(".pair") ? "--pair-file" : "--immo-backup");
            args.add(credentialText);
        }
        uploading = true;
        updateControls();
        String operation = read ? "Read" : "Write";
        activity.setText(operation + " in progress - keep ECU power and CAN connected.");
        status.setText("");
        int current = generation;
        long start = System.nanoTime();
        Consumer<String> log = message -> onEdt(current, () -> appendMessage(String.format(java.util.Locale.ROOT,
                "[%4d] %s", (System.nanoTime() - start) / 1_000_000_000L, message)));
        worker.execute(() -> {
            synchronized (backend) {
                try {
                    if (generation != current || Thread.currentThread().isInterrupted()) return;
                    log.accept(operation + ": " + selection.path);
                    int result = backend.transfer(args.toArray(new String[0]), log);
                    if (result != 0) throw new java.io.IOException("Command returned " + result + "; see Messages");
                    onEdt(current, () -> activity.setText(operation + " complete - see Messages for verification details."));
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    log.accept(operation + " interrupted; check ECU state before retrying.");
                    onEdt(current, () -> activity.setText(operation + " interrupted"));
                } catch (Exception | LinkageError e) {
                    log.accept(operation + " failed: " + e.getMessage());
                    onEdt(current, () -> activity.setText(operation + " failed: " + e.getMessage()));
                } finally {
                    onEdt(current, () -> {
                        setFirmware(M749FirmwareDetection.Result.UNKNOWN);
                        uploading = false;
                        updateControls();
                    });
                }
            }
        });
    }

    private void flashFirmware() {
        if (!flash.isEnabled()) return;
        M749ConnectionOptions options = selectedConnection.copy();
        Path image = imagePath;
        String credentialText = installed.m749 ? "" : credential.getText().trim();
        uploading = true;
        updateControls();
        activity.setText("Flashing firmware - keep ECU power and CAN connected.");
        int current = generation;
        M749Monitor activeMonitor = monitor;
        long start = System.nanoTime();
        Consumer<String> log = message -> {
            String line = String.format(java.util.Locale.ROOT, "[%4d] %s",
                    (System.nanoTime() - start) / 1_000_000_000L, message);
            onEdt(current, () -> appendMessage(line));
        };
        worker.execute(() -> {
            synchronized (backend) {
                try {
                    if (generation != current || Thread.currentThread().isInterrupted()) return;
                    log.accept("Flashing " + image + " on " + options.connector() + " " + options.endpoint());
                    activeMonitor.flash(options, image, credentialText.isEmpty() ? null : Path.of(credentialText), log);
                    onEdt(current, () -> {
                        setFirmware(M749FirmwareDetection.Result.M749_READY);
                        status.setText("Upload complete: CRCs and boot marker verified after reset.");
                        activity.setText("Upload complete");
                    });
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    log.accept("Upload interrupted; check ECU state before retrying.");
                    onEdt(current, () -> uploadFailed("Upload interrupted"));
                } catch (Exception | LinkageError e) {
                    log.accept("Upload failed: " + e.getMessage());
                    onEdt(current, () -> uploadFailed("Upload failed: " + e.getMessage()));
                } finally {
                    onEdt(current, () -> { uploading = false; updateControls(); });
                }
            }
        });
    }

    private void uploadFailed(String message) {
        setFirmware(M749FirmwareDetection.Result.UNKNOWN);
        status.setText("");
        activity.setText(message);
    }

    @Override
    public void addNotify() {
        super.addNotify();
        if (worker != null) return;
        int current = ++generation;
        querying = false;
        uploading = false;
        setFirmware(M749FirmwareDetection.Result.UNKNOWN);
        activity.setText(" ");
        status.setText("");
        monitor = new M749Monitor(backend, new M749Monitor.View() {
            public void detection(boolean detected, String text) {
                onConnectionEdt(current, () -> {
                    String connector = (String) transferTransport.getSelectedItem();
                    detection.setText(connector + (detected ? " detected" : " not detected"));
                    detection.setForeground(detected ? DETECTED_COLOR : MISSING_COLOR);
                    detail.setText(text);
                    detail.setToolTipText(text);
                });
            }

            public void identification(java.util.List<String> summary) {
                onConnectionEdt(current, () -> status.setText(String.join("\n", summary)));
            }

            public void channels(List<PcanDevice.Channel> available) {
                onConnectionEdt(current, () -> showChannels(available));
            }

            public void connection(M749ConnectionOptions options, M749Monitor.Identification result) {
                onConnectionEdt(current, () -> {
                    if (uploading) return;
                    changingOptions = true;
                    changingChannels = true;
                    try {
                        if (options.channel != null && !options.channel.equalsIgnoreCase("auto")) {
                            selectedChannel = options.channel;
                            autoSelect = false;
                            for (int i = 0; i < channels.getItemCount(); i++) {
                                if (channels.getItemAt(i).handle.name().equals(options.channel)) channels.setSelectedIndex(i);
                            }
                        } else if (options.channel == null) transferEndpoint.setText(options.endpoint());
                        selectedConnection = options.copy();
                    } finally { changingOptions = false; changingChannels = false; }
                    status.setText(String.join("\n", result.summary));
                    setFirmware(result.firmware);
                });
            }

            public void message(String message) {
                onConnectionEdt(current, () -> appendMessage(message));
            }

            public void busy(boolean busy) {
                onConnectionEdt(current, () -> {
                    querying = busy;
                    updateControls();
                    if (!uploading) activity.setText(busy ? "Reading ECU identification..." : " ");
                });
            }
        });
        worker = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "m749-can");
            thread.setDaemon(true);
            return thread;
        });
        M749Monitor activeMonitor = monitor;
        worker.execute(() -> refreshFirmware(current));
        worker.scheduleWithFixedDelay(() -> poll(activeMonitor, current, false), 0, 2, TimeUnit.SECONDS);
        updateControls();
    }

    private void poll(M749Monitor activeMonitor, int current, boolean force) {
        // A removed/reinserted panel waits for the previous query to release its channel.
        synchronized (backend) {
            if (generation == current && !uploading && !Thread.currentThread().isInterrupted()) {
                M749ConnectionOptions options = selectedConnection;
                if (options != null) {
                    activeSelection = selectionRevision;
                    activeMonitor.pollConnection(force, options);
                }
            }
        }
    }

    @Override
    public void removeNotify() {
        generation++;
        if (worker != null) {
            worker.shutdownNow();
            worker = null;
        }
        updateControls();
        super.removeNotify();
    }

    private void onConnectionEdt(int current, Runnable action) {
        int revision = activeSelection;
        onEdt(current, () -> { if (selectionRevision == revision) action.run(); });
    }

    private void onEdt(int current, Runnable action) {
        SwingUtilities.invokeLater(() -> {
            if (generation == current) action.run();
        });
    }

    private void appendMessage(String message) {
        messages.append("[" + TIME.format(LocalTime.now()) + "] " + message + "\n");
        // Keep a long-running console bounded, including repeated manual queries.
        if (messages.getDocument().getLength() > 200_000) {
            try {
                messages.getDocument().remove(0, 50_000);
            } catch (BadLocationException e) {
                throw new IllegalStateException(e);
            }
        }
        messages.setCaretPosition(messages.getDocument().getLength());
    }
}
