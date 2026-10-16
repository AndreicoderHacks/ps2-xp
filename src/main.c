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
#include <libpad.h>
#include <audsrv.h>
#include <libpwroff.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
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

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

extern unsigned char usbd_irx[];     extern unsigned int size_usbd_irx;
extern unsigned char poweroff_irx[]; extern unsigned int size_poweroff_irx;
extern unsigned char libsd_irx[];    extern unsigned int size_libsd_irx;
extern unsigned char audsrv_irx[];   extern unsigned int size_audsrv_irx;
extern unsigned char ps2mouse_irx[]; extern unsigned int size_ps2mouse_irx;
extern unsigned char ps2kbd_irx[];   extern unsigned int size_ps2kbd_irx;
extern unsigned char sio2man_irx[];  extern unsigned int size_sio2man_irx;
extern unsigned char padman_irx[];   extern unsigned int size_padman_irx;
extern unsigned char iomanX_irx[];   extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[];  extern unsigned int size_fileXio_irx;
extern unsigned char bdm_irx[];      extern unsigned int size_bdm_irx;
extern unsigned char bdmfs_fatfs_irx[]; extern unsigned int size_bdmfs_fatfs_irx;
extern unsigned char usbmass_bd_irx[];  extern unsigned int size_usbmass_bd_irx;

#define RGB(r,g,b) GS_SETREG_RGBAQ(r,g,b,0x80,0)
#define TASKBAR_H 30
#define TITLE_H 22
#define NAPPS 6      // apps in Start menu (incl. Settings, Video Player)
#define NICONS 5     // desktop icons (4 apps + Recycle Bin)
#define MAXW 8

static GSGLOBAL *gs;

static const char *app_names[NAPPS] = {"My Computer", "Minesweeper", "Solitaire", "Media Player", "Settings", "Video Player"};

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
    SifExecModuleBuffer(sio2man_irx, size_sio2man_irx, 0, NULL, &ret);
    SifExecModuleBuffer(padman_irx, size_padman_irx, 0, NULL, &ret);
    SifExecModuleBuffer(iomanX_irx, size_iomanX_irx, 0, NULL, &ret);
    SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, &ret);
    SifExecModuleBuffer(usbd_irx, size_usbd_irx, 0, NULL, &ret);
    SifExecModuleBuffer(bdm_irx, size_bdm_irx, 0, NULL, &ret);
    SifExecModuleBuffer(bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL, &ret);
    SifExecModuleBuffer(usbmass_bd_irx, size_usbmass_bd_irx, 0, NULL, &ret);
    SifExecModuleBuffer(poweroff_irx, size_poweroff_irx, 0, NULL, &ret);
    SifExecModuleBuffer(libsd_irx, size_libsd_irx, 0, NULL, &ret);
    SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &ret);
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

static int is_mp3(const char *n)
{
    int l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".mp3");
}

static void mp_open_from_fm(int idx);
static void open_app(int app);

/* ---------- global settings + persistent ini ---------- */
static int g_vol = 80, stick_dz = 40, cursor_speed = 5;
static int audsrv_ok = 0, sleeping = 0;
static int icon_px[NICONS], icon_py[NICONS];
static int ini_loaded = 0;
#define INI_PATH "mass:/ps2xp.ini"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void ini_save(void)
{
    FILE *f = fopen(INI_PATH, "w");
    int i;
    if (!f) return;
    fprintf(f, "vol=%d\ndz=%d\nspeed=%d\n", g_vol, stick_dz, cursor_speed);
    for (i = 0; i < NICONS; i++) fprintf(f, "icon%d=%d,%d\n", i, icon_px[i], icon_py[i]);
    fclose(f);
}

static void ini_load(void)
{
    FILE *f = fopen(INI_PATH, "r");
    char line[64];
    int a, b, i;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "vol=%d", &a) == 1) g_vol = clampi(a, 0, 100);
        else if (sscanf(line, "dz=%d", &a) == 1) stick_dz = clampi(a, 10, 100);
        else if (sscanf(line, "speed=%d", &a) == 1) cursor_speed = clampi(a, 1, 10);
        else if (sscanf(line, "icon%d=%d,%d", &i, &a, &b) == 3 && i >= 0 && i < NICONS) {
            icon_px[i] = clampi(a, 0, 560); icon_py[i] = clampi(b, 0, 380);
        }
    }
    fclose(f);
    ini_loaded = 1;
    if (audsrv_ok) audsrv_set_volume(g_vol);
}

