package dev.celtera.libremidi;

import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.util.Log;

/**
 * Callback for MIDI device hotplug events (added/removed).
 */
public class MidiDeviceStatusCallback extends MidiManager.DeviceCallback {
    private static final String TAG = "MidiDeviceStatusCallback";
    private long nativeObserverPtr;

    public MidiDeviceStatusCallback(long observerPtr) {
        nativeObserverPtr = observerPtr;
        Log.i(TAG, "Created with observer ptr: " + observerPtr);
    }

    @Override
    public void onDeviceAdded(MidiDeviceInfo device) {
        Log.i(TAG, "Device added: " + device.toString());
        if (nativeObserverPtr != 0) {
            onDeviceAddedNative(nativeObserverPtr, device);
        }
    }

    @Override
    public void onDeviceRemoved(MidiDeviceInfo device) {
        Log.i(TAG, "Device removed: " + device.toString());
        if (nativeObserverPtr != 0) {
            onDeviceRemovedNative(nativeObserverPtr, device);
        }
    }

    public void invalidate() {
        nativeObserverPtr = 0;
    }

    private native void onDeviceAddedNative(long observerPtr, MidiDeviceInfo device);
    private native void onDeviceRemovedNative(long observerPtr, MidiDeviceInfo device);
}
