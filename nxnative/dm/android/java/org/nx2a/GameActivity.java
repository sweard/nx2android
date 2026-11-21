package org.nx2a;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Intent;
import java.nio.charset.StandardCharsets;

/** System text input and document import for ordinary single-screen games. */
public class GameActivity extends NativeActivity {
    private final GameKeyboard keyboard = new GameKeyboard(this);
    private final GameFiles files = new GameFiles(this);

    private native void chooseCheckpoint(boolean restore);

    private void offerRuntimeCheckpoint() {
        getWindow().getDecorView().post(() -> new AlertDialog.Builder(this)
                .setTitle("恢复游戏检查点？")
                .setMessage("回到手动保存检查点时的位置。此测试版会在独立的存档副本中继续游戏。")
                .setPositiveButton("恢复检查点", (dialog, which) -> chooseCheckpoint(true))
                .setNegativeButton("正常启动", (dialog, which) -> chooseCheckpoint(false))
                .setOnCancelListener(dialog -> chooseCheckpoint(false))
                .show());
    }

    private void offerCheckpoint() {
        runOnUiThread(() -> new AlertDialog.Builder(this)
                .setTitle("Game checkpoint")
                .setMessage("Restore the settings saved with your checkpoint. Use Continue in the game to load your last in-game save.")
                .setPositiveButton("Restore checkpoint", (dialog, which) -> chooseCheckpoint(true))
                .setNegativeButton("Start normally", (dialog, which) -> chooseCheckpoint(false))
                .setOnCancelListener(dialog -> chooseCheckpoint(false))
                .show());
    }

    public void importLosslessDll() { files.openLossless(); }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        if (!files.result(request, result, data)) super.onActivityResult(request, result, data);
    }

    private static String text(byte[] value) {
        return value == null ? "" : new String(value, StandardCharsets.UTF_8);
    }

    // Guest callers wait off the UI thread. Ordinary UTF-8, not JNI modified UTF-8.
    public byte[] readKeyboard(byte[] title, byte[] subtitle, byte[] guide, byte[] initial, int min, int max) {
        return keyboard.read(text(title), text(subtitle), text(guide), text(initial), min, max);
    }

    @Override protected void onResume() { super.onResume(); keyboard.pause(false); }
    @Override protected void onPause() { keyboard.pause(true); super.onPause(); }
    @Override protected void onDestroy() { files.close(); keyboard.pause(true); super.onDestroy(); }
}
