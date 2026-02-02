package dev.celtera.libremidi_flutter;

import androidx.annotation.NonNull;
import io.flutter.embedding.engine.plugins.FlutterPlugin;

// Import MIDI callbacks to ensure they're loaded by the class loader
import dev.celtera.libremidi.MidiDeviceCallback;
import dev.celtera.libremidi.MidiDeviceStatusCallback;

public class LibremidiFlutterPlugin implements FlutterPlugin {
    private static final String TAG = "LibremidiFlutterPlugin";

    // Static block: Load native library from Java context
    // This ensures JNI FindClass can access app classes
    static {
        try {
            System.loadLibrary("libremidi_flutter");
            android.util.Log.i(TAG, "Native library loaded successfully from Java context");

            // Force load callback classes to make them available for JNI
            Class.forName("dev.celtera.libremidi.MidiDeviceCallback");
            Class.forName("dev.celtera.libremidi.MidiDeviceStatusCallback");
            android.util.Log.i(TAG, "MIDI callback classes loaded");
        } catch (UnsatisfiedLinkError e) {
            android.util.Log.e(TAG, "Failed to load native library: " + e.getMessage());
        } catch (ClassNotFoundException e) {
            android.util.Log.e(TAG, "Failed to load callback class: " + e.getMessage());
        }
    }

    @Override
    public void onAttachedToEngine(@NonNull FlutterPluginBinding binding) {
        android.util.Log.i(TAG, "LibremidiFlutterPlugin attached to engine");
    }

    @Override
    public void onDetachedFromEngine(@NonNull FlutterPluginBinding binding) {
        android.util.Log.i(TAG, "LibremidiFlutterPlugin detached from engine");
    }
}
