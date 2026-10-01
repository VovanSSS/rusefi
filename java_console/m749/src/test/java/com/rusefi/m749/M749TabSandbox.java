package com.rusefi.m749;

import javax.swing.*;

/** Standalone launcher for the M74.9 custom tab and its direct CAN connections. */
public final class M749TabSandbox {
    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            JFrame frame = new JFrame("M74.9");
            frame.setDefaultCloseOperation(WindowConstants.DISPOSE_ON_CLOSE);
            frame.setContentPane(new M749Panel());
            frame.setSize(1000, 650);
            frame.setLocationRelativeTo(null);
            frame.setVisible(true);
        });
    }
}
