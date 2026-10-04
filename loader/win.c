#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <vita2d.h>
#include <arm_neon.h>

#include "loader.h"
#ifdef DIAGNOSTICS
#include "lib/vshot/vshot.h"
#endif

uint32_t trap_for_name(const char *name);
void *native_for(const char *name);
void guest_path_to_host(const char *guest, int writes, char *host, size_t cap);

typedef uint16_t wchar16;

#define WIN(X) X(RegisterClassW) X(CreateWindowExW) X(ShowWindow) X(SetForegroundWindow) \
    X(GetForegroundWindow) X(DestroyWindow) X(GetWindowLongW) X(SetWindowLongW) X(DefWindowProcW) \
    X(PeekMessageW) X(GetMessageW) X(DispatchMessageW) X(PostQuitMessage) X(BeginPaint) X(EndPaint) \
    X(GetDC) X(ReleaseDC) X(ExtEscape) X(ChangeDisplaySettingsEx) X(GetSystemMetrics) X(LoadCursorW) \
    X(CreateSolidBrush) X(DeleteObject) X(DeleteDC) X(SetCapture) X(ReleaseCapture) \
    X(LoadLibraryW) X(GetProcAddressW) X(FreeLibrary) X(SetKMode) X(SystemIdleTimerReset) X(SHFullScreen) \
    X(GXOpenInput) X(GXCloseInput) X(GXGetDefaultKeys) \
    X(GetVersionExW) X(GetSystemInfo) X(GlobalMemoryStatus) X(GetLocaleInfoW) X(SystemParametersInfoW) \
    X(_wcsicmp) X(GetLocalTime) X(GetTickCount) X(Sleep) X(CreateMutexW) X(ReleaseMutex) \
    X(WaitForSingleObject) X(CreateEventW) X(EventModify) X(InitializeCriticalSection) \
    X(EnterCriticalSection) X(LeaveCriticalSection) X(InterlockedCompareExchange) X(CreateThread) \
    X(ResumeThread) X(waveOutOpen) X(waveOutPrepareHeader) X(waveOutWrite) X(waveOutReset) \
    X(waveOutUnprepareHeader) X(waveOutClose) X(vsprintf) X(fopen) X(fprintf) X(fclose) X(OutputDebugStringW)

#define ENUM(n) W_##n,
enum { WIN(ENUM) N_WIN };
#define EXTERN(n) extern char ce_##n[];
WIN(EXTERN)
#define THUNK(n) ce_##n,
static void *const thunks[N_WIN] = {WIN(THUNK)};
#define NAME(n) #n,
static const char *const names[N_WIN] = {WIN(NAME)};
#ifdef DIAGNOSTICS
extern const char *volatile last_native;
#endif
#if defined(DIAGNOSTICS)
#define CALLED(n) (last_native = #n)
#else
#define CALLED(n) ((void)0)
#endif

static const struct { const char *import; int native; } aliases[] = {
    {"?GXOpenInput@@YAHXZ", W_GXOpenInput},
    {"?GXCloseInput@@YAHXZ", W_GXCloseInput},
    {"?GXGetDefaultKeys@@YA?AUGXKeyList@@H@Z", W_GXGetDefaultKeys},
};

void *win_native_for(const char *name)
{
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (strcmp(name, aliases[i].import) == 0)
            return thunks[aliases[i].native];
    for (unsigned i = 0; i < N_WIN; i++)
        if (strcmp(name, names[i]) == 0)
            return thunks[i];
    return NULL;
}

typedef void (*GuestCompose)(uint8_t *view, void *rect);
static GuestCompose view_compose;
static uint32_t menu_vtable;
static uint32_t main_menu_vtable;

#define VIEW_NEED_REDRAW(v) ((v)[0xa4])

static uint32_t main_view_vtable;
static const uint8_t *game_obj;
static const uint8_t *snd_player;
static int ss_valid;
static int mb_valid;
static uint32_t *cycle_delay;
static volatile int fps30;
static volatile unsigned fps_asked, scale_asked;

void win_set_cycle(uint32_t *d) { cycle_delay = d; }

static volatile unsigned main_read_vc;
static unsigned compose_vc;
static int compose_seen;
static volatile int painting;

static uint32_t battle_view_vtable;
static const int32_t *bv_origin;
static struct { int on, dir, tip; uint32_t cfg; float x, y; } ring;
static int rm_preview = -1;
static volatile unsigned ring_seq;
static uint8_t *ring_bv;
static volatile unsigned music_asked;
static const char *const music_keys[MUSIC_SETS] = {"none", "original", "homm2"};
static const char *const music_labels[MUSIC_SETS] = {"NO MUSIC", "ORIGINAL MUSIC", "HOMM2 MUSIC"};

static volatile int outlines_on;
static volatile int outline_redraw;
static volatile unsigned outline_asked;

enum { MELEE_ORIGINAL, MELEE_RING_TAP, MELEE_RING_DRAG, MELEE_MODES };
static const char *const melee_keys[MELEE_MODES] = {"original", "ring_tap", "ring_drag"};
static const char *const melee_labels[MELEE_MODES] = {"ORIGINAL", "RING TAP", "RING DRAG"};
static volatile int melee_mode = MELEE_RING_TAP;
static volatile unsigned melee_asked;
static uint32_t ring_gfx, ring_blit, ring_surface;
static uint32_t ring_unit_type, ring_melee_entry;
static void ring_sprites_take(void);
static volatile int ring_open;

static void ring_take(const uint8_t *cur)
{
    int bv = cur && battle_view_vtable && *(const uint32_t *)cur == battle_view_vtable;
    ring_bv = bv ? (uint8_t *)cur : NULL;
    const uint8_t *e = bv && cur[0x121] && !cur[0x164] && *(const uint32_t *)(cur + 0xd8) == 0
                           ? *(const uint8_t *const *)(cur + 0x170)
                           : NULL;
    int on = e != NULL && melee_mode != MELEE_ORIGINAL, dir = on ? *(const int32_t *)(cur + 0x174) : -1;
    int tip = on && *(const uint32_t *)(cur + 0x12c) != 0;
    uint32_t cfg = on ? *(const uint16_t *)e : 0;
    float x = 0, y = 0;
    if (on) {
        int32_t cx = *(const int32_t *)(e + 4), cy = *(const int32_t *)(e + 8);
        x = (float)(cx * 24 - (cy & 1) * 13 + bv_origin[0] + 10);
        y = (float)(cy * 17 + bv_origin[1] + 9);
    }
    if (on)
        ring_sprites_take();
    if (on != ring.on || dir != ring.dir || tip != ring.tip || cfg != ring.cfg || x != ring.x || y != ring.y) {
        ring.on = on, ring.dir = dir, ring.tip = tip, ring.cfg = cfg, ring.x = x, ring.y = y;
        ring_seq++;
    }
}

static void view_mgr_compose(uint8_t *mgr, void *rect)
{
    uint8_t *cur = *(uint8_t **)(mgr + 0x08);
    if (rm_preview >= 0 && cur && battle_view_vtable && *(uint32_t *)cur == battle_view_vtable &&
        *(int32_t *)(cur + 0x174) == rm_preview) {
        *(int32_t *)(cur + 0x174) = -1;
        rm_preview = -1;
    }
    uint8_t **dlg = *(uint8_t ***)(mgr + 0x1c), **dlg_end = *(uint8_t ***)(mgr + 0x24);
    if (outline_redraw && cur && *(uint32_t *)cur == main_view_vtable) {
        VIEW_NEED_REDRAW(cur) = 1;
        outline_redraw = 0;
    }
    int behind = cur && VIEW_NEED_REDRAW(cur);
    int survey = cur && *(uint32_t *)cur == main_view_vtable && cur[0x18f];
    if (!survey)
        ss_valid = 0;
    if (!cur || *(uint32_t *)cur != main_view_vtable || survey)
        mb_valid = 0;
    if (cycle_delay)
        *cycle_delay = fps30 ? 30 : 15;
    compose_vc = main_read_vc;
    compose_seen = 1;
    painting = 1;
    int frozen = 0;
    if (cur && *(uint32_t *)cur == menu_vtable)
        for (uint8_t **d = dlg; d != dlg_end && !frozen; d++)
            frozen = *(uint32_t *)*d != main_menu_vtable;
    static unsigned last_ndlg;
    static uint8_t *last_popup;
    unsigned ndlg = (unsigned)(dlg_end - dlg);
    uint8_t *popup = *(uint8_t **)(mgr + 0x14);
    int restack = ndlg != last_ndlg || popup != last_popup;
    if (ndlg < last_ndlg)
        music_dialog_closed();
    last_ndlg = ndlg;
    last_popup = popup;
    int flagged = restack || (behind && !frozen);
    for (uint8_t **d = dlg; d != dlg_end && !flagged; d++)
        flagged = VIEW_NEED_REDRAW(*d);
    if (flagged) {
        if (cur && (behind || restack))
            view_compose(cur, rect);
        for (unsigned i = 0; i < (unsigned)(*(uint8_t ***)(mgr + 0x24) - *(uint8_t ***)(mgr + 0x1c)); i++)
            view_compose((*(uint8_t ***)(mgr + 0x1c))[i], rect);
    }
    if (popup)
        view_compose(popup, rect);
    uint8_t *drag = *(uint8_t **)(mgr + 0x10);
    if (drag)
        (*(void (**)(uint8_t *))*(uint32_t **)drag)(drag);
    ring_take(cur);
    static int in_battle;
    static unsigned battle_wait;
    static uint64_t battle_by;
    if (game_obj && (*(const int32_t *)(game_obj + 0x4fc) == 2) != in_battle) {
        in_battle = !in_battle;
        battle_wait = 0;
        if (in_battle) {
            music_stop();
            const uint8_t *ch = snd_player ? *(const uint8_t *const *)(snd_player + 0x14) : NULL;
            for (int i = 0; ch && i < 8; i++)
                battle_wait |= (*(const uint32_t *)(ch + i * 0x24 + 0x14) != 0) << i;
            battle_by = sceKernelGetProcessTimeWide() + 4000000;
            battle_wait |= 1u << 31;
        }
    }
    if (battle_wait) {
        const uint8_t *ch = snd_player ? *(const uint8_t *const *)(snd_player + 0x14) : NULL;
        unsigned busy = 0;
        for (int i = 0; ch && i < 8; i++)
            busy |= (*(const uint32_t *)(ch + i * 0x24 + 0x14) != 0) << i;
        if (!(busy & battle_wait & 0xff) || sceKernelGetProcessTimeWide() >= battle_by) {
            battle_wait = 0;
            music_battle();
        }
    }
    if (game_obj && *(const int32_t *)(game_obj + 0x4fc) == 3) {
        const uint8_t *cv = *(const uint8_t *const *)(game_obj + 0x4e0);
        const uint8_t *castle = cv ? *(const uint8_t *const *)(cv + 0xd0) : NULL;
        const uint8_t *proto = castle ? *(const uint8_t *const *)(castle + 0x78) : NULL;
        if (proto)
            music_town(castle, *(const int32_t *)(proto + 0x2c));
    }
}

int win_patch_compose(uint32_t image)
{
    uint32_t *entry = (uint32_t *)(uintptr_t)(image + 0xe2c74 - 0x10000);
    if (entry[0] != 0xE92D4070u)
        return 0;
    view_compose = (GuestCompose)(uintptr_t)(image + 0xe1bb8 - 0x10000);
    menu_vtable = image + 0x1322cc - 0x10000;
    main_menu_vtable = image + 0x132354 - 0x10000;
    main_view_vtable = image + 0x13005c - 0x10000;
#define AT(va) (*(const uint32_t *)(uintptr_t)(image + (va) - 0x10000))
    if (AT(0x12344) == 0xE59534FCu && AT(0x12664) == 0xE58504E0u && AT(0x96ab8) == 0xE58480D0u &&
        AT(0x97cb0) == 0xE5902078u && AT(0x97cb8) == 0xE592302Cu)
        game_obj = (const uint8_t *)(uintptr_t)(image + 0x221780 - 0x10000);
    if (AT(0x36888) == 0xE2810F81u && AT(0x368c0) == 0x002214FCu - 0x10000 + image &&
        AT(0xda604) == 0xE597E014u && AT(0xda60c) == 0xE59E3014u && AT(0xda620) == 0xE28EE024u)
        snd_player = (const uint8_t *)(uintptr_t)(image + 0x221700 - 0x10000);
#undef AT
    if (*(uint32_t *)(uintptr_t)(image + 0x132154 - 0x10000) == image + 0xbad3c - 0x10000) {
        battle_view_vtable = image + 0x13214c - 0x10000;
        bv_origin = (const int32_t *)(uintptr_t)(image + 0x20ab8c - 0x10000);
        ring_gfx = image + 0x221ca0 - 0x10000;
        ring_blit = image + 0x2c4dc - 0x10000;
        ring_surface = image + 0x221698 - 0x10000;
        if (*(uint32_t *)(uintptr_t)(image + 0xb98c0 - 0x10000) == (0xEB000000u | (((0xb55f4 - (0xb98c0 + 8)) >> 2) & 0xFFFFFFu))) {
            ring_unit_type = *(uint32_t *)(uintptr_t)(image + 0xb9bc4 - 0x10000);
            ring_melee_entry = image + 0xb55f4 - 0x10000;
        }
    }
    entry[0] = 0xE51FF004u;
    entry[1] = (uint32_t)(uintptr_t)view_mgr_compose;
    return 1;
}

#define DIB_PIX(d) (*(uint16_t **)((d) + 0x04))
#define DIB_W(d) (*(uint32_t *)((d) + 0x0c))
#define DIB_H(d) (*(uint32_t *)((d) + 0x10))

static void pyramid_ref(const uint16_t *src, uint32_t sw, uint16_t *dst, uint32_t ds, uint32_t dw, uint32_t dh,
                        uint32_t b, uint32_t x0)
{
    uint32_t n = 1u << b, s = b << 1;
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = x0; x < dw; x++) {
            const uint16_t *p = src + (y << b) * sw + (x << b);
            uint32_t r = 0, g = 0, bl = 0;
            for (uint32_t cy = 0; cy < n; cy++)
                for (uint32_t cx = 0; cx < n; cx++) {
                    uint32_t v = p[cy * sw + cx];
                    r += v >> 11, g += (v >> 5) & 0x3f, bl += v & 0x1f;
                }
            dst[y * ds + x] = (uint16_t)((((r >> s) << 6 | g >> s) << 5) | bl >> s);
        }
}

static void pyramid_2x2(const uint16_t *src, uint32_t sw, uint16_t *dst, uint32_t ds, uint32_t dw, uint32_t dh)
{
    const uint16x8_t m6 = vdupq_n_u16(0x3f), m5 = vdupq_n_u16(0x1f);
    uint32_t w8 = dw & ~7u;
    for (uint32_t y = 0; y < dh; y++) {
        const uint16_t *s0 = src + 2 * y * sw, *s1 = s0 + sw;
        uint16_t *d = dst + y * ds;
        for (uint32_t x = 0; x < w8; x += 8) {
            uint16x8x2_t a = vld2q_u16(s0 + 2 * x), c = vld2q_u16(s1 + 2 * x);
            uint16x8_t r = vaddq_u16(vaddq_u16(vshrq_n_u16(a.val[0], 11), vshrq_n_u16(a.val[1], 11)),
                                     vaddq_u16(vshrq_n_u16(c.val[0], 11), vshrq_n_u16(c.val[1], 11)));
            uint16x8_t g = vaddq_u16(vaddq_u16(vandq_u16(vshrq_n_u16(a.val[0], 5), m6), vandq_u16(vshrq_n_u16(a.val[1], 5), m6)),
                                     vaddq_u16(vandq_u16(vshrq_n_u16(c.val[0], 5), m6), vandq_u16(vshrq_n_u16(c.val[1], 5), m6)));
            uint16x8_t bl = vaddq_u16(vaddq_u16(vandq_u16(a.val[0], m5), vandq_u16(a.val[1], m5)),
                                      vaddq_u16(vandq_u16(c.val[0], m5), vandq_u16(c.val[1], m5)));
            uint16x8_t o = vshrq_n_u16(bl, 2);
            o = vsliq_n_u16(o, vshrq_n_u16(g, 2), 5);
            o = vsliq_n_u16(o, vshrq_n_u16(r, 2), 11);
            vst1q_u16(d + x, o);
        }
    }
    if (w8 < dw)
        pyramid_ref(src, sw, dst, ds, dw, dh, 1, w8);
}

void pyramidal_stretch(uint8_t *src, uint8_t *dst, uint32_t b)
{
    const uint16_t *s = DIB_PIX(src);
    uint16_t *d = DIB_PIX(dst);
    uint32_t sw = DIB_W(src), dw = DIB_W(dst), dh = DIB_H(dst);
    if (b == 1)
        pyramid_2x2(s, sw, d, dw, dw, dh);
    else
        pyramid_ref(s, sw, d, dw, dw, dh, b, 0);
}

void pyramid_thunk(void);

int win_patch_pyramid(uint32_t image)
{
    uint32_t *entry = (uint32_t *)(uintptr_t)(image + 0xdbe28 - 0x10000);
    if (entry[0] != 0xE92D4FF0u || entry[1] != 0xE24DD018u)
        return 0;
    entry[0] = 0xE51FF004u;
    entry[1] = (uint32_t)(uintptr_t)pyramid_thunk;
    return 1;
}

void fog_grid_thunk(void);

