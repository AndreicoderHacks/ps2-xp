// PS2 XP-style desktop - step 2: window system, icons, taskbar, drag
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <malloc.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <libmouse.h>
#include <dirent.h>
#include <sys/stat.h>
#include "font.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

extern unsigned char usbd_irx[];     extern unsigned int size_usbd_irx;
extern unsigned char ps2mouse_irx[]; extern unsigned int size_ps2mouse_irx;
extern unsigned char ps2kbd_irx[];   extern unsigned int size_ps2kbd_irx;
extern unsigned char iomanX_irx[];   extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[];  extern unsigned int size_fileXio_irx;
extern unsigned char bdm_irx[];      extern unsigned int size_bdm_irx;
extern unsigned char bdmfs_fatfs_irx[]; extern unsigned int size_bdmfs_fatfs_irx;
extern unsigned char usbmass_bd_irx[];  extern unsigned int size_usbmass_bd_irx;

#define RGB(r,g,b) GS_SETREG_RGBAQ(r,g,b,0x80,0)
#define TASKBAR_H 30
#define TITLE_H 22
#define NAPPS 4
#define MAXW 8

static GSGLOBAL *gs;

static const char *app_names[NAPPS] = {"My Computer", "Minesweeper", "Solitaire", "Media Player"};

typedef struct { int x, y, w, h, app, open; } Win;
static Win wins[MAXW];
static int order[MAXW];   // slot ids, bottom -> top
static int nopen = 0;

/* ---------- init ---------- */
static void load_modules(void)
{
    int ret;
    SifInitRpc(0);
    SifIopReset("", 0);
    while (!SifIopSync()) {}
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    SifExecModuleBuffer(iomanX_irx, size_iomanX_irx, 0, NULL, &ret);
    SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, &ret);
    SifExecModuleBuffer(usbd_irx, size_usbd_irx, 0, NULL, &ret);
    SifExecModuleBuffer(bdm_irx, size_bdm_irx, 0, NULL, &ret);
    SifExecModuleBuffer(bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL, &ret);
    SifExecModuleBuffer(usbmass_bd_irx, size_usbmass_bd_irx, 0, NULL, &ret);
    SifExecModuleBuffer(ps2mouse_irx, size_ps2mouse_irx, 0, NULL, &ret);
    SifExecModuleBuffer(ps2kbd_irx, size_ps2kbd_irx, 0, NULL, &ret);
}

static void init_gs(void)
{
    gs = gsKit_init_global_custom(1024 * 512, 1024 * 16);   // bigger draw queue for many sprites
    gs->PSM = GS_PSM_CT16;
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
}

/* ---------- drawing primitives ---------- */
static void rect(int x1, int y1, int x2, int y2, u64 c)
{
    gsKit_prim_sprite(gs, x1, y1, x2, y2, 1, c);
}

static void vgrad(int x1, int y1, int x2, int y2, int r1, int g1, int b1, int r2, int g2, int b2)
{
    int h = y2 - y1, i;
    for (i = 0; i < h; i++) {
        int r = r1 + (r2 - r1) * i / h, g = g1 + (g2 - g1) * i / h, b = b1 + (b2 - b1) * i / h;
        rect(x1, y1 + i, x2, y1 + i + 1, RGB(r, g, b));
    }
}

static void draw_char(int x, int y, char c, int sc, u64 col)
{
    const unsigned char *g;
    int cx, cy, s;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 32 || c > 95) c = '?';
    g = &font5x7[(c - 32) * 5];
    for (cx = 0; cx < 5; cx++) {
        cy = 0;
        while (cy < 7) {
            if (g[cx] & (1 << cy)) {
                s = cy;
                while (cy < 7 && (g[cx] & (1 << cy))) cy++;
                rect(x + cx * sc, y + s * sc, x + cx * sc + sc, y + cy * sc, col);
            } else cy++;
        }
    }
}

static void draw_text(int x, int y, const char *s, int maxc, int sc, u64 col)
{
    int n = 0;
    while (*s && n < maxc) { draw_char(x, y, *s, sc, col); x += 6 * sc; s++; n++; }
}

