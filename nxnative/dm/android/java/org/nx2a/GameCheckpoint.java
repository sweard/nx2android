package org.nx2a;

import static org.nx2a.GameUi.tr;

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
                String message = result == 0 ? tr("Game save updated. The previous save is under Import save (before-overwrite).", "游戏存档已更新。可以在“导入存档”中找到覆盖前的备份（before-overwrite）。")
                        : result == 1 ? tr("This confirmation expired. No files were overwritten.", "此次确认已过期，存档文件未被覆盖。")
                        : tr("Backup or overwrite failed. Your existing game saves are safe.", "备份或覆盖失败，原有游戏存档已保留。");
                Toast.makeText(activity, message, Toast.LENGTH_LONG).show();
            });
        }, "game-save-conflict").start();
    }

    private void offerSaveAfterConflict(long ticket) {
        if (saveConflict != null || saveOnFocus || savingCheckpoint) return;
        String message = tr("The game couldn’t save because the same file changed in normal play. "
                + "Save checkpoint replaces your previous checkpoint. Wait for “Checkpoint saved” before quitting.",
                "游戏无法保存，因为同一文件已在正常游玩时发生变化。保存检查点会替换上一个检查点。请等到出现“检查点已保存”后再退出。");
        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                .setTitle(tr("Game save conflict", "游戏存档冲突"))
                .setPositiveButton(tr("Save checkpoint", "保存检查点"), (dialog, which) -> {
                    resolveConflict(ticket, false);
                    saveOnFocus = true;
                })
                .setNegativeButton(tr("Keep playing", "继续游戏"), (dialog, which) -> resolveConflict(ticket, false))
                .setOnCancelListener(dialog -> resolveConflict(ticket, false));
        if (ticket != 0) {
            message += "\n\n" + tr("Overwrite backs up your normal save first. Replacing a shared file can affect more than one slot.",
                    "覆盖前会先备份正常游玩时的存档。替换共用文件可能影响多个存档位。");
            builder.setNeutralButton(tr("Overwrite conflicting files", "覆盖冲突文件"), (dialog, which) -> resolveConflict(ticket, true));
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
                Toast.makeText(activity, tr("Game save failed. Your existing saves are safe.", "游戏存档保存失败，原有存档已保留。"), Toast.LENGTH_LONG).show();
                return;
            }
            if (state == SAVING) {
                savingCheckpoint = true;
                showCheckpointProgress(tr("Saving checkpoint…", "正在保存检查点…"), true);
                return;
            }
            if (state == RESTORED && savingCheckpoint) return;
            savingCheckpoint = false;
            hideCheckpointProgress();
            if (state == RESTORED) return;
            if (state == RESTORE_FAILED) {
                new AlertDialog.Builder(activity).setTitle(tr("Couldn’t restore checkpoint", "无法恢复检查点"))
                        .setMessage(tr("Reopen the game and choose Start normally or try restoring the checkpoint again.", "请重新打开游戏，选择“正常启动”或再次尝试恢复检查点。"))
                        .setCancelable(false).setPositiveButton(tr("Close game", "关闭游戏"), (dialog, which) -> activity.finish()).show();
            } else if (state == SAVED || state == SAVE_FAILED || state == NO_SAVE) {
                String message = state == SAVED ? tr("Checkpoint saved", "检查点已保存")
                        : state == NO_SAVE ? tr("No in-game save yet", "尚无游戏内存档") : tr("Couldn’t save checkpoint", "无法保存检查点");
                Toast.makeText(activity, message, Toast.LENGTH_SHORT).show();
            }
        });
    }

    void offerRuntimeCheckpoint() {
        activity.getWindow().getDecorView().post(() -> new AlertDialog.Builder(activity)
                .setTitle(tr("Restore checkpoint?", "恢复检查点？"))
                .setMessage(tr("Restoring resumes this moment. New in-game saves carry over to normal play. Start normally deletes this checkpoint.", "恢复后将从此处继续。在游戏内新保存的进度也可在正常启动后读取。选择“正常启动”会删除此检查点。"))
                .setPositiveButton(tr("Restore checkpoint", "恢复检查点"), (dialog, which) -> {
                    showCheckpointProgress(tr("Restoring checkpoint…", "正在恢复检查点…"), false);
                    activity.chooseCheckpoint(RESTORE);
                })
                .setNegativeButton(tr("Start normally", "正常启动"), (dialog, which) -> activity.chooseCheckpoint(DISCARD))
                .setOnCancelListener(dialog -> activity.chooseCheckpoint(SKIP))
                .show());
    }

    void offerCheckpoint() {
        activity.runOnUiThread(() -> new AlertDialog.Builder(activity)
                .setTitle(tr("Game checkpoint", "游戏检查点"))
                .setMessage(tr("Restore the settings saved with your checkpoint. Use Continue in the game to load your last in-game save.", "恢复检查点中保存的设置，然后在游戏中选择“继续”以读取上次的游戏内存档。"))
                .setPositiveButton(tr("Restore checkpoint", "恢复检查点"), (dialog, which) -> activity.chooseCheckpoint(RESTORE))
                .setNegativeButton(tr("Start normally", "正常启动"), (dialog, which) -> activity.chooseCheckpoint(SKIP))
                .setOnCancelListener(dialog -> activity.chooseCheckpoint(SKIP))
                .show());
    }
}