int win_patch_fog(uint32_t image, uint32_t block)
{
    uint32_t *ins = (uint32_t *)(uintptr_t)(image + 0x84744 - 0x10000);
    if (ins[0] != 0xE51BE144u || ins[2] != 0xE51B0120u || ins[18] != 0xE51B311Cu)
        return 0;
    uint32_t *v = (uint32_t *)(uintptr_t)(image + block - 32);
    v[0] = 0xE51FF004u;
    v[1] = (uint32_t)(uintptr_t)fog_grid_thunk;
    ins[0] = 0xEB000000u | ((((uint32_t)(uintptr_t)v) - ((uint32_t)(uintptr_t)ins + 8)) >> 2 & 0xFFFFFFu);
    return 1;
}

#define SS_RIGHT 52
typedef uint32_t (*Guest4c)(uint32_t, uint32_t, uint32_t, uint32_t);
static Guest4c ss_stock;
static uint16_t *ss_cache, *ss_band;
static uint8_t *ss_dib;
static uint32_t ss_w, ss_h;
static int32_t ss_ax, ss_ay;

#define AV_SLACK 128
uint32_t av_xx1, av_ret_loop, av_ret_skip;
static int32_t av_bw, av_bh;

static int32_t fdiv(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

uint32_t av_row_range(int32_t row, int32_t ys, int32_t xs, int32_t w)
{
    (void)row;
    av_xx1 = (uint32_t)w;
    if (!av_bw || w <= 0)
        return 0;
    int32_t lo = 0, hi = w, a, b;
    a = fdiv(-400 - AV_SLACK - xs, 20), b = fdiv(av_bw + 52 + AV_SLACK - xs, 20) + 1;
    lo = a > lo ? a : lo, hi = b < hi ? b : hi;
    a = fdiv(-240 - AV_SLACK - ys, 10), b = fdiv(av_bh + 88 + AV_SLACK - ys, 10) + 1;
    lo = a > lo ? a : lo, hi = b < hi ? b : hi;
    av_xx1 = (uint32_t)(hi > lo ? hi : lo);
    return (uint32_t)lo;
}

static uint32_t ss_compose_band(uint8_t *dib, const int32_t *a, uint32_t puz, uint32_t tgt, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h)
{
    uint8_t band[0x20];
    uint32_t bw = w + SS_RIGHT;
    memcpy(band, dib, sizeof(band));
    *(uint16_t **)(band + 4) = ss_band;
    *(uint32_t *)(band + 0xc) = bw;
    *(uint32_t *)(band + 0x10) = h;
    memset(ss_band, 0, bw * h * 2);
    int32_t ba[2] = {a[0] + (int32_t)x, a[1] + (int32_t)y};
    av_bw = (int32_t)bw, av_bh = (int32_t)h;
    ss_stock((uint32_t)(uintptr_t)band, (uint32_t)(uintptr_t)ba, puz, tgt);
    av_bw = 0;
    return bw;
}

static void ss_compose_rect(uint8_t *dib, const int32_t *a, uint32_t puz, uint32_t tgt, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h)
{
    uint32_t bw = ss_compose_band(dib, a, puz, tgt, x, y, w, h);
    uint16_t *pix = DIB_PIX(dib);
    uint32_t stride = DIB_W(dib);
    for (uint32_t r = 0; r < h; r++)
        memcpy(pix + (y + r) * stride + x, ss_band + r * bw, w * 2);
}

static uint32_t *ss_settings;
static int ss_half;

static void ss_shrink(const uint16_t *src, uint32_t sw, uint16_t *dst, uint32_t ds, uint32_t dw, uint32_t dh, int filter)
{
    if (filter) {
        pyramid_2x2(src, sw, dst, ds, dw, dh);
        return;
    }
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++)
            dst[y * ds + x] = src[2 * y * sw + 2 * x];
}

static void ss_compose_half(uint8_t *dib, uint8_t *small, const int32_t *a, uint32_t tgt, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h, int filter)
{
    uint32_t bw = ss_compose_band(dib, a, 0, tgt, x, y, w, h), ds = DIB_W(small);
    ss_shrink(ss_band, bw, DIB_PIX(small) + (y / 2) * ds + x / 2, ds, w / 2, h / 2, filter);
}

static uint32_t survey_half(uint8_t *dib, uint8_t *small, uint32_t tgt, int banded, int32_t dx, int32_t dy)
{
    uint32_t w = DIB_W(dib), h = DIB_H(dib), sw = DIB_W(small), sh = DIB_H(small);
    uint16_t *sp = DIB_PIX(small);
    int filter = ss_settings[11] != 0;
    int32_t a[2] = {ss_ax, ss_ay};
    if (!banded) {
        ss_compose_half(dib, small, a, tgt, 0, 0, w, h, filter);
        return 1;
    }
    int32_t hx = dx / 2, hy = dy / 2;
    uint32_t y0 = hy < 0 ? (uint32_t)-hy : 0, y1 = hy > 0 ? sh - (uint32_t)hy : sh;
    uint32_t x0 = hx < 0 ? (uint32_t)-hx : 0, x1 = hx > 0 ? sw - (uint32_t)hx : sw;
    if (hy > 0)
        for (uint32_t y = y0; y < y1; y++)
            memmove(sp + y * sw + x0, sp + (y + hy) * sw + x0 + hx, (x1 - x0) * 2);
    else
        for (uint32_t y = y1; y-- > y0;)
            memmove(sp + y * sw + x0, sp + (y + hy) * sw + x0 + hx, (x1 - x0) * 2);
    if (y0)
        ss_compose_half(dib, small, a, tgt, 0, 0, w, 2 * y0, filter);
    if (y1 < sh)
        ss_compose_half(dib, small, a, tgt, 0, 2 * y1, w, h - 2 * y1, filter);
    if (x0)
        ss_compose_half(dib, small, a, tgt, 0, 2 * y0, 2 * x0, 2 * (y1 - y0), filter);
    if (x1 < sw)
        ss_compose_half(dib, small, a, tgt, 2 * x1, 2 * y0, w - 2 * x1, 2 * (y1 - y0), filter);
    return 1;
}

uint32_t survey_strips(uint32_t dib_, uint32_t anchor_, uint32_t puz, uint32_t tgt)
{
    uint8_t *dib = (uint8_t *)(uintptr_t)dib_, *small = dib - 0x14;
    const int32_t *a = (const int32_t *)(uintptr_t)anchor_;
    uint32_t w = DIB_W(dib), h = DIB_H(dib);
    uint16_t *pix = DIB_PIX(dib);
    if (!ss_cache || w * h > ss_w * ss_h) {
        free(ss_cache);
        free(ss_band);
        ss_cache = malloc(w * h * 2);
        ss_band = malloc((w + SS_RIGHT) * h * 2);
        ss_valid = 0;
    }
    int half = ss_settings && !puz && DIB_PIX(small) && DIB_W(small) * 2 == w && DIB_H(small) * 2 == h;
    int32_t ax = half ? a[0] & ~1 : a[0], ay = half ? a[1] & ~1 : a[1];
    int32_t dx = ax - ss_ax, dy = ay - ss_ay;
    int banded = ss_valid && half == ss_half && dib == ss_dib && w == ss_w && h == ss_h && !puz &&
                 (uint32_t)abs(dx) < w / 2 && (uint32_t)abs(dy) < h / 2;
    ss_dib = dib, ss_w = w, ss_h = h, ss_ax = ax, ss_ay = ay, ss_half = half;
    if (!ss_cache || !ss_band || puz) {
        memset(pix, 0, w * h * 2);
        return ss_valid = 0, ss_stock(dib_, anchor_, puz, tgt);
    }
    if (half) {
        ss_valid = 1;
        return survey_half(dib, small, tgt, banded, dx, dy);
    }
    if (!banded) {
        ss_compose_rect(dib, a, puz, tgt, 0, 0, w, h);
        memcpy(ss_cache, pix, w * h * 2);
        ss_valid = 1;
        return 0;
    }
    uint32_t y0 = dy < 0 ? (uint32_t)-dy : 0, y1 = dy > 0 ? h - (uint32_t)dy : h;
    uint32_t x0 = dx < 0 ? (uint32_t)-dx : 0, x1 = dx > 0 ? w - (uint32_t)dx : w;
    for (uint32_t y = y0; y < y1; y++)
        memcpy(pix + y * w + x0, ss_cache + (y + dy) * w + x0 + dx, (x1 - x0) * 2);
    if (y0)
        ss_compose_rect(dib, a, puz, tgt, 0, 0, w, y0);
    if (y1 < h)
        ss_compose_rect(dib, a, puz, tgt, 0, y1, w, h - y1);
    if (x0)
        ss_compose_rect(dib, a, puz, tgt, 0, y0, x0, y1 - y0);
    if (x1 < w)
        ss_compose_rect(dib, a, puz, tgt, x1, y0, w - x1, y1 - y0);
    memcpy(ss_cache, pix, w * h * 2);
    return 0;
}

void survey_strips_thunk(void);

int win_patch_strips(uint32_t image, uint32_t block)
{
    uint32_t *sh = (uint32_t *)(uintptr_t)(image + 0x2dca0 - 0x10000);
    if (sh[0] != 0xE0412002u || sh[1] != 0xE043300Bu || sh[3] != 0xE3A05000u || sh[-3] != 0xE1A04003u)
        return 0;
    sh[0] = 0xE0412006u;
    sh[1] = 0xE59D3024u;
    sh[3] = 0xE0443003u;
    uint32_t *oc = (uint32_t *)(uintptr_t)(image + 0x89f18 - 0x10000);
    if (oc[0] == 0xE0120194u && oc[1] == 0x0A000006u && oc[8] == 0x1AFFFFFCu && oc[14] == 0xE595302Cu &&
        oc[15] == 0xE3530000u && oc[16] == 0x0A000004u && oc[17] == 0xE2881E19u && oc[49] == 0xE28D2014u &&
        oc[-25] == 0xE59F553Cu) {
        oc[0] = 0xEA000007u;
        oc[1] = 0xE3500000u;
        oc[2] = 0x1A00002Du;
        oc[3] = 0xE595302Cu;
        oc[4] = 0xE3530000u;
        oc[5] = 0x0A00000Fu;
        oc[6] = 0xEA000009u;
        oc[14] = 0xEAFFFFF1u;
        ss_settings = *(uint32_t **)(uintptr_t)(image + 0x8a3f8 - 0x10000);
    }
    uint32_t at = image + 0x89f4c - 0x10000, *ins = (uint32_t *)(uintptr_t)at;
    if (*ins != (0xEB000000u | (((0x84f7c - (0x89f4c + 8)) >> 2) & 0xFFFFFFu)))
        return 0;
    ss_stock = (Guest4c)(uintptr_t)(image + 0x84f7c - 0x10000);
    uint32_t *v = (uint32_t *)(uintptr_t)(image + block - 24);
    v[0] = 0xE51FF004u;
    v[1] = (uint32_t)(uintptr_t)survey_strips_thunk;
    *ins = 0xEB000000u | ((((uint32_t)(uintptr_t)v) - (at + 8)) >> 2 & 0xFFFFFFu);
    return 1;
}

void av_row_thunk(void);
void av_cmp_thunk(void);

int win_patch_walk(uint32_t image, uint32_t block)
{
    uint32_t *row = (uint32_t *)(uintptr_t)(image + 0x8543c - 0x10000);
    uint32_t *col = (uint32_t *)(uintptr_t)(image + 0x857f0 - 0x10000);
    if (row[0] != 0xE3A03000u || row[1] != 0xE58D301Cu || row[2] != 0xE35A0000u || row[8] != 0xE59722A0u ||
        col[0] != 0xE153000Au || col[1] != 0xBAFFFF18u)
        return 0;
    av_ret_loop = image + 0x8545c - 0x10000;
    av_ret_skip = image + 0x85808 - 0x10000;
    uint32_t *v = (uint32_t *)(uintptr_t)(image + block - 16);
    v[0] = 0xE51FF004u;
    v[1] = (uint32_t)(uintptr_t)av_row_thunk;
    v[2] = 0xE51FF004u;
    v[3] = (uint32_t)(uintptr_t)av_cmp_thunk;
    row[0] = 0xEA000000u | ((((uint32_t)(uintptr_t)v) - ((uint32_t)(uintptr_t)row + 8)) >> 2 & 0xFFFFFFu);
    col[0] = 0xEB000000u | ((((uint32_t)(uintptr_t)(v + 2)) - ((uint32_t)(uintptr_t)col + 8)) >> 2 & 0xFFFFFFu);
    return 1;
}

static void to_ascii(const wchar16 *w, char *out, size_t cap)
{
    size_t o = 0;
    for (; w && *w && o + 1 < cap; w++)
        out[o++] = *w < 0x80 ? (char)*w : '?';
    out[o] = 0;
}

static void put_w(wchar16 *out, uint32_t cap, const char *s)
{
    uint32_t i = 0;
    for (; s[i] && i + 1 < cap; i++)
        out[i] = (uint8_t)s[i];
    if (cap)
        out[i] = 0;
}

#define FB_W 240
#define FB_H 320
#define VITA_W 960
#define VITA_H 544
#define UP_W FB_H
#define UP_H FB_W
#define DISP_W (UP_W * VITA_H / UP_H)
#define DISP_X ((VITA_W - DISP_W) / 2)
enum { MEMO_N = 11 };
static const uint32_t memo_fn[MEMO_N] = {
    0x2c4dc,
    0x2d6cc,
    0x2e888,
    0x2aa38,
    0x2ad40,
    0xdadc4,
    0xdafa0,
    0xdb4c8,
    0xdb5a4,
    0x2d4e0,
    0xdb858,
};
uint32_t memo_rec;
uint32_t memo_tramp[MEMO_N];
extern const char memo_stub_0[], memo_stub_1[], memo_stub_2[], memo_stub_3[], memo_stub_4[], memo_stub_5[],
    memo_stub_6[], memo_stub_7[], memo_stub_8[], memo_stub_9[], memo_stub_10[];
static const char *const memo_stubs[MEMO_N] = {memo_stub_0, memo_stub_1, memo_stub_2, memo_stub_3,
                                               memo_stub_4, memo_stub_5, memo_stub_6, memo_stub_7,
                                               memo_stub_8, memo_stub_9, memo_stub_10};
static uint32_t memo_h;
static const uint8_t *memo_font;
static uint32_t memo_oncompose[2];
static uint8_t *memo_surface;
static void (*memo_scr_rect)(uint8_t *view, int32_t *out);
static struct { uint32_t h; int32_t r[4]; int valid; uint16_t pix[320 * 21]; } memo[2];

static void mh(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    for (uint32_t i = 0; i < n; i++)
        memo_h = (memo_h ^ b[i]) * 16777619u;
}
static void mw(uint32_t w) { mh(&w, 4); }
static void mtext(const uint32_t *s)
{
    uint32_t len = s[2] < 4096 ? s[2] : 4096;
    mw(len);
    mh((const void *)(uintptr_t)s[0], len * 2);
}

static uint32_t map_prim(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t k, const uint32_t *sp);

uint32_t memo_prim(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t k, const uint32_t *sp)
{
    if (memo_rec != 1)
        return map_prim(r0, r1, r2, r3, k, sp);
    const void *p1 = (const void *)(uintptr_t)r1, *p2 = (const void *)(uintptr_t)r2, *p3 = (const void *)(uintptr_t)r3;
    mw(k);
    switch (k) {
    case 0: mw(r1), mw(r2), mw(r3), mw(sp[0]); break;
    case 1: mw(r1), mw(r2), mw(r3), mw(sp[0]), mw(sp[1] & 0xff); break;
    case 2: mw(r1), mw(r2), mh(p3, 16); break;
    case 3:
        mh(p1, 28), mw(r2), mh(p3, 8), mtext((const uint32_t *)(uintptr_t)sp[0]);
        mh((const void *)(uintptr_t)sp[1], 16), mw(sp[2]), mh((const void *)(uintptr_t)sp[3], 8);
        break;
    case 4:
        mh(memo_font, 28), mw(r1), mh(p2, 8), mtext(p3);
        mh((const void *)(uintptr_t)sp[0], 16), mw(sp[1]), mh((const void *)(uintptr_t)sp[2], 8);
        break;
    case 5: mw(r0), mh(p1, 16), mw(r2 & 0xffff), mw(r3 & 0xff); break;
    case 6: mw(r0), mh(p1, 16); break;
    case 7:
    case 8: mw(r0), mh(p1, 8), mw(r2), mw(r3 & 0xffff), mw(sp[0] & 0xff); break;
    case 9: mw(r1), mw(r2), mw(r3), mw(sp[0]), mw(sp[1]); break;
    case 10: mw(r0), mw(r1), mh(p2, 8); break;
    }
    return 1;
}

static int memo_slot(const uint8_t *v)
{
    const uint32_t *vt = *(const uint32_t *const *)v;
    const uint8_t *parent = *(const uint8_t *const *)(v + 0xac);
    if (!parent || *(const uint32_t *)parent != main_view_vtable)
        return -1;
    return vt[2] == memo_oncompose[0] ? 0 : vt[2] == memo_oncompose[1] ? 1 : -1;
}

static void memo_copy(int32_t *r, uint16_t *pix, int out)
{
    uint16_t *s = DIB_PIX(memo_surface) + r[1] * DIB_W(memo_surface) + r[0];
    for (int32_t y = 0; y < r[3]; y++, s += DIB_W(memo_surface))
        if (out)
            memcpy(pix + y * r[2], s, r[2] * 2);
        else
            memcpy(s, pix + y * r[2], r[2] * 2);
}

