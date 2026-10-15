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
#define NAPPS 5      // apps in Start menu (incl. Settings)
#define NICONS 4     // apps with desktop icons
#define MAXW 8

static GSGLOBAL *gs;

static const char *app_names[NAPPS] = {"My Computer", "Minesweeper", "Solitaire", "Media Player", "Settings"};

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

/* ---------- input (controller / mouse) + Settings app + Start menu ---------- */
static int input_mode = 0;       // 0 = controller (default at every startup), 1 = USB mouse
static int pad_ok = 0, pad_mode_set = 0;
static u32 mouse_en_g = 0;
static unsigned short pad_prev = 0;
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
        if (abs(hx) > 24) sx = hx / 12;
        if (abs(vy) > 24) sy = vy / 12;
    }
    if (*btns & PAD_LEFT)  sx -= 4;
    if (*btns & PAD_RIGHT) sx += 4;
    if (*btns & PAD_UP)    sy -= 4;
    if (*btns & PAD_DOWN)  sy += 4;
    *dx = sx; *dy = sy;
    return 1;
}

static void draw_settings(Win *w)
{
    int x = w->x + 3, y = w->y + TITLE_H, i;
    draw_text(x + 16, y + 14, "INPUT DEVICE", 20, 2, RGB(30, 30, 30));
    for (i = 0; i < 2; i++) {
        int ry = y + 50 + i * 30;
        rect(x + 16, ry, x + 30, ry + 14, RGB(90, 90, 90));
        rect(x + 17, ry + 1, x + 29, ry + 13, RGB(255, 255, 255));
        if (input_mode == i) rect(x + 20, ry + 4, x + 26, ry + 10, RGB(0, 80, 220));
        draw_text(x + 40, ry + 4, i == 0 ? "CONTROLLER (DEFAULT)" : "USB MOUSE", 24, 1, RGB(20, 20, 20));
    }
    draw_text(x + 16, y + 116, pad_ok ? "CONTROLLER: DETECTED" : "CONTROLLER: NOT FOUND", 24, 1, pad_ok ? RGB(0, 120, 0) : RGB(200, 30, 30));
    draw_text(x + 16, y + 132, mouse_en_g ? "MOUSE: DETECTED" : "MOUSE: NOT FOUND", 24, 1, mouse_en_g ? RGB(0, 120, 0) : RGB(200, 30, 30));
    draw_text(x + 16, y + 154, "X / D-PAD / STICK = CURSOR. START = MENU", 44, 1, RGB(90, 90, 90));
}

static void settings_click(Win *w, int mx, int my)
{
    int x = w->x + 3, y = w->y + TITLE_H, i;
    for (i = 0; i < 2; i++) {
        int ry = y + 50 + i * 30;
        if (mx >= x + 16 && mx < x + 220 && my >= ry && my < ry + 16) {
            if (i == 1 && !mouse_en_g) { set_status("NO MOUSE DETECTED"); return; }
            input_mode = i;
            set_status(i == 0 ? "INPUT: CONTROLLER" : "INPUT: USB MOUSE");
        }
    }
}

#define MENU_W 180
#define MENU_ITEM_H 26
static int menu_h(void) { return NAPPS * MENU_ITEM_H + 6; }

static void draw_menu(int mx, int my)
{
    int H = gs->Height, i, x = 0, y = H - TASKBAR_H - menu_h();
    rect(x, y, x + MENU_W, y + menu_h(), RGB(20, 60, 170));
    rect(x + 2, y + 2, x + MENU_W - 2, y + menu_h() - 2, RGB(240, 240, 250));
    for (i = 0; i < NAPPS; i++) {
        int iy = y + 3 + i * MENU_ITEM_H;
        int hov = (mx >= x && mx < x + MENU_W && my >= iy && my < iy + MENU_ITEM_H);
        if (hov) rect(x + 3, iy, x + MENU_W - 3, iy + MENU_ITEM_H, RGB(50, 90, 200));
        draw_text(x + 14, iy + 8, app_names[i], 20, 1, hov ? RGB(255, 255, 255) : RGB(20, 20, 20));
    }
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
    wins[slot].w = (app == 0) ? 380 : (app == 1) ? 198 : 300;
    wins[slot].h = (app == 0) ? 290 : (app == 1) ? 250 : 200;
    wins[slot].x = 110 + (nopen % 6) * 28;
    wins[slot].y = 20 + (nopen % 6) * 28;
    order[nopen++] = slot;
    if (app == 0) fm_refresh();
    if (app == 1) ms_reset();
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
    padInit(0);
    padPortOpen(0, 0, pad_buf);

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
                if ((pb2 & PAD_START) && !(pad_prev & PAD_START)) menu_open = !menu_open;
                pad_prev = pb2;
            } else buttons = 0;
        }
        if (input_mode == 1 && configured && PS2MouseRead(&md) >= 0) {
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
            if (menu_open) {
                int mh = menu_h(), myp = H - TASKBAR_H - mh;
                if (mx >= 0 && mx < MENU_W && my >= myp && my < myp + mh) {
                    int it = (my - myp - 3) / MENU_ITEM_H;
                    if (it >= 0 && it < NAPPS) open_app(it);
                    menu_open = 0; handled = 1;
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
                    handled = 1;
                }
            }
            if (!handled) {
                int hit = -1;
                for (i = 0; i < NICONS; i++)
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
        if ((buttons & 2) && !(prev & 2)) {   // right click / Circle: flag in Minesweeper
            for (i = nopen - 1; i >= 0; i--) {
                Win *w = &wins[order[i]];
                if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
                    if (w->app == 1 && my >= w->y + TITLE_H) ms_click(w, mx, my, 1);
                    break;
                }
            }
        }
        if (released) drag = -1;
        prev = buttons;

        // ----- draw -----
        g_frame = frame;
        if (frame == 300 || frame == 900) wp_try_cfg();
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