static int path_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* ---------- file manager (My Computer) ---------- */
#define FM_MAX 256
typedef struct { char name[100]; int dir; unsigned int size; } FMEntry;
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
    strncpy(fm.e[fm.count].name, name, 99);
    fm.e[fm.count].name[99] = 0;
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
        } else if (is_mp3(fm.e[idx].name)) {
            if (fm.demo) set_status("NO USB - PRESS PLAY IN PLAYER FOR TEST TONE");
            else mp_open_from_fm(idx);
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

#define FM_NB 8
static const int fm_bx[FM_NB] = {6, 48, 104, 136, 168, 226, 284, 324};
static const int fm_bw[FM_NB] = {36, 50, 28, 28, 52, 52, 36, 44};
static const char *fm_bl[FM_NB] = {"UP", "REFR", "^", "V", "NOWP", "MKDIR", "DEL", "REST"};
static int sb_drag = 0, sb_slot = 0;
static int del_idx = -1, del_frame = -1000;

static int fm_vis(Win *w) { return (w->h - TITLE_H - 38 - 6) / 14; }

static int fm_in_bin(void) { return strncasecmp(fm.path, "mass:/RECYCLE/", 14) == 0; }

static void fm_scroll_by(Win *w, int d)
{
    int mx_s = fm.count - fm_vis(w);
    fm.scroll += d;
    if (fm.scroll > mx_s) fm.scroll = mx_s;
    if (fm.scroll < 0) fm.scroll = 0;
}

static void fm_sb_set(Win *w, int my)
{
    int vis = fm_vis(w), total = fm.count;
    int top = w->y + TITLE_H + 38, trackh = (w->y + w->h - 6) - top;
    int thumbh, range, pos;
    if (total <= vis) { fm.scroll = 0; return; }
    thumbh = trackh * vis / total; if (thumbh < 16) thumbh = 16;
    range = trackh - thumbh;
    if (range < 1) return;
    pos = my - top - thumbh / 2;
    fm.scroll = clampi(pos * (total - vis) / range, 0, total - vis);
}

static int move_to(const char *src, const char *name, const char *dstdir)
{
    char d[400];
    int i;
    snprintf(d, sizeof(d), "%s%s", dstdir, name);
    for (i = 2; path_exists(d) && i < 100; i++) snprintf(d, sizeof(d), "%s%d_%s", dstdir, i, name);
    return rename(src, d);
}

static void fm_mkdir(void)
{
    char p[300];
    int i;
    if (fm.demo) { set_status("NO USB - CANT CREATE FOLDER"); return; }
    for (i = 1; i < 100; i++) {
        if (i == 1) snprintf(p, sizeof(p), "%sNEW FOLDER", fm.path);
        else snprintf(p, sizeof(p), "%sNEW FOLDER %d", fm.path, i);
        if (!path_exists(p)) break;
    }
    if (mkdir(p, 0777) == 0) {
        const char *nm = strrchr(p, '/') + 1;
        set_status("FOLDER CREATED");
        fm_load(fm.path);
        for (i = 0; i < fm.count; i++) if (!strcmp(fm.e[i].name, nm)) { fm.sel = i; if (i >= fm.scroll + 12) fm.scroll = i - 6; }
    } else set_status("CANT CREATE FOLDER");
}

static void fm_del(void)
{
    char p[400];
    int idx = fm.sel, r;
    if (fm.demo) { set_status("NO USB"); return; }
    if (idx < 0 || idx >= fm.count || !strcmp(fm.e[idx].name, "..")) { set_status("SELECT A FILE OR FOLDER FIRST"); return; }
    snprintf(p, sizeof(p), "%s%s", fm.path, fm.e[idx].name);
    if (fm_in_bin()) {
        if (!(del_idx == idx && g_frame - del_frame < 250)) {
            del_idx = idx; del_frame = g_frame;
            set_status("PRESS DEL AGAIN TO DELETE FOREVER");
            return;
        }
        r = fm.e[idx].dir ? rmdir(p) : remove(p);
        set_status(r == 0 ? "DELETED PERMANENTLY" : "CANT DELETE (FOLDER NOT EMPTY?)");
    } else {
        mkdir("mass:/RECYCLE", 0777);
        if (move_to(p, fm.e[idx].name, "mass:/RECYCLE/") == 0) set_status("MOVED TO RECYCLE BIN");
        else set_status("CANT MOVE TO RECYCLE BIN");
    }
    fm_load(fm.path);
}

static void fm_restore(void)
{
    char p[400];
    int idx = fm.sel;
    if (fm.demo) { set_status("NO USB"); return; }
    if (!fm_in_bin()) { set_status("OPEN THE RECYCLE BIN TO RESTORE"); return; }
    if (idx < 0 || idx >= fm.count || !strcmp(fm.e[idx].name, "..")) { set_status("SELECT AN ITEM FIRST"); return; }
    snprintf(p, sizeof(p), "%s%s", fm.path, fm.e[idx].name);
    if (move_to(p, fm.e[idx].name, "mass:/") == 0) set_status("RESTORED TO MAIN FOLDER");
    else set_status("CANT RESTORE");
    fm_load(fm.path);
}

static void open_recycle(void)
{
    mkdir("mass:/RECYCLE", 0777);
    strcpy(fm.path, "mass:/RECYCLE/");
    open_app(0);
    fm_refresh();
}

static void draw_fm(Win *w)
{
    int i, r, vis = fm_vis(w);
    int bx = w->x + 3, by = w->y + TITLE_H;
    char buf[32];
    for (i = 0; i < FM_NB; i++) {
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
        draw_text(bx + 24, ry + 1, fm.e[idx].name, 40, 1, tc);
        if (!fm.e[idx].dir) {
            snprintf(buf, sizeof(buf), "%u KB", (fm.e[idx].size + 1023) / 1024);
            draw_text(w->x + w->w - 100, ry + 1, buf, 12, 1, tc);
        }
    }
    // scrollbar
    {
        int top = by + 38, bot = w->y + w->h - 6, sx = w->x + w->w - 20;
        rect(sx, top, sx + 12, bot, RGB(225, 225, 232));
        if (fm.count > vis) {
            int trackh = bot - top, thumbh = trackh * vis / fm.count, ty;
            if (thumbh < 16) thumbh = 16;
            ty = top + (trackh - thumbh) * fm.scroll / (fm.count - vis);
            rect(sx, ty, sx + 12, ty + thumbh, RGB(80, 110, 200));
            rect(sx + 1, ty + 1, sx + 11, ty + thumbh - 1, RGB(130, 160, 235));
        }
    }
}

static void fm_click(Win *w, int mx, int my, int frame)
{
    int i, bx = w->x + 3, by = w->y + TITLE_H, vis = fm_vis(w);
    for (i = 0; i < FM_NB; i++) {
        if (mx >= bx + fm_bx[i] && mx < bx + fm_bx[i] + fm_bw[i] && my >= by + 4 && my < by + 20) {
            if (i == 0) fm_up();
            else if (i == 1) fm_refresh();
            else if (i == 2) fm_scroll_by(w, -3);
            else if (i == 4) wp_clear();
            else if (i == 5) fm_mkdir();
            else if (i == 6) fm_del();
            else if (i == 7) fm_restore();
            else fm_scroll_by(w, 3);
            return;
        }
    }
    if (my >= by + 38 && mx >= w->x + w->w - 20 && mx < w->x + w->w - 8) {   // scrollbar
        sb_drag = 1; sb_slot = (int)(w - wins);
        fm_sb_set(w, my);
        return;
    }
    if (my >= by + 38 && mx >= bx + 3 && mx < w->x + w->w - 22) {
        int row = (my - (by + 38)) / 14, idx = fm.scroll + row;
        if (row < vis && idx < fm.count) {
            if (idx == fm.last_row && frame - fm.last_frame < 30) { fm_enter(idx); fm.last_row = -1; }
            else { fm.sel = idx; fm.last_row = idx; fm.last_frame = frame; }
        }
    }
}

/* ---------- input (controller / mouse) + Settings app + Start menu ---------- */
static int input_mode = 0;       // 0 = controller (default at every startup), 1 = USB mouse
static int pad_ok = 0, pad_mode_set = 0;
static u32 mouse_en_g = 0;
static unsigned short pad_prev = 0, pad_last = 0;
static char pad_buf[256] __attribute__((aligned(64)));
static int menu_open = 0;

static int pad_poll(int *dx, int *dy, unsigned short *btns)
{
    struct padButtonStatus pb;
    int st = padGetState(0, 0), sx = 0, sy = 0;
    if (st != PAD_STATE_STABLE && st != PAD_STATE_FINDCTP1) { pad_ok = 0; return 0; }
    if (!pad_mode_set) { padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK); pad_mode_set = 1; }
    if (padRead(0, 0, &pb) == 0) return 0;
    pad_ok = 1;
    *btns = 0xffff ^ pb.btns;
    if ((pb.mode & 0xf0) == 0x70) {          // analog (DualShock) mode
        int hx = (int)pb.ljoy_h - 128, vy = (int)pb.ljoy_v - 128;
        if (abs(hx) > stick_dz) sx = (hx > 0 ? hx - stick_dz : hx + stick_dz) * cursor_speed / 40;
        if (abs(vy) > stick_dz) sy = (vy > 0 ? vy - stick_dz : vy + stick_dz) * cursor_speed / 40;
    }
    {
        int ds = 1 + cursor_speed * 3 / 5;
        if (*btns & PAD_LEFT)  sx -= ds;
        if (*btns & PAD_RIGHT) sx += ds;
        if (*btns & PAD_UP)    sy -= ds;
        if (*btns & PAD_DOWN)  sy += ds;
    }
    *dx = sx; *dy = sy;
    return 1;
}

static void draw_stepper(int x, int y, const char *label, int val)
{
    char b[12];
    draw_text(x + 16, y + 5, label, 20, 1, RGB(20, 20, 20));
    rect(x + 190, y, x + 212, y + 16, RGB(150, 150, 165)); rect(x + 191, y + 1, x + 211, y + 15, RGB(228, 228, 236));
    draw_text(x + 198, y + 5, "-", 1, 1, RGB(20, 20, 20));
    snprintf(b, sizeof(b), "%d", val);
    draw_text(x + 222, y + 5, b, 4, 1, RGB(0, 60, 160));
    rect(x + 256, y, x + 278, y + 16, RGB(150, 150, 165)); rect(x + 257, y + 1, x + 277, y + 15, RGB(228, 228, 236));
    draw_text(x + 264, y + 5, "+", 1, 1, RGB(20, 20, 20));
}

static int stepper_hit(int x, int y, int mx, int my)
{
    if (my < y || my >= y + 16) return 0;
    if (mx >= x + 190 && mx < x + 212) return -1;
    if (mx >= x + 256 && mx < x + 278) return 1;
    return 0;
}

static void draw_settings(Win *w)
{
    int x = w->x + 3, y = w->y + TITLE_H, i;
    draw_text(x + 16, y + 10, "INPUT DEVICE", 20, 2, RGB(30, 30, 30));
    for (i = 0; i < 2; i++) {
        int ry = y + 34 + i * 22;
        rect(x + 16, ry, x + 30, ry + 14, RGB(90, 90, 90));
        rect(x + 17, ry + 1, x + 29, ry + 13, RGB(255, 255, 255));
        if (input_mode == i) rect(x + 20, ry + 4, x + 26, ry + 10, RGB(0, 80, 220));
        draw_text(x + 40, ry + 4, i == 0 ? "CONTROLLER (DEFAULT)" : "USB MOUSE", 24, 1, RGB(20, 20, 20));
    }
    draw_text(x + 16, y + 82, pad_ok ? "CONTROLLER: DETECTED" : "CONTROLLER: NOT FOUND", 24, 1, pad_ok ? RGB(0, 120, 0) : RGB(200, 30, 30));
    draw_text(x + 16, y + 96, mouse_en_g ? "MOUSE: DETECTED" : "MOUSE: NOT FOUND", 24, 1, mouse_en_g ? RGB(0, 120, 0) : RGB(200, 30, 30));
    draw_stepper(x, y + 124, "VOLUME", g_vol);
    draw_stepper(x, y + 150, "STICK DEAD ZONE", stick_dz);
    draw_stepper(x, y + 176, "CURSOR SPEED", cursor_speed);
    draw_text(x + 16, y + 206, "X = CLICK, CIRCLE = RIGHT CLICK", 40, 1, RGB(90, 90, 90));
    draw_text(x + 16, y + 220, "START = MENU, L1/R1 = SCROLL", 40, 1, RGB(90, 90, 90));
}

