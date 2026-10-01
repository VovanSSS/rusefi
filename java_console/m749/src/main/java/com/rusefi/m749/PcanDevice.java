package com.rusefi.m749;

import peak.can.MutableInteger;
import peak.can.basic.*;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class PcanDevice {
    static final class Channel {
        final TPCANHandle handle;
        final boolean available;

        Channel(TPCANHandle handle, boolean available) {
            this.handle = handle;
            this.available = available;
        }

        @Override
        public String toString() {
            return handle.name() + (available ? "" : " (in use)");
        }
    }

    private PCANBasic api;

    List<Channel> scan() throws IOException {
        if (api == null) {
            PCANBasic candidate = new PCANBasic();
            if (!candidate.initializeAPI()) {
                throw new IOException("Cannot initialize PCAN-Basic. Check the PCAN driver installation.");
            }
            api = candidate;
        }
        List<Channel> channels = new ArrayList<>();
        for (TPCANHandle handle : TPCANHandle.values()) {
            if (handle == TPCANHandle.PCAN_NONEBUS) {
                continue;
            }
            MutableInteger condition = new MutableInteger(0);
            TPCANStatus status = api.GetValue(handle, TPCANParameter.PCAN_CHANNEL_CONDITION, condition, 4);
            if (status == TPCANStatus.PCAN_ERROR_OK && (condition.value & 3) != 0) {
                channels.add(new Channel(handle,
                        condition.value == TPCANParameterValue.PCAN_CHANNEL_AVAILABLE.getValue()));
            }
        }
        return channels;
    }

    RawCanTransport open(Channel channel) throws IOException {
        requireOk("Open " + channel.handle, api.Initialize(channel.handle,
                TPCANBaudrate.PCAN_BAUD_500K, TPCANType.PCAN_TYPE_NONE, 0, (short) 0));
        return new RawCanTransport() {
            @Override
            public void sendCan(int id, byte[] frame) throws IOException {
                if (id < 0 || id > 0x7FF || frame.length > 8) {
                    throw new IOException("Invalid standard CAN frame");
                }
                requireOk("CAN write", api.Write(channel.handle, new TPCANMsg(id,
                        TPCANMessageType.PCAN_MESSAGE_STANDARD.getValue(), (byte) frame.length, frame)));
            }

            @Override
            public Frame receiveCan() throws IOException {
                // Bound each poll even on a saturated bus, so timeouts and cancellation still run.
                for (int i = 0; i < 64; i++) {
                    TPCANMsg message = new TPCANMsg();
                    TPCANStatus status = api.Read(channel.handle, message, null);
                    if (status == TPCANStatus.PCAN_ERROR_QRCVEMPTY) {
                        return null;
                    }
                    requireOk("CAN read", status);
                    if (message.getType() == TPCANMessageType.PCAN_MESSAGE_STANDARD.getValue()) {
                        int length = message.getLength() & 0xFF;
                        if (length > 8 || message.getData() == null || message.getData().length < length) {
                            throw new IOException("Invalid CAN frame length");
                        }
                        return new Frame(message.getID(), Arrays.copyOf(message.getData(), length));
                    }
                }
                return null;
            }

            @Override
            public void close() throws IOException {
                // Release only this channel, never channels owned by another console feature.
                requireOk("Close " + channel.handle, api.Uninitialize(channel.handle));
            }
        };
    }

    private static void requireOk(String operation, TPCANStatus status) throws IOException {
        if (status != TPCANStatus.PCAN_ERROR_OK) {
            throw new IOException(operation + ": " + status);
        }
    }
}
