package org.nx2a;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.widget.Toast;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
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
    private File imported() { return store().imported(prefs().getString("file", "")); }

    // Called once from ANativeActivity_onCreate, before any game thread is started.
    String forLaunch() {
        File file = imported();
        store().cleanAtLaunch(file);
        int mode = prefs().getInt("mode", DEFAULT);
        if (mode == DEFAULT) return null;
        if (mode == SYSTEM) return "system";
        if (VulkanDriverStore.canLaunch(file)) return file.getPath();
        prefs().edit().putInt("mode", SYSTEM).commit();
        note("The imported Vulkan driver was unavailable or did not reach the first game frame. Using the system driver.");
        return "system";
    }

    void open() {
        activity.runOnUiThread(() -> {
            if (busy || activity.isFinishing() || activity.isDestroyed()) return;
            File file = imported();
            boolean hasFile = file != null && file.isFile();
            String[] choices = hasFile
                    ? new String[]{"Package default", "System driver", "Imported: " + prefs().getString("label", "driver.so")}
                    : new String[]{"Package default", "System driver"};
            AlertDialog.Builder dialog = new AlertDialog.Builder(activity).setTitle("Vulkan driver for next launch")
                    .setSingleChoiceItems(choices, prefs().getInt("mode", DEFAULT), (d, which) -> {
                        d.dismiss();
                        if (which == IMPORTED && VulkanDriverStore.pending(file).exists() && !VulkanDriverStore.pending(file).delete()) {
                            note("Could not reset the driver for another attempt.");
                            return;
                        }
                        boolean saved = prefs().edit().putInt("mode", which).commit();
                        note(saved ? "Driver selected. Fully close and reopen the game to apply." : "Could not save driver selection.");
                    }).setNegativeButton("Cancel", null);
            if (new File(activity.getApplicationInfo().nativeLibraryDir, "libadrenotools.so").isFile())
                dialog.setNeutralButton("Import .so", (d, w) -> pick());
            dialog.show();
        });
    }

    private void pick() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        try { busy = true; activity.startActivityForResult(intent, REQUEST); }
        catch (RuntimeException e) { busy = false; note("No document picker is available."); }
    }

    boolean result(int request, int code, Intent data) {
        if (request != REQUEST) return false;
        Uri uri = code == Activity.RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) busy = false;
        else worker.execute(() -> copy(uri));
        return true;
    }

    private String label(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) return c.getString(0);
        } catch (RuntimeException ignored) { }
        return "driver.so";
    }

    private void copy(Uri uri) {
        File file = null;
        try (InputStream stream = activity.getContentResolver().openInputStream(uri)) {
            file = store().copy(stream);
            if (!prefs().edit().putString("file", file.getName()).putString("label", label(uri))
                    .putInt("mode", IMPORTED).commit()) throw new IOException("Could not save selection");
            file = null;
            note("Driver imported and selected. Fully close and reopen the game to apply.");
        } catch (Exception e) {
            android.util.Log.w("nx2a-driver", "Driver import failed", e);
            note("Driver import failed. " + (e.getMessage() == null ? "Select a valid arm64 .so file." : e.getMessage()));
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