static void settings_click(Win *w, int mx, int my)
{
    int x = w->x + 3, y = w->y + TITLE_H, i, d;
    for (i = 0; i < 2; i++) {
        int ry = y + 34 + i * 22;
        if (mx >= x + 16 && mx < x + 220 && my >= ry && my < ry + 16) {
            if (i == 1 && !mouse_en_g) { set_status("NO MOUSE DETECTED"); return; }
            input_mode = i;
            set_status(i == 0 ? "INPUT: CONTROLLER" : "INPUT: USB MOUSE");
        }
    }
    if ((d = stepper_hit(x, y + 124, mx, my))) {
        g_vol = clampi(g_vol + d * 10, 0, 100);
        if (audsrv_ok) audsrv_set_volume(g_vol);
        ini_save();
    } else if ((d = stepper_hit(x, y + 150, mx, my))) { stick_dz = clampi(stick_dz + d * 5, 10, 100); ini_save(); }
    else if ((d = stepper_hit(x, y + 176, mx, my))) { cursor_speed = clampi(cursor_speed + d, 1, 10); ini_save(); }
}

#define MENU_W 190
#define MENU_ITEM_H 26
#define MENU_NPOWER 3
static const char *power_names[MENU_NPOWER] = {"Restart", "Sleep", "Power Off"};
static int menu_h(void) { return (NAPPS + MENU_NPOWER) * MENU_ITEM_H + 10; }

static int menu_item_at(int my, int myp)
{
    int rel = my - myp - 3, k;
    if (rel < 0) return -1;
    if (rel < NAPPS * MENU_ITEM_H) return rel / MENU_ITEM_H;
    rel -= NAPPS * MENU_ITEM_H + 4;
    if (rel < 0) return -1;
    k = rel / MENU_ITEM_H;
    return k < MENU_NPOWER ? NAPPS + k : -1;
}

static void draw_menu(int mx, int my)
{
    int H = gs->Height, i, x = 0, y = H - TASKBAR_H - menu_h();
    int hv = (mx >= x && mx < x + MENU_W) ? menu_item_at(my, y) : -1;
    rect(x, y, x + MENU_W, y + menu_h(), RGB(20, 60, 170));
    rect(x + 2, y + 2, x + MENU_W - 2, y + menu_h() - 2, RGB(240, 240, 250));
    for (i = 0; i < NAPPS + MENU_NPOWER; i++) {
        int iy = y + 3 + i * MENU_ITEM_H + (i >= NAPPS ? 4 : 0);
        const char *nm = i < NAPPS ? app_names[i] : power_names[i - NAPPS];
        if (i == hv) rect(x + 3, iy, x + MENU_W - 3, iy + MENU_ITEM_H, RGB(50, 90, 200));
        draw_text(x + 14, iy + 8, nm, 20, 1, i == hv ? RGB(255, 255, 255) : RGB(20, 20, 20));
    }
    rect(x + 8, y + 3 + NAPPS * MENU_ITEM_H + 1, x + MENU_W - 8, y + 3 + NAPPS * MENU_ITEM_H + 3, RGB(170, 175, 200));
}

/* ---------- Minesweeper (9x9, 10 mines) ---------- */
#define MS_N 9
#define MS_MINES 10
#define MS_C 20
static struct {
    unsigned char mine[MS_N][MS_N], rev[MS_N][MS_N], flag[MS_N][MS_N], adj[MS_N][MS_N];
    int state;                 // 0 ready, 1 playing, 2 won, 3 lost
    int flags, revealed, start_frame, end_secs, exx, exy;
} ms;

static void ms_reset(void) { memset(&ms, 0, sizeof(ms)); ms.exx = ms.exy = -1; }

static void ms_place(int sx, int sy)
{
    int placed = 0, x, y, dx, dy;
    srand(g_frame * 7919 + 13);
    while (placed < MS_MINES) {
        x = rand() % MS_N; y = rand() % MS_N;
        if (ms.mine[y][x] || (abs(x - sx) <= 1 && abs(y - sy) <= 1)) continue;
        ms.mine[y][x] = 1; placed++;
    }
    for (y = 0; y < MS_N; y++)
        for (x = 0; x < MS_N; x++) {
            int n = 0;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < MS_N && ny < MS_N && ms.mine[ny][nx]) n++;
                }
            ms.adj[y][x] = n;
        }
}

static void ms_reveal(int x0, int y0)
{
    int stack[MS_N * MS_N * 8], sp = 0, dx, dy;
    stack[sp++] = y0 * MS_N + x0;
    while (sp > 0) {
        int c = stack[--sp], cx = c % MS_N, cy = c / MS_N;
        if (ms.rev[cy][cx] || ms.flag[cy][cx]) continue;
        ms.rev[cy][cx] = 1; ms.revealed++;
        if (ms.adj[cy][cx] == 0 && !ms.mine[cy][cx])
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    int nx = cx + dx, ny = cy + dy;
                    if (nx >= 0 && ny >= 0 && nx < MS_N && ny < MS_N && !ms.rev[ny][nx] && sp < MS_N * MS_N * 8 - 1)
                        stack[sp++] = ny * MS_N + nx;
                }
    }
}

static int ms_secs(void)
{
    int t = (ms.state == 1) ? (g_frame - ms.start_frame) / 50 : ms.end_secs;
    return t > 999 ? 999 : t;
}

static void ms_click(Win *w, int mx, int my, int right)
{
    int bx = w->x + 9, by = w->y + TITLE_H + 36;
    int sx = w->x + w->w / 2 - 12, sy = w->y + TITLE_H + 6;
    int cx, cy;
    if (!right && mx >= sx && mx < sx + 24 && my >= sy && my < sy + 24) { ms_reset(); return; }
    if (ms.state >= 2) return;
    if (mx < bx || my < by || mx >= bx + MS_N * MS_C || my >= by + MS_N * MS_C) return;
    cx = (mx - bx) / MS_C; cy = (my - by) / MS_C;
    if (right) {
        if (ms.rev[cy][cx]) return;
        if (ms.flag[cy][cx]) { ms.flag[cy][cx] = 0; ms.flags--; }
        else { ms.flag[cy][cx] = 1; ms.flags++; }
        return;
    }
    if (ms.flag[cy][cx] || ms.rev[cy][cx]) return;
    if (ms.state == 0) { ms_place(cx, cy); ms.state = 1; ms.start_frame = g_frame; }
    if (ms.mine[cy][cx]) {
        ms.state = 3; ms.exx = cx; ms.exy = cy;
        ms.end_secs = (g_frame - ms.start_frame) / 50;
        return;
    }
    ms_reveal(cx, cy);
    if (ms.revealed == MS_N * MS_N - MS_MINES) {
        ms.state = 2;
        ms.end_secs = (g_frame - ms.start_frame) / 50;
        ms.flags = MS_MINES;
    }
}

static void draw_ms(Win *w)
{
    static const int nc[9][3] = {{0,0,0},{0,0,255},{0,128,0},{255,0,0},{0,0,128},{128,0,0},{0,128,128},{0,0,0},{128,128,128}};
    int x = w->x, y = w->y + TITLE_H, cx, cy;
    int bx = w->x + 9, by = w->y + TITLE_H + 36;
    int sx = w->x + w->w / 2 - 12, sy = y + 6;
    char buf[8];
    rect(x + 3, y, x + w->w - 3, w->y + w->h - 3, RGB(192, 192, 192));
    // counters
    rect(x + 9, y + 8, x + 53, y + 30, RGB(0, 0, 0));
    snprintf(buf, sizeof(buf), "%03d", MS_MINES - ms.flags);
    draw_text(x + 13, y + 12, buf, 3, 2, RGB(255, 30, 30));
    rect(x + w->w - 53, y + 8, x + w->w - 9, y + 30, RGB(0, 0, 0));
    snprintf(buf, sizeof(buf), "%03d", ms_secs());
    draw_text(x + w->w - 49, y + 12, buf, 3, 2, RGB(255, 30, 30));
    // smiley
    rect(sx, sy, sx + 24, sy + 24, RGB(60, 60, 60));
    rect(sx + 1, sy + 1, sx + 23, sy + 23, RGB(250, 220, 40));
    if (ms.state == 2) rect(sx + 4, sy + 7, sx + 20, sy + 11, RGB(0, 0, 0));
    else { rect(sx + 7, sy + 7, sx + 10, sy + 11, RGB(0, 0, 0)); rect(sx + 14, sy + 7, sx + 17, sy + 11, RGB(0, 0, 0)); }
    if (ms.state == 3) {
        rect(sx + 6, sy + 17, sx + 8, sy + 19, RGB(0, 0, 0)); rect(sx + 8, sy + 15, sx + 16, sy + 17, RGB(0, 0, 0)); rect(sx + 16, sy + 17, sx + 18, sy + 19, RGB(0, 0, 0));
    } else {
        rect(sx + 6, sy + 14, sx + 8, sy + 16, RGB(0, 0, 0)); rect(sx + 8, sy + 16, sx + 16, sy + 18, RGB(0, 0, 0)); rect(sx + 16, sy + 14, sx + 18, sy + 16, RGB(0, 0, 0));
    }
    // grid
    for (cy = 0; cy < MS_N; cy++)
        for (cx = 0; cx < MS_N; cx++) {
            int x0 = bx + cx * MS_C, y0 = by + cy * MS_C;
            int show = ms.rev[cy][cx] || (ms.state == 3 && ms.mine[cy][cx] && !ms.flag[cy][cx]);
            if (show) {
                rect(x0, y0, x0 + MS_C, y0 + MS_C, RGB(128, 128, 128));
                rect(x0 + 1, y0 + 1, x0 + MS_C, y0 + MS_C, (cx == ms.exx && cy == ms.exy) ? RGB(255, 40, 40) : RGB(192, 192, 192));
                if (ms.mine[cy][cx]) rect(x0 + 6, y0 + 6, x0 + 14, y0 + 14, RGB(0, 0, 0));
                else if (ms.adj[cy][cx] > 0) {
                    int a = ms.adj[cy][cx];
                    draw_char(x0 + 5, y0 + 3, '0' + a, 2, RGB(nc[a][0], nc[a][1], nc[a][2]));
                }
            } else {
                rect(x0, y0, x0 + MS_C, y0 + MS_C, RGB(128, 128, 128));
                rect(x0, y0, x0 + MS_C - 1, y0 + MS_C - 1, RGB(255, 255, 255));
                rect(x0 + 2, y0 + 2, x0 + MS_C - 1, y0 + MS_C - 1, RGB(192, 192, 192));
                if (ms.flag[cy][cx]) {
                    rect(x0 + 10, y0 + 4, x0 + 12, y0 + 15, RGB(0, 0, 0));
                    rect(x0 + 5, y0 + 4, x0 + 11, y0 + 9, RGB(230, 20, 20));
                    rect(x0 + 6, y0 + 15, x0 + 15, y0 + 17, RGB(0, 0, 0));
                }
            }
        }
}

