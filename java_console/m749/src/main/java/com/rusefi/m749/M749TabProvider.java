package com.rusefi.m749;

import com.rusefi.ui.UIContext;
import com.rusefi.ui.plugins.ConsoleTabProvider;
import com.rusefi.ui.plugins.StartupTabProvider;

import javax.swing.JComponent;

public final class M749TabProvider implements ConsoleTabProvider, StartupTabProvider {
    @Override
    public String getTitle() {
        return "M74.9";
    }

    @Override
    public JComponent createTab(UIContext uiContext) {
        return uiContext == null ? new M749Panel() : new M749Panel(M749Monitor.canBackend(
                new M749ConsoleAccess(com.rusefi.ProductionConnectivity.CONTEXT.getPortScanner(),
                        uiContext.getLinkManager()::disconnect)));
    }
}
