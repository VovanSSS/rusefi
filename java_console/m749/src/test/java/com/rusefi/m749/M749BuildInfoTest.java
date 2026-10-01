package com.rusefi.m749;

import org.junit.jupiter.api.Test;
import java.util.Properties;
import static org.junit.jupiter.api.Assertions.*;

class M749BuildInfoTest {
    @Test void describesGeneratedProperties() {
        Properties p = new Properties();
        p.setProperty("board.git", "2576fc937f70-dirty");
        p.setProperty("rusefi.git", "7d26cdb0e92e");
        p.setProperty("build.date", "2026-09-30T14:38:20Z");
        assertEquals("M749 build: board 2576fc937f70-dirty, rusefi 7d26cdb0e92e, built 2026-09-30T14:38:20Z",
            M749BuildInfo.describe(p));
    }

    @Test void missingValuesReadUnknownInsteadOfBlank() {
        Properties p = new Properties();
        p.setProperty("board.git", " ");
        assertEquals("M749 build: board unknown, rusefi unknown, built unknown", M749BuildInfo.describe(p));
    }

    @Test void gradleBuildSuppliesTheResource() {
        // processResources depends on generateM749BuildInfo, so the test classpath carries real values.
        String line = M749BuildInfo.describe();
        assertTrue(line.matches("M749 build: board [0-9a-f]{12}(-dirty)?, rusefi [0-9a-f]{12}(-dirty)?, built \\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z"), line);
    }
}