/* ---------- Solitaire (Klondike, click-to-select) ---------- */
#define SL_CW 38
#define SL_CH 50
#define SL_PITCH 42
static struct {
    int tab[7][20], tn[7], down[7];
    int stock[24], sn, waste[24], wn;
    int found[4];
    int sel_kind, sel_col, sel_idx;   // 0 none, 1 waste, 2 tableau, 3 foundation
    int won;
} sl;

static const unsigned char suit_gl[4][7] = {
    {0x36, 0x7F, 0x7F, 0x7F, 0x3E, 0x1C, 0x08},   // hearts
    {0x08, 0x1C, 0x3E, 0x7F, 0x3E, 0x1C, 0x08},   // diamonds
    {0x1C, 0x1C, 0x6B, 0x7F, 0x6B, 0x08, 0x1C},   // clubs
    {0x08, 0x1C, 0x3E, 0x7F, 0x7F, 0x08, 0x1C}    // spades
};
static const char *rank_str[13] = {"A","2","3","4","5","6","7","8","9","10","J","Q","K"};

static int sl_red(int c) { return (c / 13) < 2; }
static int sl_rank(int c) { return c % 13; }

static void sl_deal(void)
{
    int deck[52], i, j, k = 0, t;
    memset(&sl, 0, sizeof(sl));
    for (i = 0; i < 52; i++) deck[i] = i;
    srand(g_frame * 31337 + 7);
    for (i = 51; i > 0; i--) { j = rand() % (i + 1); t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    for (i = 0; i < 7; i++) {
        for (j = 0; j <= i; j++) sl.tab[i][sl.tn[i]++] = deck[k++];
        sl.down[i] = i;
    }
    while (k < 52) sl.stock[sl.sn++] = deck[k++];
}

static int sl_top_tab(int col) { return sl.tn[col] ? sl.tab[col][sl.tn[col] - 1] : -1; }

static int sl_can_tab(int card, int col)
{
    int t = sl_top_tab(col);
    if (t < 0) return sl_rank(card) == 12;
    return sl_red(t) != sl_red(card) && sl_rank(t) == sl_rank(card) + 1;
}

static int sl_can_found(int card) { return sl.found[card / 13] == sl_rank(card); }

static void sl_after_move(void)
{
    int i;
    for (i = 0; i < 7; i++) {
        if (sl.tn[i] == 0) sl.down[i] = 0;
        else if (sl.down[i] >= sl.tn[i]) sl.down[i] = sl.tn[i] - 1;
    }
    sl.sel_kind = 0;
    if (sl.found[0] + sl.found[1] + sl.found[2] + sl.found[3] == 52) sl.won = 1;
}

static int sl_try_tab(int dest)
{
    int card, j;
    if (sl.sel_kind == 1) {
        if (!sl.wn) return 0;
        card = sl.waste[sl.wn - 1];
        if (!sl_can_tab(card, dest)) return 0;
        sl.tab[dest][sl.tn[dest]++] = card; sl.wn--;
    } else if (sl.sel_kind == 2) {
        int c = sl.sel_col, k = sl.sel_idx;
        if (c == dest) return 0;
        card = sl.tab[c][k];
        if (!sl_can_tab(card, dest)) return 0;
        for (j = k; j < sl.tn[c]; j++) sl.tab[dest][sl.tn[dest]++] = sl.tab[c][j];
        sl.tn[c] = k;
    } else if (sl.sel_kind == 3) {
        int s = sl.sel_col;
        card = s * 13 + sl.found[s] - 1;
        if (!sl_can_tab(card, dest)) return 0;
        sl.tab[dest][sl.tn[dest]++] = card; sl.found[s]--;
    } else return 0;
    sl_after_move();
    return 1;
}

static int sl_try_found(void)
{
    int card;
    if (sl.sel_kind == 1 && sl.wn) {
        card = sl.waste[sl.wn - 1];
        if (!sl_can_found(card)) return 0;
        sl.found[card / 13]++; sl.wn--;
    } else if (sl.sel_kind == 2 && sl.sel_idx == sl.tn[sl.sel_col] - 1) {
        card = sl.tab[sl.sel_col][sl.sel_idx];
        if (!sl_can_found(card)) return 0;
        sl.found[card / 13]++; sl.tn[sl.sel_col]--;
    } else return 0;
    sl_after_move();
    return 1;
}

static int sl_tab_y(int y0, int col, int idx)
{
    int j, y = y0;
    for (j = 0; j < idx; j++) y += (j < sl.down[col]) ? 6 : 14;
    return y;
}

// returns card index under my in column, -2 if empty column area hit, -1 if none
static int sl_hit(int y0, int col, int my)
{
    int i;
    if (sl.tn[col] == 0) return (my >= y0 && my < y0 + SL_CH) ? -2 : -1;
    for (i = sl.tn[col] - 1; i >= 0; i--) {
        int yy = sl_tab_y(y0, col, i);
        int hh = (i == sl.tn[col] - 1) ? SL_CH : ((i < sl.down[col]) ? 6 : 14);
        if (my >= yy && my < yy + hh) return i;
    }
    return -1;
}

static void sl_click(Win *w, int mx, int my, int right)
{
    int bx = w->x + 9, ty = w->y + TITLE_H + 8, y0 = w->y + TITLE_H + 70;
    int col, within, idx;
    if (mx < bx) return;
    col = (mx - bx) / SL_PITCH;
    within = (mx - bx) % SL_PITCH;
    if (col < 0 || col > 6 || within >= SL_CW) return;

    if (my >= ty && my < ty + SL_CH) {            // top row
        if (col == 2) { if (!right && my < ty + 22) sl_deal(); return; }
        if (sl.won) return;
        if (col == 0) {
            if (right) return;
            if (sl.sn > 0) sl.waste[sl.wn++] = sl.stock[--sl.sn];
            else while (sl.wn > 0) sl.stock[sl.sn++] = sl.waste[--sl.wn];
            sl.sel_kind = 0;
        } else if (col == 1) {
            if (!sl.wn) return;
            if (right) { sl.sel_kind = 1; if (!sl_try_found()) sl.sel_kind = 0; }
            else sl.sel_kind = (sl.sel_kind == 1) ? 0 : 1;
        } else if (col >= 3) {
            int f = col - 3;
            if (right) return;
            if (sl.sel_kind == 1 || sl.sel_kind == 2) { if (!sl_try_found()) sl.sel_kind = 0; }
            else if (sl.sel_kind == 3) sl.sel_kind = 0;
            else if (sl.found[f] > 0) { sl.sel_kind = 3; sl.sel_col = f; }
        }
        return;
    }
    if (my < y0 || sl.won) return;
    idx = sl_hit(y0, col, my);
    if (idx == -1) return;
    if (right) {                                   // auto-send top card to foundation
        if (idx >= 0 && idx == sl.tn[col] - 1) {
            sl.sel_kind = 2; sl.sel_col = col; sl.sel_idx = idx;
            if (!sl_try_found()) sl.sel_kind = 0;
        }
        return;
    }
    if (sl.sel_kind != 0) {
        if (sl_try_tab(col)) return;
        sl.sel_kind = 0;
    }
    if (idx >= 0 && idx >= sl.down[col]) { sl.sel_kind = 2; sl.sel_col = col; sl.sel_idx = idx; }
}

static void draw_suit(int x, int y, int s, int sc, u64 col)
{
    int r, c;
    for (r = 0; r < 7; r++) {
        c = 0;
        while (c < 7) {
            if ((suit_gl[s][r] >> (6 - c)) & 1) {
                int st = c;
                while (c < 7 && ((suit_gl[s][r] >> (6 - c)) & 1)) c++;
                rect(x + st * sc, y + r * sc, x + c * sc, y + (r + 1) * sc, col);
            } else c++;
        }
    }
}

static void draw_card(int x, int y, int card, int up, int sel)
{
    rect(x, y, x + SL_CW, y + SL_CH, sel ? RGB(255, 230, 0) : RGB(30, 30, 30));
    if (up) {
        u64 col = sl_red(card) ? RGB(210, 20, 20) : RGB(15, 15, 15);
        rect(x + 1, y + 1, x + SL_CW - 1, y + SL_CH - 1, RGB(250, 250, 245));
        draw_text(x + 3, y + 3, rank_str[sl_rank(card)], 2, 1, col);
        draw_suit(x + 3, y + 13, card / 13, 1, col);
        draw_suit(x + 19, y + 30, card / 13, 2, col);
    } else {
        rect(x + 1, y + 1, x + SL_CW - 1, y + SL_CH - 1, RGB(30, 60, 170));
        rect(x + 4, y + 4, x + SL_CW - 4, y + SL_CH - 4, RGB(90, 130, 230));
        rect(x + 6, y + 6, x + SL_CW - 6, y + SL_CH - 6, RGB(30, 60, 170));
    }
}

static void draw_slot(int x, int y)
{
    rect(x, y, x + SL_CW, y + SL_CH, RGB(0, 80, 35));
    rect(x + 1, y + 1, x + SL_CW - 1, y + SL_CH - 1, RGB(0, 110, 50));
}

static void draw_sol(Win *w)
{
    int bx = w->x + 9, ty = w->y + TITLE_H + 8, y0 = w->y + TITLE_H + 70;
    int i, j;
    rect(w->x + 3, w->y + TITLE_H, w->x + w->w - 3, w->y + w->h - 3, RGB(0, 110, 50));
    // stock
    if (sl.sn > 0) draw_card(bx, ty, 0, 0, 0); else draw_slot(bx, ty);
    // waste
    if (sl.wn > 0) draw_card(bx + SL_PITCH, ty, sl.waste[sl.wn - 1], 1, sl.sel_kind == 1);
    else draw_slot(bx + SL_PITCH, ty);
    // NEW button
    rect(bx + 2 * SL_PITCH, ty, bx + 2 * SL_PITCH + SL_CW, ty + 20, RGB(60, 60, 60));
    rect(bx + 2 * SL_PITCH + 1, ty + 1, bx + 2 * SL_PITCH + SL_CW - 1, ty + 19, RGB(225, 225, 232));
    draw_text(bx + 2 * SL_PITCH + 8, ty + 7, "NEW", 3, 1, RGB(20, 20, 20));
    // foundations
    for (i = 0; i < 4; i++) {
        int fx = bx + (3 + i) * SL_PITCH;
        if (sl.found[i] > 0) draw_card(fx, ty, i * 13 + sl.found[i] - 1, 1, sl.sel_kind == 3 && sl.sel_col == i);
        else { draw_slot(fx, ty); draw_suit(fx + 12, ty + 18, i, 2, RGB(0, 150, 70)); }
    }
    // tableau
    for (i = 0; i < 7; i++) {
        int cx = bx + i * SL_PITCH;
        if (sl.tn[i] == 0) { draw_slot(cx, y0); continue; }
        for (j = 0; j < sl.tn[i]; j++) {
            int sel = (sl.sel_kind == 2 && sl.sel_col == i && j >= sl.sel_idx);
            draw_card(cx, sl_tab_y(y0, i, j), sl.tab[i][j], j >= sl.down[i], sel);
        }
    }
    if (sl.won) {
        rect(bx + 40, y0 + 80, bx + 250, y0 + 130, RGB(0, 0, 0));
        draw_text(bx + 52, y0 + 94, "YOU WIN!", 8, 3, RGB(255, 230, 0));
    }
}

/* ---------- Media Player (MP3 via minimp3 + audsrv, spectrum visualizer) ---------- */
#define MP_MAX 64
#define FFT_N 512
#define FFT_LOG 9
#define NBARS 24
static struct {
    char dir[256];
    char names[MP_MAX][100];
    int n, cur, state, scroll;          // state: 0 stopped, 1 playing, 2 paused
    unsigned char *buf;
    long size, pos;
    mp3dec_t dec;
    int hz, kbps, demo;
    unsigned int played, tone_n;
    float tone_ph;
    short vis[FFT_N];
    float bars[NBARS];
} mp;
static float fft_c[FFT_N / 2], fft_s[FFT_N / 2], fft_w[FFT_N];
static int bar_edge[NBARS + 1];

static void mp_init(void)
{
    int i, prev = 1;
    for (i = 0; i < FFT_N / 2; i++) { fft_c[i] = cosf(6.2831853f * i / FFT_N); fft_s[i] = sinf(6.2831853f * i / FFT_N); }
    for (i = 0; i < FFT_N; i++) fft_w[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (FFT_N - 1));
    for (i = 0; i <= NBARS; i++) {
        int e = (int)powf(230.0f, (float)i / NBARS);
        if (e <= prev && i > 0) e = prev + 1;
        bar_edge[i] = e; prev = e;
    }
    if (audsrv_ok) audsrv_set_volume(g_vol);
}

static void mp_stop(void)
{
    if (mp.buf) { free(mp.buf); mp.buf = NULL; }
    mp.state = 0;
    if (audsrv_ok) audsrv_stop_audio();
}

static void mp_fix_scroll(void)
{
    if (mp.cur < mp.scroll) mp.scroll = mp.cur;
    if (mp.cur >= mp.scroll + 6) mp.scroll = mp.cur - 5;
    if (mp.scroll < 0) mp.scroll = 0;
}

static int mp_load(int idx)
{
    char full[400];
    FILE *f;
    long sz;
    mp_stop();
    mp.demo = 0;
    if (idx < 0 || idx >= mp.n) return -1;
    snprintf(full, sizeof(full), "%s%s", mp.dir, mp.names[idx]);
    f = fopen(full, "rb");
    if (!f) { set_status("CANT OPEN FILE"); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 20 * 1024 * 1024) { fclose(f); set_status("MP3 TOO BIG (MAX 20 MB)"); return -1; }
    mp.buf = malloc(sz);
    if (!mp.buf) { fclose(f); set_status("OUT OF MEMORY"); return -1; }
    if (fread(mp.buf, 1, sz, f) != (size_t)sz) { fclose(f); free(mp.buf); mp.buf = NULL; set_status("READ ERROR"); return -1; }
    fclose(f);
    mp.size = sz; mp.pos = 0; mp.played = 0; mp.hz = 0; mp.kbps = 0; mp.cur = idx;
    mp3dec_init(&mp.dec);
    mp.state = 1;
    mp_fix_scroll();
    return 0;
}

static void mp_next(void)
{
    if (mp.cur + 1 < mp.n) mp_load(mp.cur + 1);
    else { mp_stop(); set_status("END OF PLAYLIST"); }
}

static void mp_open_from_fm(int idx)
{
    int i, cur = 0;
    mp.n = 0;
    strncpy(mp.dir, fm.path, sizeof(mp.dir) - 1);
    mp.dir[sizeof(mp.dir) - 1] = 0;
    for (i = 0; i < fm.count && mp.n < MP_MAX; i++) {
        if (!fm.e[i].dir && is_mp3(fm.e[i].name)) {
            if (i == idx) cur = mp.n;
            strncpy(mp.names[mp.n], fm.e[i].name, 99);
            mp.names[mp.n][99] = 0;
            mp.n++;
        }
    }
    open_app(3);
    mp_load(cur);
}

static void mp_pump(void)
{
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME], out[1152 * 2];
    int iter, i, samples = 0, hz = 44100;
    if (mp.state != 1 || !audsrv_ok) return;
    for (iter = 0; iter < 3; iter++) {
        if (audsrv_available() < 4608 + 512) break;
        if (mp.demo) {
            samples = 1152;
            for (i = 0; i < samples; i++) {
                float t = (float)mp.tone_n++ / 44100.0f;
                float f = 330.0f + 220.0f * sinf(t * 0.8f);
                float amp = 0.4f * (0.65f + 0.35f * sinf(t * 6.0f));
                float v;
                mp.tone_ph += 6.2831853f * f / 44100.0f;
                if (mp.tone_ph > 6.2831853f * 1000.0f) mp.tone_ph = 0;
                v = sinf(mp.tone_ph) * 0.6f + sinf(mp.tone_ph * 2.01f) * 0.25f + sinf(mp.tone_ph * 3.02f) * 0.12f;
                out[2 * i] = out[2 * i + 1] = (short)(v * amp * 20000.0f);
            }
        } else {
            mp3dec_frame_info_t info;
            if (!mp.buf || mp.pos >= mp.size) { mp_next(); return; }
            samples = mp3dec_decode_frame(&mp.dec, mp.buf + mp.pos, (int)(mp.size - mp.pos), pcm, &info);
            if (info.frame_bytes <= 0) { mp_next(); return; }
            mp.pos += info.frame_bytes;
            if (samples <= 0) continue;
            hz = info.hz;
            if (!mp.kbps) mp.kbps = info.bitrate_kbps;
            if (info.channels == 1) for (i = 0; i < samples; i++) out[2 * i] = out[2 * i + 1] = pcm[i];
            else memcpy(out, pcm, samples * 4);
        }
        if (mp.hz != hz) {
            audsrv_fmt_t fmt;
            fmt.freq = hz; fmt.bits = 16; fmt.channels = 2;
            audsrv_set_format(&fmt);
            mp.hz = hz;
        }
        audsrv_play_audio((char *)out, samples * 4);
        mp.played += samples;
        for (i = 0; i < FFT_N; i++) {
            int si = samples - FFT_N + i;
            mp.vis[i] = (si >= 0) ? (short)(((int)out[2 * si] + out[2 * si + 1]) / 2) : 0;
        }
    }
}

