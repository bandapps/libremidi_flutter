package dev.celtera.libremidi_flutter;

import androidx.annotation.NonNull;
import io.flutter.embedding.engine.plugins.FlutterPlugin;

import dev.celtera.libremidi.MidiBridge;

public class LibremidiFlutterPlugin implements FlutterPlugin {
    private static final String TAG = "LibremidiFlutterPlugin";

    // Load the native library from Java so that JNI_OnLoad runs: it receives
    // the JavaVM and binds the MidiBridge methods while the app's class loader
    // is current. Dart opens the same library afterwards.
    static {
        try {
            System.loadLibrary("libremidi_flutter");
        } catch (UnsatisfiedLinkError e) {
            android.util.Log.e(TAG, "Failed to load native library: " + e.getMessage());
        }
    }

    @Override
    public void onAttachedToEngine(@NonNull FlutterPluginBinding binding) {
        MidiBridge.init(binding.getApplicationContext());
    }

    @Override
    public void onDetachedFromEngine(@NonNull FlutterPluginBinding binding) {
        // MidiBridge is process-wide and shared by all engines; nothing to undo.
    }
}
