import pathlib
import shutil
import subprocess
import tempfile
import unittest
import csrc


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class TemporalInputTest(unittest.TestCase):
    def test_ue_padded_display_edges(self):
        source = csrc.read('nxnative/dm/android/gfx_vk_exec_gsr2_display.inc')
        code = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
typedef int VkCommandBuffer;
typedef struct { unsigned w,h; int img; } vkimg_t;
typedef struct { int aspect,mip,layer,layers; } VkImageSubresourceLayers;
typedef struct { int x,y,z; } Offset;
typedef struct { VkImageSubresourceLayers srcSubresource; Offset srcOffsets[2];
                 VkImageSubresourceLayers dstSubresource; Offset dstOffsets[2]; } VkImageBlit;
enum { VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, VK_FILTER_NEAREST };
static void vk_img_barrier_all(int cb) { (void)cb; }
static int copied;
static void vkCmdBlitImage(int cb, int s, int l, int d, int dl, unsigned n, VkImageBlit *r, int f) {
    (void)cb; (void)l; (void)dl;
    assert(s == d && f == VK_FILTER_NEAREST);
    for (unsigned i = 0; i < n; i++) {
        Offset *src = r[i].srcOffsets, *dst = r[i].dstOffsets;
        assert(src[1].x == 2560 && src[1].y == 1440);
        assert(dst[1].x > dst[0].x && dst[1].y > dst[0].y);
        int right = dst[0].x == 2560, bottom = dst[0].y == 1440;
        assert(right || bottom);
        assert(src[0].x == (right ? 2559 : 0) && src[0].y == (bottom ? 1439 : 0));
        assert(dst[1].x == (right ? 2564 : 2560) && dst[1].y == (bottom ? 1444 : 1440));
        int bit = 1 << (right + 2 * bottom);
        assert(!(copied & bit));
        copied |= bit;
    }
}
'''
        for func in ('static int gs_ue_axis(', 'static uint32_t gs_scaled_axis(', 'static void gs_extend_edges('):
            code += csrc.function(source, func)
        code += r'''
int main(void) {
    assert(gs_ue_axis(810,812) && gs_ue_axis(810,816) && gs_ue_axis(810,810));
    assert(!gs_ue_axis(810.5f,812) && !gs_ue_axis(810,813) && !gs_ue_axis(812,810));
    unsigned height = gs_scaled_axis(1440,810,812);
    assert(height == 1444);
    vkimg_t image = {2560,height,1};
    gs_extend_edges(0,&image,2560,1440); assert(copied == 4);
    copied = 0; image.w = 2564;
    gs_extend_edges(0,&image,2560,1440); assert(copied == (2|4|8));
    copied = 0; image.h = 1440;
    gs_extend_edges(0,&image,2560,1440); assert(copied == 2);
    copied = 0; image.w = 2560;
    gs_extend_edges(0,&image,2560,1440); assert(!copied);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / 'edges'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-lm', '-o', str(exe)],
                           input=code, text=True, check=True)
            subprocess.run([str(exe)], check=True)

    def test_octopath_view_extent_and_padding(self):
        source = csrc.read('nxnative/dm/android/gfx_vk_gsr2_ot2.inc')
        code = '#include <assert.h>\n#include <math.h>\n#include <stdint.h>\n'
        code += csrc.function(source, 'static int ot2_active_extent(')
        code += r'''
int main(void) {
    // Native docked view and two smaller viewports inside aligned allocations.
    const uint32_t cases[][4] = {{1279,720,1280,720}, {960,540,960,544}, {639,360,640,360}};
    for (unsigned n = 0; n < 3; n++) {
        const uint32_t *p = cases[n];
        float view[16] = {0};
        uint32_t render[2];
        for (unsigned i = 0; i < 2; i++) {
            view[i] = p[i];
            view[8+i] = p[2+i];
            view[12+i] = 0.5f / p[2+i];
            view[14+i] = (p[i]-0.5f) / p[2+i];
        }
        assert(ot2_active_extent(view,p[2],p[3],render));
        assert(render[0]==p[0] && render[1]==p[1]);
        assert(!ot2_active_extent(view,p[2]+1,p[3],render));
        view[12] += 1.0f/p[2];
        assert(!ot2_active_extent(view,p[2],p[3],render));
        view[12] = 0.5f/p[2];
        view[15] -= 1.0f/p[3];
        assert(!ot2_active_extent(view,p[2],p[3],render));
    }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / 'view'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-lm', '-o', str(exe)],
                           input=code, text=True, check=True)
            subprocess.run([str(exe)], check=True)

    def test_title_adapter_and_replay_bindings(self):
        android = pathlib.Path(__file__).resolve().parents[1] / 'nxnative/dm/android'
        with tempfile.TemporaryDirectory() as tmp:
            work = pathlib.Path(tmp)
            # Compile the replay code against small native type doubles without an Android SDK.
            shutil.copy(android / 'gfx_vk_temporal.h', work)
            shutil.copy(android / 'gfx_vk_gsr2_profile.h', work)
            (work / 'gfx_vk.h').write_text("""
#include <stdint.h>
typedef struct { uint64_t serial; struct { unsigned width, height; } d; } tex_t;
typedef struct { unsigned layer; } view_t;
""")
            source = pathlib.Path(__file__).with_name('vk_temporal_harness.c')
            exe = work / 'test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(work),
                            '-I', str(android), str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, capture_output=True)
