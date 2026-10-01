package com.rusefi.m749;

import java.io.IOException;
import java.nio.file.Path;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;

/** Identification, offline image validation and OEM-loader firmware upload. */
public final class M749Cli {
    private M749Cli() {
    }

    interface UploadAction {
        void upload(M749ConnectionOptions channel, M749Image image, boolean verifyBytes, M749Immo immo, Consumer<String> out)
                throws IOException, InterruptedException;
    }

    interface ReadAction {
        void read(M749ConnectionOptions channel, Integer address, Path path, M749PairFile known, M749Immo immo, Consumer<String> out)
                throws IOException, InterruptedException;
    }

    public static void main(String[] args) {
        ElapsedTimeOutputStream.install();
        System.out.println(M749BuildInfo.describe());
        int exit;
        try {
            exit = execute(args, M749Monitor.canBackend(), M749Cli::upload, System.out::println);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            System.err.println("Interrupted; no further requests sent. Check ECU state before retrying.");
            exit = 1;
        } catch (IOException | RuntimeException | LinkageError e) {
            System.err.println(e instanceof LinkageError
                    ? "PCAN native library unavailable. Install the PCAN driver and matching PCAN-Basic/JNI libraries."
                    : e.getMessage());
            exit = 1;
        }
        System.exit(exit);
    }

    static int execute(String[] args, M749Monitor.Backend backend, UploadAction uploader, Consumer<String> out)
            throws IOException, InterruptedException {
        return execute(args, backend, uploader, M749Cli::read, out);
    }

    static int execute(String[] args, M749Monitor.Backend backend, UploadAction uploader,
                       ReadAction reader, Consumer<String> out) throws IOException, InterruptedException {
        List<String> arguments = java.util.Arrays.asList(args);
        if (arguments.contains("--help") || arguments.contains("-h")) {
            if (arguments.contains("--read-flash")) return M749ReadFlashCli.execute(new String[]{"--read-flash", "--help"}, out);
            if (arguments.contains("--identify")) return M749ReadFlashCli.identify(new String[]{"--identify", "--help"}, out);
            if (arguments.contains("--check-target")) return M749TargetCli.execute(new String[]{"--help"}, out);
            usage(out);
            return 0;
        }
        for (String arg : args) {
            if (arg.equals("--read-flash")) { return M749ReadFlashCli.execute(args, out); }
            if (arg.equals("--identify")) { return M749ReadFlashCli.identify(args, out); }
            if (arg.equals("--check-target")) { return M749TargetCli.execute(args, out); }
        }
        boolean list = false;
        boolean dryRun = false;
        boolean calibration = false;
        boolean verifyBytes = false;
        M749ConnectionOptions connection = new M749ConnectionOptions();
        String file = null;
        String immoBackup = null;
        String pairFile = null, readPair = null, exportPair = null, readAddress = null;
        java.util.Set<String> seen = new java.util.HashSet<>();
        try {
            for (int i = 0; i < args.length; i++) {
                String arg = args[i];
                if (!seen.add(arg)) throw new IllegalArgumentException("Duplicate option " + arg);
                if (M749ConnectionOptions.FLAGS.contains(arg)) {
                    if (++i == args.length) throw new IllegalArgumentException("Missing value for " + arg);
                    connection.accept(arg, args[i]);
                    continue;
                }
                switch (arg) {
                    case "--help": case "-h": usage(out); return 0;
                    case "--list": list = true; break;
                    case "--dry-run": dryRun = true; break;
                    case "--calibration": calibration = true; break;
                    case "--verify-bytes": verifyBytes = true; break;
                    case "--pair-file":
                        if (++i == args.length || pairFile != null) { usage(out); return 2; }
                        pairFile = args[i]; break;
                    case "--read-pair":
                        if (++i == args.length || readPair != null) { usage(out); return 2; }
                        readPair = args[i]; break;
                    case "--export-pair":
                        if (++i == args.length || exportPair != null) { usage(out); return 2; }
                        exportPair = args[i]; break;
                    case "--read-byte":
                        if (++i == args.length || readAddress != null) { usage(out); return 2; }
                        readAddress = args[i]; break;
                    case "--immo-backup":
                        if (++i == args.length || immoBackup != null) { usage(out); return 2; }
                        immoBackup = args[i];
                        break;
                    case "--write-flash":
                    case "--upload":
                        if (++i == args.length || file != null) { usage(out); return 2; }
                        file = args[i];
                        break;
                    default:
                        if (arg.startsWith("-")) { usage(out); return 2; }
                        connection.accept("--channel", arg);
                }
            }
        } catch (IllegalArgumentException e) { out.accept(e.getMessage()); usage(out); return 2; }
        int modes = (file != null ? 1 : 0) + (readPair != null ? 1 : 0) +
                (readAddress != null ? 1 : 0) + (exportPair != null ? 1 : 0);
        boolean readMode = readPair != null || readAddress != null;
        if (modes > 1 || (file == null && (dryRun || calibration || verifyBytes)) ||
                (pairFile != null && (file == null && !readMode || immoBackup != null)) ||
                (immoBackup != null && file == null && !readMode && exportPair == null) ||
                (list && (modes != 0 || connection.specified())) ||
                (exportPair != null && (immoBackup == null || connection.specified()))) {
            usage(out);
            return 2;
        }
        try { connection.validate(); }
        catch (IllegalArgumentException e) { out.accept(e.getMessage()); usage(out); return 2; }
        if (exportPair != null) {
            M749PairFile source = M749Immo.load(Path.of(immoBackup)).pairFile();
            Path path = Path.of(exportPair);
            M749PairFile destination = Files.exists(path) ? M749PairFile.load(path) : new M749PairFile();
            for (int i = 0; i < M749PairFile.SIZE; i++) { destination.put(i, source.get(i)); }
            destination.save(path);
            out.accept("Saved complete pair file (24 indexed bytes); no adapter opened");
            return 0;
        }
        if (readPair != null) {
            Path path = Path.of(readPair);
            M749PairFile known = Files.exists(path) ? M749PairFile.load(path) : new M749PairFile();
            reader.read(connection, null, path, known, loadCredential(pairFile, immoBackup), out);
            return 0;
        }
        if (readAddress != null) {
            int address;
            try {
                address = Integer.parseUnsignedInt(readAddress.replaceFirst("^0[xX]", ""), 16);
                M749ChecksumReader.requireAddress(address);
            } catch (IllegalArgumentException e) {
                out.accept("Invalid flash address; use hexadecimal 0x08000000..0x083EFFFF");
                return 2;
            }
            reader.read(connection, address, null, null, loadCredential(pairFile, immoBackup), out);
            return 0;
        }
        if (file == null) {
            if (list) return run(null, true, backend, out);
            backend.inspect(connection, out);
            return 0;
        }
        // Parse all input and prove the complete CRC domain before native library/device access.
        M749Image image = M749Image.load(Path.of(file), calibration ? M749Image.Domain.CALIBRATION : M749Image.Domain.SOFTWARE);
        image.requireActivationSupport();
        M749Immo immo = loadCredential(pairFile, immoBackup);
        image.describe(out);
        if (immo != null) {
            out.accept("I865 IMMO credential loaded; normal CAN authorization enabled");
        }
        if (dryRun) {
            out.accept("Dry run complete; no adapter was opened. Target compatibility and retained-domain CRC remain device checks.");
            return 0;
        }
        uploader.upload(connection, image, verifyBytes, immo, out);
        return 0;
    }