void memo_child(uint8_t *v, int32_t *rect)
{
    int k = memo_slot(v), ok = k >= 0 && v[0x25] && DIB_PIX(memo_surface);
    int32_t r[4];
    if (ok) {
        memo_scr_rect(v, r);
        ok = r[0] >= 0 && r[1] >= 0 && r[2] > 0 && r[3] > 0 && (uint32_t)(r[0] + r[2]) <= DIB_W(memo_surface) &&
             (uint32_t)(r[1] + r[3]) <= DIB_H(memo_surface) && r[2] * r[3] <= 320 * 21;
    }
    if (!ok) {
        if (k >= 0)
            memo[k].valid = 0;
        view_compose(v, rect);
        return;
    }
    memo_h = 2166136261u;
    memo_rec = 1;
    view_compose(v, rect);
    memo_rec = 0;
    if (memo[k].valid && memo[k].h == memo_h && !memcmp(memo[k].r, r, sizeof(r))) {
        memo_copy(r, memo[k].pix, 0);
        return;
    }
    view_compose(v, rect);
    memo_copy(r, memo[k].pix, 1);
    memo[k].h = memo_h;
    memcpy(memo[k].r, r, sizeof(r));
    memo[k].valid = 1;
}

void memo_child_thunk(void);

int win_patch_bars(uint32_t image, uint32_t block)
{
    static const uint32_t first[MEMO_N][2] = {
        {0xE1A0C00Du, 0xE92D000Fu}, {0xE1A0C00Du, 0xE92D000Fu}, {0xE92D4FF0u, 0xE24DD038u},
        {0xE92D4FF0u, 0xE24DD078u}, {0xE92D4FF0u, 0xE24DD030u}, {0xE92D47F0u, 0xE24DD020u},
        {0xE92D41F0u, 0xE24DD020u}, {0xE92D40F0u, 0xE24DD01Cu}, {0xE92D4FF0u, 0xE24DD01Cu},
        {0xE1A0C00Du, 0xE92D000Fu}, {0xE92D4FF0u, 0xE24DD044u},
    };
    uint32_t at = image + 0xe1c08 - 0x10000, *call = (uint32_t *)(uintptr_t)at;
    if (*call != 0xEBFFFFEAu || *(uint32_t *)(uintptr_t)(image + 0x2ad54 - 0x10000) != 0xE59F006Cu || !main_view_vtable)
        return 0;
    for (int k = 0; k < MEMO_N; k++) {
        const uint32_t *e = (const uint32_t *)(uintptr_t)(image + memo_fn[k] - 0x10000);
        if (e[0] != first[k][0] || e[1] != first[k][1])
            return 0;
    }
    uint32_t *t = (uint32_t *)(uintptr_t)(image + block - 0x1000);
    for (int k = 0; k < MEMO_N; k++, t += 4) {
        uint32_t *e = (uint32_t *)(uintptr_t)(image + memo_fn[k] - 0x10000);
        t[0] = e[0], t[1] = e[1], t[2] = 0xE51FF004u, t[3] = (uint32_t)(uintptr_t)(e + 2);
        memo_tramp[k] = (uint32_t)(uintptr_t)t;
        e[0] = 0xE51FF004u;
        e[1] = (uint32_t)(uintptr_t)memo_stubs[k];
    }
    t[0] = 0xE51FF004u, t[1] = (uint32_t)(uintptr_t)memo_child_thunk;
    *call = 0xEB000000u | ((((uint32_t)(uintptr_t)t) - (at + 8)) >> 2 & 0xFFFFFFu);
    memo_font = *(const uint8_t **)(uintptr_t)(image + 0x2adc8 - 0x10000);
    memo_oncompose[0] = image + 0x8ce64 - 0x10000;
    memo_oncompose[1] = image + 0x9d768 - 0x10000;
    memo_surface = (uint8_t *)(uintptr_t)(image + 0x221698 - 0x10000);
    memo_scr_rect = (void (*)(uint8_t *, int32_t *))(uintptr_t)(image + 0x11068 - 0x10000);
    return 1;
}

typedef struct { uint8_t k, p; uint16_t pad; uint32_t sid, obj; int32_t x, y; } MapOp;
typedef uint32_t (*Guest6)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#define MB_OPS 4096
#define MB_HASH 8192
#define MB_DIRTY 16
static MapOp mb_ops[2][MB_OPS];
static unsigned mb_n[2], mb_cur;
static int16_t mb_tab_i[MB_HASH];
static uint16_t mb_tab_n[MB_HASH];
static uint16_t mb_cache[320 * 240], mb_tmp[320 * 240];
static int32_t mb_rc[4], mb_a[2], mb_layer[2];
static int32_t mb_kept_rc[4], mb_kept_a[2];
static uint8_t *mb_comp, *mb_kept_comp;
static int mb_flushed, mb_grid = -1;
static uint8_t *mb_gfx;
static uint32_t *mb_settings;
static Guest4c mb_stock;

static void map_flush(void);

static uint32_t map_prim(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t k, const uint32_t *sp)
{
    uint32_t dib = k == 4 || k == 10 ? r1 : k >= 5 && k <= 8 ? r0 : r2;
    int text = k == 3 || k == 4;
    if (dib != (uint32_t)(uintptr_t)memo_surface)
        return 0;
    if ((k <= 1 || k == 9 || k == 10) && mb_n[mb_cur] < MB_OPS) {
        MapOp *o = &mb_ops[mb_cur][mb_n[mb_cur]++];
        o->k = (uint8_t)k, o->pad = 0, o->obj = r0;
        if (k == 10) {
            o->sid = 0, o->p = 0;
            o->x = ((const int32_t *)(uintptr_t)r2)[0], o->y = ((const int32_t *)(uintptr_t)r2)[1];
        } else {
            o->sid = r1, o->x = (int32_t)r3, o->y = (int32_t)sp[0];
            o->p = k == 0 ? 0 : (uint8_t)sp[1];
        }
        return 1;
    }
    map_flush();
    if (!text)
        mb_valid = 0;
    return 0;
}

static void mb_op_rect(const MapOp *o, int32_t *r)
{
    uint32_t bank = o->sid >> 16;
    const uint8_t *spr = bank < 16 ? *(const uint8_t *const *)(mb_gfx + 0x90 + bank * 0x2c) : NULL;
    if (!spr) {
        r[0] = r[1] = -0x10000, r[2] = r[3] = 0x10000;
        return;
    }
    spr += (o->sid & 0xffff) * 16;
    int32_t w = *(const uint16_t *)(spr + 8), h = *(const uint16_t *)(spr + 0xa);
    int32_t x0 = o->x + *(const int16_t *)(spr + 0xc), y0 = o->y + *(const int16_t *)(spr + 0xe);
    int32_t reach = o->k == 9 && o->p == 2 ? (h + 1) / 2 : 0;
    r[0] = x0 - reach - 2, r[1] = y0 - 2, r[2] = x0 + w + 2, r[3] = y0 + h + 3;
}

static uint32_t mb_slot(const MapOp *o, const int32_t *a, const MapOp *list, const int32_t *la)
{
    uint32_t h = o->k * 0x9e3779b1u ^ o->p * 0x85ebca6bu ^ o->sid * 0xc2b2ae35u ^ o->obj;
    h ^= (uint32_t)(o->x + a[0]) * 0x27d4eb2fu ^ (uint32_t)(o->y + a[1]) * 0x165667b1u;
    for (h = (h ^ h >> 15) & (MB_HASH - 1);; h = (h + 1) & (MB_HASH - 1)) {
        const MapOp *q = mb_tab_i[h] < 0 ? NULL : &list[mb_tab_i[h]];
        if (!q || (q->k == o->k && q->p == o->p && q->sid == o->sid && q->obj == o->obj &&
                   q->x + la[0] == o->x + a[0] && q->y + la[1] == o->y + a[1]))
            return h;
    }
}

static void mb_add(int32_t (*rs)[4], unsigned *n, unsigned first, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > mb_rc[2] ? mb_rc[2] : x1, y1 = y1 > mb_rc[3] ? mb_rc[3] : y1;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (unsigned i = first; i < *n; i++) {
        int32_t *r = rs[i];
        if (x0 <= r[2] + 8 && r[0] <= x1 + 8 && y0 <= r[3] + 8 && r[1] <= y1 + 8) {
            r[0] = x0 < r[0] ? x0 : r[0], r[1] = y0 < r[1] ? y0 : r[1];
            r[2] = x1 > r[2] ? x1 : r[2], r[3] = y1 > r[3] ? y1 : r[3];
            return;
        }
    }
    if (*n - first == MB_DIRTY) {
        for (unsigned i = first + 1; i < *n; i++) {
            int32_t *r = rs[i];
            x0 = r[0] < x0 ? r[0] : x0, y0 = r[1] < y0 ? r[1] : y0;
            x1 = r[2] > x1 ? r[2] : x1, y1 = r[3] > y1 ? r[3] : y1;
        }
        *n = first;
        mb_add(rs, n, first, x0, y0, x1, y1);
        return;
    }
    rs[*n][0] = x0, rs[*n][1] = y0, rs[*n][2] = x1, rs[*n][3] = y1;
    (*n)++;
}

static void mb_render(uint8_t *band, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    int whole_rows = rw == mb_rc[2];
    *(uint16_t **)(band + 4) = whole_rows ? mb_cache + ry * rw : mb_tmp;
    *(uint32_t *)(band + 0xc) = (uint32_t)rw;
    *(uint32_t *)(band + 0x10) = (uint32_t)rh;
    int32_t ox = mb_rc[0] + rx, oy = mb_rc[1] + ry;
    const MapOp *o = mb_ops[mb_cur];
    for (unsigned i = 0; i < mb_n[mb_cur]; i++, o++) {
        if (o->k == 10) {
            int32_t at[2] = {o->x - ox, o->y - oy};
            ((Guest4c)(uintptr_t)memo_tramp[10])(o->obj, (uint32_t)(uintptr_t)band, (uint32_t)(uintptr_t)at, 0);
            continue;
        }
        int32_t r[4];
        mb_op_rect(o, r);
        if (r[2] <= ox || r[0] >= ox + rw || r[3] <= oy || r[1] >= oy + rh)
            continue;
        ((Guest6)(uintptr_t)memo_tramp[o->k])(o->obj, o->sid, (uint32_t)(uintptr_t)band, (uint32_t)(o->x - ox),
                                              (uint32_t)(o->y - oy), o->p);
    }
    if (!whole_rows)
        for (int32_t y = 0; y < rh; y++)
            memcpy(mb_cache + (ry + y) * mb_rc[2] + rx, mb_tmp + y * rw, (size_t)rw * 2);
}

static void map_flush(void)
{
    memo_rec = 0;
    mb_flushed = 1;
    int32_t w = mb_rc[2], h = mb_rc[3], dx = mb_a[0] - mb_kept_a[0], dy = mb_a[1] - mb_kept_a[1];
    const int32_t *layer = (const int32_t *)(mb_comp + 0x20);
    int grid = mb_settings ? (int)mb_settings[8] : 0, regrid = 0;
    if (layer[0] != mb_layer[0] || layer[1] != mb_layer[1]) {
        regrid = grid != mb_grid;
        mb_grid = grid;
    }
    int invalid = !mb_valid || mb_comp != mb_kept_comp || memcmp(mb_rc, mb_kept_rc, sizeof(mb_rc));
    int jump = !invalid && ((uint32_t)abs(dx) >= (uint32_t)w / 2 || (uint32_t)abs(dy) >= (uint32_t)h / 2);
    int32_t rs[4 + MB_DIRTY][4];
    unsigned n = 0, first = 0;
    int whole = invalid || jump || regrid, area = 0;
    if (!whole) {
        const MapOp *old = mb_ops[mb_cur ^ 1], *cur = mb_ops[mb_cur];
        unsigned n_old = mb_n[mb_cur ^ 1], n_cur = mb_n[mb_cur];
        memset(mb_tab_i, 0xff, sizeof(mb_tab_i));
        for (unsigned i = 0; i < n_old; i++)
            if (old[i].k != 10) {
                uint32_t s = mb_slot(&old[i], mb_kept_a, old, mb_kept_a);
                if (mb_tab_i[s] < 0)
                    mb_tab_i[s] = (int16_t)i, mb_tab_n[s] = 0;
                mb_tab_n[s]++;
            }
        int32_t r[4];
        for (unsigned i = 0; i < n_cur; i++)
            if (cur[i].k != 10) {
                uint32_t s = mb_slot(&cur[i], mb_a, old, mb_kept_a);
                if (mb_tab_i[s] >= 0 && mb_tab_n[s]) {
                    mb_tab_n[s]--;
                    continue;
                }
                mb_op_rect(&cur[i], r);
                mb_add(rs, &n, first, r[0] - mb_rc[0], r[1] - mb_rc[1], r[2] - mb_rc[0], r[3] - mb_rc[1]);
            }
        for (unsigned i = 0; i < n_old; i++)
            if (old[i].k != 10) {
                uint32_t s = mb_slot(&old[i], mb_kept_a, old, mb_kept_a);
                if (!mb_tab_n[s])
                    continue;
                mb_tab_n[s]--;
                mb_op_rect(&old[i], r);
                mb_add(rs, &n, first, r[0] - mb_rc[0] - dx, r[1] - mb_rc[1] - dy, r[2] - mb_rc[0] - dx,
                       r[3] - mb_rc[1] - dy);
            }
        uint32_t px = 0;
        for (unsigned i = 0; i < n; i++)
            px += (uint32_t)((rs[i][2] - rs[i][0]) * (rs[i][3] - rs[i][1]));
        px += (uint32_t)(abs(dx) * h + abs(dy) * w);
        area = px > (uint32_t)(w * h) * 3 / 5;
        whole = area;
    }
    if (whole) {
        n = 1;
        rs[0][0] = rs[0][1] = 0, rs[0][2] = w, rs[0][3] = h;
    } else {
        int32_t y0 = dy < 0 ? -dy : 0, y1 = dy > 0 ? h - dy : h, x0 = dx < 0 ? -dx : 0, x1 = dx > 0 ? w - dx : w;
        if (dx || dy) {
            if (dy > 0)
                for (int32_t y = y0; y < y1; y++)
                    memmove(mb_cache + y * w + x0, mb_cache + (y + dy) * w + x0 + dx, (size_t)(x1 - x0) * 2);
            else
                for (int32_t y = y1; y-- > y0;)
                    memmove(mb_cache + y * w + x0, mb_cache + (y + dy) * w + x0 + dx, (size_t)(x1 - x0) * 2);
        }
        int32_t bands[4][4] = {{0, 0, w, y0}, {0, y1, w, h}, {0, y0, x0, y1}, {x1, y0, w, y1}};
        for (int i = 0; i < 4; i++)
            if (bands[i][0] < bands[i][2] && bands[i][1] < bands[i][3])
                memcpy(rs[n++], bands[i], sizeof(bands[i]));
    }
    uint8_t band[0x20];
    memcpy(band, memo_surface, sizeof(band));
    for (unsigned i = 0; i < n; i++)
        mb_render(band, rs[i][0], rs[i][1], rs[i][2] - rs[i][0], rs[i][3] - rs[i][1]);
    uint16_t *s = DIB_PIX(memo_surface) + mb_rc[1] * DIB_W(memo_surface) + mb_rc[0];
    for (int32_t y = 0; y < h; y++, s += DIB_W(memo_surface))
        memcpy(s, mb_cache + y * w, (size_t)w * 2);
    mb_valid = 1;
    mb_kept_comp = mb_comp;
    memcpy(mb_kept_rc, mb_rc, sizeof(mb_rc));
    mb_kept_a[0] = mb_a[0], mb_kept_a[1] = mb_a[1];
}

enum { OL_SITES = 2, OL_MAX = 256, OL_SPR = 192, OL_CACHE = 512, OL_SAMPLES = 48 };
static const uint32_t ol_site[OL_SITES] = {0x84178, 0x842f8};
static uint32_t ol_ret[OL_SITES];
uint32_t outline_tramp;
static int ol_noting;
static unsigned ol_n;
static struct { uint32_t sid; int32_t x, y; } ol_list[OL_MAX];
static struct { uint32_t sid; uint16_t n_ring, n_samp; int16_t *pts; } ol_cache[OL_CACHE];
static unsigned ol_cached;

void outline_note(uint32_t sid, const int32_t *pos, uint32_t ret)
{
    if (!ol_noting || ol_n >= OL_MAX)
        return;
    for (int i = 0; i < OL_SITES; i++)
        if (ret == ol_ret[i]) {
            ol_list[ol_n].sid = sid, ol_list[ol_n].x = pos[0], ol_list[ol_n].y = pos[1];
            ol_n++;
            return;
        }
}

