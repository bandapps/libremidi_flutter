package dev.test.loopback;

// The loopback device with a service that takes 7 s to start — longer than
// the plugin's 5 s open timeout, as a slow Bluetooth bind can be.
public class SlowLoopbackService extends LoopbackService {
    @Override
    public void onCreate() {
        try {
            Thread.sleep(7000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        super.onCreate();
    }
}
