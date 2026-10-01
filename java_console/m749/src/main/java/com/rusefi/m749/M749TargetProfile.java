package com.rusefi.m749;

import java.io.IOException;

/** Known resident-loader contracts. A software name alone is not compatibility. */
enum M749TargetProfile {
    I865(0xD7B6B894, 0x08060000,
            new int[]{0x08201E2C, 0x08201D84, 0x08204B7C},
            new String[]{"2de9f04184b004460d4617461e4601f0", "70b506460d46144601f024fd012801d0",
                    "08b50a4b1b68fff7e7ff012807d0fff7"}),
    I812(0x4F256CD9, 0x08069000,
            new int[]{0x08201DE8, 0x08201D40, 0x08204CC4},
            new String[]{"2de9f04184b004460d4617461e46", "70b506460d46144601f0eafd0128",
                    "08b50a4b1b68fff7e7ff012807d0"});

    final int bootCrc, calibrationStart;
    final int[] addresses;
    final String[] sentinels;

    M749TargetProfile(int bootCrc, int calibrationStart, int[] addresses, String[] sentinels) {
        this.bootCrc = bootCrc;
        this.calibrationStart = calibrationStart;
        this.addresses = addresses;
        this.sentinels = sentinels;
    }

    static M749TargetProfile detect(M749ChecksumReader reader) throws IOException, InterruptedException {
        for (M749TargetProfile profile : values()) {
            boolean matches = true;
            for (int i = 0; i < 4; i++) {
                if (!reader.matches(0x0822DFFC + i, 1, (profile.bootCrc >>> (i * 8)) & 255)) {
                    matches = false;
                    break;
                }
            }
            if (!matches) { continue; }
            // Wrong-candidate controls reject a stuck "match" responder.
            for (int i = 0; i < 4; i++) {
                reader.verifyByte(0x0822DFFC + i, (profile.bootCrc >>> (i * 8)) & 255);
            }
            for (int n = 0; n < profile.addresses.length; n++) {
                String value = profile.sentinels[n];
                for (int i = 0; i < value.length(); i += 2) {
                    reader.verifyByte(profile.addresses[n] + i / 2, Integer.parseInt(value.substring(i, i + 2), 16));
                }
            }
            return profile;
        }
        throw new IOException("Unsupported resident loader: I812/I865 compatibility check failed");
    }
}