static void mp_viz_update(void)
{
    float re[FFT_N], im[FFT_N];
    int i, j, k, len, b;
    if (mp.state != 1) { for (b = 0; b < NBARS; b++) mp.bars[b] *= 0.85f; return; }
    for (i = 0; i < FFT_N; i++) {
        int r = 0, bb = i;
        for (k = 0; k < FFT_LOG; k++) { r = (r << 1) | (bb & 1); bb >>= 1; }
        re[r] = mp.vis[i] * fft_w[i];
        im[r] = 0;
    }
    for (len = 2; len <= FFT_N; len <<= 1) {
        int half = len / 2, step = FFT_N / len;
        for (i = 0; i < FFT_N; i += len)
            for (j = 0; j < half; j++) {
                float wr = fft_c[j * step], wi = -fft_s[j * step];
                float xr = re[i + j + half], xi = im[i + j + half];
                float tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
                re[i + j + half] = re[i + j] - tr; im[i + j + half] = im[i + j] - ti;
                re[i + j] += tr; im[i + j] += ti;
            }
    }
    for (b = 0; b < NBARS; b++) {
        float sum = 0, v, x;
        int lo = bar_edge[b], hi = bar_edge[b + 1], n = 0;
        if (hi > FFT_N / 2 - 1) hi = FFT_N / 2 - 1;
        for (i = lo; i < hi; i++) { sum += re[i] * re[i] + im[i] * im[i]; n++; }
        x = n ? sqrtf(sum / n) / (32768.0f * FFT_N / 4.0f) : 0;
        v = (20.0f * log10f(x + 1e-6f) + 62.0f) / 56.0f;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        mp.bars[b] = (v > mp.bars[b]) ? v : mp.bars[b] * 0.86f;
    }
}

