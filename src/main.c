// PS2 XP-style desktop - step 1: desktop + taskbar + mouse cursor
#include <stdio.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <libmouse.h>

extern unsigned char usbd_irx[];     extern unsigned int size_usbd_irx;
extern unsigned char ps2mouse_irx[]; extern unsigned int size_ps2mouse_irx;
extern unsigned char ps2kbd_irx[];   extern unsigned int size_ps2kbd_irx;

#define RGB(r,g,b) GS_SETREG_RGBAQ(r,g,b,0x80,0)

static GSGLOBAL *gs;

static void load_modules(void)
{
    int ret;
    SifInitRpc(0);
    SifIopReset("", 0);
    while (!SifIopSync()) {}
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    SifExecModuleBuffer(usbd_irx, size_usbd_irx, 0, NULL, &ret);
    SifExecModuleBuffer(ps2mouse_irx, size_ps2mouse_irx, 0, NULL, &ret);
    SifExecModuleBuffer(ps2kbd_irx, size_ps2kbd_irx, 0, NULL, &ret);
}

static void init_gs(void)
{
    gs = gsKit_init_global();           // auto NTSC/PAL
    gs->PSM = GS_PSM_CT16;
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
}

static void rect(int x1, int y1, int x2, int y2, u64 c)
{
    gsKit_prim_sprite(gs, x1, y1, x2, y2, 1, c);
}

// vertical gradient rectangle
static void vgrad(int x1, int y1, int x2, int y2, int r1, int g1, int b1, int r2, int g2, int b2)
{
    int h = y2 - y1, i;
    for (i = 0; i < h; i++) {
        int r = r1 + (r2 - r1) * i / h, g = g1 + (g2 - g1) * i / h, b = b1 + (b2 - b1) * i / h;
        rect(x1, y1 + i, x2, y1 + i + 1, RGB(r, g, b));
    }
}

static void draw_desktop(void)
{
    int W = gs->Width, H = gs->Height, tb = 30;
    // placeholder fallback wallpaper (will be replaced by JPEG picker)
    vgrad(0, 0, W, H - tb, 60, 110, 200, 160, 200, 250);
    // taskbar (XP blue) + start button (green)
    vgrad(0, H - tb, W, H, 36, 94, 220, 20, 60, 170);
    vgrad(0, H - tb, 90, H, 70, 170, 70, 40, 120, 40);
}

static void draw_cursor(int x, int y)
{
    int i;
    for (i = 0; i < 12; i++) rect(x, y + i, x + 1 + i / 2 + 2, y + i + 1, RGB(0, 0, 0));
    for (i = 1; i < 11; i++) rect(x + 1, y + i, x + 1 + i / 2 + 1, y + i + 1, RGB(255, 255, 255));
}

int main(void)
{
  PS2MouseData md;
    int mx = 320, my = 224;

    load_modules();
    init_gs();
    PS2MouseInit();
    PS2MouseSetReadMode(PS2MOUSE_READMODE_DIFF);

    while (1) {
        if (PS2MouseRead(&md) >= 0) {
            mx += md.x; my += md.y;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > gs->Width - 1) mx = gs->Width - 1;
            if (my > gs->Height - 1) my = gs->Height - 1;
        }
        draw_desktop();
        draw_cursor(mx, my);
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
    return 0;
}
