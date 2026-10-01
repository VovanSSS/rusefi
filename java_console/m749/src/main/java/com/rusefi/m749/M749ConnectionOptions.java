package com.rusefi.m749;

import com.rusefi.io.can.slcan.SlcanPortScanner;
import java.io.IOException;
import java.util.*;
import java.util.function.Consumer;

/** Shared transport selection, validation and receive flow control for every command. */
class M749ConnectionOptions {
    String slcan, socketcan, channel;
    int baud = 115200, bus = 1, block = 16, stmin = -1;
    private final Set<String> supplied = new HashSet<>();
    static final Set<String> FLAGS = Set.of("--slcan", "--socketcan", "--channel", "--serial-baud",
            "--slcan-bus", "--block-size", "--stmin");

    void accept(String option, String value) {
        if (!FLAGS.contains(option)) throw new IllegalArgumentException("Unknown transport option " + option);
        if (!supplied.add(option)) throw new IllegalArgumentException("Duplicate option " + option);
        if (value.isBlank() || value.startsWith("--")) throw new IllegalArgumentException("Missing value for " + option);
        switch (option) {
            case "--slcan": slcan = value; break;
            case "--socketcan": socketcan = value; break;
            case "--channel": channel = value; break;
            case "--serial-baud": baud = Integer.decode(value); break;
            case "--slcan-bus": bus = Integer.decode(value); break;
            case "--block-size": block = Integer.decode(value); break;
            case "--stmin": stmin = Integer.decode(value); break;
        }
    }

    boolean specified() { return !supplied.isEmpty(); }

    void validate() {
        if ((slcan != null ? 1 : 0) + (socketcan != null ? 1 : 0) + (channel != null ? 1 : 0) > 1) {
            throw new IllegalArgumentException("Choose one transport: --slcan PORT|auto, --socketcan IFACE or --channel CHANNEL|auto");
        }
        if (slcan == null && socketcan == null && channel == null) slcan = "auto";
        if (socketcan != null) SocketCanTransport.requireInterface(socketcan);
        if (channel != null && !channel.equalsIgnoreCase("auto")) {
            try { peak.can.basic.TPCANHandle.valueOf(channel.toUpperCase(Locale.ROOT)); }
            catch (IllegalArgumentException e) { throw new IllegalArgumentException("Invalid PCAN channel: " + channel); }
        }
        if (block < 0 || block > 255 || baud < 9600 || baud > 4_000_000 || bus < 1 || bus > 3 ||
                stmin < -1 || stmin > 127 || supplied.contains("--stmin") && stmin < 0) {
            throw new IllegalArgumentException("Invalid flow control, serial baud or bus");
        }
        if (slcan == null && (supplied.contains("--serial-baud") || supplied.contains("--slcan-bus"))) {
            throw new IllegalArgumentException("Serial settings require --slcan");
        }
        if ("auto".equalsIgnoreCase(slcan) && baud != 115200) {
            throw new IllegalArgumentException("SLCAN auto scan uses 115200; select --slcan PORT for a different serial baud");
        }
        if (stmin < 0) stmin = slcan == null ? 1 : Math.max(3, (270000 + baud - 1) / baud);
    }

    UdsClient client(DiagnosticTransport transport) { return new UdsClient(transport, block, stmin); }

    List<String> arguments() {
        List<String> result = new ArrayList<>();
        if (slcan != null) result.addAll(List.of("--slcan", slcan, "--serial-baud", Integer.toString(baud), "--slcan-bus", Integer.toString(bus)));
        else if (socketcan != null) result.addAll(List.of("--socketcan", socketcan));
        else if (channel != null) result.addAll(List.of("--channel", channel));
        result.addAll(List.of("--block-size", Integer.toString(block)));
        if (stmin >= 0) result.addAll(List.of("--stmin", Integer.toString(stmin)));
        return result;
    }