/* ---------- status line + wallpaper (JPEG) ---------- */
#define WP_W 512
#define WP_H 512
#define WP_CFG "mass:/ps2xp.cfg"

static GSTEXTURE wp_tex;
static int wp_ready = 0, wp_alloc = 0;
static char status[64];
static int status_until = 0, g_frame = 0;

static void set_status(const char *m)
{
    strncpy(status, m, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
    status_until = g_frame + 240;
}

static void wp_save_cfg(const char *path)
{
    FILE *f = fopen(WP_CFG, "w");
    if (!f) return;
    fputs(path, f);
    fclose(f);
}

static int wp_load(const char *path, int quiet)
{
    FILE *f;
    long sz;
    unsigned char *buf, *img;
    int w, h, c, x, y;
    unsigned short *dst;

    f = fopen(path, "rb");
    if (!f) { if (!quiet) set_status("CANT OPEN FILE"); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 6 * 1024 * 1024) { fclose(f); if (!quiet) set_status("FILE TOO BIG (MAX 6 MB)"); return -1; }
    buf = malloc(sz);
    if (!buf) { fclose(f); if (!quiet) set_status("OUT OF MEMORY"); return -1; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); if (!quiet) set_status("READ ERROR"); return -1; }
    fclose(f);

    if (!stbi_info_from_memory(buf, (int)sz, &w, &h, &c) || (long)w * h > 4200000) {
        free(buf);
        if (!quiet) set_status("NOT A JPEG OR TOO BIG (MAX 4 MP)");
        return -1;
    }
    img = stbi_load_from_memory(buf, (int)sz, &w, &h, &c, 3);
    free(buf);
    if (!img) { if (!quiet) set_status("JPEG DECODE FAILED"); return -1; }

    if (!wp_tex.Mem) wp_tex.Mem = memalign(128, WP_W * WP_H * 2);
    if (!wp_tex.Mem) { stbi_image_free(img); if (!quiet) set_status("OUT OF MEMORY"); return -1; }
    dst = (unsigned short *)wp_tex.Mem;
    for (y = 0; y < WP_H; y++) {
        int sy = y * h / WP_H;
        for (x = 0; x < WP_W; x++) {
            int sx = x * w / WP_W;
            unsigned char *p = img + (sy * w + sx) * 3;
            dst[y * WP_W + x] = (p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | 0x8000;
        }
    }
    stbi_image_free(img);

    wp_tex.Width = WP_W;
    wp_tex.Height = WP_H;
    wp_tex.PSM = GS_PSM_CT16;
    wp_tex.Filter = GS_FILTER_LINEAR;
    if (!wp_alloc) {
        wp_tex.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(WP_W, WP_H, GS_PSM_CT16), GSKIT_ALLOC_USERBUFFER);
        wp_alloc = 1;
    }
    gsKit_texture_upload(gs, &wp_tex);
    wp_ready = 1;
    wp_save_cfg(path);
    if (!quiet) set_status("WALLPAPER SET");
    return 0;
}

static void wp_try_cfg(void)
{
    char path[300];
    FILE *f;
    int n;
    if (wp_ready) return;
    f = fopen(WP_CFG, "r");
    if (!f) return;
    n = fread(path, 1, sizeof(path) - 1, f);
    fclose(f);
    if (n <= 0) return;
    path[n] = 0;
    while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r')) path[--n] = 0;
    wp_load(path, 1);
}

static void wp_clear(void)
{
    wp_ready = 0;
    remove(WP_CFG);
    set_status("WALLPAPER RESET");
}

static int is_jpg(const char *n)
{
    int l = strlen(n);
    return (l > 4 && !strcasecmp(n + l - 4, ".jpg")) || (l > 5 && !strcasecmp(n + l - 5, ".jpeg"));
}

/* ---------- file manager (My Computer) ---------- */
#define FM_MAX 256
typedef struct { char name[40]; int dir; unsigned int size; } FMEntry;
static struct {
    char path[256];
    FMEntry e[FM_MAX];
    int count, scroll, sel, demo;
    char msg[48];
    int last_row, last_frame;
} fm;