static void tri_right(int x, int cy, u64 c)
{
    int k;
    for (k = 0; k < 8; k++) { int h = 7 - k; rect(x + k, cy - h, x + k + 1, cy + h + 1, c); }
}
static void tri_left(int x, int cy, u64 c)
{
    int k;
    for (k = 0; k < 8; k++) { int h = 7 - k; rect(x + 7 - k, cy - h, x + 8 - k, cy + h + 1, c); }
}

static void draw_mp(Win *w)
{
    int bx = w->x + 3, by = w->y + TITLE_H, i, s, r;
    char buf[48];
    int el = mp.hz ? (int)(mp.played / mp.hz) : 0;
    int dur = (mp.kbps > 0 && !mp.demo) ? (int)(mp.size * 8 / (mp.kbps * 1000)) : 0;
    u64 white = RGB(255, 255, 255), bg = RGB(40, 46, 66);
    rect(bx, by, w->x + w->w - 3, w->y + w->h - 3, bg);
    // visualizer
    rect(bx + 8, by + 8, bx + 352, by + 118, RGB(90, 100, 130));
    rect(bx + 9, by + 9, bx + 351, by + 117, RGB(0, 0, 0));
    for (i = 0; i < NBARS; i++) {
        int n = (int)(mp.bars[i] * 20.0f), bxl = bx + 12 + i * 14;
        for (s = 0; s < n; s++) {
            u64 c = s < 12 ? RGB(40, 220, 60) : s < 17 ? RGB(240, 220, 40) : RGB(240, 50, 40);
            rect(bxl, by + 113 - s * 5 - 4, bxl + 12, by + 113 - s * 5, c);
        }
    }
    // title
    if (!audsrv_ok) draw_text(bx + 8, by + 124, "AUDIO NOT AVAILABLE", 56, 1, RGB(255, 120, 120));
    else if (mp.demo && mp.state) draw_text(bx + 8, by + 124, "TEST TONE (NO MP3 LOADED)", 56, 1, white);
    else if (mp.n > 0) draw_text(bx + 8, by + 124, mp.names[mp.cur], 56, 1, white);
    else draw_text(bx + 8, by + 124, "NO FILE - DOUBLE-CLICK AN MP3 IN MY COMPUTER", 56, 1, RGB(190, 200, 230));
    // time
    if (dur > 0) snprintf(buf, sizeof(buf), "%02d:%02d / %02d:%02d", el / 60, el % 60, dur / 60, dur % 60);
    else snprintf(buf, sizeof(buf), "%02d:%02d / --:--", el / 60, el % 60);
    draw_text(bx + 8, by + 138, buf, 20, 1, RGB(120, 220, 255));
    // progress
    rect(bx + 8, by + 154, bx + 352, by + 160, RGB(90, 100, 130));
    if (dur > 0) { int fw = (int)(344L * el / dur); if (fw > 344) fw = 344; rect(bx + 8, by + 154, bx + 8 + fw, by + 160, RGB(255, 150, 30)); }
    // transport buttons: prev, play/pause, stop, next
    for (i = 0; i < 4; i++) {
        int x0 = bx + 8 + i * 36;
        rect(x0, by + 168, x0 + 32, by + 192, RGB(170, 180, 205));
        rect(x0 + 1, by + 169, x0 + 31, by + 191, RGB(225, 228, 240));
    }
    rect(bx + 14, by + 173, bx + 16, by + 187, RGB(20, 20, 20)); tri_left(bx + 17, by + 180, RGB(20, 20, 20));
    if (mp.state == 1) { rect(bx + 54, by + 173, bx + 58, by + 187, RGB(20, 20, 20)); rect(bx + 62, by + 173, bx + 66, by + 187, RGB(20, 20, 20)); }
    else tri_right(bx + 56, by + 180, RGB(20, 130, 40));
    rect(bx + 90, by + 174, bx + 102, by + 186, RGB(20, 20, 20));
    tri_right(bx + 124, by + 180, RGB(20, 20, 20)); rect(bx + 133, by + 173, bx + 135, by + 187, RGB(20, 20, 20));
    // volume
    draw_text(bx + 190, by + 176, "VOL", 3, 1, white);
    rect(bx + 220, by + 168, bx + 246, by + 192, RGB(225, 228, 240)); draw_text(bx + 230, by + 176, "-", 1, 1, RGB(20, 20, 20));
    snprintf(buf, sizeof(buf), "%d", g_vol);
    draw_text(bx + 254, by + 176, buf, 3, 1, white);
    rect(bx + 286, by + 168, bx + 312, by + 192, RGB(225, 228, 240)); draw_text(bx + 296, by + 176, "+", 1, 1, RGB(20, 20, 20));
    // playlist
    rect(bx + 8, by + 200, bx + 352, by + 296, RGB(250, 250, 252));
    for (r = 0; r < 6; r++) {
        int idx = mp.scroll + r, ry = by + 204 + r * 15;
        if (idx >= mp.n) break;
        if (idx == mp.cur) rect(bx + 10, ry - 2, bx + 350, ry + 12, RGB(50, 90, 200));
        draw_text(bx + 16, ry + 2, mp.names[idx], 52, 1, idx == mp.cur ? white : RGB(20, 20, 20));
    }
}