static int ol_shape(uint32_t sid)
{
    unsigned h = (sid * 2654435761u) % OL_CACHE;
    for (unsigned i = 0; i < OL_CACHE; i++, h = (h + 1) % OL_CACHE) {
        if (ol_cache[h].pts && ol_cache[h].sid == sid)
            return (int)h;
        if (!ol_cache[h].pts)
            break;
    }
    if (ol_cached >= OL_CACHE * 3 / 4) {
        for (unsigned i = 0; i < OL_CACHE; i++) {
            free(ol_cache[i].pts);
            ol_cache[i].pts = NULL;
        }
        ol_cached = 0;
        h = (sid * 2654435761u) % OL_CACHE;
    }
    uint32_t bank = sid >> 16;
    const uint8_t *spr = bank < 16 && mb_gfx ? *(const uint8_t *const *)(mb_gfx + 0x90 + bank * 0x2c) : NULL;
    if (!spr)
        return -1;
    spr += (sid & 0xffff) * 16;
    int w = *(const uint16_t *)(spr + 8), hh = *(const uint16_t *)(spr + 0xa);
    int ox = *(const int16_t *)(spr + 0xc), oy = *(const int16_t *)(spr + 0xe);
    if (w <= 0 || hh <= 0 || w > OL_SPR - 2 || hh > OL_SPR - 2)
        return -1;
    static uint16_t a[OL_SPR * OL_SPR], b[OL_SPR * OL_SPR];
    int W = w + 2, H = hh + 2;
    uint8_t d[0x20];
    memcpy(d, memo_surface, sizeof(d));
    *(uint32_t *)(d + 0xc) = (uint32_t)W;
    *(uint32_t *)(d + 0x10) = (uint32_t)H;
    memset(a, 0, (size_t)(W * H) * 2);
    memset(b, 0xff, (size_t)(W * H) * 2);
    *(uint16_t **)(d + 4) = a;
    ((Guest6)(uintptr_t)memo_tramp[0])((uint32_t)(uintptr_t)mb_gfx, sid, (uint32_t)(uintptr_t)d, (uint32_t)(1 - ox), (uint32_t)(1 - oy), 0);
    *(uint16_t **)(d + 4) = b;
    ((Guest6)(uintptr_t)memo_tramp[0])((uint32_t)(uintptr_t)mb_gfx, sid, (uint32_t)(uintptr_t)d, (uint32_t)(1 - ox), (uint32_t)(1 - oy), 0);
#define OL_IN(x, y) ((x) >= 0 && (x) < W && (y) >= 0 && (y) < H && a[(y) * W + (x)] == b[(y) * W + (x)])
    unsigned n_ring = 0, n_in = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (OL_IN(x, y)) {
                n_in++;
                continue;
            }
            for (int k = 0; k < 9; k++)
                if (k != 4 && OL_IN(x + k % 3 - 1, y + k / 3 - 1)) {
                    n_ring++;
                    break;
                }
        }
    unsigned step = n_in / OL_SAMPLES + 1, n_samp = n_in / step;
    int16_t *pts = malloc((n_ring + n_samp + 1) * 2 * sizeof(int16_t));
    if (!pts)
        return -1;
    unsigned r = 0, sm = 0, seen = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int dx = x - 1 + ox, dy = y - 1 + oy;
            if (OL_IN(x, y)) {
                if (seen++ % step == 0 && sm < n_samp) {
                    pts[2 * (n_ring + sm)] = (int16_t)dx, pts[2 * (n_ring + sm) + 1] = (int16_t)dy;
                    sm++;
                }
                continue;
            }
            for (int k = 0; k < 9; k++)
                if (k != 4 && OL_IN(x + k % 3 - 1, y + k / 3 - 1)) {
                    pts[2 * r] = (int16_t)dx, pts[2 * r + 1] = (int16_t)dy;
                    r++;
                    break;
                }
        }
#undef OL_IN
    ol_cache[h].sid = sid, ol_cache[h].n_ring = (uint16_t)n_ring, ol_cache[h].n_samp = (uint16_t)sm, ol_cache[h].pts = pts;
    ol_cached++;
    return (int)h;
}

static void outline_draw(const int32_t *rc)
{
    uint16_t *pix = DIB_PIX(memo_surface);
    int32_t sw = (int32_t)DIB_W(memo_surface);
    int32_t x0 = rc[0], y0 = rc[1], x1 = rc[0] + rc[2], y1 = rc[1] + rc[3];
    for (unsigned i = 0; i < ol_n; i++) {
        int h = ol_shape(ol_list[i].sid);
        if (h < 0)
            continue;
        const int16_t *pts = ol_cache[h].pts;
        int32_t px = ol_list[i].x, py = ol_list[i].y;
        unsigned lit = 0, seen = 0, nr = ol_cache[h].n_ring, ns = ol_cache[h].n_samp;
        for (unsigned k = 0; k < ns; k++) {
            int32_t x = px + pts[2 * (nr + k)], y = py + pts[2 * (nr + k) + 1];
            if (x < x0 || x >= x1 || y < y0 || y >= y1)
                continue;
            seen++;
            lit += pix[y * sw + x] != 0;
        }
        if (!seen || lit * 8 < seen)
            continue;
        for (unsigned k = 0; k < nr; k++) {
            int32_t x = px + pts[2 * k], y = py + pts[2 * k + 1];
            if (x >= x0 && x < x1 && y >= y0 && y < y1)
                pix[y * sw + x] = 0xFEC0;
        }
    }
}

uint32_t map_bands(uint32_t comp_, uint32_t b, uint32_t c, uint32_t d)
{
    uint8_t *comp = (uint8_t *)(uintptr_t)comp_, *surf = memo_surface;
    const int32_t *rc = (const int32_t *)(comp + 0x08), *an = (const int32_t *)(comp + 0x18);
    if (memo_rec || !DIB_PIX(surf) || rc[0] < 0 || rc[1] < 0 || rc[2] <= 0 || rc[3] <= 0 ||
        (uint32_t)(rc[0] + rc[2]) > DIB_W(surf) || (uint32_t)(rc[1] + rc[3]) > DIB_H(surf) || rc[2] * rc[3] > 320 * 240) {
        mb_valid = 0;
        return mb_stock(comp_, b, c, d);
    }
    memcpy(mb_rc, rc, sizeof(mb_rc));
    mb_a[0] = an[0], mb_a[1] = an[1];
    mb_layer[0] = ((const int32_t *)(comp + 0x20))[0], mb_layer[1] = ((const int32_t *)(comp + 0x20))[1];
    mb_comp = comp;
    mb_cur ^= 1;
    mb_n[mb_cur] = 0;
    mb_flushed = 0;
    memo_rec = 2;
    ol_n = 0;
    ol_noting = outlines_on && outline_tramp;
    uint32_t r = mb_stock(comp_, b, c, d);
    ol_noting = 0;
    if (!mb_flushed)
        map_flush();
    memo_rec = 0;
    if (outlines_on && ol_n)
        outline_draw(rc);
    return r;
}

void outline_stub(void);

int win_patch_outline(uint32_t image, uint32_t block)
{
    uint32_t *e = (uint32_t *)(uintptr_t)(image + 0x81b5c - 0x10000);
    if (e[0] != 0xE92D4FF0u || e[1] != 0xE24DD014u || !mb_gfx || !memo_tramp[0] || !memo_surface)
        return 0;
    for (int i = 0; i < OL_SITES; i++) {
        uint32_t at = image + ol_site[i] - 0x10000;
        if (*(uint32_t *)(uintptr_t)at != (0xEB000000u | (((0x81b5c - (ol_site[i] + 8)) >> 2) & 0xFFFFFFu)))
            return 0;
        ol_ret[i] = at + 4;
    }
    uint32_t *t = (uint32_t *)(uintptr_t)(image + block - 0x1000 + 0x220);
    t[0] = e[0], t[1] = e[1], t[2] = 0xE51FF004u, t[3] = (uint32_t)(uintptr_t)(e + 2);
    outline_tramp = (uint32_t)(uintptr_t)t;
    e[0] = 0xE51FF004u;
    e[1] = (uint32_t)(uintptr_t)outline_stub;
    return 1;
}

void map_bands_thunk(void);

int win_patch_mapbands(uint32_t image, uint32_t block)
{
    uint32_t at = image + 0x8a014 - 0x10000, *ins = (uint32_t *)(uintptr_t)at;
    mb_gfx = (uint8_t *)(uintptr_t)(image + 0x221ca0 - 0x10000);
    if (!memo_tramp[9] || !memo_tramp[10] || !memo_surface ||
        *(uint32_t *)(uintptr_t)(image + 0x81c20 - 0x10000) != (uint32_t)(uintptr_t)mb_gfx)
        return 0;
    mb_settings = *(uint32_t **)(uintptr_t)(image + 0x8a3f8 - 0x10000);
    if (*ins != (0xEB000000u | (((0x8232c - (0x8a014 + 8)) >> 2) & 0xFFFFFFu)))
        return 0;
    mb_stock = (Guest4c)(uintptr_t)(image + 0x8232c - 0x10000);
    uint32_t *v = (uint32_t *)(uintptr_t)(image + block - 0x1000 + 0x200);
    v[0] = 0xE51FF004u;
    v[1] = (uint32_t)(uintptr_t)map_bands_thunk;
    *ins = 0xEB000000u | ((((uint32_t)(uintptr_t)v) - (at + 8)) >> 2 & 0xFFFFFFu);
    return 1;
}

static uint32_t *dp_gapi;

static void dp_right(const uint16_t *src, uint16_t *dst, uint32_t w, uint32_t h, uint32_t s, uint32_t rows)
{
    uint32_t h8 = h & ~7u, w8 = w & ~7u;
    for (uint32_t y = 0; y < h8; y += 8)
        for (uint32_t x = 0; x < w8; x += 8) {
            const uint16_t *p = src + y * w + x;
            uint16x8x2_t t01 = vtrnq_u16(vld1q_u16(p), vld1q_u16(p + w));
            uint16x8x2_t t23 = vtrnq_u16(vld1q_u16(p + 2 * w), vld1q_u16(p + 3 * w));
            uint16x8x2_t t45 = vtrnq_u16(vld1q_u16(p + 4 * w), vld1q_u16(p + 5 * w));
            uint16x8x2_t t67 = vtrnq_u16(vld1q_u16(p + 6 * w), vld1q_u16(p + 7 * w));
            uint32x4x2_t a = vtrnq_u32(vreinterpretq_u32_u16(t01.val[0]), vreinterpretq_u32_u16(t23.val[0]));
            uint32x4x2_t b = vtrnq_u32(vreinterpretq_u32_u16(t01.val[1]), vreinterpretq_u32_u16(t23.val[1]));
            uint32x4x2_t c = vtrnq_u32(vreinterpretq_u32_u16(t45.val[0]), vreinterpretq_u32_u16(t67.val[0]));
            uint32x4x2_t e = vtrnq_u32(vreinterpretq_u32_u16(t45.val[1]), vreinterpretq_u32_u16(t67.val[1]));
            uint32x4_t col[8] = {
                vcombine_u32(vget_low_u32(a.val[0]), vget_low_u32(c.val[0])),
                vcombine_u32(vget_low_u32(b.val[0]), vget_low_u32(e.val[0])),
                vcombine_u32(vget_low_u32(a.val[1]), vget_low_u32(c.val[1])),
                vcombine_u32(vget_low_u32(b.val[1]), vget_low_u32(e.val[1])),
                vcombine_u32(vget_high_u32(a.val[0]), vget_high_u32(c.val[0])),
                vcombine_u32(vget_high_u32(b.val[0]), vget_high_u32(e.val[0])),
                vcombine_u32(vget_high_u32(a.val[1]), vget_high_u32(c.val[1])),
                vcombine_u32(vget_high_u32(b.val[1]), vget_high_u32(e.val[1])),
            };
            for (int k = 0; k < 8; k++)
                vst1q_u16(dst + (rows - 1 - (x + k)) * s + y, vreinterpretq_u16_u32(col[k]));
        }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = (y < h8 ? w8 : 0); x < w; x++)
            dst[(rows - 1 - x) * s + y] = src[y * w + x];
}

void do_paint(uint8_t *d)
{
    uint8_t *fb = (uint8_t *)(uintptr_t)dp_gapi[1];
    const uint16_t *src = *(const uint16_t **)(d + 0x38);
    uint32_t fw = *(uint32_t *)(d + 0x08), s = *(uint32_t *)(d + 0x18), rows = *(uint32_t *)(d + 0x1c);
    if (!d[6]) {
        uint32_t fh = *(uint32_t *)(d + 0x0c), m = (fh - rows) >> 1, n = fw * m;
        memset(fb, 0, n * 2);
        memset(fb + (fh - m), 0, n * 2);
        fb += n * 2;
    }
    uint32_t w = *(uint32_t *)(d + 0x4c), h = *(uint32_t *)(d + 0x50), left = *(uint32_t *)(d + 0x58) & 0x10;
    uint16_t *dst = (uint16_t *)fb;
    if (!d[0x31] && !left) {
        dp_right(src, dst, w, h, s, rows);
    } else if (!d[0x31]) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                dst[x * fw + (fw - 1 - y)] = src[y * w + x];
    } else if (!left) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint16_t v = src[y * w + x];
                uint16_t *p = dst + (rows - 1 - 2 * x) * s + 2 * y;
                p[0] = p[1] = v;
                p[-(int32_t)s] = p[1 - (int32_t)s] = v;
            }
    } else {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint16_t v = src[y * w + x];
                uint16_t *p = dst + 2 * x * fw + (fw - 1 - 2 * y);
                p[0] = p[-1] = v;
                p[fw] = p[fw - 1] = v;
            }
    }
}

void present_thunk(void);

int win_patch_present(uint32_t image)
{
    uint32_t *entry = (uint32_t *)(uintptr_t)(image + 0xdcae0 - 0x10000);
    if (entry[0] != 0xE92D43F0u || entry[1] != 0xE1A08000u || entry[2] != 0xE59F31CCu)
        return 0;
    dp_gapi = *(uint32_t **)(uintptr_t)(image + 0xdccbc - 0x10000);
    entry[0] = 0xE51FF004u;
    entry[1] = (uint32_t)(uintptr_t)present_thunk;
    return 1;
}

static uint16_t *guest_fb;
static int portrait;
static unsigned presents;

enum { SCALE_NEAREST, SCALE_2X, SCALE_SHARP, SCALE_LCD3X, SCALE_FXAA, SCALE_MODES };
static const char *const scale_names[SCALE_MODES] = {"nearest", "2x", "sharp", "lcd3x", "fxaa"};
static int scale_mode = SCALE_SHARP;
static int disp_x = DISP_X, disp_y = 0, disp_w = DISP_W, disp_h = VITA_H;
static unsigned frame_us;

static SceUID paint_done = -1;
static volatile unsigned paint_vc;

static vita2d_texture *tex;
static vita2d_texture *twice;
static int redraw;

static void settings_load(void);
static void settings_save(void);

static void set_scale(int mode)
{
    scale_mode = mode;
    redraw = 1;
    if (mode == SCALE_2X) {
        disp_w = UP_W * 2; disp_h = UP_H * 2;
    } else {
        disp_w = DISP_W; disp_h = VITA_H;
    }
    disp_x = (VITA_W - disp_w) / 2;
    disp_y = (VITA_H - disp_h) / 2;
    say("disp  scale %s %dx%d at %d,%d\n", scale_names[mode], disp_w, disp_h, disp_x, disp_y);
}

static void draw_panel(float x, float y, float w, float h)
{
    vita2d_draw_texture_scale_rotate_hotspot(tex, x + w / 2, y + h / 2, h / FB_W, w / FB_H, 1.57079633f,
                                             FB_W / 2.0f, FB_H / 2.0f);
}

extern const SceGxmProgram lcd3x_v, lcd3x_f, fxaa_v, fxaa_f;
extern float _vita2d_ortho_matrix[16];
static struct shader {
    int mode;
    const SceGxmProgram *v, *f;
    const char *size_name;
    int post;
    SceGxmVertexProgram *vp;
    SceGxmFragmentProgram *fp;
    const SceGxmProgramParameter *wvp, *vsize, *fsize;
} shaders[] = {
    {.mode = SCALE_LCD3X, .v = &lcd3x_v, .f = &lcd3x_f, .size_name = "IN.texture_size"},
    {.mode = SCALE_FXAA, .v = &fxaa_v, .f = &fxaa_f, .size_name = "IN.video_size", .post = 1},
};
#define N_SHADERS (sizeof(shaders) / sizeof(shaders[0]))

static struct shader *shader_for(int mode)
{
    for (unsigned i = 0; i < N_SHADERS; i++)
        if (shaders[i].mode == mode)
            return shaders[i].vp ? &shaders[i] : NULL;
    return NULL;
}