static int fm_cmp(const void *a, const void *b)
{
    const FMEntry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static void fm_add(const char *name, int dir, unsigned int size)
{
    if (fm.count >= FM_MAX) return;
    strncpy(fm.e[fm.count].name, name, 39);
    fm.e[fm.count].name[39] = 0;
    fm.e[fm.count].dir = dir;
    fm.e[fm.count].size = size;
    fm.count++;
}

static int fm_is_root(void) { return strcmp(fm.path, "mass:/") == 0; }

static int fm_load(const char *path)
{
    DIR *dir;
    struct dirent *de;
    int start;
    dir = opendir(path);
    if (!dir) return -1;
    fm.count = 0; fm.scroll = 0; fm.sel = -1;
    if (strcmp(path, "mass:/") != 0) fm_add("..", 1, 0);
    start = fm.count;
    while ((de = readdir(dir)) != NULL) {
        struct stat st;
        char fullpath[512];
        int is_dir = 0;
        unsigned int size = 0;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (snprintf(fullpath, sizeof(fullpath), "%s%s%s", path,
                     (path[0] && path[strlen(path) - 1] != '/') ? "/" : "",
                     de->d_name) < (int)sizeof(fullpath) && stat(fullpath, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            if (st.st_size > 0) size = (unsigned int)st.st_size;
        }
        fm_add(de->d_name, is_dir, size);
    }
    closedir(dir);
    qsort(fm.e + start, fm.count - start, sizeof(FMEntry), fm_cmp);
    return 0;
}

static void fm_demo(void)
{
    fm.count = 0; fm.scroll = 0; fm.sel = -1; fm.demo = 1;
    fm_add("PHOTOS", 1, 0); fm_add("MUSIC", 1, 0); fm_add("GAMES", 1, 0);
    fm_add("WALLPAPER.JPG", 0, 245000); fm_add("SONG.MP3", 0, 4100000); fm_add("README.TXT", 0, 1200);
}

static void fm_refresh(void)
{
    int r;
    if (!fm.path[0]) strcpy(fm.path, "mass:/");
    r = fm_load(fm.path);
    if (r < 0) {
        snprintf(fm.msg, sizeof(fm.msg), "NO USB (ERR %d) - DEMO LIST", r);
        fm_demo();
    } else { fm.demo = 0; fm.msg[0] = 0; }
}

static void fm_up(void)
{
    char *p;
    int n;
    if (fm.demo || fm_is_root()) return;
    n = strlen(fm.path);
    if (n > 0 && fm.path[n - 1] == '/') fm.path[n - 1] = 0;
    p = strrchr(fm.path, '/');
    if (p) p[1] = 0;
    if (fm_load(fm.path) < 0) fm_refresh();
}

static void fm_enter(int idx)
{
    char old[256];
    if (idx < 0 || idx >= fm.count) return;
    if (!fm.e[idx].dir) {
        if (is_jpg(fm.e[idx].name)) {
            if (fm.demo) set_status("NO USB - CANT SET WALLPAPER");
            else {
                char full[300];
                snprintf(full, sizeof(full), "%s%s", fm.path, fm.e[idx].name);
                wp_load(full, 0);
            }
        }
        return;
    }
    if (!strcmp(fm.e[idx].name, "..")) { fm_up(); return; }
    if (fm.demo) return;
    strcpy(old, fm.path);
    strncat(fm.path, fm.e[idx].name, sizeof(fm.path) - strlen(fm.path) - 2);
    strcat(fm.path, "/");
    if (fm_load(fm.path) < 0) { strcpy(fm.path, old); fm_load(fm.path); }
}

static const int fm_bx[5] = {6, 48, 104, 136, 168};
static const int fm_bw[5] = {36, 50, 28, 28, 52};
static const char *fm_bl[5] = {"UP", "REFR", "^", "V", "NOWP"};

static int fm_vis(Win *w) { return (w->h - TITLE_H - 38 - 6) / 14; }

static void draw_fm(Win *w)
{
    int i, r, vis = fm_vis(w);
    int bx = w->x + 3, by = w->y + TITLE_H;
    char buf[32];
    for (i = 0; i < 5; i++) {
        rect(bx + fm_bx[i], by + 4, bx + fm_bx[i] + fm_bw[i], by + 20, RGB(180, 180, 190));
        rect(bx + fm_bx[i] + 1, by + 5, bx + fm_bx[i] + fm_bw[i] - 1, by + 19, RGB(225, 225, 232));
        draw_text(bx + fm_bx[i] + 6, by + 9, fm_bl[i], 5, 1, RGB(30, 30, 30));
    }
    if (fm.demo) draw_text(bx + 6, by + 24, fm.msg, 52, 1, RGB(200, 30, 30));
    else         draw_text(bx + 6, by + 24, fm.path, 52, 1, RGB(60, 60, 60));
    rect(bx + 3, by + 36, w->x + w->w - 6, w->y + w->h - 6, RGB(255, 255, 255));
    for (r = 0; r < vis; r++) {
        int idx = fm.scroll + r, ry = by + 40 + r * 14;
        if (idx >= fm.count) break;
        if (idx == fm.sel) rect(bx + 4, ry - 2, w->x + w->w - 7, ry + 11, RGB(50, 90, 200));
        u64 tc = (idx == fm.sel) ? RGB(255, 255, 255) : RGB(20, 20, 20);
        rect(bx + 8, ry, bx + 18, ry + 9, fm.e[idx].dir ? RGB(240, 200, 60) : RGB(150, 160, 190));
        draw_text(bx + 24, ry + 1, fm.e[idx].name, 24, 1, tc);
        if (!fm.e[idx].dir) {
            snprintf(buf, sizeof(buf), "%u KB", (fm.e[idx].size + 1023) / 1024);
            draw_text(w->x + w->w - 80, ry + 1, buf, 12, 1, tc);
        }
    }
}

static void fm_click(Win *w, int mx, int my, int frame)
{
    int i, bx = w->x + 3, by = w->y + TITLE_H, vis = fm_vis(w);
    for (i = 0; i < 5; i++) {
        if (mx >= bx + fm_bx[i] && mx < bx + fm_bx[i] + fm_bw[i] && my >= by + 4 && my < by + 20) {
            if (i == 0) fm_up();
            else if (i == 1) fm_refresh();
            else if (i == 2) { if (fm.scroll > 0) fm.scroll -= 3; if (fm.scroll < 0) fm.scroll = 0; }
            else if (i == 4) wp_clear();
            else { if (fm.scroll + vis < fm.count) fm.scroll += 3; }
            return;
        }
    }
    if (my >= by + 38 && mx >= bx + 3 && mx < w->x + w->w - 6) {
        int row = (my - (by + 38)) / 14, idx = fm.scroll + row;
        if (row < vis && idx < fm.count) {
            if (idx == fm.last_row && frame - fm.last_frame < 30) { fm_enter(idx); fm.last_row = -1; }
            else { fm.sel = idx; fm.last_row = idx; fm.last_frame = frame; }
        }
    }
}

/* ---------- window manager ---------- */
static int top_slot(void) { return nopen ? order[nopen - 1] : -1; }

static void focus(int slot)
{
    int i, p = -1;
    for (i = 0; i < nopen; i++) if (order[i] == slot) p = i;
    if (p < 0) return;
    for (i = p; i < nopen - 1; i++) order[i] = order[i + 1];
    order[nopen - 1] = slot;
}

static void close_win(int slot)
{
    int i, p = -1;
    for (i = 0; i < nopen; i++) if (order[i] == slot) p = i;
    if (p < 0) return;
    for (i = p; i < nopen - 1; i++) order[i] = order[i + 1];
    nopen--;
    wins[slot].open = 0;
}

static void open_app(int app)
{
    int i, slot = -1;
    for (i = 0; i < MAXW; i++) if (wins[i].open && wins[i].app == app) { focus(i); return; }
    for (i = 0; i < MAXW; i++) if (!wins[i].open) { slot = i; break; }
    if (slot < 0) return;
    wins[slot].open = 1;
    wins[slot].app = app;
    wins[slot].w = (app == 0) ? 380 : 300;
    wins[slot].h = (app == 0) ? 290 : 200;
    wins[slot].x = 110 + (nopen % 6) * 28;
    wins[slot].y = 20 + (nopen % 6) * 28;
    order[nopen++] = slot;
    if (app == 0) fm_refresh();
}

static void draw_window(int slot, int active)
{
    Win *w = &wins[slot];
    int x = w->x, y = w->y;
    // frame
    rect(x, y, x + w->w, y + w->h, active ? RGB(0, 60, 200) : RGB(100, 120, 190));
    // title bar
    if (active) vgrad(x, y, x + w->w, y + TITLE_H, 0, 84, 227, 61, 149, 255);
    else        vgrad(x, y, x + w->w, y + TITLE_H, 122, 150, 220, 160, 185, 235);
    draw_text(x + 8, y + 7, app_names[w->app], 28, 1, RGB(255, 255, 255));
    // close button
    rect(x + w->w - 20, y + 3, x + w->w - 4, y + 19, RGB(200, 60, 40));
    draw_text(x + w->w - 16, y + 7, "X", 1, 1, RGB(255, 255, 255));
    // body
    rect(x + 3, y + TITLE_H, x + w->w - 3, y + w->h - 3, RGB(236, 233, 216));
    if (w->app == 0) draw_fm(w);
    else draw_text(x + 20, y + 80, "COMING SOON", 20, 2, RGB(90, 90, 90));
}

/* ---------- desktop ---------- */
static int icon_x(void) { return 24; }
static int icon_y(int i) { return 20 + i * 84; }

static void draw_icon(int i, int selected)
{
    int x = icon_x(), y = icon_y(i);
    if (selected) rect(x - 8, y - 4, x + 56, y + 76, RGB(50, 90, 200));
    // simple colored placeholder icon per app
    rect(x, y, x + 48, y + 48, RGB(255, 255, 255));
    rect(x + 3, y + 3, x + 45, y + 45, i == 0 ? RGB(240, 200, 60) : i == 1 ? RGB(120, 120, 120) : i == 2 ? RGB(40, 150, 60) : RGB(230, 120, 30));
    draw_text(x - 4, y + 54, app_names[i], 9, 1, RGB(255, 255, 255));
}

static void draw_taskbar(int top)
{
    int W = gs->Width, H = gs->Height, i, n = 0;
    vgrad(0, H - TASKBAR_H, W, H, 36, 94, 220, 20, 60, 170);
    vgrad(0, H - TASKBAR_H, 90, H, 70, 170, 70, 40, 120, 40);
    draw_text(14, H - 22, "START", 5, 2, RGB(255, 255, 255));
    for (i = 0; i < MAXW; i++) {
        if (!wins[i].open) continue;
        int bx = 100 + n * 114;
        rect(bx, H - 27, bx + 110, H - 3, i == top ? RGB(10, 40, 130) : RGB(60, 110, 220));
        draw_text(bx + 6, H - 19, app_names[wins[i].app], 16, 1, RGB(255, 255, 255));
        n++;
    }
}

/* returns slot of taskbar button at x, or -1 */
static int taskbar_hit(int mx)
{
    int i, n = 0;
    for (i = 0; i < MAXW; i++) {
        if (!wins[i].open) continue;
        int bx = 100 + n * 114;
        if (mx >= bx && mx < bx + 110) return i;
        n++;
    }
    return -1;
}

static void draw_cursor(int x, int y)
{
    int i;
    for (i = 0; i < 12; i++) rect(x, y + i, x + 1 + i / 2 + 2, y + i + 1, RGB(0, 0, 0));
    for (i = 1; i < 11; i++) rect(x + 1, y + i, x + 1 + i / 2 + 1, y + i + 1, RGB(255, 255, 255));
}

static void draw_debug(u32 en, u32 buttons)
{
    int W = gs->Width, H = gs->Height, i;
    rect(W - 90, H - 24, W - 76, H - 6, en > 0 ? RGB(0, 255, 0) : RGB(255, 0, 0));
    for (i = 0; i < 3; i++)
        rect(W - 68 + i * 18, H - 24, W - 54 + i * 18, H - 6, (buttons & (1 << i)) ? RGB(255, 255, 0) : RGB(30, 30, 60));
}

/* ---------- main ---------- */
int main(void)
{
    PS2MouseData md;
    int mx = 320, my = 224, frame = 0, configured = 0, i;
    u32 en = 0, buttons = 0, prev = 0;
    int drag = -1, dox = 0, doy = 0;
    int sel_icon = -1, last_icon = -1, last_click = -1000;

    load_modules();
    init_gs();
    PS2MouseInit();

    while (1) {
        int W = gs->Width, H = gs->Height;

        if (!configured && (frame % 30) == 0) {
            en = PS2MouseEnum();
            if (en > 0) {
                PS2MouseSetReadMode(PS2MOUSE_READMODE_ABS);
                PS2MouseSetBoundary(0, W - 1, 0, H - 1);
                PS2MouseSetPosition(W / 2, H / 2);
                configured = 1;
            }
        }
        if (configured && PS2MouseRead(&md) >= 0) {
            mx = md.x; my = md.y; buttons = md.buttons;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > W - 1) mx = W - 1;
            if (my > H - 1) my = H - 1;
        }

        int pressed = (buttons & 1) && !(prev & 1);
        int released = !(buttons & 1) && (prev & 1);

        if (pressed) {
            int handled = 0;
            if (my >= H - TASKBAR_H) {
                int s = taskbar_hit(mx);
                if (s >= 0) focus(s);
                handled = 1;
            }
            for (i = nopen - 1; i >= 0 && !handled; i--) {
                int s = order[i];
                Win *w = &wins[s];
                if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
                    focus(s);
                    if (my < w->y + TITLE_H) {
                        if (mx >= w->x + w->w - 20 && mx < w->x + w->w - 4 && my >= w->y + 3 && my < w->y + 19)
                            close_win(s);
                        else { drag = s; dox = mx - w->x; doy = my - w->y; }
                    } else if (w->app == 0) fm_click(w, mx, my, frame);
                    handled = 1;
                }
            }
            if (!handled) {
                int hit = -1;
                for (i = 0; i < NAPPS; i++)
                    if (mx >= icon_x() - 8 && mx < icon_x() + 56 && my >= icon_y(i) - 4 && my < icon_y(i) + 76) hit = i;
                if (hit >= 0 && hit == last_icon && frame - last_click < 30) { open_app(hit); last_icon = -1; }
                else { last_icon = hit; last_click = frame; }
                sel_icon = hit;
            }
        }
        if (drag >= 0 && (buttons & 1)) {
            Win *w = &wins[drag];
            w->x = mx - dox; w->y = my - doy;
            if (w->y < 0) w->y = 0;
            if (w->y > H - TASKBAR_H - TITLE_H) w->y = H - TASKBAR_H - TITLE_H;
            if (w->x < -w->w + 40) w->x = -w->w + 40;
            if (w->x > W - 40) w->x = W - 40;
        }
        if (released) drag = -1;
        prev = buttons;

        // ----- draw -----
        g_frame = frame;
        if (frame == 300 || frame == 900) wp_try_cfg();
        if (wp_ready)
            gsKit_prim_sprite_texture(gs, &wp_tex, 0, 0, 0, 0, W, H - TASKBAR_H, WP_W, WP_H, 1, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
        else
            vgrad(0, 0, W, H - TASKBAR_H, 60, 110, 200, 160, 200, 250);
        for (i = 0; i < NAPPS; i++) draw_icon(i, i == sel_icon);
        for (i = 0; i < nopen; i++) draw_window(order[i], i == nopen - 1);
        draw_taskbar(top_slot());
        if (frame < status_until) {
            int tw = strlen(status) * 6 + 12;
            rect(W / 2 - tw / 2, 6, W / 2 + tw / 2, 24, RGB(20, 20, 40));
            draw_text(W / 2 - tw / 2 + 6, 12, status, 60, 1, RGB(255, 255, 255));
        }
        draw_debug(en, buttons);
        draw_cursor(mx, my);
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
        frame++;
    }
    return 0;
}
