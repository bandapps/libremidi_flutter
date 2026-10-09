package dev.celtera.libremidi;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;

import java.io.IOException;
import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Device management for the native MIDI layer.
 *
 * Everything that android.media.midi only offers in Java lives here: listing
 * devices, opening and closing them, and hotplug. The native side only does the
 * data path (AMidi ports) on the MidiDevice objects handed out by
 * {@link #openDevice}. All MidiManager callbacks run on one dedicated thread, so
 * they never depend on the app's main thread being free.
 *
 * Called from native code only; the native methods are bound with
 * RegisterNatives in JNI_OnLoad and the class is kept by consumer-rules.pro.
 */
public final class MidiBridge {
    private static final String TAG = "libremidi";

    /** Fields per port in the array returned by {@link #listPorts}. */
    static final int FIELDS_PER_PORT = 7;

    private static final Object lock = new Object();
    private static MidiManager manager;
    private static Handler handler;

    private MidiBridge() {}

    private static native void nativeDevicesChanged();

    /** Called by the Flutter plugin with the application context. Idempotent. */
    public static void init(Context context) {
        synchronized (lock) {
            if (manager != null) return;
            Context app = context.getApplicationContext();
            if (!app.getPackageManager().hasSystemFeature(PackageManager.FEATURE_MIDI)) {
                Log.w(TAG, "This device has no MIDI support");
                return;
            }
            MidiManager m = (MidiManager) app.getSystemService(Context.MIDI_SERVICE);
            if (m == null) {
                Log.w(TAG, "MidiManager unavailable");
                return;
            }
            HandlerThread thread = new HandlerThread("libremidi");
            thread.start();
            handler = new Handler(thread.getLooper());
            manager = m;
            m.registerDeviceCallback(new MidiManager.DeviceCallback() {
                @Override
                public void onDeviceAdded(MidiDeviceInfo device) {
                    nativeDevicesChanged();
                }

                @Override
                public void onDeviceRemoved(MidiDeviceInfo device) {
                    nativeDevicesChanged();
                }
            }, handler);
        }
    }

    /**
     * Whether MIDI is usable: the device supports it and {@link #init} has
     * registered the hotplug listener. Both happen together, so this also says
     * whether hotplug events can arrive.
     */
    static boolean isAvailable() {
        return manager() != null;
    }

    private static MidiManager manager() {
        synchronized (lock) {
            return manager;
        }
    }

    /**
     * Snapshot of all ports in one direction, in a flat array of
     * {@link #FIELDS_PER_PORT} strings per port: device id, port index, device
     * type, name, manufacturer, product, serial number. Missing properties are
     * empty strings. Returns null when MIDI is unavailable.
     *
     * @param inputs true for ports the app receives from (the device's output
     *               ports), false for ports it sends to.
     */
    static String[] listPorts(boolean inputs) {
        MidiManager m = manager();
        if (m == null) return null;
        ArrayList<String> out = new ArrayList<>();
        for (MidiDeviceInfo info : m.getDevices()) {
            int count = inputs ? info.getOutputPortCount() : info.getInputPortCount();
            Bundle p = info.getProperties();
            for (int i = 0; i < count; i++) {
                out.add(Integer.toString(info.getId()));
                out.add(Integer.toString(i));
                out.add(Integer.toString(info.getType()));
                out.add(str(p, MidiDeviceInfo.PROPERTY_NAME));
                out.add(str(p, MidiDeviceInfo.PROPERTY_MANUFACTURER));
                out.add(str(p, MidiDeviceInfo.PROPERTY_PRODUCT));
                out.add(str(p, MidiDeviceInfo.PROPERTY_SERIAL_NUMBER));
            }
        }
        return out.toArray(new String[0]);
    }

    private static String str(Bundle b, String key) {
        String v = b != null ? b.getString(key) : null;
        return v != null ? v : "";
    }

    /**
     * Opens a device and waits for the result. Returns the MidiDevice on
     * success, otherwise a String saying why it failed.
     *
     * The caller owns the returned device and must hand it back to
     * {@link #closeDevice}. Must not be called on the MIDI thread, which
     * delivers the result.
     */
    static Object openDevice(int deviceId, long timeoutMs) {
        MidiManager m = manager();
        if (m == null) return "MIDI is not available on this device";
        if (Looper.myLooper() == handler.getLooper()) {
            return "cannot open a device from the MIDI callback thread";
        }

        MidiDeviceInfo info = null;
        for (MidiDeviceInfo d : m.getDevices()) {
            if (d.getId() == deviceId) {
                info = d;
                break;
            }
        }
        if (info == null) return "device is no longer connected";

        final OpenResult result = new OpenResult();
        m.openDevice(info, device -> result.complete(device), handler);

        boolean done;
        try {
            done = result.latch.await(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            done = false;
        }
        MidiDevice device = result.takeOrAbandon(done);
        if (!done) return "timed out waiting for the device to open";
        if (device == null) return "the system could not open the device";
        return device;
    }

    /** Closes a device obtained from {@link #openDevice}. */
    static void closeDevice(MidiDevice device) {
        if (device == null) return;
        try {
            device.close();
        } catch (IOException e) {
            Log.w(TAG, "closing MIDI device failed", e);
        }
    }

    /**
     * Hand-over of an asynchronous open. If the waiter gives up first, a device
     * that still arrives later is closed here instead of being leaked.
     */
    private static final class OpenResult {
        final CountDownLatch latch = new CountDownLatch(1);
        private MidiDevice device;
        private boolean abandoned;

        synchronized void complete(MidiDevice d) {
            if (abandoned) {
                closeDevice(d);
            } else {
                device = d;
            }
            latch.countDown();
        }

        synchronized MidiDevice takeOrAbandon(boolean done) {
            if (done) return device;
            abandoned = true;
            if (device != null) {
                // Arrived between the timeout and this call.
                closeDevice(device);
                device = null;
            }
            return null;
        }
    }
}
