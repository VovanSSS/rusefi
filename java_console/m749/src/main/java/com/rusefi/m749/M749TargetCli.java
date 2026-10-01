package com.rusefi.m749;

import java.io.IOException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;

/** Check a software payload against the ECU without changing flash. */
final class M749TargetCli {
    static int execute(String[] args, Consumer<String> out) throws IOException, InterruptedException {
        return execute(args, o -> M749ReadFlashCli.open(o, out), out);
    }

    static int execute(String[] args, M749ReadFlashCli.TransportFactory factory, Consumer<String> out)
            throws IOException, InterruptedException {
        String usage = "m749-cli --check-target FILE [--slcan PORT|auto | --socketcan IFACE | --channel CHANNEL|auto] " +
                "[--pair-file ECU.pair | --immo-backup PAIRED_FULLFLASH.bin] [--calibration]";
        String file = null;
        boolean calibration = false;
        List<String> transport = new ArrayList<>();
        transport.add("--read-flash");
        for (int i = 0; i < args.length; i++) {
            String option = args[i];
            if (option.equals("--help") || option.equals("-h")) { out.accept(usage); M749ConnectionOptions.usage(out); return 0; }
            if (option.equals("--calibration") && !calibration) { calibration = true; continue; }
            if (i + 1 >= args.length || args[i + 1].startsWith("--")) { out.accept(usage); return 2; }
            String value = args[++i];
            if (option.equals("--check-target") && file == null) { file = value; continue; }
            if (!M749ConnectionOptions.FLAGS.contains(option) && !List.of("--pair-file", "--immo-backup").contains(option)) {
                out.accept(usage); return 2;
            }
            transport.add(option);
            transport.add(value);
        }
        if (file == null) { out.accept(usage); return 2; }
        M749ReadFlashCli.Options options;
        try { options = M749ReadFlashCli.parse(transport.toArray(new String[0])); }
        catch (IllegalArgumentException e) { out.accept(e.getMessage()); return 2; }
        M749Image image = M749Image.load(Path.of(file), calibration ? M749Image.Domain.CALIBRATION : M749Image.Domain.SOFTWARE);
        image.requireActivationSupport();
        M749Immo credential = options.pair != null ? M749PairFile.load(Path.of(options.pair)).credential() :
                options.immo == null ? null : M749Immo.load(Path.of(options.immo));
        image.describe(out);
        out.accept("Preflight enters programming session 02 and leaves the loader active; flash is not changed.");
        try (RawCanTransport can = factory.open(options)) {
            if (credential != null) { credential.authorize(can, out); }
            new M749Uploader(options.client(can), out).checkTarget(image);
        }
        return 0;
    }
}