    private static M749Immo loadCredential(String pairFile, String immoBackup) throws IOException {
        return pairFile != null ? M749PairFile.load(Path.of(pairFile)).credential() :
                immoBackup == null ? null : M749Immo.load(Path.of(immoBackup));
    }

    static void upload(M749ConnectionOptions connection, M749Image image, boolean verifyBytes, M749Immo immo, Consumer<String> out)
            throws IOException, InterruptedException {
        try (RawCanTransport transport = M749ConnectionOptions.open(connection, out)) {
            if (immo != null) { immo.authorize(transport, out); }
            new M749Uploader(connection.client(transport), out).upload(image, verifyBytes);
        }
    }

    private static void read(M749ConnectionOptions connection, Integer address, Path path, M749PairFile known, M749Immo immo, Consumer<String> out)
            throws IOException, InterruptedException {
        try (RawCanTransport transport = M749ConnectionOptions.open(connection, out)) {
            if (immo != null) { immo.authorize(transport, out); }
            M749ChecksumReader reader = new M749ChecksumReader(connection.client(transport));
            M749TargetProfile profile = reader.prepareRead();
            if (address != null) {
                out.accept(String.format("0x%08X = %02X", address, reader.readByte(address)));
            } else {
                if (profile != M749TargetProfile.I865) {
                    throw new IOException("The paired-credential file layout is supported only on I865");
                }
                known.readMissing(reader, path, out);
                out.accept("Pair file complete; no erase/download requests sent");
            }
        }
    }

