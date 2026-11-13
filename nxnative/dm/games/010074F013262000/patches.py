# Xenoblade Chronicles 3 2.2.0 and 2.2.1 share this main NSO byte for byte.
# The compositor's resolution table and layout canvas sites are documented by xc3-recomp
# (https://github.com/jmars125/xc3-recomp, runtime/resolution.c and recomp/patches-upd.txt).
# The game queries the operation mode at startup and after a mode notification. Wrap both
# calls so they update the compositor's width/height before the game handles the new mode.
# The stub occupies zero padding mapped with text. The size table is on a writable page.
import struct

from nso_asm import ADDI, ADRP, B, LDP_POST, STP_PRE

stub = 0x1295DA0
mode_query = 0x1295AD0
size_pair = 0x14DFD94

def call(site, target):
    return struct.pack('<I', B(site, target) | 0x80000000).hex()

code = (
    STP_PRE(29, 30, 31, -16),                   # save the caller's return address
    B(stub + 4, mode_query) | 0x80000000,       # bl GetOperationMode; keep the result in w0
    ADRP(8, stub + 8, size_pair),
    ADDI(8, 8, size_pair & 0xFFF),
    0x34000080,                               # cbz w0, handheld
    0x5280F009, 0x5280870A,                   # w9 = 1920, w10 = 1080
    B(stub + 28, stub + 40),
    0x5280A009, 0x52805A0A,                   # handheld: w9 = 1280, w10 = 720
    0x29002909,                               # stp w9, w10, [x8]
    LDP_POST(29, 30, 31, 16),
    0xD65F03C0,                               # ret
)

CUTS = {'82d187fe9ef9be921b8e1b1cddf7bd28': [
    (0x1253D2C, call(0x1253D2C, mode_query), call(0x1253D2C, stub), 'choose the frame size at startup'),
    (0x1283AD0, call(0x1283AD0, mode_query), call(0x1283AD0, stub), 'choose the frame size at operation mode change'),
    (stub, '00000000' * len(code), struct.pack(f'<{len(code)}I', *code).hex(), 'compose at 1920x1080 docked and 1280x720 handheld'),
    # The layout canvas stays in 1280x720 coordinates so world-space labels stay on their targets.
    (0x10377B4, '089d44f9020140bd', '08a080520201271e', 'preserve the layout canvas width'),
    (0x10377E4, '08a144f9020140bd', '085a80520201271e', 'preserve the layout canvas height'),
]}