    M749ConnectionOptions copy() {
        M749ConnectionOptions copy = new M749ConnectionOptions();
        copy.slcan = slcan; copy.socketcan = socketcan; copy.channel = channel;
        copy.baud = baud; copy.bus = bus; copy.block = block; copy.stmin = stmin;
        copy.supplied.addAll(supplied);
        return copy;
    }

    String endpoint() { return slcan != null ? slcan : socketcan != null ? socketcan : channel; }
    String connector() { return slcan != null ? "SLCAN" : socketcan != null ? "SocketCAN" : "PCAN"; }
    String key() { return arguments().toString(); }

    static void usage(Consumer<String> out) {
        out.accept("Transport (all hardware commands): --slcan PORT|auto | --socketcan IFACE | --channel CHANNEL|auto");
        out.accept("Default: SLCAN auto. Auto requires exactly one available adapter; SocketCAN requires an explicit interface.");
        out.accept("Options: --serial-baud 115200 --slcan-bus 1 (SLCAN only); --block-size 16 --stmin 0..127 (all transports).");
        out.accept("Default STmin: 1 ms for PCAN/SocketCAN; at least 3 ms for SLCAN, adjusted for serial baud. CAN: 500 kbit/s.");
    }

    static RawCanTransport open(M749ConnectionOptions o, Consumer<String> out) throws IOException {
        o.validate();
        if (o.socketcan != null) {
            out.accept("Using SocketCAN " + o.socketcan + " (interface must already be up at 500 kbit/s)");
            return SocketCanTransport.open(o.socketcan);
        }
        if (o.slcan != null) {
            String port = o.slcan;
            if (port.equalsIgnoreCase("auto")) {
                out.accept("Scanning serial ports for SLCAN (skipping detected TunerStudio consoles)");
                port = selectSlcan(SlcanPortScanner.scanOnce(SlcanPortScanner.Probes.REAL), out);
            }
            out.accept("Using SLCAN " + port);
            o.slcan = port;
            return SlcanTransport.open(port, o.baud, o.bus, out);
        }
        if (!System.getProperty("os.name").toLowerCase(Locale.ROOT).startsWith("windows")) {
            throw new IOException("PCAN requires native Windows Java; use --slcan or Linux --socketcan");
        }
        PcanDevice device = new PcanDevice();
        List<PcanDevice.Channel> channels = device.scan();
        String selected = o.channel;
        if (selected.equalsIgnoreCase("auto")) {
            List<String> available = new java.util.ArrayList<>();
            for (PcanDevice.Channel channel : channels) { if (channel.available) { available.add(channel.handle.name()); } }
            selected = selectOnly(available, "PCAN", "--channel");
        }
        for (PcanDevice.Channel channel : channels) {
            if (channel.handle.name().equalsIgnoreCase(selected)) {
                if (!channel.available) { throw new IOException("PCAN channel is in use: " + o.channel); }
                out.accept("Using PCAN " + channel.handle.name());
                o.channel = channel.handle.name();
                return device.open(channel);
            }
        }
        throw new IOException("PCAN channel not found: " + o.channel);
    }

    static String selectSlcan(List<SlcanPortScanner.Result> results, Consumer<String> out) throws IOException {
        if (Thread.currentThread().isInterrupted()) { throw new IOException("SLCAN scan interrupted"); }
        List<String> candidates = new java.util.ArrayList<>();
        for (SlcanPortScanner.Result result : results) {
            out.accept(result.toString());
            if (result.type == SlcanPortScanner.Type.SLCAN) { candidates.add(result.port); }
        }
        return selectOnly(candidates, "SLCAN", "--slcan");
    }

    static String selectOnly(List<String> candidates, String kind, String option) throws IOException {
        if (candidates.isEmpty()) { throw new IOException("No available " + kind + " adapter found; connect one or specify " + option); }
        if (candidates.size() != 1) { throw new IOException("Multiple " + kind + " adapters found: " + candidates + "; choose " + option); }
        return candidates.get(0);
    }

}
