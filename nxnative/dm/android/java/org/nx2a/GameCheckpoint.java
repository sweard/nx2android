package org.nx2a;

import android.app.AlertDialog;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

final class GameCheckpoint {
    private final GameActivity activity;

    GameCheckpoint(GameActivity activity) { this.activity = activity; }

    private static final int SKIP = 0, RESTORE = 1, DISCARD = 2;

    // Status values shared with checkpoint_android.h.
    private static final int SAVING = 2, SAVED = 3, NO_SAVE = 4, SAVE_FAILED = 5,
            RESTORED = 6, RESTORE_FAILED = 7;
    private AlertDialog checkpointProgress;

    void hideCheckpointProgress() {
        if (checkpointProgress != null) {
            checkpointProgress.dismiss();
            checkpointProgress = null;
        }
    }

    private void showCheckpointProgress(String message, boolean keepGameFocus) {
        hideCheckpointProgress();
        int unit = Math.round(8 * activity.getResources().getDisplayMetrics().density);
        LinearLayout row = new LinearLayout(activity);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(3 * unit, 2 * unit, 3 * unit, 2 * unit);
        row.addView(new ProgressBar(activity), 4 * unit, 4 * unit);
        TextView label = new TextView(activity);
        label.setPadding(2 * unit, 0, 0, 0);
        label.setTextSize(18);
        label.setText(message);
        row.addView(label);
        checkpointProgress = new AlertDialog.Builder(activity).setView(row).setCancelable(false).create();
        Window window = checkpointProgress.getWindow();
        // A focus loss posts an applet event and wakes guest threads. During a
        // save their wait queues must stay frozen until the capture finishes.
        if (keepGameFocus) window.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        checkpointProgress.show();
        // Override the theme's minimum dialog width, preserving its background insets.
        View decor = window.getDecorView();
        int insets = decor.getPaddingLeft() + decor.getPaddingRight();
        int available = Math.max(1, activity.getWindow().getDecorView().getWidth() - 4 * unit - insets);
        row.measure(View.MeasureSpec.makeMeasureSpec(available, View.MeasureSpec.AT_MOST),
                View.MeasureSpec.UNSPECIFIED);
        window.setLayout(row.getMeasuredWidth() + insets, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    void checkpointStatus(int state) {
        activity.runOnUiThread(() -> {
            if (activity.isFinishing() || activity.isDestroyed()) return;
            if (state == SAVING) {
                showCheckpointProgress("Saving checkpoint…", true);
                return;
            }
            hideCheckpointProgress();
            if (state == RESTORED) return;
            if (state == RESTORE_FAILED) {
                new AlertDialog.Builder(activity).setTitle("Couldn’t restore checkpoint")
                        .setMessage("Reopen the game and choose Start normally or try restoring the checkpoint again.")
                        .setCancelable(false).setPositiveButton("Close game", (dialog, which) -> activity.finish()).show();
            } else if (state == SAVED || state == SAVE_FAILED || state == NO_SAVE) {
                String message = state == SAVED ? "Checkpoint saved"
                        : state == NO_SAVE ? "No in-game save yet" : "Couldn’t save checkpoint";
                Toast.makeText(activity, message, Toast.LENGTH_SHORT).show();
            }
        });
    }

    void offerRuntimeCheckpoint() {
        activity.getWindow().getDecorView().post(() -> new AlertDialog.Builder(activity)
                .setTitle("Restore checkpoint?")
                .setMessage("Restoring uses a separate copy of your save data. Start normally deletes this checkpoint.")
                .setPositiveButton("Restore checkpoint", (dialog, which) -> {
                    showCheckpointProgress("Restoring checkpoint…", false);
                    activity.chooseCheckpoint(RESTORE);
                })
                .setNegativeButton("Start normally", (dialog, which) -> activity.chooseCheckpoint(DISCARD))
                .setOnCancelListener(dialog -> activity.chooseCheckpoint(SKIP))
                .show());
    }

    void offerCheckpoint() {
        activity.runOnUiThread(() -> new AlertDialog.Builder(activity)
                .setTitle("Game checkpoint")
                .setMessage("Restore the settings saved with your checkpoint. Use Continue in the game to load your last in-game save.")
                .setPositiveButton("Restore checkpoint", (dialog, which) -> activity.chooseCheckpoint(RESTORE))
                .setNegativeButton("Start normally", (dialog, which) -> activity.chooseCheckpoint(SKIP))
                .setOnCancelListener(dialog -> activity.chooseCheckpoint(SKIP))
                .show());
    }
}