static void shader_init(struct shader *sh)
{
    SceGxmShaderPatcher *sp = vita2d_get_shader_patcher();
    SceGxmShaderPatcherId vid, fid;
    const SceGxmProgramParameter *pos = sceGxmProgramFindParameterByName(sh->v, "aPosition");
    const SceGxmProgramParameter *uv = sceGxmProgramFindParameterByName(sh->v, "aTexcoord");
    sh->wvp = sceGxmProgramFindParameterByName(sh->v, "wvp");
    sh->vsize = sceGxmProgramFindParameterByName(sh->v, sh->size_name);
    sh->fsize = sceGxmProgramFindParameterByName(sh->f, sh->size_name);
    int r = sceGxmProgramCheck(sh->v);
    if (!r)
        r = sceGxmProgramCheck(sh->f);
    if (!r && !(pos && (uv || sh->post) && sh->wvp))
        r = -1;
    if (!r)
        r = sceGxmShaderPatcherRegisterProgram(sp, sh->v, &vid);
    if (!r)
        r = sceGxmShaderPatcherRegisterProgram(sp, sh->f, &fid);
    if (!r) {
        SceGxmVertexAttribute attr[2] = {
            {.streamIndex = 0, .offset = 0, .format = SCE_GXM_ATTRIBUTE_FORMAT_F32, .componentCount = 3,
             .regIndex = sceGxmProgramParameterGetResourceIndex(pos)},
            {.streamIndex = 0, .offset = 12, .format = SCE_GXM_ATTRIBUTE_FORMAT_F32, .componentCount = 2,
             .regIndex = uv ? sceGxmProgramParameterGetResourceIndex(uv) : 0},
        };
        SceGxmVertexStream stream = {.stride = sizeof(vita2d_texture_vertex),
                                     .indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
        r = sceGxmShaderPatcherCreateVertexProgram(sp, vid, attr, uv ? 2 : 1, &stream, 1, &sh->vp);
    }
    if (!r)
        r = sceGxmShaderPatcherCreateFragmentProgram(sp, fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                     SCE_GXM_MULTISAMPLE_NONE, NULL, sh->v, &sh->fp);
    if (r)
        sh->vp = NULL, sh->fp = NULL;
    say("disp  %s programs 0x%08x vp=%p fp=%p size=%p/%p\n", scale_names[sh->mode], (unsigned)r, sh->vp, sh->fp,
        sh->vsize, sh->fsize);
}

static void draw_upright(float x, float y, float w, float h, SceGxmTextureFilter filter)
{
    vita2d_texture_set_filters(twice, filter, filter);
    vita2d_start_drawing_advanced(twice, 0);
    vita2d_clear_screen();
    draw_panel(x, y, w, h);
    vita2d_end_drawing();
}

static void draw_shader(const struct shader *sh, float x, float y, float w, float h, float u1, float v1)
{
    static const float size[2] = {VITA_W, VITA_H};
    SceGxmContext *ctx = vita2d_get_context();
    vita2d_texture_vertex *v = vita2d_pool_memalign(4 * sizeof(*v), sizeof(*v));
    for (int i = 0; i < 4; i++) {
        v[i].x = x + (i & 1) * w;
        v[i].y = y + (i >> 1) * h;
        v[i].z = 0.5f;
        v[i].u = (i & 1) * u1;
        v[i].v = (i >> 1) * v1;
    }
    void *buf;
    sceGxmSetVertexProgram(ctx, sh->vp);
    sceGxmSetFragmentProgram(ctx, sh->fp);
    sceGxmReserveVertexDefaultUniformBuffer(ctx, &buf);
    sceGxmSetUniformDataF(buf, sh->wvp, 0, 16, _vita2d_ortho_matrix);
    if (sh->vsize)
        sceGxmSetUniformDataF(buf, sh->vsize, 0, 2, size);
    if (sh->fsize) {
        sceGxmReserveFragmentDefaultUniformBuffer(ctx, &buf);
        sceGxmSetUniformDataF(buf, sh->fsize, 0, 2, size);
    }
    sceGxmSetFragmentTexture(ctx, 0, &twice->gxm_tex);
    sceGxmSetVertexStream(ctx, 0, v);
    sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, vita2d_get_linear_indices(), 4);
}

static uint64_t label_until;
static const char *label_text;
static const struct { char c; uint8_t rows[7]; } glyphs[] = {
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}}, {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}}, {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
    {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}}, {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}}, {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}}, {'N', {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11}},
    {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}}, {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}}, {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}}, {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}},
    {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}}, {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}},
};

static void draw_label(const char *s)
{
    enum { DOT = 4, X0 = 16, Y0 = 16 };
    int n = strlen(s);
    vita2d_draw_rectangle(X0 - 2 * DOT, Y0 - 2 * DOT, (n * 6 + 3) * DOT, 11 * DOT, 0xC0000000);
    for (int k = 0; k < n; k++)
        for (unsigned g = 0; g < sizeof(glyphs) / sizeof(glyphs[0]); g++)
            if (glyphs[g].c == toupper((unsigned char)s[k]))
                for (int r = 0; r < 7; r++)
                    for (int c = 0; c < 5; c++)
                        if (glyphs[g].rows[r] & (0x10 >> c))
                            vita2d_draw_rectangle(X0 + (k * 6 + c) * DOT, Y0 + r * DOT, DOT, DOT, 0xFFFFFFFF);
}

static volatile float cursor_x = UP_W / 2, cursor_y = UP_H / 2;
static volatile int cursor_shown;
#ifdef DIAGNOSTICS
static volatile int cursor_veiled;
#else
#define cursor_veiled 0
#endif

static void draw_cursor(void)
{
    enum { GAP = 6, ARM = 12, W = 4 };
    int x = disp_x + (int)(cursor_x * disp_w / UP_W), y = disp_y + (int)(cursor_y * disp_h / UP_H);
    for (int pass = 0; pass < 2; pass++) {
        int o = pass ? 0 : 3;
        uint32_t c = pass ? 0xFF00D8FF : 0xFF000000;
        vita2d_draw_rectangle(x - GAP - ARM - o, y - W / 2 - o, ARM + 2 * o, W + 2 * o, c);
        vita2d_draw_rectangle(x + GAP - o, y - W / 2 - o, ARM + 2 * o, W + 2 * o, c);
        vita2d_draw_rectangle(x - W / 2 - o, y - GAP - ARM - o, W + 2 * o, ARM + 2 * o, c);
        vita2d_draw_rectangle(x - W / 2 - o, y + GAP - o, W + 2 * o, ARM + 2 * o, c);
    }
}

static void ring_quad(float ax, float ay, float bx, float by, float hw, uint32_t c)
{
    float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy);
    if (l <= 0)
        return;
    float nx = -dy / l * hw, ny = dx / l * hw;
    vita2d_color_vertex *v = vita2d_pool_memalign(4 * sizeof(*v), sizeof(*v));
    if (!v)
        return;
    v[0] = (vita2d_color_vertex){ax + nx, ay + ny, 0.5f, c};
    v[1] = (vita2d_color_vertex){ax - nx, ay - ny, 0.5f, c};
    v[2] = (vita2d_color_vertex){bx + nx, by + ny, 0.5f, c};
    v[3] = (vita2d_color_vertex){bx - nx, by - ny, 0.5f, c};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

static void ring_arc(float cx, float cy, float a0, float a1, float r0, float r1, int seg, uint32_t c)
{
    vita2d_color_vertex *v = vita2d_pool_memalign(2 * (seg + 1) * sizeof(*v), sizeof(*v));
    if (!v)
        return;
    for (int i = 0; i <= seg; i++) {
        float a = (a0 + (a1 - a0) * i / seg) * (float)M_PI / 180, sx = sinf(a), sy = -cosf(a);
        v[2 * i] = (vita2d_color_vertex){cx + r0 * sx, cy + r0 * sy, 0.5f, c};
        v[2 * i + 1] = (vita2d_color_vertex){cx + r1 * sx, cy + r1 * sy, 0.5f, c};
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 2 * (seg + 1));
}

enum { RING_R0 = 64, RING_R1 = 124 };

static int ring_layout(float *lo, float *hi)
{
    int shown = 0;
    for (int k = 0; k < 12; k++)
        if (!(k & 1) || (ring.cfg & 1u << k))
            shown |= 1 << k;
    for (int k = 0; k < 12; k++) {
        if (!(shown >> k & 1))
            continue;
        int p = k, n = k;
        do p = (p + 11) % 12; while (!(shown >> p & 1));
        do n = (n + 1) % 12; while (!(shown >> n & 1));
        float a = (float)(k * 30 - 30);
        lo[k] = a - (float)(((k - p) + 12) % 12) * 15, hi[k] = a + (float)(((n - k) + 12) % 12) * 15;
    }
    return shown;
}

static void ring_centre(float *px, float *py)
{
    float cx = disp_x + ring.x * disp_w / UP_W, cy = disp_y + ring.y * disp_h / UP_H;
    float lo = disp_x + RING_R1 + 4, hi = disp_x + disp_w - RING_R1 - 4;
    *px = cx < lo ? lo : cx > hi ? hi : cx;
    lo = disp_y + RING_R1 + 4, hi = disp_y + disp_h - RING_R1 - 4;
    *py = cy < lo ? lo : cy > hi ? hi : cy;
}

enum { RS_MAX = 64 };
static struct { int w, h; uint32_t *px; } rs;
static volatile int rs_ready;
static vita2d_texture *rs_tex;

static void ring_sprites_take(void)
{
    static int tried;
    if (tried || !ring_gfx)
        return;
    tried = 1;
    static uint16_t a[RS_MAX * RS_MAX], b[RS_MAX * RS_MAX];
    Guest6 blit = (Guest6)(uintptr_t)(memo_tramp[0] ? memo_tramp[0] : ring_blit);
    const uint8_t *bank = *(const uint8_t *const *)(uintptr_t)(ring_gfx + 0x90);
    if (!bank)
        return;
    const uint8_t *spr = bank + 0xbf * 16;
    int w = *(const uint16_t *)(spr + 8), h = *(const uint16_t *)(spr + 0xa);
    int ox = *(const int16_t *)(spr + 0xc), oy = *(const int16_t *)(spr + 0xe);
    if (w <= 0 || h <= 0 || w > RS_MAX || h > RS_MAX)
        return;
    uint8_t d[0x20];
    memcpy(d, (const void *)(uintptr_t)ring_surface, sizeof(d));
    *(uint32_t *)(d + 0xc) = (uint32_t)w;
    *(uint32_t *)(d + 0x10) = (uint32_t)h;
    memset(a, 0, (size_t)(w * h) * 2);
    memset(b, 0xff, (size_t)(w * h) * 2);
    *(uint16_t **)(d + 4) = a;
    blit(ring_gfx, 0xbf, (uint32_t)(uintptr_t)d, (uint32_t)-ox, (uint32_t)-oy, 0);
    *(uint16_t **)(d + 4) = b;
    blit(ring_gfx, 0xbf, (uint32_t)(uintptr_t)d, (uint32_t)-ox, (uint32_t)-oy, 0);
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (a[y * w + x] == b[y * w + x])
                x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
    if (x1 < 0)
        return;
    int W = x1 - x0 + 1, H = y1 - y0 + 1;
    uint32_t *px = malloc((size_t)(W * H * 4) * 4);
    if (!px)
        return;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t c = 0;
            int sx = x0 + x, sy = y0 + y;
            if (a[sy * w + sx] == b[sy * w + sx]) {
                uint16_t p = a[sy * w + sx];
                uint32_t r = p >> 11 & 31, g = p >> 5 & 63, bl = p & 31;
                c = 0xFF000000u | (bl << 3 | bl >> 2) << 16 | (g << 2 | g >> 4) << 8 | (r << 3 | r >> 2);
            }
            uint32_t *o = px + (2 * y) * (2 * W) + 2 * x;
            o[0] = o[1] = o[2 * W] = o[2 * W + 1] = c;
        }
    rs.w = 2 * W, rs.h = 2 * H, rs.px = px;
    say("ring sword: %dx%d at %d,%d of %dx%d\n", W, H, x0, y0, w, h);
    __sync_synchronize();
    rs_ready = 1;
}

enum { BAR_Y = 219, BAR_H = 21, TIP_W = 320, TIP_H = 30 };
static uint32_t tip_px[TIP_W * TIP_H];
static int tip_box[4];
static volatile unsigned tip_seq;

static uint32_t ring_textout;
uint32_t ring_tip_text(uint32_t tc, uint32_t fc, uint32_t dib, uint32_t pos, uint32_t text, uint32_t orc, uint32_t al,
                       uint32_t off)
{
    typedef uint32_t (*Guest8)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    Guest8 out = (Guest8)(uintptr_t)ring_textout;
    if (!ring_open)
        return out(tc, fc, dib, pos, text, orc, al, off);
    static uint16_t a[TIP_W * TIP_H], b[TIP_W * TIP_H];
    uint8_t d[0x20];
    memcpy(d, (const void *)(uintptr_t)dib, sizeof(d));
    *(uint32_t *)(d + 0xc) = TIP_W;
    *(uint32_t *)(d + 0x10) = TIP_H;
    memset(a, 0, sizeof(a));
    memset(b, 0xff, sizeof(b));
    *(uint16_t **)(d + 4) = a;
    out(tc, fc, (uint32_t)(uintptr_t)d, pos, text, orc, al, off);
    *(uint16_t **)(d + 4) = b;
    uint32_t r = out(tc, fc, (uint32_t)(uintptr_t)d, pos, text, orc, al, off);
    int x0 = TIP_W, y0 = TIP_H, x1 = 0, y1 = 0;
    for (int i = 0; i < TIP_W * TIP_H; i++) {
        uint16_t p = a[i], q = b[i];
        int ar = (p >> 11 & 31) * 255 / 31, ag = (p >> 5 & 63) * 255 / 63, ab = (p & 31) * 255 / 31;
        int br = (q >> 11 & 31) * 255 / 31, bg = (q >> 5 & 63) * 255 / 63, bb = (q & 31) * 255 / 31;
        int alpha = 255 - ((br - ar) + (bg - ag) + (bb - ab)) / 3;
        uint32_t c = 0;
        if (alpha > 12) {
            int cr = ar * 255 / alpha, cg = ag * 255 / alpha, cb = ab * 255 / alpha;
            cr = cr > 255 ? 255 : cr, cg = cg > 255 ? 255 : cg, cb = cb > 255 ? 255 : cb;
            c = (uint32_t)(alpha > 255 ? 255 : alpha) << 24 | (uint32_t)cb << 16 | (uint32_t)cg << 8 | (uint32_t)cr;
            int x = i % TIP_W, y = i / TIP_W;
            x0 = x < x0 ? x : x0, x1 = x + 1 > x1 ? x + 1 : x1, y0 = y < y0 ? y : y0, y1 = y + 1 > y1 ? y + 1 : y1;
        }
        tip_px[i] = c;
    }
    tip_box[0] = x0, tip_box[1] = y0, tip_box[2] = x1, tip_box[3] = y1;
    tip_seq++;
    redraw = 1;
    return r;
}

void ring_tip_thunk(void);

int win_patch_ring_tip(uint32_t image, uint32_t block)
{
    uint32_t at = image + 0xbc024 - 0x10000, *ins = (uint32_t *)(uintptr_t)at;
    if (*ins != (0xEB000000u | (((0x2aa38 - (0xbc024 + 8)) >> 2) & 0xFFFFFFu)) || ins[-13] != 0xE2895F49u)
        return 0;
    ring_textout = image + 0x2aa38 - 0x10000;
    uint32_t *v = (uint32_t *)(uintptr_t)(image + block - 0x1000 + 0x210);
    v[0] = 0xE51FF004u;
    v[1] = (uint32_t)(uintptr_t)ring_tip_thunk;
    *ins = 0xEB000000u | ((((uint32_t)(uintptr_t)v) - (at + 8)) >> 2 & 0xFFFFFFu);
    return 1;
}

