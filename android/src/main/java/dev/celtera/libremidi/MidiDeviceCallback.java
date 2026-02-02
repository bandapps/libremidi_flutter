package dev.celtera.libremidi;

import android.media.midi.MidiDevice;
import android.media.midi.MidiManager;

public class MidiDeviceCallback implements MidiManager.OnDeviceOpenedListener {
    private long nativePtr;
    private boolean isOutput;

    public MidiDeviceCallback(long ptr, boolean output) {
        nativePtr = ptr;
        isOutput = output;
    }

    @Override
    public void onDeviceOpened(MidiDevice device) {
        if (device != null) {
            onDeviceOpened(device, nativePtr, isOutput);
        }
    }

    private native void onDeviceOpened(MidiDevice device, long targetPtr, boolean isOutput);
}
