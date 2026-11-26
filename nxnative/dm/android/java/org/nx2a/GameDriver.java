package org.nx2a;

import static org.nx2a.GameUi.tr;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.icu.text.Collator;
import android.icu.text.RuleBasedCollator;
import android.text.InputFilter;
import android.widget.EditText;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.widget.Toast;
import java.io.File;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Per-game Vulkan driver selection. Only a cold start consumes the selection. */
final class GameDriver {
    private static final int REQUEST = 0x564b;
    private static final int DEFAULT = 0, SYSTEM = 1, IMPORTED = 2;
    private final Activity activity;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile boolean busy;

    GameDriver(Activity activity) { this.activity = activity; }

    private SharedPreferences prefs() { return activity.getSharedPreferences("vulkan_driver", 0); }
    private VulkanDriverStore store() {
        return new VulkanDriverStore(activity.getDir("gpu_drivers", Activity.MODE_PRIVATE));
    }

    // Called once from ANativeActivity_onCreate, before any game thread is started.
    String forLaunch() {
        Map<String, ?> preferences = prefs().getAll();
        VulkanDriverStore store = store();
        File file = store.selected(preferences);
        Map<File, String> drivers = store.drivers(preferences);
        store.cleanAtLaunch(drivers.keySet());
        int mode = prefs().getInt("mode", DEFAULT);
        if (mode == DEFAULT) return null;
        if (mode == SYSTEM) return "system";
        if (drivers.containsKey(file) && VulkanDriverStore.canLaunch(file)) return file.getPath();
        prefs().edit().putInt("mode", SYSTEM).commit();
        note(tr("The imported Vulkan driver was unavailable or did not reach the first game frame. Using the system driver.", "导入的 Vulkan 驱动不可用，或未能显示游戏首帧。已切换到系统驱动。"));
        return "system";
    }

    void open() {
        activity.runOnUiThread(() -> {
            if (busy || activity.isFinishing() || activity.isDestroyed()) return;
            Map<String, ?> preferences = prefs().getAll();
            VulkanDriverStore store = store();
            List<Map.Entry<File, String>> drivers = new ArrayList<>(store.drivers(preferences).entrySet());
            RuleBasedCollator order = (RuleBasedCollator)Collator.getInstance();
            order.setNumericCollation(true);
            drivers.sort((a, b) -> order.compare(a.getValue(), b.getValue()));
            String[] choices = new String[2 + drivers.size()];
            choices[DEFAULT] = tr("Package default", "安装包默认驱动");
            choices[SYSTEM] = tr("System driver", "系统驱动");
            int mode = prefs().getInt("mode", DEFAULT);
            int checked = mode == IMPORTED ? -1 : mode;
            File selected = store.selected(preferences);
            for (int i = 0; i < drivers.size(); i++) {
                choices[i + 2] = drivers.get(i).getValue();
                if (mode == IMPORTED && drivers.get(i).getKey().equals(selected)) checked = i + 2;
            }
            AlertDialog.Builder dialog = new AlertDialog.Builder(activity).setTitle(tr("Vulkan driver for next launch", "下次启动时使用的 Vulkan 驱动"))
                    .setSingleChoiceItems(choices, checked, (d, which) -> {
                        d.dismiss();
                        SharedPreferences.Editor edit = prefs().edit();
                        if (which >= 2) {
                            File file = drivers.get(which - 2).getKey();
                            File marker = VulkanDriverStore.pending(file);
                            if (marker.exists() && !marker.delete()) {
                                note(tr("Could not reset the driver for another attempt.", "无法重置驱动以重试。"));
                                return;
                            }
                            edit.putInt("mode", IMPORTED).putString("selected", file.getName());
                        } else {
                            edit.putInt("mode", which);
                        }
                        note(edit.commit() ? tr("Driver selected. Fully close and reopen the game to apply.", "已选择驱动。完全退出并重新打开游戏后生效。") : tr("Could not save driver selection.", "无法保存驱动选择。"));
                    }).setNegativeButton(tr("Cancel", "取消"), null);
            if (new File(activity.getApplicationInfo().nativeLibraryDir, "libadrenotools.so").isFile())
                dialog.setNeutralButton(tr("Import .so", "导入 .so"), (d, w) -> pick());
            dialog.show();
        });
    }

    private void pick() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        try { busy = true; activity.startActivityForResult(intent, REQUEST); }
        catch (RuntimeException e) { busy = false; note(tr("No document picker is available.", "无法打开系统文件选择器。")); }
    }

    boolean result(int request, int code, Intent data) {
        if (request != REQUEST) return false;
        Uri uri = code == Activity.RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) busy = false;
        else {
            busy = true;
            worker.execute(() -> {
                String version = VulkanDriverStore.suggestedVersion(label(uri));
                activity.runOnUiThread(() -> nameImport(uri, version));
            });
        }
        return true;
    }

    private String label(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) return c.getString(0);
        } catch (RuntimeException ignored) { }
        return "driver.so";
    }

    private void nameImport(Uri uri, String version) {
        if (activity.isDestroyed() || activity.isFinishing()) return;
        EditText input = new EditText(activity);
        input.setSingleLine(true);
        input.setFilters(new InputFilter[]{new InputFilter.LengthFilter(80)});
        input.setHint(tr("e.g. V40", "例如 V40"));
        input.setText(version);
        input.selectAll();
        AlertDialog dialog = new AlertDialog.Builder(activity).setTitle(tr("Driver version or name", "驱动版本或名称"))
                .setView(input).setPositiveButton(tr("Import", "导入"), null)
                .setNegativeButton(tr("Cancel", "取消"), (d, which) -> busy = false)
                .setOnCancelListener(d -> busy = false).create();
        dialog.show();
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            String name = input.getText().toString().trim();
            if (name.isEmpty()) { input.setError(tr("Enter a version or name.", "请输入版本或名称。")); return; }
            for (String existing : store().drivers(prefs().getAll()).values()) {
                if (existing.equalsIgnoreCase(name)) { input.setError(tr("This name is already in use.", "此名称已被使用。")); return; }
            }
            dialog.dismiss();
            worker.execute(() -> copy(uri, name));
        });
    }

    private void copy(Uri uri, String name) {
        File file = null;
        try (InputStream stream = activity.getContentResolver().openInputStream(uri)) {
            file = store().copy(stream);
            if (!prefs().edit().putString(file.getName(), name).putString("selected", file.getName())
                    .putInt("mode", IMPORTED).commit()) throw new VulkanDriverStore.ImportException("Could not save selection", "无法保存驱动选择。");
            file = null;
            note(tr("Driver imported and selected. Fully close and reopen the game to apply.", "已导入并选择驱动。完全退出并重新打开游戏后生效。"));
        } catch (Exception e) {
            android.util.Log.w("nx2a-driver", "Driver import failed", e);
            String detail = e instanceof VulkanDriverStore.ImportException
                    ? tr(e.getMessage(), ((VulkanDriverStore.ImportException)e).chinese)
                    : tr(e.getMessage() == null ? "Select a valid arm64 .so file." : e.getMessage(),
                            "请选择可读取的 arm64 共享库（.so）文件。");
            note(tr("Driver import failed. ", "驱动导入失败。") + detail);
        } finally {
            if (file != null) file.delete();
            busy = false;
        }
    }

    private void note(String message) {
        activity.runOnUiThread(() -> {
            if (!activity.isFinishing() && !activity.isDestroyed())
                Toast.makeText(activity, message, Toast.LENGTH_LONG).show();
        });
    }

    void close() { worker.shutdownNow(); }
}