static void ring_textures(void)
{
    static int made;
    if (made || !rs_ready)
        return;
    made = 1;
    vita2d_texture *t = vita2d_create_empty_texture_format(rs.w, rs.h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (!t)
        return;
    uint8_t *dst = vita2d_texture_get_datap(t);
    unsigned stride = vita2d_texture_get_stride(t);
    for (int y = 0; y < rs.h; y++)
        memcpy(dst + y * stride, rs.px + y * rs.w, (size_t)rs.w * 4);
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    rs_tex = t;
}

static void draw_ring(void)
{
    enum { R0 = RING_R0, R1 = RING_R1 };
    float cx, cy;
    ring_centre(&cx, &cy);
    if (ring_open)
        ring_arc(cx, cy, 0, 360, R0, 1200, 48, 0x90000000);
    float lo[12], hi[12];
    int shown = ring_layout(lo, hi);
    for (int k = 0; k < 12; k++) {
        if (!(shown >> k & 1))
            continue;
        if (!(ring.cfg & 1u << k)) {
            ring_arc(cx, cy, lo[k] + 2.5f, hi[k] - 2.5f, R0, R1, 8, 0x50000000);
            continue;
        }
        ring_arc(cx, cy, lo[k] + 1, hi[k] - 1, R0 - 3, R1 + 3, 8, 0xFF000000);
        ring_arc(cx, cy, lo[k] + 2.5f, hi[k] - 2.5f, R0, R1, 8, k == ring.dir ? 0xF02828E0 : 0xE000B8F0);
        float a = (float)(k * 30 - 30) * (float)M_PI / 180, sx = sinf(a), sy = -cosf(a);
        if (rs_tex) {
            float sc = 48.0f / (float)(rs.w > rs.h ? rs.w : rs.h), m = (lo[k] + hi[k]) / 2 * (float)M_PI / 180;
            float mx = cx + (R0 + R1) / 2.0f * sinf(m), my = cy - (R0 + R1) / 2.0f * cosf(m);
            float shx = -1.0f * disp_w / UP_W, shy = 2.0f * disp_h / UP_H;
            vita2d_draw_texture_tint_scale_rotate(rs_tex, mx + shx, my + shy, sc, sc, m, 0x80000000);
            vita2d_draw_texture_scale_rotate(rs_tex, mx, my, sc, sc, m);
            continue;
        }
        float tip = R0 + 8, hilt = R1 - 26, pommel = R1 - 10, guard = 11;
        for (int pass = 0; pass < 2; pass++) {
            float o = pass ? 0 : 2;
            uint32_t c = pass ? 0xFFFFFFFF : 0xFF000000;
            ring_quad(cx + (tip - o) * sx, cy + (tip - o) * sy, cx + hilt * sx, cy + hilt * sy, 3 + o, c);
            ring_quad(cx + hilt * sx - (guard + o) * sy, cy + hilt * sy + (guard + o) * sx,
                      cx + hilt * sx + (guard + o) * sy, cy + hilt * sy - (guard + o) * sx, 2.5f + o, c);
            ring_quad(cx + hilt * sx, cy + hilt * sy, cx + (pommel + o) * sx, cy + (pommel + o) * sy, 2.5f + o, c);
        }
    }
}

static vita2d_texture *tip_tex;

static void draw_ring_tip(void)
{
    float sx = (float)disp_w / UP_W, sy = (float)disp_h / UP_H;
    vita2d_draw_rectangle(disp_x, disp_y + BAR_Y * sy, disp_w, BAR_H * sy, 0xE8101010);
    static unsigned done;
    if (!tip_tex)
        tip_tex = vita2d_create_empty_texture_format(TIP_W, TIP_H, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (!tip_tex)
        return;
    if (done != tip_seq) {
        done = tip_seq;
        uint8_t *dst = vita2d_texture_get_datap(tip_tex);
        unsigned stride = vita2d_texture_get_stride(tip_tex);
        for (int y = 0; y < TIP_H; y++)
            memcpy(dst + y * stride, tip_px + y * TIP_W, TIP_W * 4);
    }
    int x0 = tip_box[0], y0 = tip_box[1], w = tip_box[2] - x0, h = tip_box[3] - y0;
    if (!ring.tip || w <= 0 || h <= 0)
        return;
    float x = disp_x + (UP_W - w) / 2.0f * sx, y = disp_y + (BAR_Y + (BAR_H - h) / 2.0f) * sy;
    vita2d_draw_texture_part_scale(tip_tex, x, y, x0, y0, w, h, sx, sy);
}

static int ring_hit(float px, float py)
{
    float cx, cy;
    ring_centre(&cx, &cy);
    float dx = px - cx, dy = py - cy, r = sqrtf(dx * dx + dy * dy);
    if (r < RING_R0 * 0.6f || r > RING_R1 + 40)
        return -1;
    float lo[12], hi[12];
    int shown = ring_layout(lo, hi);
    float a = atan2f(dx, -dy) * 180 / (float)M_PI;
    for (int k = 0; k < 12; k++) {
        if (!(shown >> k & 1) || !(ring.cfg & 1u << k))
            continue;
        float d = fmodf(a - lo[k] + 720, 360);
        if (d < hi[k] - lo[k])
            return k;
    }
    return -1;
}

static void draw(const uint16_t *src_fb)
{
    vita2d_wait_rendering_done();
    ring_textures();
    memcpy(vita2d_texture_get_datap(tex), src_fb, FB_W * FB_H * 2);
    vita2d_pool_reset();
    const struct shader *sh = shader_for(scale_mode);
    if (scale_mode == SCALE_SHARP)
        draw_upright(0, 0, UP_W * 2, UP_H * 2, SCE_GXM_TEXTURE_FILTER_LINEAR);
    else if (sh && sh->post)
        draw_upright(disp_x, disp_y, disp_w, disp_h, SCE_GXM_TEXTURE_FILTER_LINEAR);
    else if (sh)
        draw_upright(0, 0, UP_W, UP_H, SCE_GXM_TEXTURE_FILTER_POINT);
    vita2d_start_drawing_advanced(NULL, 0);
    vita2d_clear_screen();
    if (scale_mode == SCALE_SHARP)
        vita2d_draw_texture_part_scale(twice, disp_x, disp_y, 0, 0, UP_W * 2, UP_H * 2,
                                       (float)disp_w / (UP_W * 2), (float)disp_h / (UP_H * 2));
    else if (sh && sh->post)
        draw_shader(sh, 0, 0, VITA_W, VITA_H, 1, 1);
    else if (sh)
        draw_shader(sh, disp_x, disp_y, disp_w, disp_h, (float)UP_W / VITA_W, (float)UP_H / VITA_H);
    else
        draw_panel(disp_x, disp_y, disp_w, disp_h);
    if (ring.on) {
        draw_ring();
        if (ring_open)
            draw_ring_tip();
    }
    if (cursor_shown && !cursor_veiled)
        draw_cursor();
    if (label_until && label_text)
        draw_label(label_text);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static int frame_equal(const uint16_t *a, const uint16_t *b)
{
    const uint64_t *p = (const uint64_t *)a, *q = (const uint64_t *)b;
    for (unsigned i = 0; i < FB_W * FB_H / 4; i += 4)
        if ((p[i] ^ q[i]) | (p[i + 1] ^ q[i + 1]) | (p[i + 2] ^ q[i + 2]) | (p[i + 3] ^ q[i + 3]))
            return 0;
    return 1;
}

static int display_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    uint32_t held = 0;
    unsigned fps_done = 0, scale_done = 0;
    unsigned still = 0;
    static uint16_t shadow[FB_W * FB_H] __attribute__((aligned(64)));
    for (;;) {
        unsigned pass_vc = (unsigned)sceDisplayGetVcount();
        SceUInt wait_us = 15000;
        int painted = sceKernelWaitSema(paint_done, 1, &wait_us) == 0;
        while (!painted && painting) {
            wait_us = 20000;
            painted = sceKernelWaitSema(paint_done, 1, &wait_us) == 0;
        }
        unsigned start_vc = paint_vc;
        int changed = !frame_equal(shadow, guest_fb);
        for (int tries = 0; changed; tries++) {
            memcpy(shadow, guest_fb, sizeof(shadow));
            if (frame_equal(shadow, guest_fb))
                break;
            if (tries == 3) {
                changed = 0;
                break;
            }
            sceKernelDelayThread(1000);
        }
        if (label_until && sceKernelGetProcessTimeWide() >= label_until) {
            label_until = 0;
            redraw = 1;
        }
        static unsigned ring_drawn;
        if (ring_seq != ring_drawn) {
            ring_drawn = ring_seq;
            redraw = 1;
        }
        if (redraw || changed) {
            if (painted) {
                unsigned after = fps30 ? 2 : 1;
                while ((int)((unsigned)sceDisplayGetVcount() - (start_vc + after)) < 0)
                    sceDisplayWaitVblankStart();
            }
            uint64_t t0 = sceKernelGetProcessTimeWide();
            draw(shadow);
            redraw = 0;
            frame_us = (unsigned)(sceKernelGetProcessTimeWide() - t0);
            presents++;
            still = 0;
        } else if (++still >= 4 && still % 2 == 0) {
            vita2d_wait_rendering_done();
            SceDisplayFrameBuf fb = {sizeof(fb), vita2d_get_current_fb(), VITA_W, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
                                     VITA_W, VITA_H};
            sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
        }
        SceCtrlData pad;
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
            uint32_t pressed = pad.buttons & ~held;
            (void)pressed;
#ifdef DIAGNOSTICS
            if (vshot_combo(pad.buttons))
                say("shot  %d\n", vshot_take(DATA_DIR));
#endif
            if (fps_asked != fps_done) {
                fps_done = fps_asked;
                fps30 = !fps30;
                label_text = fps30 ? "30 FPS" : "60 FPS";
                label_until = sceKernelGetProcessTimeWide() + 3000000;
                redraw = 1;
                settings_save();
            }
            static unsigned outline_done;
            if (outline_asked != outline_done) {
                outline_done = outline_asked;
                label_text = outlines_on ? "OUTLINES ON" : "OUTLINES OFF";
                label_until = sceKernelGetProcessTimeWide() + 3000000;
                redraw = 1;
            }
            static unsigned music_done;
            if (music_asked != music_done) {
                music_done = music_asked;
                label_text = music_labels[music_set()];
                label_until = sceKernelGetProcessTimeWide() + 3000000;
                redraw = 1;
                settings_save();
            }
            static unsigned melee_done;
            if (melee_asked != melee_done) {
                melee_done = melee_asked;
                label_text = melee_labels[melee_mode];
                label_until = sceKernelGetProcessTimeWide() + 3000000;
                redraw = 1;
                settings_save();
            }
            if (scale_asked != scale_done) {
                scale_done = scale_asked;
                int mode = (scale_mode + 1) % SCALE_MODES;
                while (mode > SCALE_SHARP && !shader_for(mode))
                    mode = (mode + 1) % SCALE_MODES;
                set_scale(mode);
                label_text = scale_names[scale_mode];
                label_until = sceKernelGetProcessTimeWide() + 3000000;
                settings_save();
            }
            held = pad.buttons;
        }
        if ((unsigned)sceDisplayGetVcount() == pass_vc)
            sceDisplayWaitVblankStart();
    }
    return 0;
}

static int display_start(void)
{
    guest_fb = calloc(FB_W * FB_H, 2);
    paint_done = sceKernelCreateSema("ph-paint", 0, 0, 1, NULL);
    int r = vita2d_init();
    vita2d_set_clear_color(0xFF000000);
    tex = vita2d_create_empty_texture_format(FB_W, FB_H, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
    twice = vita2d_create_empty_texture_rendertarget(VITA_W, VITA_H, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (tex && twice) {
        for (unsigned i = 0; i < N_SHADERS; i++)
            shader_init(&shaders[i]);
        settings_load();
        vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        SceUID th = sceKernelCreateThread("ph-display", display_thread, 0x10000100, 0x10000, 0, 0, NULL);
        sceKernelStartThread(th, 0, NULL);
        cpu_track("display", th);
    }
    say("disp  guest_fb=%p vita2d=%d tex=%p stride=%u twice=%p\n", guest_fb, r, tex,
        tex ? vita2d_texture_get_stride(tex) : 0, twice);
    return tex && twice ? 0 : -1;
}

#define HWND_MAIN 0x00F30001u
#define HDC_SCREEN 0x00F40001u
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_PAINT 0x000F
#define WM_QUIT 0x0012
#define WM_SHOWWINDOW 0x0018
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define MK_LBUTTON 0x0001
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101

typedef struct { uint32_t hwnd, message, wparam, lparam, time; int32_t x, y; } Msg;
typedef uint32_t (*WndProc)(uint32_t, uint32_t, uint32_t, uint32_t);
static WndProc wndproc;
static uint32_t window_long[32];

#define N_QUEUES 4
typedef struct { uint32_t tid; Msg m[64]; unsigned head, tail; } Queue;
static Queue queues[N_QUEUES];
static SceUID q_lock = -1;

static void q_init(void)
{
    if (q_lock < 0)
        q_lock = sceKernelCreateMutex("ph-queues", 0, 0, NULL);
}

static Queue *queue_of(uint32_t tid)
{
    for (unsigned i = 0; i < N_QUEUES; i++)
        if (queues[i].tid == tid)
            return &queues[i];
    for (unsigned i = 1; i < N_QUEUES; i++)
        if (!queues[i].tid) {
            queues[i].tid = tid;
            return &queues[i];
        }
    return NULL;
}

static void post_to(uint32_t tid, uint32_t hwnd, uint32_t m, uint32_t w, uint32_t l)
{
    sceKernelLockMutex(q_lock, 1, NULL);
    Queue *q = queue_of(tid);
    if (q && q->tail - q->head < 64) {
        Msg *e = &q->m[q->tail++ % 64];
        e->hwnd = hwnd; e->message = m; e->wparam = w; e->lparam = l; e->time = 0; e->x = e->y = 0;
    }
    sceKernelUnlockMutex(q_lock, 1);
}

static void post(uint32_t hwnd, uint32_t m, uint32_t w, uint32_t l) { post_to(queues[0].tid, hwnd, m, w, l); }

uint32_t n_RegisterClassW(const uint32_t *wc)
{
    CALLED(RegisterClassW);
    wndproc = (WndProc)(uintptr_t)wc[1];
    say("win   RegisterClassW wndproc=0x%08x\n", (unsigned)wc[1]);
    return 0xC001;
}

uint32_t n_CreateWindowExW(uint32_t ex, const wchar16 *cls, const wchar16 *title, uint32_t style, const uint32_t *stk)
{
    CALLED(CreateWindowExW);
    q_init();
    queues[0].tid = (uint32_t)sceKernelGetThreadId();
    char c[64], t[64];
    to_ascii(cls, c, sizeof(c));
    to_ascii(title, t, sizeof(t));
    say("win   CreateWindowExW ex=0x%x class=\"%s\" title=\"%s\" style=0x%08x %dx%d at %d,%d -> 0x%08x\n",
        (unsigned)ex, c, t, (unsigned)style, (int)stk[2], (int)stk[3], (int)stk[0], (int)stk[1], HWND_MAIN);
    post(HWND_MAIN, WM_SIZE, 0, 0x00F00140);
    post(HWND_MAIN, WM_SHOWWINDOW, 1, 0);
    post(HWND_MAIN, WM_ACTIVATE, 1, 0);
    post(HWND_MAIN, WM_SETFOCUS, 0, 0);
    post(HWND_MAIN, WM_PAINT, 0, 0);
    return HWND_MAIN;
}

int n_ShowWindow(uint32_t h, int cmd) { CALLED(ShowWindow); (void)h; (void)cmd; return 1; }
int n_SetForegroundWindow(uint32_t h) { CALLED(SetForegroundWindow); (void)h; return 1; }
uint32_t n_GetForegroundWindow(void) { CALLED(GetForegroundWindow); return HWND_MAIN; }
int n_DestroyWindow(uint32_t h) { CALLED(DestroyWindow); say("win   DestroyWindow 0x%08x\n", (unsigned)h); return 1; }

uint32_t n_GetWindowLongW(uint32_t h, int32_t i)
{
    CALLED(GetWindowLongW);
    (void)h;
    return i < 0 && -i < 32 ? window_long[-i] : 0;
}

uint32_t n_SetWindowLongW(uint32_t h, int32_t i, uint32_t v)
{
    CALLED(SetWindowLongW);
    (void)h;
    uint32_t old = 0;
    if (i < 0 && -i < 32) {
        old = window_long[-i];
        window_long[-i] = v;
    }
    return old;
}

uint32_t n_DefWindowProcW(uint32_t h, uint32_t m, uint32_t w, uint32_t l)
{
    CALLED(DefWindowProcW);
    (void)h; (void)m; (void)w; (void)l;
    return 0;
}

static int take(Msg *out, int remove)
{
    int got = 0;
    sceKernelLockMutex(q_lock, 1, NULL);
    Queue *q = queue_of((uint32_t)sceKernelGetThreadId());
    if (q && q->head != q->tail) {
        *out = q->m[q->head % 64];
        if (remove)
            q->head++;
        got = 1;
    }
    sceKernelUnlockMutex(q_lock, 1);
    return got;
}

static int touch_on, touch_held;
static int cursor_down;
static uint32_t touch_at;
static unsigned touch_downs, touch_ups, touch_moves;

static void buttons_poll(void);

static void touch_poll_now(void);

enum { RM_OFF, RM_OPEN, RM_DRAG, RM_EAT };
static int rm, rm_pick = -1;
static const uint8_t *rm_switch;
static uint64_t rm_lift_at;

static int bv_locked(const uint8_t *bv)
{
    return bv && *(const uint32_t *)bv == battle_view_vtable && bv[0x121] && !bv[0x164] &&
           *(const uint32_t *)(bv + 0xd8) == 0 && *(const uint32_t *)(bv + 0x170);
}

static uint32_t ring_panel(int X, int Y)
{
    X = X < 0 ? 0 : X >= UP_W ? UP_W - 1 : X;
    Y = Y < 0 ? 0 : Y >= UP_H ? UP_H - 1 : Y;
    return (uint32_t)(FB_H - 1 - X) << 16 | (uint32_t)Y;
}

static void ring_move(int k)
{
    const uint8_t *e = ring_bv ? *(const uint8_t *const *)(ring_bv + 0x170) : NULL;
    float cx = ring.x, cy = ring.y;
    if (e) {
        int32_t x = *(const int32_t *)(e + 4), y = *(const int32_t *)(e + 8);
        cx = (float)(x * 24 - (y & 1) * 13 + bv_origin[0] + 10), cy = (float)(y * 17 + bv_origin[1] + 9);
    }
    int X, Y;
    if (k >= 0) {
        float a = (float)(k * 30 - 30) * (float)M_PI / 180;
        X = (int)lroundf(cx + 20 * sinf(a)), Y = (int)lroundf(cy - 20 * cosf(a));
    } else {
        X = cx < UP_W / 2 ? UP_W - 1 : 0, Y = cy < UP_H / 2 ? UP_H - 1 : 0;
    }
    touch_at = ring_panel(X, Y);
    post(HWND_MAIN, WM_MOUSEMOVE, MK_LBUTTON, touch_at);
    rm_preview = -1;
}

static void ring_preview(void)
{
    const uint8_t *e = ring_bv ? *(const uint8_t *const *)(ring_bv + 0x170) : NULL;
    uint32_t cfg = e ? *(const uint16_t *)e : 0;
    int k = 0;
    while (k < 12 && !(cfg & 1u << k))
        k++;
    ring_move(k < 12 ? k : -1);
    rm_preview = k < 12 ? k : -1;
}

static uint32_t ring_group(void)
{
    const uint8_t *bt = *(const uint8_t *const *)(ring_bv + 0xdc);
    uint32_t u = bt ? *(const uint32_t *)(bt + 0x4c) : 0;
    if (bt && !u)
        u = *(const uint32_t *)(bt + 0x48);
    if (!u || !ring_unit_type)
        return 0;
    u = ((Guest4c)(uintptr_t)**(const uint32_t *const *)(uintptr_t)u)(u, ring_unit_type, 0, 0);
    return u ? *(const uint32_t *)(uintptr_t)(u + 0xc) : 0;
}

static const uint8_t *ring_target_at(float px, float py)
{
    if (!ring_melee_entry || !ring_bv)
        return NULL;
    int X = (int)((px - disp_x) * UP_W / disp_w), Y = (int)((py - disp_y) * UP_H / disp_h);
    int32_t cell[2];
    cell[1] = (Y - bv_origin[1]) / 17;
    cell[0] = (X - bv_origin[0] + (cell[1] & 1) * 13) / 24;
    uint32_t group = ring_group();
    return group ? (const uint8_t *)(uintptr_t)((Guest4c)(uintptr_t)ring_melee_entry)(group, (uint32_t)(uintptr_t)cell, 0, 0)
                 : NULL;
}

static void ring_close(int k, const char *why)
{
    ring_move(k);
    post(HWND_MAIN, WM_LBUTTONUP, 0, touch_at);
    touch_ups++;
    say("ring %s dir=%d\n", why, k);
    rm = RM_OFF, rm_pick = -1, ring_open = 0, redraw = 1;
}

static int ring_touch(const SceTouchData *t)
{
    if ((rm == RM_OPEN || rm == RM_DRAG) && !bv_locked(ring_bv)) {
        post(HWND_MAIN, WM_LBUTTONUP, 0, touch_at);
        rm = RM_OFF, rm_pick = -1, ring_open = 0, touch_held = 0;
        say("ring lost\n");
        return 1;
    }
    if (rm == RM_EAT) {
        if (t->reportNum == 0)
            rm = RM_OFF, touch_held = 0;
        return 0;
    }
    if (rm == RM_OPEN) {
        if (t->reportNum == 0) {
            if (touch_held) {
                touch_held = 0;
                if (rm_pick >= 0) {
                    ring_close(rm_pick, "attack");
                } else if (rm_pick == -2) {
                    *(const uint8_t **)(ring_bv + 0x170) = rm_switch;
                    *(int32_t *)(ring_bv + 0x174) = -1;
                    (*(void (**)(uint8_t *))(*(uint8_t **)ring_bv + 0x64))(ring_bv);
                    rm_pick = -1;
                    ring_preview();
                    say("ring switch cell=%d,%d cfg=0x%03x\n", (int)*(const int32_t *)(rm_switch + 4),
                        (int)*(const int32_t *)(rm_switch + 8), (unsigned)*(const uint16_t *)rm_switch);
                } else {
                    ring_close(-1, "cancel");
                }
            }
            return 0;
        }
        touch_held = 1;
        float px = t->report[0].x / 2.0f, py = t->report[0].y / 2.0f;
        int k = ring_hit(px, py);
        const uint8_t *e = k >= 0 ? NULL : ring_target_at(px, py);
        if (e && e != *(const uint8_t *const *)(ring_bv + 0x170))
            k = -2, rm_switch = e;
        if (k != rm_pick) {
            rm_pick = k;
            if (k >= 0)
                ring_move(k);
            else
                ring_preview();
            ring_seq++;
            touch_moves++;
        }
        return 0;
    }
    if (rm == RM_DRAG) {
        if (t->reportNum == 0) {
            touch_held = 0;
            ring_close(rm_pick, rm_pick >= 0 ? "attack" : "cancel");
            return 0;
        }
        int k = ring_hit(t->report[0].x / 2.0f, t->report[0].y / 2.0f);
        if (k != rm_pick) {
            rm_pick = k;
            if (k >= 0)
                ring_move(k);
            else
                ring_preview();
            ring_seq++;
            touch_moves++;
        }
        return 0;
    }
    if (melee_mode == MELEE_RING_DRAG && t->reportNum > 0 && touch_held && bv_locked(ring_bv)) {
        rm = RM_DRAG, rm_pick = -2, ring_open = 1, redraw = 1;
        tip_box[2] = 0;
        say("ring drag cell=%d,%d cfg=0x%03x\n", (int)*(const int32_t *)(*(uint8_t **)(ring_bv + 0x170) + 4),
            (int)*(const int32_t *)(*(uint8_t **)(ring_bv + 0x170) + 8), (unsigned)*(const uint16_t *)*(uint8_t **)(ring_bv + 0x170));
        return ring_touch(t);
    }
    if (melee_mode == MELEE_RING_TAP && t->reportNum == 0 && touch_held && ring_bv && *(const uint32_t *)(ring_bv + 0xd8) == 0) {
        if (bv_locked(ring_bv)) {
            tip_box[2] = 0;
            ring_preview();
            rm = RM_OPEN, rm_pick = -1, ring_open = 1, touch_held = 0, rm_lift_at = 0, redraw = 1;
            say("ring open cell=%d,%d cfg=0x%03x\n", (int)*(const int32_t *)(*(uint8_t **)(ring_bv + 0x170) + 4),
                (int)*(const int32_t *)(*(uint8_t **)(ring_bv + 0x170) + 8), (unsigned)*(const uint16_t *)*(uint8_t **)(ring_bv + 0x170));
            return 0;
        }
        uint64_t now = sceKernelGetProcessTimeWide();
        if (*(const int32_t *)(ring_bv + 0xf0) == 0x7fff && (!rm_lift_at || now - rm_lift_at < 100000)) {
            if (!rm_lift_at)
                rm_lift_at = now;
            return 0;
        }
    }
    rm_lift_at = 0;
    return 1;
}

static void touch_poll(void)
{
    if ((uint32_t)sceKernelGetThreadId() != queues[0].tid)
        return;
    static uint64_t last;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - last < 8000)
        return;
    last = now;
    touch_poll_now();
}

static void touch_poll_now(void)
{
    buttons_poll();
    if (!touch_on) {
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        touch_on = 1;
    }
    SceTouchData t;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 1)
        return;
    if (cursor_down)
        return;
    if (!ring_touch(&t))
        return;
    if (t.reportNum == 0) {
        if (touch_held) {
            post(HWND_MAIN, WM_LBUTTONUP, 0, touch_at);
            touch_ups++;
            touch_held = 0;
            say("touch up   panel %u,%u\n", (unsigned)(touch_at & 0xFFFF), (unsigned)(touch_at >> 16));
        }
        return;
    }
    int X = (t.report[0].x / 2 - disp_x) * UP_W / disp_w, Y = (t.report[0].y / 2 - disp_y) * UP_H / disp_h;
    X = X < 0 ? 0 : X >= UP_W ? UP_W - 1 : X;
    Y = Y < 0 ? 0 : Y >= UP_H ? UP_H - 1 : Y;
    uint32_t at = (uint32_t)(FB_H - 1 - X) << 16 | (uint32_t)Y;
    if (!touch_held) {
        post(HWND_MAIN, WM_LBUTTONDOWN, MK_LBUTTON, at);
        touch_downs++;
        touch_held = 1;
        if (cursor_shown) {
            cursor_shown = 0;
            redraw = 1;
        }
        say("touch down panel %u,%u (upright %d,%d)\n", (unsigned)Y, (unsigned)(FB_H - 1 - X), X, Y);
    } else if (at != touch_at) {
        post(HWND_MAIN, WM_MOUSEMOVE, MK_LBUTTON, at);
        touch_moves++;
    }
    touch_at = at;
}

static const struct { uint32_t button, vk; } keymap[] = {
    {SCE_CTRL_UP, 0x25}, {SCE_CTRL_RIGHT, 0x26}, {SCE_CTRL_DOWN, 0x27}, {SCE_CTRL_LEFT, 0x28},
    {SCE_CTRL_CROSS, 0x0D}, {SCE_CTRL_SQUARE, 0xC1}, {SCE_CTRL_TRIANGLE, 0xC2}, {SCE_CTRL_CIRCLE, 0xC3},
    {SCE_CTRL_START, 0xC4}, {SCE_CTRL_LTRIGGER, 0xC5},
};
static uint32_t buttons_held;
static unsigned key_downs;

#define STICK_DEAD 48
static uint32_t stick_dirs(int x, int y)
{
    int dx = x - 128, dy = y - 128;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax < STICK_DEAD && ay < STICK_DEAD)
        return 0;
    uint32_t dirs = 0;
    if (ax * 5 >= ay * 2)
        dirs |= dx < 0 ? SCE_CTRL_LEFT : SCE_CTRL_RIGHT;
    if (ay * 5 >= ax * 2)
        dirs |= dy < 0 ? SCE_CTRL_UP : SCE_CTRL_DOWN;
    return dirs;
}

