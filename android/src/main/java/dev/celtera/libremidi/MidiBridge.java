package dev.celtera.libremidi;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.XmlResourceParser;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.media.midi.MidiDeviceService;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
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
    // "manufacturer|product" of every virtual device this app provides itself
    // through a MidiDeviceService. Written once in init.
    private static Set<String> ownDevices = new HashSet<>();
    // Opens still in progress in MidiService, by device id. Guarded by lock.
    private static final Map<Integer, PendingOpen> pending = new HashMap<>();

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
            ownDevices = readOwnDevices(app);
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
     *
     * An open that times out is not abandoned: MidiService may still be
     * connecting (Bluetooth binding can be slow). The next call for the same
     * device joins it instead of starting a second open, which MidiService
     * would reject while the first is still binding, and receives the device
     * if it arrives. Only a device that arrives with nobody waiting is closed.
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

        // A MidiDeviceService of this app is started on the main thread, which
        // is blocked here while we wait for it: the open could never finish.
        if (Looper.myLooper() == Looper.getMainLooper() && isOwnService(info)) {
            return "a MIDI device provided by this app cannot be opened from the main "
                    + "thread; use openInputAsync/openOutputAsync";
        }

        PendingOpen open;
        boolean start = false;
        synchronized (lock) {
            open = pending.get(deviceId);
            if (open == null) {
                open = new PendingOpen();
                pending.put(deviceId, open);
                start = true;
            }
            open.waiters++;
        }
        if (start) {
            final PendingOpen started = open;
            m.openDevice(info, device -> complete(deviceId, started, device), handler);
        }

        try {
            open.latch.await(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }

        synchronized (lock) {
            open.waiters--;
            // Decided by the result, not by whether await() timed out: a device
            // that arrived just as the wait ran out is still taken here rather
            // than left unclaimed.
            if (!open.done) {
                // Still connecting. It stays pending: the next call joins it,
                // and if nobody is waiting when it completes, complete() closes
                // the device.
                return "timed out waiting for the device to open";
            }
            if (open.claimed) return "the device is being opened by another caller";
            open.claimed = true;
            pending.remove(deviceId);
            if (open.device == null) return "the system could not open the device";
            return open.device;
        }
    }

    // Runs on the MIDI thread when MidiService has answered.
    private static void complete(int deviceId, PendingOpen open, MidiDevice device) {
        synchronized (lock) {
            open.done = true;
            open.device = device;
            if (open.waiters == 0) {
                // Everyone gave up; nobody will take it.
                pending.remove(deviceId);
                open.claimed = true;
                closeDevice(device);
            }
        }
        open.latch.countDown();
    }

    private static boolean isOwnService(MidiDeviceInfo info) {
        if (info.getType() != MidiDeviceInfo.TYPE_VIRTUAL) return false;
        Set<String> own;
        synchronized (lock) {
            own = ownDevices;
        }
        Bundle p = info.getProperties();
        return !own.isEmpty() && own.contains(deviceKey(
                str(p, MidiDeviceInfo.PROPERTY_MANUFACTURER),
                str(p, MidiDeviceInfo.PROPERTY_PRODUCT)));
    }

    // Manufacturer and product, which MidiService copies from the service's
    // device description into the MidiDeviceInfo properties.
    private static String deviceKey(String manufacturer, String product) {
        return manufacturer + "|" + product;
    }

    /**
     * Reads the &lt;device&gt; entries of this app's own MidiDeviceServices from
     * their meta-data, the same description MidiService builds the
     * MidiDeviceInfo properties from.
     */
    private static Set<String> readOwnDevices(Context app) {
        Set<String> devices = new HashSet<>();
        PackageManager pm = app.getPackageManager();
        Intent intent = new Intent(MidiDeviceService.SERVICE_INTERFACE).setPackage(app.getPackageName());
        for (ResolveInfo r : pm.queryIntentServices(intent, PackageManager.GET_META_DATA)) {
            if (r.serviceInfo == null) continue;
            try (XmlResourceParser xml =
                         r.serviceInfo.loadXmlMetaData(pm, MidiDeviceService.SERVICE_INTERFACE)) {
                if (xml == null) continue;
                for (int ev = xml.getEventType(); ev != XmlResourceParser.END_DOCUMENT; ev = xml.next()) {
                    if (ev == XmlResourceParser.START_TAG && "device".equals(xml.getName())) {
                        devices.add(deviceKey(attr(xml, "manufacturer"), attr(xml, "product")));
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "could not read this app's MIDI device description", e);
            }
        }
        return devices;
    }

    private static String attr(XmlResourceParser xml, String name) {
        String v = xml.getAttributeValue(null, name);
        return v != null ? v : "";
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

    /** An open in progress in MidiService. Fields guarded by {@link #lock}. */
    private static final class PendingOpen {
        final CountDownLatch latch = new CountDownLatch(1);
        int waiters;
        boolean done;
        boolean claimed;
        MidiDevice device;
    }
}
