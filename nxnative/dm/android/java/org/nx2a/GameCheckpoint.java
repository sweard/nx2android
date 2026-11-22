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
            RESTORED = 6, RESTORE_FAILED = 7, SAVE_CONFLICT = 8, WRITE_FAILED = 9, CONFLICT_EXPIRED = 10;
    private AlertDialog checkpointProgress;
    private AlertDialog saveConflict;
    private boolean saveOnFocus;
    private boolean savingCheckpoint;

    void close() {
        saveOnFocus = false;
        if (saveConflict != null) saveConflict.dismiss();
        hideCheckpointProgress();
    }

    private void hideCheckpointProgress() {
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

    private void resolveConflict(long ticket, boolean overwrite) {
        if (ticket == 0) return;
        new Thread(() -> {
            int result = activity.resolveSaveConflict(ticket, overwrite);
            if (!overwrite) return;
            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed()) return;
                String message = result == 0 ? "Game save updated. The previous save is under Import save (before-overwrite)."
                        : result == 1 ? "This confirmation expired. No files were overwritten."
                        : "Backup or overwrite failed. Your existing game saves are safe.";
                Toast.makeText(activity, message, Toast.LENGTH_LONG).show();
            });
        }, "game-save-conflict").start();
    }

    private void offerSaveAfterConflict(long ticket) {
        if (saveConflict != null || saveOnFocus || savingCheckpoint) return;
        String message = "The game couldn’t save because the same file changed in normal play. "
                + "Save checkpoint replaces your previous checkpoint. Wait for “Checkpoint saved” before quitting.";
        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                .setTitle("Game save conflict")
                .setPositiveButton("Save checkpoint", (dialog, which) -> {
                    resolveConflict(ticket, false);
                    saveOnFocus = true;
                })
                .setNegativeButton("Keep playing", (dialog, which) -> resolveConflict(ticket, false))
                .setOnCancelListener(dialog -> resolveConflict(ticket, false));
        if (ticket != 0) {
            message += "\n\nOverwrite backs up your normal save first. Replacing a shared file can affect more than one slot.";
            builder.setNeutralButton("Overwrite conflicting files", (dialog, which) -> resolveConflict(ticket, true));
        }
        saveConflict = builder.setMessage(message).setOnDismissListener(dialog -> {
            if (dialog == saveConflict) saveConflict = null;
            saveWhenReady();
        }).show();
    }

    // A focus change wakes guest threads. Start capture after the choice dialog
    // is dismissed and NativeActivity has delivered focus back to the game.
    void saveWhenReady() {
        if (!saveOnFocus || saveConflict != null || !activity.hasWindowFocus()
                || activity.isFinishing() || activity.isDestroyed()) return;
        saveOnFocus = false;
        activity.requestCheckpoint();
    }

    void checkpointStatus(int state) {
        // Bind the button to the originating commit, before posting to the main thread.
        final long ticket = state == SAVE_CONFLICT ? activity.saveConflictId() : 0;
        activity.runOnUiThread(() -> {
            if (activity.isFinishing() || activity.isDestroyed()) return;
            if (state == CONFLICT_EXPIRED) {
                if (saveConflict != null) {
                    saveConflict.dismiss();
                    saveConflict = null;
                }
                return;
            }
            if (state == SAVE_CONFLICT) {
                if (ticket == 0 || ticket == activity.saveConflictId()) offerSaveAfterConflict(ticket);
                return;
            }
            if (state == WRITE_FAILED) {
                Toast.makeText(activity, "Game save failed. Your existing saves are safe.", Toast.LENGTH_LONG).show();
                return;
            }
            if (state == SAVING) {
                savingCheckpoint = true;
                showCheckpointProgress("Saving checkpoint…", true);
                return;
            }
            if (state == RESTORED && savingCheckpoint) return;
            savingCheckpoint = false;
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
                .setMessage("Restoring resumes this moment. New in-game saves carry over to normal play. Start normally deletes this checkpoint.")
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
