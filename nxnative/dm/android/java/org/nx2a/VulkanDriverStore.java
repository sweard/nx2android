package org.nx2a;

import java.io.DataInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.UUID;

/** Imported libraries have immutable names so a new import cannot replace a mapped file.
 * The activity publishes the returned basename and selection together in private preferences. */
final class VulkanDriverStore {
    private static final int MAX_BYTES = 256 * 1024 * 1024;
    private final File directory;

    VulkanDriverStore(File directory) { this.directory = directory; }

    File imported(String name) {
        return name.matches("libgpu-[0-9a-f-]{36}\\.so") ? new File(directory, name) : null;
    }

    void cleanAtLaunch(File keep) {
        File[] files = directory.listFiles();
        if (files != null) for (File file : files) {
            if (file.getName().startsWith("libgpu-") && !file.equals(keep)
                    && (keep == null || !file.equals(pending(keep)))) file.delete();
        }
    }

    static File pending(File file) { return new File(file.getPath() + ".pending"); }

    static boolean canLaunch(File file) {
        return file != null && file.isFile() && file.canRead() && !pending(file).exists();
    }

    File copy(InputStream stream) throws IOException {
        if (stream == null) throw new IOException("Unreadable document");
        DataInputStream in = new DataInputStream(stream);
        byte[] header = new byte[64];
        in.readFully(header);
        ByteBuffer elf = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        if (header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F'
                || header[4] != 2 || header[5] != 1 || header[6] != 1 || elf.getShort(16) != 3
                || elf.getShort(18) != 183 || elf.getInt(20) != 1 || elf.getShort(52) != 64)
            throw new IOException("Select an Android arm64 shared library (.so).");
        File file = new File(directory, "libgpu-" + UUID.randomUUID() + ".so");
        if (!file.createNewFile()) throw new IOException("Cannot create driver file");
        boolean ready = false;
        try {
            try (FileOutputStream out = new FileOutputStream(file)) {
                out.write(header);
                byte[] buffer = new byte[65536];
                int count, total = header.length;
                while ((count = in.read(buffer)) != -1) {
                    if (Thread.currentThread().isInterrupted()) throw new IOException("Import cancelled");
                    if (count > MAX_BYTES - total) throw new IOException("Driver exceeds the 256 MiB limit.");
                    out.write(buffer, 0, count);
                    total += count;
                }
                out.getFD().sync();
            }
            checkSegments(file, elf);
            if (!file.setReadOnly()) throw new IOException("Could not protect driver file");
            ready = true;
            return file;
        } finally {
            if (!ready) file.delete();
        }
    }

    // The HAL is probed by Vulkan at startup. Reject truncated or non-loadable ELF files
    // before replacing the user's previous selection, including stripped ELF libraries.
    private static void checkSegments(File file, ByteBuffer elf) throws IOException {
        long offset = elf.getLong(32), size = file.length();
        int count = elf.getShort(56) & 0xffff;
        if (count == 0 || elf.getShort(54) != 56 || offset < 64 || offset > size - 56L * count)
            throw new IOException("Invalid ELF program headers.");
        boolean code = false, dynamic = false;
        byte[] entry = new byte[56];
        ByteBuffer ph = ByteBuffer.wrap(entry).order(ByteOrder.LITTLE_ENDIAN);
        try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
            in.seek(offset);
            for (int i = 0; i < count; i++) {
                in.readFully(entry);
                long start = ph.getLong(8), bytes = ph.getLong(32);
                if (bytes < 0 || (bytes > 0 && (start < 0 || start > size - bytes)))
                    throw new IOException("Truncated driver file.");
                if (ph.getInt(0) == 1 && (ph.getInt(4) & 1) != 0 && bytes > 0) code = true;
                if (ph.getInt(0) == 2 && bytes >= 16) dynamic = true;
            }
        }
        if (!code || !dynamic) throw new IOException("Missing shared library segments.");
    }
}
