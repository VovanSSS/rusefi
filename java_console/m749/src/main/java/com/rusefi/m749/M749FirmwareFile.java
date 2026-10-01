package com.rusefi.m749;

import com.rusefi.core.FindFileHelper;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

/** Uses the console updater's bundle search, including target-named artifacts. */
final class M749FirmwareFile {
    interface Locator {
        Path locate() throws IOException;
    }

    static Path locate() throws IOException {
        String file = FindFileHelper.findSrecFileForTarget("re74.9");
        if (file == null) {
            file = FindFileHelper.findSrecFile();
        }
        if (file == null) {
            throw new IOException("SREC not found. Place this board's SREC in the firmware bundle and scan again.");
        }
        String target = FindFileHelper.extractTargetFromFirmwareName(file);
        if (target != null && !target.equalsIgnoreCase("re74.9")) {
            throw new IOException("SREC is for " + target + ", not re74.9");
        }
        Path path = Path.of(file).toAbsolutePath().normalize();
        if (!Files.isRegularFile(path)) {
            throw new IOException("SREC file not found: " + path);
        }
        return path;
    }
}
