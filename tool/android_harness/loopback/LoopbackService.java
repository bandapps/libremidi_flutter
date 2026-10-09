package dev.test.loopback;

import android.media.midi.MidiDeviceService;
import android.media.midi.MidiReceiver;
import java.io.IOException;

// One input, one output: everything written to the input comes back out.
public class LoopbackService extends MidiDeviceService {
    private final MidiReceiver echo = new MidiReceiver() {
        @Override
        public void onSend(byte[] data, int offset, int count, long timestamp) throws IOException {
            MidiReceiver[] outs = getOutputPortReceivers();
            if (outs != null && outs.length > 0 && outs[0] != null) {
                outs[0].send(data, offset, count, timestamp);
            }
        }
    };

    @Override
    public MidiReceiver[] onGetInputPortReceivers() {
        return new MidiReceiver[] { echo };
    }
}
