package com.rusefi.m749;

import java.io.IOException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.function.Consumer;

/** All methods run on one worker; callbacks are marshalled to Swing by the panel. */
final class M749Monitor {
    interface Backend {
        List<PcanDevice.Channel> scan() throws IOException;
        /** @return human-readable status lines for the queried ECU */
        List<String> identify(PcanDevice.Channel channel, Consumer<String> messages) throws IOException, InterruptedException;

        default Identification inspect(PcanDevice.Channel channel, Consumer<String> messages)
                throws IOException, InterruptedException {
            return new Identification(M749FirmwareDetection.Result.UNKNOWN, identify(channel, messages));
        }

        default void flash(PcanDevice.Channel channel, M749Image image, M749Immo credential, Consumer<String> messages)
                throws IOException, InterruptedException {
            throw new IOException("Flashing is unavailable for this adapter backend");
        }

        default int transfer(String[] args, Consumer<String> messages) throws IOException, InterruptedException {
            throw new IOException("File transfers are unavailable for this adapter backend");
        }

        default Identification inspect(M749ConnectionOptions options, Consumer<String> messages)
                throws IOException, InterruptedException {
            return inspect(selectedPcan(options), messages);
        }

        default void flash(M749ConnectionOptions options, M749Image image, M749Immo credential, Consumer<String> messages)
                throws IOException, InterruptedException {
            flash(selectedPcan(options), image, credential, messages);
        }

        default PcanDevice.Channel selectedPcan(M749ConnectionOptions options) throws IOException {
            if (options.channel == null) throw new IOException("Connector unavailable in this backend");
            List<PcanDevice.Channel> available = scan();
            String name = options.channel;
            if (name.equalsIgnoreCase("auto")) {
                List<String> names = new ArrayList<>();
                for (PcanDevice.Channel c : available) if (c.available) names.add(c.handle.name());
                name = M749ConnectionOptions.selectOnly(names, "PCAN", "--channel");
            }
            for (PcanDevice.Channel c : available) {
                if (c.available && c.handle.name().equalsIgnoreCase(name)) { options.channel = c.handle.name(); return c; }
            }
            throw new IOException("PCAN channel unavailable: " + name);
        }
    }

    static final class Identification {
        final M749FirmwareDetection.Result firmware;
        final List<String> summary;

        Identification(M749FirmwareDetection.Result firmware, List<String> summary) {
            this.firmware = firmware;
            this.summary = summary;
        }
    }

    interface View {
        void detection(boolean detected, String detail);
        void identification(List<String> summary);
        default void identification(PcanDevice.Channel channel, List<String> summary) { identification(summary); }
        void message(String message);
        void busy(boolean busy);
        default void channels(List<PcanDevice.Channel> channels) { }
        default void firmware(PcanDevice.Channel channel, M749FirmwareDetection.Result firmware) { }
        default void connection(M749ConnectionOptions options, Identification result) { }
    }

    private final Backend backend;
    private final View view;
    private final Set<String> attempted = new HashSet<>();
    private String previousScan = "";
    private String connectionKey = "";
    private long connectionAttempt;
    private boolean connectionReady;

    M749Monitor(Backend backend, View view) {
        this.backend = backend;
        this.view = view;
    }

    static Backend canBackend() {
        return canBackend(action -> action.run());
    }

    interface TransferAction {
        int run() throws IOException, InterruptedException;
    }

    interface TransferAccess {
        int run(TransferAction action) throws IOException, InterruptedException;
    }

    static Backend canBackend(TransferAccess access) {
        return canBackend(access, M749ConnectionOptions::open);
    }

    interface ConnectionFactory {
        RawCanTransport open(M749ConnectionOptions options, Consumer<String> messages) throws IOException;
    }

    static Backend canBackend(TransferAccess access, ConnectionFactory factory) {
        PcanDevice device = new PcanDevice();
        return new Backend() {
            public List<PcanDevice.Channel> scan() throws IOException {
                return device.scan();
            }

            public List<String> identify(PcanDevice.Channel channel, Consumer<String> messages)
                    throws IOException, InterruptedException {
                return inspect(channel, messages).summary;
            }

            public Identification inspect(PcanDevice.Channel channel, Consumer<String> messages)
                    throws IOException, InterruptedException {
                M749ConnectionOptions options = new M749ConnectionOptions();
                options.accept("--channel", channel.handle.name());
                options.validate();
                return inspect(options, messages);
            }

            public Identification inspect(M749ConnectionOptions options, Consumer<String> messages)
                    throws IOException, InterruptedException {
                Identification[] result = new Identification[1];
                access.run(() -> {
                    try (RawCanTransport transport = factory.open(options, messages)) {
                        result[0] = M749EcuProbe.inspect(options.client(transport), messages);
                    }
                    return 0;
                });
                return result[0];
            }

            public void flash(PcanDevice.Channel channel, M749Image image, M749Immo credential, Consumer<String> messages)
                    throws IOException, InterruptedException {
                M749ConnectionOptions options = new M749ConnectionOptions();
                options.accept("--channel", channel.handle.name());
                options.validate();
                flash(options, image, credential, messages);
            }

            public void flash(M749ConnectionOptions options, M749Image image, M749Immo credential, Consumer<String> messages)
                    throws IOException, InterruptedException {
                access.run(() -> {
                    try (RawCanTransport transport = factory.open(options, messages)) {
                        if (credential != null) credential.authorize(transport, messages);
                        new M749Uploader(options.client(transport), messages).upload(image, false);
                    }
                    return 0;
                });
            }

            public int transfer(String[] args, Consumer<String> messages) throws IOException, InterruptedException {
                return access.run(() -> M749Cli.execute(args, this, M749Cli::upload, messages));
            }
        };
    }

