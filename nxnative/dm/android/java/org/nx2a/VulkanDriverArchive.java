package org.nx2a;

import java.io.DataInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Enumeration;
import java.util.zip.CRC32;
import java.util.zip.CheckedInputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.json.JSONException;
import org.json.JSONObject;

/** Reads an Android GPU driver package from a document provider, including non-seekable streams. */
final class VulkanDriverArchive {
    private static final int MAX_METADATA = 64 * 1024;

    final File library;
    final String name;

    private VulkanDriverArchive(File library, String name) { this.library = library; this.name = name; }

    static VulkanDriverArchive unpack(InputStream source, File cache, VulkanDriverStore store) throws IOException {
        File archive = File.createTempFile("vulkan-driver-", ".zip", cache);
        File copied = null;
        try {
            String label;
            try (FileOutputStream out = new FileOutputStream(archive)) {
                byte[] buffer = new byte[65536];
                int count, total = 0;
                while ((count = source.read(buffer)) != -1) {
                    if (Thread.currentThread().isInterrupted()) throw new IOException("Import cancelled");
                    if (count > VulkanDriverStore.MAX_BYTES - total)
                        throw new VulkanDriverStore.ImportException("Driver ZIP exceeds 256 MiB.", "驱动 ZIP 超过 256 MiB。");
                    out.write(buffer, 0, count);
                    total += count;
                }
            }
            try (ZipFile zip = new ZipFile(archive)) {
                ZipEntry meta = entry(zip, "meta.json");
                if (meta.getSize() <= 0 || meta.getSize() > MAX_METADATA) throw invalidMetadata();
                byte[] bytes = new byte[(int)meta.getSize()];
                try (DataInputStream in = new DataInputStream(zip.getInputStream(meta))) {
                    in.readFully(bytes);
                    if (in.read() != -1) throw invalidMetadata();
                }
                JSONObject json = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
                Object name = json.opt("name"), library = json.opt("libraryName");
                if (!(name instanceof String) || !(library instanceof String)) throw invalidMetadata();
                label = ((String)name).trim();
                String basename = (String)library;
                if (label.isEmpty() || label.length() > 80 || label.matches("(?s).*\\p{Cntrl}.*")
                        || !basename.matches("[a-zA-Z0-9_+-][a-zA-Z0-9_.+-]*\\.so")) throw invalidMetadata();
                ZipEntry driver = entry(zip, basename);
                if (driver.getSize() < 64 || driver.getSize() > VulkanDriverStore.MAX_BYTES)
                    throw new VulkanDriverStore.ImportException("Invalid driver size in ZIP.", "ZIP 内驱动文件大小无效。");
                try (CheckedInputStream in = new CheckedInputStream(zip.getInputStream(driver), new CRC32())) {
                    copied = store.copy(in);
                    if (copied.length() != driver.getSize() || in.getChecksum().getValue() != driver.getCrc())
                        throw new VulkanDriverStore.ImportException("Driver ZIP checksum mismatch.", "驱动 ZIP 校验失败。");
                }
            } catch (JSONException e) {
                throw invalidMetadata();
            }
            VulkanDriverArchive result = new VulkanDriverArchive(copied, label);
            copied = null;
            return result;
        } finally {
            if (copied != null) copied.delete();
            archive.delete();
        }
    }

    private static ZipEntry entry(ZipFile zip, String name) throws IOException {
        ZipEntry found = null;
        for (Enumeration<? extends ZipEntry> entries = zip.entries(); entries.hasMoreElements();) {
            ZipEntry e = entries.nextElement();
            if (!name.equals(e.getName())) continue;
            if (found != null || e.isDirectory()) throw invalidMetadata();
            found = e;
        }
        if (found == null) throw new VulkanDriverStore.ImportException("Driver ZIP is missing " + name, "驱动 ZIP 缺少 " + name);
        return found;
    }

    private static VulkanDriverStore.ImportException invalidMetadata() {
        return new VulkanDriverStore.ImportException("Invalid driver package metadata (meta.json).", "驱动包元数据（meta.json）无效。");
    }
}