#define CURSOR_DEAD 32
#define CURSOR_SPEED 450.0f
static uint32_t cursor_at;
static unsigned cursor_downs, cursor_moves;

static void cursor_poll(const SceCtrlData *pad)
{
    static uint64_t last;
    static uint32_t r_was;
    uint64_t now = sceKernelGetProcessTimeWide();
    float dt = last ? (now - last) / 1e6f : 0;
    last = now;
    if (dt > 0.05f)
        dt = 0.05f;
    float dx = pad->rx - 128, dy = pad->ry - 128, d = sqrtf(dx * dx + dy * dy);
    if (d > CURSOR_DEAD) {
        float n = (d - CURSOR_DEAD) / (127 - CURSOR_DEAD);
        n = n > 1 ? 1 : n;
        float step = CURSOR_SPEED * n * n * dt / d;
        float x = cursor_x + dx * step, y = cursor_y + dy * step;
        cursor_x = x < 0 ? 0 : x > UP_W - 0.01f ? UP_W - 0.01f : x;
        cursor_y = y < 0 ? 0 : y > UP_H - 0.01f ? UP_H - 0.01f : y;
        cursor_shown = 1;
        redraw = 1;
    }
    int X = (int)cursor_x, Y = (int)cursor_y;
    uint32_t at = (uint32_t)(FB_H - 1 - X) << 16 | (uint32_t)Y;
    uint32_t r = pad->buttons & SCE_CTRL_RTRIGGER;
    if (cursor_veiled)
        r = r_was = 0;
    if (r && !r_was && !touch_held && rm == RM_OFF) {
        if (!cursor_shown) {
            cursor_shown = 1;
            redraw = 1;
        } else {
            post(HWND_MAIN, WM_LBUTTONDOWN, MK_LBUTTON, at);
            cursor_downs++;
            cursor_down = 1;
            say("cursor down panel %u,%u (upright %d,%d)\n", (unsigned)Y, (unsigned)(FB_H - 1 - X), X, Y);
        }
    } else if (!r && cursor_down) {
        post(HWND_MAIN, WM_LBUTTONUP, 0, cursor_at);
        cursor_down = 0;
        say("cursor up   panel %u,%u\n", (unsigned)(cursor_at & 0xFFFF), (unsigned)(cursor_at >> 16));
    } else if (cursor_down && at != cursor_at) {
        post(HWND_MAIN, WM_MOUSEMOVE, MK_LBUTTON, at);
        cursor_moves++;
    }
    cursor_at = at;
    r_was = r;
}

static void buttons_poll(void)
{
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1)
        return;
    static uint32_t raw_held, swallowed;
    uint32_t raw = pad.buttons, pressed = raw & ~raw_held;
    if ((pressed & SCE_CTRL_CIRCLE) && (rm == RM_OPEN || rm == RM_DRAG)) {
        int held = touch_held;
        ring_close(-1, "cancel circle");
        if (held)
            rm = RM_EAT;
    }
    if ((pressed & SCE_CTRL_START) && !(raw & SCE_CTRL_CIRCLE)) {
        outlines_on = !outlines_on;
        outline_redraw = 1;
        outline_asked++;
        say("start: outlines %s\n", outlines_on ? "on" : "off");
    }
    swallowed |= raw & SCE_CTRL_START;
#ifdef DIAGNOSTICS
    if ((raw & VSHOT_COMBO) == VSHOT_COMBO)
        swallowed |= VSHOT_COMBO;
    if (!!(raw & SCE_CTRL_LTRIGGER) != cursor_veiled) {
        cursor_veiled = !!(raw & SCE_CTRL_LTRIGGER);
        redraw = 1;
    }
#endif
    if (raw & SCE_CTRL_CIRCLE) {
        if (pressed & SCE_CTRL_LTRIGGER) {
            say("combo Circle+L: scaler\n");
            scale_asked++;
            swallowed |= SCE_CTRL_LTRIGGER;
        }
        if (pressed & SCE_CTRL_RTRIGGER) {
            say("combo Circle+R: fps\n");
            fps_asked++;
            swallowed |= SCE_CTRL_RTRIGGER;
        }
        if (pressed & SCE_CTRL_START) {
            melee_mode = (melee_mode + 1) % MELEE_MODES;
            say("combo Circle+Start: melee %s\n", melee_keys[melee_mode]);
            melee_asked++;
        }
        if (pressed & SCE_CTRL_SELECT) {
            music_choose((music_set() + 1) % MUSIC_SETS);
            say("combo Circle+Select: music %s\n", music_keys[music_set()]);
            music_asked++;
        }
    }
    swallowed &= raw;
    raw_held = raw;
    pad.buttons = raw & ~swallowed;
    pad.buttons |= stick_dirs(pad.lx, pad.ly);
    uint32_t changed = pad.buttons ^ buttons_held;
    for (unsigned i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        if (!(changed & keymap[i].button))
            continue;
        if (pad.buttons & keymap[i].button) {
            post(HWND_MAIN, WM_KEYDOWN, keymap[i].vk, 1);
            key_downs++;
        } else {
            post(HWND_MAIN, WM_KEYUP, keymap[i].vk, 0xC0000001u);
        }
    }
    buttons_held = pad.buttons;
    cursor_poll(&pad);
}

#define SETTINGS_PATH DATA_DIR "/settings.txt"
static int map_scroll_done;

void win_map_scroll_once(void)
{
    char buf[512];
    int n = 0;
    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
    }
    buf[n > 0 ? n : 0] = 0;
    if (strstr(buf, "map_scroll_once=done"))
        return;
    uint32_t cfg[4];
    const char *did = "no config";
    fd = sceIoOpen(DATA_DIR "/PalmHeroes.cfg", SCE_O_RDWR, 0);
    if (fd >= 0) {
        did = "kept";
        if (sceIoRead(fd, cfg, sizeof(cfg)) == (int)sizeof(cfg) && cfg[0] == 0xA173B7C3u && cfg[3] == 2) {
            uint32_t vfast = 4;
            sceIoLseek(fd, 12, SCE_SEEK_SET);
            did = sceIoWrite(fd, &vfast, 4) == 4 ? "normal to very fast" : "write failed";
        }
        sceIoClose(fd);
    }
    fd = sceIoOpen(SETTINGS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, "map_scroll_once=done\n", 21);
        sceIoClose(fd);
    }
    map_scroll_done = 1;
    say("settings map scroll once: %s\n", did);
}

static void settings_load(void)
{
    char buf[512];
    int n = 0;
    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
    }
    buf[n > 0 ? n : 0] = 0;
    for (char *line = buf, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r \t")] = 0;
        if (!strcmp(line, "scaler")) {
            for (int m = 0; m < SCALE_MODES; m++)
                if (!strcmp(v, scale_names[m]) && (m <= SCALE_SHARP || shader_for(m)))
                    set_scale(m);
        } else if (!strcmp(line, "fps")) {
            fps30 = !strcmp(v, "30");
        } else if (!strcmp(line, "melee")) {
            for (int m = 0; m < MELEE_MODES; m++)
                if (!strcmp(v, melee_keys[m]))
                    melee_mode = m;
        } else if (!strcmp(line, "music")) {
            for (int m = 0; m < MUSIC_SETS; m++)
                if (!strcmp(v, music_keys[m]))
                    music_choose(m);
        } else if (!strcmp(line, "map_scroll_once")) {
            map_scroll_done = !strcmp(v, "done");
        }
    }
    say("settings %s: scaler=%s fps=%d melee=%s\n", fd >= 0 ? "read" : "defaults", scale_names[scale_mode],
        fps30 ? 30 : 60, melee_keys[melee_mode]);
    settings_save();
}

static void settings_save(void)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "scaler=%s\nfps=%d\nmelee=%s\nmusic=%s\n%s", scale_names[scale_mode],
                     fps30 ? 30 : 60, melee_keys[melee_mode], music_keys[music_set()],
                     map_scroll_done ? "map_scroll_once=done\n" : "");
    SceUID fd = sceIoOpen(SETTINGS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, (SceSize)n);
        sceIoClose(fd);
    }
}

int n_PeekMessageW(Msg *m, uint32_t h, uint32_t lo, uint32_t hi, const uint32_t *stk)
{
    CALLED(PeekMessageW);
    (void)h; (void)lo; (void)hi;
    touch_poll();
    int r = take(m, stk[0] & 1);
    return r;
}

int n_GetMessageW(Msg *m, uint32_t h, uint32_t lo, uint32_t hi)
{
    CALLED(GetMessageW);
    (void)h; (void)lo; (void)hi;
    touch_poll();
    while (!take(m, 1)) {
        sceKernelDelayThread(10 * 1000);
        touch_poll();
    }
    return m->message != WM_QUIT;
}

uint32_t n_DispatchMessageW(const Msg *m)
{
    CALLED(DispatchMessageW);
    if (m->hwnd != HWND_MAIN || !wndproc)
        return 0;
    return wndproc(m->hwnd, m->message, m->wparam, m->lparam);
}

void n_PostQuitMessage(int code)
{
    CALLED(PostQuitMessage);
    say("win   PostQuitMessage %d\n", code);
    post_to((uint32_t)sceKernelGetThreadId(), 0, WM_QUIT, (uint32_t)code, 0);
}

uint32_t n_BeginPaint(uint32_t h, uint32_t *ps)
{
    CALLED(BeginPaint);
    (void)h;
    memset(ps, 0, 64);
    ps[0] = HDC_SCREEN;
    ps[4] = FB_W; ps[5] = FB_H;
    return HDC_SCREEN;
}

int n_EndPaint(uint32_t h, const void *ps) { CALLED(EndPaint); (void)h; (void)ps; return 1; }
uint32_t n_GetDC(uint32_t h) { CALLED(GetDC); (void)h; return HDC_SCREEN; }
int n_ReleaseDC(uint32_t h, uint32_t dc) { CALLED(ReleaseDC); (void)h; (void)dc; return 1; }

