"""Exercise the per-game private driver store, including interruption before first present."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/java/org/nx2a/VulkanDriverStore.java'


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'JDK required')
class VulkanDriverImport(unittest.TestCase):
    def test_replacement_and_cold_start(self):
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
        assert VulkanDriverStore.pending(second).createNewFile();
        store.cleanAtLaunch(second);
        assert !first.exists() && !VulkanDriverStore.canLaunch(second);
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
