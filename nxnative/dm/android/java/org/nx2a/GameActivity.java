package org.nx2a;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Intent;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;
import java.nio.charset.StandardCharsets;

/** System text input and document import for ordinary single-screen games. */
public class GameActivity extends NativeActivity {
    private final GameKeyboard keyboard = new GameKeyboard(this);
    private final GameFiles files = new GameFiles(this);

    private native void chooseCheckpoint(boolean restore);

    // Status values shared with checkpoint_android.h.
    private static final int SAVING = 2, SAVED = 3, NO_SAVE = 4, SAVE_FAILED = 5,
            RESTORED = 6, RESTORE_FAILED = 7;
    private AlertDialog checkpointProgress;

    private void hideCheckpointProgress() {
        if (checkpointProgress != null) {
            checkpointProgress.dismiss();
            checkpointProgress = null;
        }
    }

    private void showCheckpointProgress(String message) {
        hideCheckpointProgress();
        int unit = Math.round(8 * getResources().getDisplayMetrics().density);
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(3 * unit, 2 * unit, 3 * unit, 2 * unit);
        row.addView(new ProgressBar(this), 4 * unit, 4 * unit);
        TextView label = new TextView(this);
        label.setPadding(2 * unit, 0, 0, 0);
        label.setTextSize(18);
        label.setText(message);
        row.addView(label);
        checkpointProgress = new AlertDialog.Builder(this).setView(row).setCancelable(false).show();
        // Override the theme's minimum dialog width, preserving its background insets.
        Window window = checkpointProgress.getWindow();
        View decor = window.getDecorView();
        int insets = decor.getPaddingLeft() + decor.getPaddingRight();
        int available = Math.max(1, getWindow().getDecorView().getWidth() - 4 * unit - insets);
        row.measure(View.MeasureSpec.makeMeasureSpec(available, View.MeasureSpec.AT_MOST),
                View.MeasureSpec.UNSPECIFIED);
        window.setLayout(row.getMeasuredWidth() + insets, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private void checkpointStatus(int state) {
        runOnUiThread(() -> {
            if (isFinishing() || isDestroyed()) return;
            if (state == SAVING) {
                showCheckpointProgress("Saving checkpoint…");
                return;
            }
            hideCheckpointProgress();
            if (state == RESTORED) return;
            if (state == RESTORE_FAILED) {
                new AlertDialog.Builder(this).setTitle("Couldn’t restore checkpoint")
                        .setMessage("Reopen the game and choose Start normally or try restoring the checkpoint again.")
                        .setCancelable(false).setPositiveButton("Close game", (dialog, which) -> finish()).show();
            } else if (state == SAVED || state == SAVE_FAILED || state == NO_SAVE) {
                String message = state == SAVED ? "Checkpoint saved"
                        : state == NO_SAVE ? "No in-game save yet" : "Couldn’t save checkpoint";
                Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
            }
        });
    }

    private void offerRuntimeCheckpoint() {
        getWindow().getDecorView().post(() -> new AlertDialog.Builder(this)
                .setTitle("Restore checkpoint?")
                .setMessage("Resume where you saved the checkpoint. This test version uses a separate copy of your save data.")
                .setPositiveButton("Restore checkpoint", (dialog, which) -> {
                    showCheckpointProgress("Restoring checkpoint…");
                    chooseCheckpoint(true);
                })
                .setNegativeButton("Start normally", (dialog, which) -> chooseCheckpoint(false))
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
    @Override protected void onDestroy() { hideCheckpointProgress(); files.close(); keyboard.pause(true); super.onDestroy(); }
}