    void pollConnection(boolean force, M749ConnectionOptions requested) {
        if (requested.channel != null) {
            try {
                List<PcanDevice.Channel> available = backend.scan();
                view.channels(available);
                List<String> usable = new ArrayList<>();
                for (PcanDevice.Channel c : available) if (c.available) usable.add(c.handle.name());
                if (requested.channel.equalsIgnoreCase("auto")) {
                    M749ConnectionOptions.selectOnly(usable, "PCAN", "--channel");
                } else if (!usable.contains(requested.channel.toUpperCase(java.util.Locale.ROOT))) {
                    throw new IOException("Selected PCAN channel is unavailable: " + requested.channel);
                }
                view.detection(true, "PCAN channels: " + available);
            } catch (IOException | RuntimeException | LinkageError e) {
                connectionReady = false;
                view.detection(false, e.getMessage());
                view.connection(requested, new Identification(M749FirmwareDetection.Result.UNKNOWN, java.util.Collections.emptyList()));
                reportScan("PCAN discovery: " + e.getMessage());
                return;
            }
        }
        String key = requested.key();
        long now = System.nanoTime();
        if (!force && key.equals(connectionKey) && (connectionReady || now - connectionAttempt < 10_000_000_000L)) return;
        connectionKey = key;
        connectionAttempt = now;
        connectionReady = false;
        view.busy(true);
        try {
            M749ConnectionOptions selected = requested.copy();
            view.message("Querying M74.9 via " + selected.connector() + " " + selected.endpoint());
            Identification result = backend.inspect(selected, view::message);
            connectionKey = selected.key();
            connectionReady = true;
            view.detection(true, selected.connector() + " " + selected.endpoint() + ": ECU identified");
            view.connection(selected, result);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (IOException | RuntimeException | LinkageError e) {
            view.detection(false, requested.connector() + " identification failed: " + e.getMessage());
            view.connection(requested, new Identification(M749FirmwareDetection.Result.UNKNOWN, java.util.Collections.emptyList()));
            view.message("Identification failed: " + e.getMessage());
        } finally { view.busy(false); }
    }

    void poll(boolean retry) {
        poll(retry, null);
    }

    void poll(boolean retry, String requestedChannel) {
        List<PcanDevice.Channel> channels;
        try {
            channels = backend.scan();
        } catch (IOException | RuntimeException | LinkageError e) {
            String error = e instanceof LinkageError
                    ? "PCAN native library unavailable. Install the PCAN driver and matching PCAN-Basic/JNI libraries, then restart."
                    : "PCAN scan failed: " + e.getMessage();
            view.detection(false, error);
            view.channels(java.util.Collections.emptyList());
            reportScan(error);
            // An API error is not proof of unplugging: do not reauthenticate automatically.
            return;
        }
        Set<String> present = new HashSet<>();
        for (PcanDevice.Channel channel : channels) present.add(channel.handle.name());
        attempted.retainAll(present);
        view.channels(channels);
        String detail = channels.isEmpty() ? "Connect a PCAN adapter to begin." : channels.toString();
        view.detection(!channels.isEmpty(), detail);
        reportScan(channels.isEmpty() ? "PCAN not detected" : "PCAN detected: " + detail);
        List<PcanDevice.Channel> candidates = new ArrayList<>();
        for (PcanDevice.Channel channel : channels) {
            if (channel.available && (requestedChannel == null || channel.handle.name().equals(requestedChannel))
                    && (retry || !attempted.contains(channel.handle.name()))) {
                candidates.add(channel);
            }
        }
        if (candidates.isEmpty()) {
            return;
        }
        view.busy(true);
        try {
            // The driver can report phantom channels (e.g. ISA with no driver), so
            // keep trying until one yields an ECU.
            for (PcanDevice.Channel selected : candidates) {
                attempted.add(selected.handle.name());
                view.message("Querying M74.9 via " + selected.handle + " at 500 kbit/s (7E0 / 7E8)");
                view.firmware(selected, M749FirmwareDetection.Result.UNKNOWN);
                try {
                    Identification result = backend.inspect(selected, view::message);
                    view.identification(selected, result.summary);
                    view.firmware(selected, result.firmware);
                    return;
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                } catch (IOException | RuntimeException | LinkageError e) {
                    view.message("Identification via " + selected.handle + " failed: " + e.getMessage());
                }
            }
            view.message("No channel produced an M74.9 identification. Use Scan / query again to retry.");
        } finally {
            view.busy(false);
        }
    }

    void flash(PcanDevice.Channel channel, Path firmware, Path credential, Consumer<String> messages)
            throws IOException, InterruptedException {
        M749ConnectionOptions options = new M749ConnectionOptions();
        options.accept("--channel", channel.handle.name());
        options.validate();
        flash(options, firmware, credential, messages);
    }

    void flash(M749ConnectionOptions options, Path firmware, Path credential, Consumer<String> messages)
            throws IOException, InterruptedException {
        // Validate everything before opening the channel or sending programming requests.
        M749Image image = M749Image.load(firmware, M749Image.Domain.SOFTWARE);
        image.requireActivationSupport();
        M749Immo immo = credential == null ? null :
                credential.getFileName().toString().toLowerCase(java.util.Locale.ROOT).endsWith(".pair")
                        ? M749PairFile.load(credential).credential() : M749Immo.load(credential);
        image.describe(messages);
        backend.flash(options, image, immo, messages);
    }

    private void reportScan(String message) {
        if (!message.equals(previousScan)) {
            view.message(message);
            previousScan = message;
        }
    }
}