#define GETRAWFRAMEBUFFER 0x00020001u
int n_ExtEscape(uint32_t dc, uint32_t esc, uint32_t cb_in, const void *in, const uint32_t *stk)
{
    CALLED(ExtEscape);
    (void)dc; (void)cb_in; (void)in;
    uint32_t cb_out = stk[0];
    uint8_t *out = (uint8_t *)(uintptr_t)stk[1];
    int r = 0;
    if (esc == GETRAWFRAMEBUFFER && out && cb_out >= 24 && (guest_fb || display_start() == 0)) {
        uint16_t fmt = 1, bpp = 16;
        uint32_t v[5] = {(uint32_t)(uintptr_t)guest_fb, 2, FB_W * 2, FB_W, FB_H};
        memcpy(out, &fmt, 2);
        memcpy(out + 2, &bpp, 2);
        memcpy(out + 4, v, 20);
        r = 24;
    }
    say("disp  ExtEscape 0x%08x cbOut=%u -> %d\n", (unsigned)esc, (unsigned)cb_out, r);
    return r;
}

int n_ChangeDisplaySettingsEx(const void *name, const void *dm, uint32_t h, uint32_t flags)
{
    CALLED(ChangeDisplaySettingsEx);
    (void)name; (void)dm; (void)h;
    if (flags != 2)
        portrait = 1;
    say("disp  ChangeDisplaySettingsEx flags=0x%08x portrait=%d\n", (unsigned)flags, portrait);
    return 0;
}

int n_GetSystemMetrics(int i)
{
    CALLED(GetSystemMetrics);
    if (i == 0)
        return portrait ? FB_W : FB_H;
    if (i == 1)
        return portrait ? FB_H : FB_W;
    return 0;
}

uint32_t n_LoadCursorW(uint32_t inst, uint32_t id) { CALLED(LoadCursorW); (void)inst; (void)id; return 0x00F70001u; }
uint32_t n_CreateSolidBrush(uint32_t c) { CALLED(CreateSolidBrush); (void)c; return 0x00F70002u; }
int n_DeleteObject(uint32_t o) { CALLED(DeleteObject); (void)o; return 1; }
int n_DeleteDC(uint32_t d) { CALLED(DeleteDC); (void)d; return 1; }
uint32_t n_SetCapture(uint32_t h) { CALLED(SetCapture); (void)h; return 0; }
int n_ReleaseCapture(void) { CALLED(ReleaseCapture); return 1; }

static char modules[8][64];
static unsigned n_modules;

uint32_t n_LoadLibraryW(const wchar16 *name)
{
    CALLED(LoadLibraryW);
    char a[64];
    to_ascii(name, a, sizeof(a));
    unsigned i = 0;
    while (i < n_modules && strcasecmp(modules[i], a) != 0)
        i++;
    if (i == n_modules && n_modules < 8)
        snprintf(modules[n_modules++], sizeof(modules[0]), "%s", a);
    say("mod   LoadLibraryW \"%s\" -> 0x%08x\n", a, 0x10000000u + i);
    return 0x10000000u + i;
}

uint32_t n_GetProcAddressW(uint32_t mod, const wchar16 *name)
{
    CALLED(GetProcAddressW);
    char a[96];
    to_ascii(name, a, sizeof(a));
    void *fn = native_for(a);
    uint32_t r = fn ? (uint32_t)(uintptr_t)fn : trap_for_name(a);
    say("mod   GetProcAddressW 0x%08x \"%s\" -> 0x%08x%s\n", (unsigned)mod, a, (unsigned)r, fn ? " native" : " trap");
    return r;
}

int n_FreeLibrary(uint32_t mod) { CALLED(FreeLibrary); (void)mod; return 1; }
int n_SetKMode(int mode) { CALLED(SetKMode); (void)mode; return 1; }
void n_SystemIdleTimerReset(void) { CALLED(SystemIdleTimerReset); }
int n_SHFullScreen(uint32_t h, uint32_t state) { CALLED(SHFullScreen); (void)h; (void)state; return 1; }

int n_GXOpenInput(void) { CALLED(GXOpenInput); return 1; }
int n_GXCloseInput(void) { CALLED(GXCloseInput); return 1; }

void *n_GXGetDefaultKeys(uint8_t *sret, int mode)
{
    CALLED(GXGetDefaultKeys);
    (void)mode;
    static const uint16_t vk[8] = {0x26, 0x28, 0x25, 0x27, 0xC1, 0xC2, 0xC3, 0xC4};
    memset(sret, 0, 0x60);
    for (int i = 0; i < 8; i++)
        memcpy(sret + 12 * i, &vk[i], 2);
    return sret;
}

int n_GetVersionExW(uint32_t *v)
{
    CALLED(GetVersionExW);
    v[1] = 5; v[2] = 2; v[3] = 19208; v[4] = 3;
    return 1;
}

void n_GetSystemInfo(uint8_t *p)
{
    CALLED(GetSystemInfo);
    uint32_t v[9] = {5, 4096, 0x10000, 0x7FFFFFFF, 1, 1, 2577, 65536, 4};
    memset(p, 0, 36);
    memcpy(p, v, 4);
    memcpy(p + 4, v + 1, 7 * 4);
    memcpy(p + 32, &v[8], 2);
}

void n_GlobalMemoryStatus(uint32_t *m)
{
    CALLED(GlobalMemoryStatus);
    uint32_t v[8] = {32, 20, 32u << 20, 24u << 20, 32u << 20, 24u << 20, 64u << 20, 48u << 20};
    memcpy(m, v, sizeof(v));
}

int n_GetLocaleInfoW(uint32_t lcid, uint32_t type, wchar16 *buf, int cap)
{
    CALLED(GetLocaleInfoW);
    (void)lcid;
    const char *s = type == 3 ? "ENU" : "1";
    put_w(buf, (uint32_t)cap, s);
    return (int)strlen(s) + 1;
}

int n_SystemParametersInfoW(uint32_t action, uint32_t cap, wchar16 *buf, uint32_t ini)
{
    CALLED(SystemParametersInfoW);
    (void)ini;
    if (action == 0x101)
        put_w(buf, cap / 2, "PocketPC");
    else if (action == 0x102)
        put_w(buf, cap / 2, "PS Vita");
    else
        return 0;
    return 1;
}

int n__wcsicmp(const wchar16 *a, const wchar16 *b)
{
    CALLED(_wcsicmp);
    for (;; a++, b++) {
        wchar16 x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y || !x)
            return (int)x - (int)y;
    }
}

void n_GetLocalTime(uint16_t *st)
{
    CALLED(GetLocalTime);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    uint16_t v[8] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday,
                     (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, 0};
    memcpy(st, v, sizeof(v));
}

uint32_t n_GetTickCount(void)
{
    CALLED(GetTickCount);
    static unsigned vbase;
    static uint32_t tbase;
    static int started;
    unsigned vc = sceDisplayGetVcount();
    if ((uint32_t)sceKernelGetThreadId() == queues[0].tid) {
        main_read_vc = vc;
    }
    if (!started) {
        vbase = vc;
        tbase = (uint32_t)(sceKernelGetProcessTimeWide() / 1000);
        started = 1;
    }
    uint32_t t = tbase + (uint32_t)((uint64_t)(vc - vbase) * 1000 / 60);
    return t;
}

void n_Sleep(uint32_t ms)
{
    CALLED(Sleep);
    static uint64_t woke;
    int main_thread = (uint32_t)sceKernelGetThreadId() == queues[0].tid;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    if (main_thread && (compose_seen || (woke && t0 - woke > 2000))) {
        paint_vc = compose_seen ? compose_vc : main_read_vc;
        painting = 0;
        sceKernelSignalSema(paint_done, 1);
    }
    sceKernelDelayThread(ms ? ms * 1000 : 100);
    if (main_thread) {
        woke = sceKernelGetProcessTimeWide();
        compose_seen = 0;
    }
}

static uint32_t next_handle = 0x00F50001u;
uint32_t n_CreateMutexW(void *sa, int own, const wchar16 *name) { CALLED(CreateMutexW); (void)sa; (void)own; (void)name; return next_handle++; }
int n_ReleaseMutex(uint32_t h) { CALLED(ReleaseMutex); (void)h; return 1; }
uint32_t n_WaitForSingleObject(uint32_t h, uint32_t ms) { CALLED(WaitForSingleObject); (void)h; (void)ms; return 0; }
uint32_t n_CreateEventW(void *sa, int manual, int init, const wchar16 *name) { CALLED(CreateEventW); (void)sa; (void)manual; (void)init; (void)name; return next_handle++; }
int n_EventModify(uint32_t h, uint32_t f) { CALLED(EventModify); (void)h; (void)f; return 1; }
static SceUID cs_mutex(uint32_t *cs)
{
    if (!cs[0])
        cs[0] = (uint32_t)sceKernelCreateMutex("ph-cs", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    return (SceUID)cs[0];
}
void n_InitializeCriticalSection(uint32_t *cs) { CALLED(InitializeCriticalSection); memset(cs, 0, 20); cs_mutex(cs); }
void n_EnterCriticalSection(uint32_t *cs) { CALLED(EnterCriticalSection); sceKernelLockMutex(cs_mutex(cs), 1, NULL); }
void n_LeaveCriticalSection(uint32_t *cs) { CALLED(LeaveCriticalSection); sceKernelUnlockMutex(cs_mutex(cs), 1); }
int32_t n_InterlockedCompareExchange(int32_t *p, int32_t x, int32_t cmp)
{
    CALLED(InterlockedCompareExchange);
    int32_t old = *p;
    if (old == cmp)
        *p = x;
    return old;
}

static int guest_thread(SceSize args, void *argp)
{
    (void)args;
    const uint32_t *a = argp;
    uint32_t r = ((uint32_t (*)(uint32_t))(uintptr_t)a[0])(a[1]);
    say("thrd  0x%08x returned %u\n", (unsigned)a[0], (unsigned)r);
    return 0;
}

static struct { SceUID uid; uint32_t start, param; int started; } threads[4];

uint32_t n_CreateThread(void *sa, uint32_t stack, uint32_t start, uint32_t param, const uint32_t *stk)
{
    CALLED(CreateThread);
    (void)sa; (void)stack;
    q_init();
    uint32_t flags = stk[0], *id = (uint32_t *)(uintptr_t)stk[1];
    unsigned i = 0;
    while (i < 4 && threads[i].uid > 0)
        i++;
    SceUID uid = i < 4 ? sceKernelCreateThread("ph-guest-thread", guest_thread, 165, 0x40000, 0, 0, NULL) : -1;
    cpu_track("guest2", uid);
    if (uid >= 0) {
        threads[i].uid = uid; threads[i].start = start; threads[i].param = param;
        if (!(flags & 4)) {
            sceKernelStartThread(uid, 8, &threads[i].start);
            threads[i].started = 1;
        }
    }
    if (id)
        *id = (uint32_t)uid;
    say("thrd  CreateThread start=0x%08x param=0x%08x flags=0x%x -> 0x%08x\n", (unsigned)start, (unsigned)param,
        (unsigned)flags, (unsigned)uid);
    return uid >= 0 ? (uint32_t)uid : 0;
}

uint32_t n_ResumeThread(uint32_t h)
{
    CALLED(ResumeThread);
    for (unsigned i = 0; i < 4; i++)
        if (threads[i].uid == (SceUID)h && !threads[i].started) {
            threads[i].started = 1;
            sceKernelStartThread(threads[i].uid, 8, &threads[i].start);
            say("thrd  ResumeThread 0x%08x started\n", (unsigned)h);
            return 1;
        }
    return 0;
}

#define HWAVEOUT 0x00F80001u
#define MM_WOM_DONE 0x3BD
#define WHDR_DONE 0x01
#define WHDR_PREPARED 0x02
#define WHDR_INQUEUE 0x10
#define GRAIN 512
typedef struct { uint8_t *data; uint32_t length, recorded, user, flags, loops, next, reserved; } WaveHdr;
static WaveHdr *wave_fifo[16];
static unsigned wave_head, wave_tail, wave_pos;
static uint32_t wave_tid, wave_rate = 22050, wave_ch = 1;
static SceUID wave_lock = -1, wave_thread = -1;
static unsigned wave_played, wave_underruns;

static int wave_out(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, wave_rate,
                                   wave_ch == 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO);
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    if (port >= 0)
        sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    say("audio port=0x%08x rate=%u ch=%u grain=%u\n", (unsigned)port, (unsigned)wave_rate, (unsigned)wave_ch, GRAIN);
    if (port < 0)
        return 0;
    static int16_t grains[2][GRAIN * 2] __attribute__((aligned(64)));
    int16_t *grain = grains[0];
    const uint32_t bytes = GRAIN * wave_ch * 2;
    for (;;) {
        grain = grain == grains[0] ? grains[1] : grains[0];
        uint32_t o = 0;
        sceKernelLockMutex(wave_lock, 1, NULL);
        while (o < bytes && wave_head != wave_tail) {
            WaveHdr *h = wave_fifo[wave_head % 16];
            uint32_t n = h->length - wave_pos < bytes - o ? h->length - wave_pos : bytes - o;
            memcpy((uint8_t *)grain + o, h->data + wave_pos, n);
            o += n;
            wave_pos += n;
            if (wave_pos >= h->length) {
                wave_head++;
                wave_pos = 0;
                h->flags = (h->flags & ~WHDR_INQUEUE) | WHDR_DONE;
                wave_played++;
                post_to(wave_tid, 0, MM_WOM_DONE, HWAVEOUT, (uint32_t)(uintptr_t)h);
            }
        }
        sceKernelUnlockMutex(wave_lock, 1);
        if (o < bytes) {
            memset((uint8_t *)grain + o, 0, bytes - o);
            wave_underruns++;
        }
        sceAudioOutOutput(port, grain);
    }
    return 0;
}

uint32_t n_waveOutOpen(uint32_t *out, uint32_t dev, const uint16_t *fmt, uint32_t cb)
{
    CALLED(waveOutOpen);
    if (out)
        *out = HWAVEOUT;
    if (cb) {
        wave_tid = cb;
        wave_ch = fmt[1];
        wave_rate = fmt[2] | fmt[3] << 16;
    }
    say("audio waveOutOpen dev=0x%08x fmt=%u ch=%u rate=%u bits=%u callback=0x%08x -> 0\n",
        (unsigned)dev, fmt[0], fmt[1], (unsigned)(fmt[2] | fmt[3] << 16), fmt[7], (unsigned)cb);
    return 0;
}

uint32_t n_waveOutPrepareHeader(uint32_t h, WaveHdr *hdr, uint32_t cb)
{
    CALLED(waveOutPrepareHeader);
    (void)h; (void)cb;
    hdr->flags |= WHDR_PREPARED;
    return 0;
}

uint32_t n_waveOutWrite(uint32_t h, WaveHdr *hdr, uint32_t cb)
{
    CALLED(waveOutWrite);
    (void)h; (void)cb;
    if (wave_lock < 0) {
        wave_lock = sceKernelCreateMutex("ph-wave", 0, 0, NULL);
        wave_thread = sceKernelCreateThread("ph-waveout", wave_out, 64, 0x4000, 0, 0, NULL);
        cpu_track("waveout", wave_thread);
        sceKernelStartThread(wave_thread, 0, NULL);
    }
    sceKernelLockMutex(wave_lock, 1, NULL);
    hdr->flags = (hdr->flags & ~WHDR_DONE) | WHDR_INQUEUE;
    if (wave_tail - wave_head < 16)
        wave_fifo[wave_tail++ % 16] = hdr;
    sceKernelUnlockMutex(wave_lock, 1);
    return 0;
}

uint32_t n_waveOutReset(uint32_t h)
{
    CALLED(waveOutReset);
    (void)h;
    if (wave_lock < 0)
        return 0;
    sceKernelLockMutex(wave_lock, 1, NULL);
    for (; wave_head != wave_tail; wave_head++) {
        WaveHdr *w = wave_fifo[wave_head % 16];
        w->flags = (w->flags & ~WHDR_INQUEUE) | WHDR_DONE;
    }
    wave_pos = 0;
    sceKernelUnlockMutex(wave_lock, 1);
    return 0;
}

uint32_t n_waveOutUnprepareHeader(uint32_t h, WaveHdr *hdr, uint32_t cb)
{
    CALLED(waveOutUnprepareHeader);
    (void)h; (void)cb;
    hdr->flags &= ~WHDR_PREPARED;
    return 0;
}

uint32_t n_waveOutClose(uint32_t h) { CALLED(waveOutClose); (void)h; return 0; }

void n_OutputDebugStringW(const wchar16 *w)
{
#ifndef DIAGNOSTICS
    (void)w;
#else
    CALLED(OutputDebugStringW);
    static unsigned lines;
    if (++lines > 300)
        return;
    char a[160];
    to_ascii(w, a, sizeof(a));
    for (char *c = a; *c; c++)
        if (*c == '\n' || *c == '\r')
            *c = ' ';
    say("dbg   %s\n", a);
#endif
}

int n_vsprintf(char *buf, const char *fmt, void *ap)
{
    CALLED(vsprintf);
    va_list v;
    memcpy(&v, &ap, sizeof(ap));
    return vsprintf(buf, fmt, v);
}

void *n_fopen(const char *name, const char *mode)
{
    CALLED(fopen);
#ifndef DIAGNOSTICS
    if (strcasecmp(name, "\\PHeroes.txt") == 0)
        return NULL;
#endif
    char host[600];
    guest_path_to_host(name, strpbrk(mode, "wa+") != NULL, host, sizeof(host));
    FILE *f = fopen(host, mode);
    return f;
}

int n_fprintf(FILE *f, const char *fmt, uint32_t a, uint32_t b)
{
    CALLED(fprintf);
    return f ? fprintf(f, fmt, a, b) : -1;
}

int n_fclose(FILE *f) { CALLED(fclose); return f ? fclose(f) : -1; }
