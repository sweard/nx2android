"""Exercise the per-game private driver store, including interruption before first present."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/java/org/nx2a/VulkanDriverStore.java'


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'JDK required')
class VulkanDriverImport(unittest.TestCase):
    def test_multiple_versions_legacy_selection_and_cold_start(self):
        probe = r'''
package org.nx2a;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
class DriverProbe {
    public static void main(String[] args) throws Exception {
        File dir = new File(args[0]);
        VulkanDriverStore store = new VulkanDriverStore(dir);
        byte[] data = new byte[256];
        ByteBuffer e = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        e.put(new byte[]{0x7f,'E','L','F',2,1,1});
        e.putShort(16,(short)3).putShort(18,(short)183).putInt(20,1);
        e.putLong(32,64).putShort(52,(short)64).putShort(54,(short)56).putShort(56,(short)2);
        e.putInt(64,1).putInt(68,5).putLong(96,256);
        e.putInt(120,2).putLong(128,176).putLong(152,32);
        File first = store.copy(new ByteArrayInputStream(data));
        assert first.equals(store.imported(first.getName()));
        assert store.imported("../" + first.getName()) == null;
        assert VulkanDriverStore.canLaunch(first);
        byte[] wrongAbi = data.clone(); wrongAbi[18] = 62;
        for (byte[] bad : new byte[][]{wrongAbi, new byte[8], Arrays.copyOf(data,200)}) {
            try { store.copy(new ByteArrayInputStream(bad)); throw new AssertionError(); }
            catch (IOException expected) {}
            assert dir.list().length == 1 && Arrays.equals(data, Files.readAllBytes(first.toPath()));
        }
        File second = store.copy(new ByteArrayInputStream(data));
        assert !first.equals(second) && first.exists();
        assert VulkanDriverStore.suggestedVersion("Turnip-V40-Adreno840.so").equals("V40");
        assert VulkanDriverStore.suggestedVersion("turnip_v27.0.0-R3.so").equals("V27.0.0-R3");
        assert VulkanDriverStore.suggestedVersion("libvulkan_freedreno.so").isEmpty();
        Map<String, Object> prefs = new HashMap<>();
        prefs.put("file", second.getName());
        prefs.put("label", "Turnip-V40-Adreno840.so");
        assert store.selected(prefs).equals(second);
        assert store.drivers(prefs).get(second).equals("V40");
        prefs.put(first.getName(), "V39");
        prefs.put("selected", first.getName());
        assert store.selected(prefs).equals(first);
        assert store.drivers(prefs).size() == 2 && store.drivers(prefs).get(first).equals("V39");
        assert VulkanDriverStore.pending(second).createNewFile();
        File unpublished = store.copy(new ByteArrayInputStream(data));
        assert VulkanDriverStore.pending(unpublished).createNewFile();
        store.cleanAtLaunch(store.drivers(prefs).keySet());
        assert !unpublished.exists() && !VulkanDriverStore.pending(unpublished).exists();
        assert first.exists() && VulkanDriverStore.canLaunch(store.selected(prefs));
        prefs.put("selected", second.getName());
        assert second.exists() && !VulkanDriverStore.canLaunch(store.selected(prefs));
        assert store.drivers(prefs).size() == 2;

        assert VulkanDriverStore.pending(second).delete();
        assert VulkanDriverStore.canLaunch(second);
    }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / 'DriverProbe.java'
            src.write_text(probe, encoding='utf-8')
            store = root / 'drivers'
            store.mkdir()
            subprocess.run(['javac', '-d', tmp, str(SOURCE), str(src)], check=True)
            subprocess.run(['java', '-ea', '-cp', tmp, 'org.nx2a.DriverProbe', str(store)], check=True)