    static int run(String requested, boolean listOnly, M749Monitor.Backend backend, Consumer<String> out)
            throws IOException, InterruptedException {
        List<PcanDevice.Channel> channels = backend.scan();
        if (channels.isEmpty()) {
            out.accept("PCAN not detected. Connect a PCAN adapter and check the driver installation.");
            return 1;
        }
        out.accept("PCAN detected: " + channels);
        if (listOnly) {
            return 0;
        }
        List<PcanDevice.Channel> candidates = new ArrayList<>();
        for (PcanDevice.Channel channel : channels) {
            if (requested == null ? channel.available : channel.handle.name().equalsIgnoreCase(requested)) {
                candidates.add(channel);
            }
        }
        if (candidates.isEmpty()) {
            out.accept(requested == null ? "All detected channels are in use by another application."
                    : "Channel " + requested + " not found among detected channels.");
            return requested == null ? 1 : 2;
        }
        // The driver can report phantom channels (e.g. ISA with no driver), so
        // without an explicit channel keep trying until one yields an ECU.
        for (PcanDevice.Channel channel : candidates) {
            out.accept("Querying M74.9 via " + channel.handle + " at 500 kbit/s (7E0 / 7E8)");
            try {
                List<String> summary = backend.identify(channel, out);
                for (String line : summary) {
                    out.accept(line);
                }
                return 0;
            } catch (IOException | RuntimeException e) {
                out.accept("Identification via " + channel.handle + " failed: " + e.getMessage());
            }
        }
        out.accept("No channel produced an M74.9 identification.");
        return 1;
    }

    private static void usage(Consumer<String> out) {
        M749ConnectionOptions.usage(out);
        out.accept("M74.9 CLI (500 kbit/s, 7E0/7E8; supported I812/I865 resident loaders)");
        out.accept("Usage: m749-cli [channel]                 identify ECU; defaults to SLCAN auto when omitted");
        out.accept("       m749-cli --identify [--slcan PORT|auto | --socketcan IFACE | --channel PCAN_USBBUS1|auto]  query ECU without session changes");
        out.accept("       m749-cli --list                    list PCAN channels");
        out.accept("       m749-cli --read-flash [OUTPUT.bin] [--slcan PORT|auto | --socketcan IFACE | --channel PCAN_USBBUS1|auto]");
        out.accept("                  [--resume] [--helper-running] [--reset-after]; add --help for read options");
        out.accept("       m749-cli --write-flash FILE [--dry-run] [--slcan PORT|auto | --socketcan IFACE | --channel CHANNEL]");
        out.accept("       HEX/SREC: rusEFI software. BIN: I812/I865 full backup, restore application/calibration only.");
        out.accept("       --upload and --write-flash are aliases; both default to SLCAN auto.");
        out.accept("       m749-cli --upload FILE --dry-run   validate HEX/SREC/OEM BIN without hardware");
        out.accept("       m749-cli --upload FILE --channel PCAN_USBBUS1 [--calibration] [--verify-bytes]");
        out.accept("       m749-cli --upload FILE --slcan PORT|auto [--verify-bytes]");
        out.accept("       m749-cli --upload FILE --socketcan IFACE [--calibration] [--verify-bytes]");
        out.accept("       m749-cli --check-target FILE [--slcan PORT|auto | --socketcan IFACE | --channel CHANNEL|auto]   programming preflight, no flash writes");
        out.accept("                  [--pair-file ECU.pair | --immo-backup PAIRED_FULLFLASH.bin]");
        out.accept("       m749-cli --read-byte 0xADDRESS --channel PCAN_USBBUS1");
        out.accept("       m749-cli --read-pair ECU.pair --channel PCAN_USBBUS1   read/resume 24 indexed bytes");
        out.accept("                  [--pair-file KNOWN.pair | --immo-backup PAIRED_FULLFLASH.bin]   optional read authorization");
        out.accept("       m749-cli --export-pair ECU.pair --immo-backup PAIRED_FULLFLASH.bin   offline conversion");
        out.accept("Byte/pair reads also accept --slcan PORT|auto or --socketcan IFACE instead of --channel.");
        out.accept("SocketCAN: Linux, explicit interface already up at 500 kbit/s (e.g. can0); --socketcan IFACE alone identifies.");
        out.accept("Reads enter OEM session 02 (can reset the ECU) and authenticate. Rejected entry stops without flash writes.");
        out.accept("Pair files checkpoint each byte; unknown indices are omitted.");
        out.accept("--immo-backup enables normal I865 CAN authorization; cycle bench power when the listener reports ready.");
        out.accept("M749ACT2 software supports both target profiles; legacy M749ACT1 supports I865 only. Calibration payloads are I865-only.");
        out.accept("--upload erases/programs the selected domain, preserves OEM programming metadata, and activates.");
        out.accept("Default verification: per-block sum plus application-side CRCs; --verify-bytes adds slow byte comparisons.");
    }
}