static void mp_click(Win *w, int mx, int my, int frame)
{
    int bx = w->x + 3, by = w->y + TITLE_H, i;
    (void)frame;
    if (my >= by + 168 && my < by + 192) {
        for (i = 0; i < 4; i++) {
            int x0 = bx + 8 + i * 36;
            if (mx >= x0 && mx < x0 + 32) {
                if (i == 0) { if (mp.n > 0) mp_load(mp.cur > 0 ? mp.cur - 1 : 0); }
                else if (i == 1) {
                    if (mp.state == 1) mp.state = 2;
                    else if (mp.state == 2) mp.state = 1;
                    else if (mp.n > 0) mp_load(mp.cur);
                    else { mp.demo = 1; mp.state = 1; mp.hz = 0; mp.played = 0; mp.kbps = 0; }
                }
                else if (i == 2) mp_stop();
                else { if (mp.n > 0) mp_next(); }
                return;
            }
        }
        if (mx >= bx + 220 && mx < bx + 246) { g_vol = g_vol > 10 ? g_vol - 10 : 0; if (audsrv_ok) audsrv_set_volume(g_vol); }
        if (mx >= bx + 286 && mx < bx + 312) { g_vol = g_vol < 90 ? g_vol + 10 : 100; if (audsrv_ok) audsrv_set_volume(g_vol); }
        return;
    }
    if (my >= by + 202 && my < by + 296 && mx >= bx + 8 && mx < bx + 352) {
        int idx = mp.scroll + (my - (by + 202)) / 15;
        if (idx < mp.n) mp_load(idx);
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
    wins[slot].w = (app == 0) ? 400 : (app == 1) ? 198 : (app == 2) ? 312 : (app == 3) ? 360 : (app == 4) ? 320 : 300;
    wins[slot].h = (app == 0) ? 330 : (app == 1) ? 250 : (app == 2) ? 360 : (app == 3) ? 326 : (app == 4) ? 262 : 200;
    wins[slot].x = 110 + (nopen % 6) * 28;
    wins[slot].y = 20 + (nopen % 6) * 28;
    order[nopen++] = slot;
    if (app == 0) fm_refresh();
    if (app == 1) ms_reset();
    if (app == 2) sl_deal();
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
    else if (w->app == 4) draw_settings(w);
    else if (w->app == 1) draw_ms(w);
    else if (w->app == 2) draw_sol(w);
    else if (w->app == 3) draw_mp(w);
    else draw_text(x + 20, y + 80, "COMING SOON", 20, 2, RGB(90, 90, 90));
}

/* ---------- desktop ---------- */
static int icon_x(int i) { return icon_px[i]; }
static int icon_y(int i) { return icon_py[i]; }

static int isqrt_i(int n) { int r = 0; while ((r + 1) * (r + 1) <= n) r++; return r; }

static void draw_disc(int cx, int cy, int r, u64 col)
{
    int dy;
    for (dy = -r; dy <= r; dy++) {
        int dx = isqrt_i(r * r - dy * dy);
        rect(cx - dx, cy + dy, cx + dx + 1, cy + dy + 1, col);
    }
}

// Windows-style 48x48 icons drawn from primitives
static void draw_app_icon(int app, int x, int y)
{
    int k;
    if (app == 0) {                                   // My Computer: monitor
        rect(x + 3, y + 4, x + 45, y + 36, RGB(70, 70, 80));
        rect(x + 4, y + 5, x + 44, y + 35, RGB(205, 205, 215));
        rect(x + 4, y + 5, x + 44, y + 7, RGB(240, 240, 248));
        vgrad(x + 8, y + 9, x + 40, y + 29, 40, 110, 230, 150, 200, 255);
        rect(x + 8, y + 23, x + 40, y + 29, RGB(70, 170, 60));       // hill
        rect(x + 8, y + 21, x + 24, y + 23, RGB(70, 170, 60));
        rect(x + 40, y + 9, x + 41, y + 29, RGB(30, 60, 140));
        rect(x + 8, y + 29, x + 40, y + 30, RGB(30, 60, 140));
        rect(x + 36, y + 32, x + 40, y + 34, RGB(0, 220, 60));        // power LED
        rect(x + 19, y + 36, x + 29, y + 40, RGB(170, 170, 182));     // neck
        rect(x + 11, y + 40, x + 37, y + 44, RGB(70, 70, 80));        // base
        rect(x + 12, y + 40, x + 36, y + 43, RGB(205, 205, 215));
    } else if (app == 1) {                            // Minesweeper: mine on tile
        rect(x + 2, y + 2, x + 46, y + 46, RGB(110, 110, 110));
        rect(x + 2, y + 2, x + 44, y + 44, RGB(255, 255, 255));
        rect(x + 5, y + 5, x + 44, y + 44, RGB(192, 192, 192));
        rect(x + 22, y + 8, x + 27, y + 41, RGB(0, 0, 0));            // spikes
        rect(x + 8, y + 22, x + 41, y + 27, RGB(0, 0, 0));
        for (k = 0; k < 6; k++) {
            rect(x + 10 + k * 2, y + 10 + k * 2, x + 14 + k * 2, y + 14 + k * 2, RGB(0, 0, 0));
            rect(x + 34 - k * 2, y + 10 + k * 2, x + 38 - k * 2, y + 14 + k * 2, RGB(0, 0, 0));
        }
        draw_disc(x + 24, y + 24, 11, RGB(0, 0, 0));
        draw_disc(x + 20, y + 20, 3, RGB(255, 255, 255));
        rect(x + 33, y + 6, x + 35, y + 17, RGB(0, 0, 0));            // little flag
        rect(x + 35, y + 6, x + 42, y + 11, RGB(230, 20, 20));
    } else if (app == 2) {                            // Solitaire: cards on felt
        rect(x + 2, y + 2, x + 46, y + 46, RGB(0, 70, 30));
        rect(x + 3, y + 3, x + 45, y + 45, RGB(0, 125, 55));
        rect(x + 7, y + 8, x + 29, y + 42, RGB(25, 25, 25));          // back card
        rect(x + 8, y + 9, x + 28, y + 41, RGB(30, 60, 170));
        rect(x + 11, y + 12, x + 25, y + 38, RGB(100, 140, 235));
        rect(x + 13, y + 14, x + 23, y + 36, RGB(30, 60, 170));
        rect(x + 17, y + 5, x + 42, y + 44, RGB(25, 25, 25));         // front card
        rect(x + 18, y + 6, x + 41, y + 43, RGB(252, 252, 248));
        draw_text(x + 21, y + 9, "A", 1, 2, RGB(210, 20, 20));
        draw_suit(x + 21, y + 22, 0, 3, RGB(210, 20, 20));
    } else if (app == 3) {                            // Media Player: orange orb + play
        draw_disc(x + 24, y + 24, 22, RGB(15, 45, 130));
        draw_disc(x + 24, y + 24, 19, RGB(240, 120, 10));
        draw_disc(x + 23, y + 20, 14, RGB(255, 165, 50));
        draw_disc(x + 22, y + 15, 8, RGB(255, 205, 120));
        for (k = 0; k < 13; k++) {
            int half = 11 - (k * 11) / 13;
            rect(x + 18 + k, y + 24 - half, x + 19 + k, y + 24 + half + 1, RGB(255, 255, 255));
        }
    } else {                                          // Settings: gear
        draw_disc(x + 24, y + 24, 18, RGB(110, 110, 120));
        rect(x + 20, y + 3, x + 28, y + 11, RGB(110, 110, 120));
        rect(x + 20, y + 37, x + 28, y + 45, RGB(110, 110, 120));
        rect(x + 3, y + 20, x + 11, y + 28, RGB(110, 110, 120));
        rect(x + 37, y + 20, x + 45, y + 28, RGB(110, 110, 120));
        draw_disc(x + 24, y + 24, 15, RGB(185, 185, 195));
        draw_disc(x + 24, y + 24, 6, RGB(236, 233, 216));
    }
}

static void draw_bin_icon(int x, int y)
{
    rect(x + 9, y + 9, x + 39, y + 14, RGB(70, 75, 85));                 // lid
    rect(x + 18, y + 5, x + 30, y + 9, RGB(70, 75, 85));                 // handle
    rect(x + 11, y + 14, x + 37, y + 43, RGB(70, 75, 85));               // body outline
    rect(x + 12, y + 14, x + 36, y + 42, RGB(190, 198, 210));
    rect(x + 12, y + 14, x + 17, y + 42, RGB(225, 230, 240));            // highlight
    rect(x + 18, y + 17, x + 20, y + 39, RGB(110, 118, 135));            // ridges
    rect(x + 23, y + 17, x + 25, y + 39, RGB(110, 118, 135));
    rect(x + 28, y + 17, x + 30, y + 39, RGB(110, 118, 135));
    rect(x + 12, y + 38, x + 36, y + 42, RGB(60, 150, 70));              // green recycle band
}

static void draw_icon(int i, int selected)
{
    int x = icon_x(i), y = icon_y(i);
    const char *nm = (i < 4) ? app_names[i] : "Recycle Bin";
    int len = strlen(nm), tx = x + 24 - len * 3;
    if (selected) rect(x - 16, y - 4, x + 64, y + 68, RGB(50, 90, 200));
    if (i < 4) draw_app_icon(i, x, y); else draw_bin_icon(x, y);
    draw_text(tx + 1, y + 55, nm, 14, 1, RGB(0, 0, 0));      // shadow
    draw_text(tx, y + 54, nm, 14, 1, RGB(255, 255, 255));
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


static void scroll_top_window(int dir)
{
    int t = top_slot();
    if (t < 0) return;
    if (wins[t].app == 0) fm_scroll_by(&wins[t], dir * (fm_vis(&wins[t]) - 1));
    else if (wins[t].app == 3) { mp.scroll = clampi(mp.scroll + dir * 5, 0, mp.n > 6 ? mp.n - 6 : 0); }
}

static void power_action(int a)
{
    if (a == 0) {                       // Restart: leave to the console browser
        if (audsrv_ok) audsrv_quit();
        Exit(0);
    } else if (a == 1) {                // Sleep: black screen until a button is pressed
        sleeping = 1;
        if (mp.state == 1) mp.state = 2;
    } else {                            // Power off
        if (audsrv_ok) audsrv_stop_audio();
        poweroffShutdown();
    }
}

/* ---------- main ---------- */
int main(void)
{
    PS2MouseData md;
    int mx = 320, my = 224, frame = 0, configured = 0, i;
    u32 en = 0, buttons = 0, prev = 0;
    int drag = -1, dox = 0, doy = 0;
    int sel_icon = -1, last_icon = -1, last_click = -1000;
    int icon_drag = -1, icon_dox = 0, icon_doy = 0, icon_moved = 0, icon_sx = 0, icon_sy = 0;

    load_modules();
    init_gs();
    for (i = 0; i < NICONS; i++) { icon_px[i] = 24; icon_py[i] = 12 + i * 80; }
    PS2MouseInit();
    padInit(0);
    padPortOpen(0, 0, pad_buf);
    audsrv_ok = (audsrv_init() == 0);
    poweroffInit();
    mp_init();

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
        mouse_en_g = en;
        if (input_mode == 0) {
            int dx = 0, dy = 0;
            unsigned short pb2 = 0;
            if (pad_poll(&dx, &dy, &pb2)) {
                mx += dx; my += dy;
                if (mx < 0) mx = 0;
                if (my < 0) my = 0;
                if (mx > W - 1) mx = W - 1;
                if (my > H - 1) my = H - 1;
                buttons = ((pb2 & PAD_CROSS) ? 1 : 0) | ((pb2 & PAD_CIRCLE) ? 2 : 0);
                if ((pb2 & PAD_R1) && !(pad_prev & PAD_R1)) scroll_top_window(1);
                if ((pb2 & PAD_L1) && !(pad_prev & PAD_L1)) scroll_top_window(-1);
                pad_last = pb2;
                if ((pb2 & PAD_START) && !(pad_prev & PAD_START)) menu_open = !menu_open;
                pad_prev = pb2;
            } else { buttons = 0; pad_last = 0; }
        }
        if (input_mode == 1 && configured && PS2MouseRead(&md) >= 0) {
            mx = md.x; my = md.y; buttons = md.buttons;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > W - 1) mx = W - 1;
            if (my > H - 1) my = H - 1;
        }

        if (sleeping) {
            if (pad_last || buttons) { sleeping = 0; prev = buttons | 1; pad_prev = pad_last; }
            else {
                rect(0, 0, W, H, RGB(0, 0, 0));
                gsKit_queue_exec(gs);
                gsKit_sync_flip(gs);
                frame++;
                continue;
            }
        }
        int pressed = (buttons & 1) && !(prev & 1);
        int released = !(buttons & 1) && (prev & 1);

        if (pressed) {
            int handled = 0;
            if (menu_open) {
                int mh = menu_h(), myp = H - TASKBAR_H - mh;
                if (mx >= 0 && mx < MENU_W && my >= myp && my < myp + mh) {
                    int it = menu_item_at(my, myp);
                    menu_open = 0; handled = 1;
                    if (it >= 0 && it < NAPPS) open_app(it);
                    else if (it >= NAPPS) power_action(it - NAPPS);
                } else if (mx < 90 && my >= H - TASKBAR_H) { menu_open = 0; handled = 1; }
                else menu_open = 0;
            } else if (mx < 90 && my >= H - TASKBAR_H) { menu_open = 1; handled = 1; }
            if (!handled && my >= H - TASKBAR_H) {
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
                    else if (w->app == 4) settings_click(w, mx, my);
                    else if (w->app == 1) ms_click(w, mx, my, 0);
                    else if (w->app == 2) sl_click(w, mx, my, 0);
                    else if (w->app == 3) mp_click(w, mx, my, frame);
                    handled = 1;
                }
            }
            if (!handled) {
                int hit = -1;
                for (i = 0; i < NICONS; i++)
                    if (mx >= icon_x(i) - 16 && mx < icon_x(i) + 64 && my >= icon_y(i) - 4 && my < icon_y(i) + 68) hit = i;
                if (hit >= 0 && hit == last_icon && frame - last_click < 30) { if (hit < 4) open_app(hit); else open_recycle(); last_icon = -1; }
                else { last_icon = hit; last_click = frame; }
                sel_icon = hit;
                if (hit >= 0 && last_icon == hit) { icon_drag = hit; icon_dox = mx - icon_px[hit]; icon_doy = my - icon_py[hit]; icon_moved = 0; icon_sx = mx; icon_sy = my; }
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
        if ((buttons & 2) && !(prev & 2)) {   // right click / Circle: flag in Minesweeper
            for (i = nopen - 1; i >= 0; i--) {
                Win *w = &wins[order[i]];
                if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
                    if (w->app == 1 && my >= w->y + TITLE_H) ms_click(w, mx, my, 1);
                    if (w->app == 2 && my >= w->y + TITLE_H) sl_click(w, mx, my, 1);
                    break;
                }
            }
        }
        if (icon_drag >= 0 && (buttons & 1)) {
            if (!icon_moved && (abs(mx - icon_sx) > 4 || abs(my - icon_sy) > 4)) icon_moved = 1;
            if (icon_moved) {
                icon_px[icon_drag] = clampi(mx - icon_dox, 0, W - 80);
                icon_py[icon_drag] = clampi(my - icon_doy, 0, H - TASKBAR_H - 70);
            }
        }
        if (sb_drag) {
            if ((buttons & 1) && wins[sb_slot].open && wins[sb_slot].app == 0) fm_sb_set(&wins[sb_slot], my);
            else sb_drag = 0;
        }
        if (released) {
            drag = -1;
            if (icon_drag >= 0) { if (icon_moved) { last_icon = -1; ini_save(); } icon_drag = -1; }
            sb_drag = 0;
        }
        prev = buttons;

        mp_pump();
        mp_viz_update();

        // ----- draw -----
        g_frame = frame;
        if (frame == 300 || frame == 900) wp_try_cfg();
        if ((frame == 300 || frame == 900) && !ini_loaded) ini_load();
        if (wp_ready) {
            gsKit_prim_sprite_texture(gs, &wp_tex, 0, 0, 0, 0, W, H - TASKBAR_H, WP_W, WP_H, 1, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
        } else {
            vgrad(0, 0, W, H - TASKBAR_H, 60, 110, 200, 160, 200, 250);
        }
        for (i = 0; i < NICONS; i++) draw_icon(i, i == sel_icon);
        for (i = 0; i < nopen; i++) draw_window(order[i], i == nopen - 1);
        draw_taskbar(top_slot());
        if (menu_open) draw_menu(mx, my);
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
