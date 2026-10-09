package dev.celtera.libremidi;

import android.media.midi.MidiDevice;
import android.media.midi.MidiManager;
import android.util.Log;

public class MidiDeviceCallback implements MidiManager.OnDeviceOpenedListener {
    private static final String TAG = "MidiDeviceCallback";
    private long nativePtr;
    private boolean isOutput;

    public MidiDeviceCallback(long ptr, boolean output) {
        nativePtr = ptr;
        isOutput = output;
    }

    @Override
    public void onDeviceOpened(MidiDevice device) {
        // MidiManager reports a failed open with null. Forward it anyway: the
        // native side is waiting on this token and clears the pending open
        // only when told, otherwise the port stays "opening" forever.
        if (device == null) {
            Log.w(TAG, "MidiManager could not open the device");
        }
        onDeviceOpened(device, nativePtr, isOutput);
    }

    private native void onDeviceOpened(MidiDevice device, long targetPtr, boolean isOutput);
}
