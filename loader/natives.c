#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <kubridge.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "loader.h"

#define MODULE_DIR "/Program Files/PalmHeroes"
#define MODULE_PATH "\\Program Files\\PalmHeroes\\hmmppc.exe"
#define PATH_CAP 512
#define HOST_CAP (PATH_CAP + sizeof(DATA_DIR) + 1)

#define NATIVES(X) X(new) X(delete) X(LocalAlloc) X(LocalReAlloc) X(LocalFree) X(LocalSize) \
    X(malloc) X(free) X(VirtualAlloc) X(VirtualFree) X(memchr) X(memcmp) X(memcpy) X(memmove) X(memset) \
    X(CreateFileW) X(ReadFile) X(WriteFile) X(SetFilePointer) X(GetFileSize) X(CloseHandle) \
    X(FlushFileBuffers) X(FindFirstFileW) X(FindNextFileW) X(FindClose) X(GetFileAttributesW) \
    X(DeleteFileW) X(MoveFileW) X(CreateDirectoryW) X(GetModuleFileNameW) X(GetLastError) \
    X(wcschr) X(wcsrchr) X(wcsstr) X(_wcsupr) X(_vsnwprintf) X(rand) X(GetCurrentFT) X(CeGetRandomSeed) \
    X(strlen) X(wcstoul) X(WideCharToMultiByte) X(RegOpenKeyExW) X(FileTimeToSystemTime) X(SystemTimeToFileTime) \
    X(__adds) X(__subs) X(__muls) X(__divs) X(__addd) X(__subd) X(__muld) X(__divd) \
    X(__stod) X(__dtos) X(__stoi) X(__utos) X(__itos) X(__dtoi) X(__dtou) X(__itod) X(__utod) \
    X(__ges) X(__gts) X(__lts) X(__rt_sdiv) X(__rt_udiv) X(__rt_sdiv64by64) X(__rt_udiv64by64) \
    X(__rt_urem64by64) X(sin) X(cos) X(sqrt)

#define ENUM(n) I_##n,
enum { NATIVES(ENUM) N_NATIVES };
#define EXTERN(n) extern char ce_##n[];
NATIVES(EXTERN)
#define THUNK(n) ce_##n,
static void *const thunks[N_NATIVES] = {NATIVES(THUNK)};
#define NAME(n) #n,
static const char *const names[N_NATIVES] = {NATIVES(NAME)};
#if defined(DIAGNOSTICS)
const char *volatile last_native = "";
#define CALLED(n) (last_native = #n)
#else
#define CALLED(n) ((void)0)
static const char *const last_native = "";
#endif
enum { IO_PATH, IO_OPEN, IO_LOG, IO_READ, IO_CLOSE, IO_FIND, IO_N };

static const struct { const char *import; int native; } aliases[] = {
    {"??2@YAPAXI@Z", I_new}, {"??_U@YAPAXI@Z", I_new}, {"??3@YAXPAX@Z", I_delete}, {"??_V@YAXPAX@Z", I_delete},
};

void *win_native_for(const char *name);

void *native_for(const char *name)
{
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (strcmp(name, aliases[i].import) == 0)
            return thunks[aliases[i].native];
    for (unsigned i = 0; i < N_NATIVES; i++)
        if (strcmp(name, names[i]) == 0)
            return thunks[i];
    return win_native_for(name);
}

#define MEM(k, n) ((void)0)

#define LMEM_ZEROINIT 0x40u
int _newlib_heap_size_user = 128 * 1024 * 1024;

static int ours(void *p, const char *who, void *lr)
{
    uintptr_t a = (uintptr_t)p;
    if (!p || (a >= 0x81000000u && a < 0x90000000u && !(a & 7)))
        return 1;
    say("heap  %s(0x%08x) not ours, skipped; guest lr=0x%08x\n", who, (unsigned)a, (unsigned)(uintptr_t)lr);
    return 0;
}

void __real__exit(int code);
void __wrap__exit(int code)
{
    say("exit  _exit(%d) from 0x%08x last_native=%s\n", code, (unsigned)(uintptr_t)__builtin_return_address(0),
        last_native);
    __real__exit(code);
}

void abort(void)
{
    say("exit  abort() from 0x%08x last_native=%s\n", (unsigned)(uintptr_t)__builtin_return_address(0), last_native);
    sceKernelExitProcess(1);
    for (;;)
        ;
}

void *n_new(size_t n) { CALLED(new); return malloc(n ? n : 1); }
void n_delete(void *p) { CALLED(delete); if (ours(p, "delete", __builtin_return_address(0))) free(p); }
void *n_LocalAlloc(uint32_t flags, size_t n)
{
    CALLED(LocalAlloc);
    return flags & LMEM_ZEROINIT ? calloc(1, n ? n : 1) : malloc(n ? n : 1);
}
void *n_LocalReAlloc(void *p, size_t n, uint32_t flags)
{
    CALLED(LocalReAlloc);
    size_t old = p ? malloc_usable_size(p) : 0;
    void *q = realloc(p, n ? n : 1);
    if (q && (flags & LMEM_ZEROINIT) && n > old)
        memset((char *)q + old, 0, n - old);
    return q;
}
void *n_LocalFree(void *p) { CALLED(LocalFree); if (ours(p, "LocalFree", __builtin_return_address(0))) free(p); return NULL; }
size_t n_LocalSize(void *p) { CALLED(LocalSize); return p ? malloc_usable_size(p) : 0; }
void *n_malloc(size_t n) { CALLED(malloc); return malloc(n); }
void n_free(void *p) { CALLED(free); if (ours(p, "free", __builtin_return_address(0))) free(p); }
#define VPOOL_BASE 0x98000000u
#define VPOOL_SIZE 0x00400000u
static uint32_t vpool_used;
static int vpool_ready;

static int vpool_init(void)
{
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = 0x1;
    opt.field_C = VPOOL_BASE;
    SceUID uid = kuKernelAllocMemBlock("ph-virtual", 0x0C20D050u , VPOOL_SIZE, &opt);
    int pr = uid >= 0 ? kuKernelMemProtect((void *)VPOOL_BASE, VPOOL_SIZE,
                                           KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC) : -1;
    say("virt  pool uid=0x%08x protect=0x%08x\n", (unsigned)uid, (unsigned)pr);
    vpool_ready = uid >= 0 && pr >= 0 ? 1 : -1;
    return vpool_ready;
}

void *n_VirtualAlloc(void *addr, uint32_t size, uint32_t type, uint32_t prot)
{
    CALLED(VirtualAlloc);
    uint32_t n = (size + 0xFFFu) & ~0xFFFu;
    void *r = NULL;
    if (addr == NULL && (vpool_ready == 1 || (vpool_ready == 0 && vpool_init() == 1)) && vpool_used + n <= VPOOL_SIZE) {
        r = (void *)(uintptr_t)(VPOOL_BASE + vpool_used);
        vpool_used += n;
        memset(r, 0, n);
    }
    say("virt  VirtualAlloc(%p, 0x%x, 0x%x, 0x%x) -> %p\n", addr, (unsigned)size, (unsigned)type, (unsigned)prot, r);
    return r;
}

int n_VirtualFree(void *addr, uint32_t size, uint32_t type)
{
    CALLED(VirtualFree);
    say("virt  VirtualFree(%p, 0x%x, 0x%x)\n", addr, (unsigned)size, (unsigned)type);
    return 1;
}

void *n_memchr(const void *s, int c, size_t n) { CALLED(memchr); return memchr(s, c, n); }
int n_memcmp(const void *a, const void *b, size_t n) { CALLED(memcmp); return memcmp(a, b, n); }
void *n_memcpy(void *d, const void *s, size_t n) { CALLED(memcpy); MEM(M_CPY, n); return memcpy(d, s, n); }
void *n_memmove(void *d, const void *s, size_t n) { CALLED(memmove); MEM(M_MOVE, n); return memmove(d, s, n); }
void *n_memset(void *d, int c, size_t n) { CALLED(memset); MEM(M_SET, n); return memset(d, c, n); }

#define SOFT __attribute__((pcs("aapcs")))

SOFT float n___adds(float a, float b) { CALLED(__adds); return a + b; }
SOFT float n___subs(float a, float b) { CALLED(__subs); return a - b; }
SOFT float n___muls(float a, float b) { CALLED(__muls); return a * b; }
SOFT float n___divs(float a, float b) { CALLED(__divs); return a / b; }
SOFT double n___addd(double a, double b) { CALLED(__addd); return a + b; }
SOFT double n___subd(double a, double b) { CALLED(__subd); return a - b; }
SOFT double n___muld(double a, double b) { CALLED(__muld); return a * b; }
SOFT double n___divd(double a, double b) { CALLED(__divd); return a / b; }
SOFT double n___stod(float a) { CALLED(__stod); return a; }
SOFT float n___dtos(double a) { CALLED(__dtos); return (float)a; }
SOFT int32_t n___stoi(float a) { CALLED(__stoi); return (int32_t)a; }
SOFT float n___utos(uint32_t a) { CALLED(__utos); return (float)a; }
SOFT float n___itos(int32_t a) { CALLED(__itos); return (float)a; }
SOFT int32_t n___dtoi(double a) { CALLED(__dtoi); return (int32_t)a; }
SOFT uint32_t n___dtou(double a) { CALLED(__dtou); return (uint32_t)a; }
SOFT double n___itod(int32_t a) { CALLED(__itod); return a; }
SOFT double n___utod(uint32_t a) { CALLED(__utod); return a; }
SOFT int n___ges(float a, float b) { CALLED(__ges); return a >= b; }
SOFT int n___gts(float a, float b) { CALLED(__gts); return a > b; }
SOFT int n___lts(float a, float b) { CALLED(__lts); return a < b; }
SOFT double n_sin(double a) { CALLED(sin); return sin(a); }
SOFT double n_cos(double a) { CALLED(cos); return cos(a); }
SOFT double n_sqrt(double a) { CALLED(sqrt); return sqrt(a); }

uint64_t n___rt_sdiv(int32_t d, int32_t n)
{
    CALLED(__rt_sdiv);
    if (d == 0) {
        say("div   __rt_sdiv(%d / 0)\n", (int)n);
        return 0;
    }
    int32_t q = d == -1 ? (int32_t)(0u - (uint32_t)n) : n / d, r = d == -1 ? 0 : n % d;
    return (uint64_t)(uint32_t)r << 32 | (uint32_t)q;
}
uint64_t n___rt_udiv(uint32_t d, uint32_t n)
{
    CALLED(__rt_udiv);
    if (d == 0) {
        say("div   __rt_udiv(%u / 0)\n", (unsigned)n);
        return 0;
    }
    return (uint64_t)(n % d) << 32 | (n / d);
}
int64_t n___rt_sdiv64by64(int64_t n, int64_t d)
{
    CALLED(__rt_sdiv64by64);
    if (d == 0) {
        say("div   __rt_sdiv64by64(/ 0)\n");
        return 0;
    }
    return d == -1 ? (int64_t)(0u - (uint64_t)n) : n / d;
}
uint64_t n___rt_udiv64by64(uint64_t n, uint64_t d)
{
    CALLED(__rt_udiv64by64);
    if (d == 0) {
        say("div   __rt_udiv64by64(/ 0)\n");
        return 0;
    }
    return n / d;
}
uint64_t n___rt_urem64by64(uint64_t n, uint64_t d)
{
    CALLED(__rt_urem64by64);
    if (d == 0) {
        say("div   __rt_urem64by64(/ 0)\n");
        return 0;
    }
    return n % d;
}

typedef uint16_t wchar16;

uint16_t *n_wcschr(const wchar16 *s, uint32_t c)
{
    CALLED(wcschr);
    for (;; s++) {
        if (*s == (wchar16)c)
            return (uint16_t *)s;
        if (!*s)
            return NULL;
    }
}

uint16_t *n_wcsrchr(const wchar16 *s, uint32_t c)
{
    CALLED(wcsrchr);
    const wchar16 *last = NULL;
    for (;; s++) {
        if (*s == (wchar16)c)
            last = s;
        if (!*s)
            return (uint16_t *)last;
    }
}

uint16_t *n_wcsstr(const wchar16 *s, const wchar16 *sub)
{
    CALLED(wcsstr);
    for (;; s++) {
        const wchar16 *a = s, *b = sub;
        while (*b && *a == *b)
            a++, b++;
        if (!*b)
            return (uint16_t *)s;
        if (!*s)
            return NULL;
    }
}

uint16_t *n__wcsupr(wchar16 *s)
{
    CALLED(_wcsupr);
    for (wchar16 *c = s; *c; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 0xE0 && *c <= 0xFE && *c != 0xF7) || (*c >= 0x430 && *c <= 0x44F))
            *c -= 0x20;
        else if (*c >= 0x450 && *c <= 0x45F)
            *c -= 0x50;
    }
    return s;
}

uint32_t n_strlen(const char *s)
{
    CALLED(strlen);
    return strlen(s);
}

uint32_t n_wcstoul(const wchar16 *s, wchar16 **end, int base)
{
    CALLED(wcstoul);
    char a[64];
    unsigned i = 0;
    for (; s[i] && s[i] < 0x80 && i + 1 < sizeof(a); i++)
        a[i] = (char)s[i];
    a[i] = 0;
    char *e;
    unsigned long v = strtoul(a, &e, base);
    if (end)
        *end = (wchar16 *)(s + (e - a));
    return (uint32_t)v;
}

static void to_rel(char *s);
static void read_path(const char *rel, char *host, size_t cap);

int n_WideCharToMultiByte(uint32_t cp, uint32_t flags, const wchar16 *w, int32_t cch, const uint32_t *stk)
{
    CALLED(WideCharToMultiByte);
    (void)cp; (void)flags;
    char *out = (char *)(uintptr_t)stk[0];
    int32_t cb = (int32_t)stk[1];
    if (cch < 0) {
        cch = 0;
        while (w[cch++])
            ;
    }
    if (cb == 0)
        return cch;
    if (cch > cb)
        return 0;
    for (int32_t i = 0; i < cch; i++)
        out[i] = w[i] < 0x100 ? (char)w[i] : '?';
    int32_t n = cch > 0 && out[cch - 1] == 0 ? cch - 1 : cch;
    if (n > 10 && n < PATH_CAP && strncasecmp(out + n - 4, ".mp3", 4) == 0) {
        char rel[PATH_CAP], host[HOST_CAP];
        memcpy(rel, out, n);
        rel[n] = 0;
        for (char *c = rel; *c; c++)
            if (*c == '\\')
                *c = '/';
        to_rel(rel);
        if (strncasecmp(rel, "Music/", 6) == 0) {
            read_path(rel, host, sizeof(host));
            music_play(host);
        }
    }
    return cch;
}

int32_t n_RegOpenKeyExW(uint32_t key, const wchar16 *sub, uint32_t opts, uint32_t sam)
{
    CALLED(RegOpenKeyExW);
    (void)key; (void)sub; (void)opts; (void)sam;
    return 2;
}

static unsigned wide_fmt_logged;

int n__vsnwprintf(wchar16 *buf, uint32_t n, const wchar16 *fmt, const uint32_t *ap)
{
    CALLED(_vsnwprintf);
    uint32_t o = 0;
#define PUT(ch) do { wchar16 c_ = (wchar16)(ch); if (o < n) buf[o] = c_; o++; } while (0)
    while (*fmt) {
        if (*fmt != '%') {
            PUT(*fmt++);
            continue;
        }
        fmt++;
        char spec[40];
        int sl = 0, left = 0, width = 0, prec = -1;
        spec[sl++] = '%';
        while (*fmt && strchr("-+ #0", (char)*fmt) && sl < 8) {
            left |= *fmt == '-';
            spec[sl++] = (char)*fmt++;
        }
        if (*fmt == '*') {
            width = (int32_t)*ap++;
            fmt++;
            if (width < 0) {
                left = 1;
                spec[sl++] = '-';
                width = -width;
            }
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = (int32_t)*ap++;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int size = 0;
        if (*fmt == 'h' || *fmt == 'w' || *fmt == 'l') {
            size = *fmt == 'h' ? 'h' : 'l';
            fmt++;
            if (size == 'l' && *fmt == 'l') {
                size = 'L';
                fmt++;
            }
        } else if (fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4') {
            size = 'L';
            fmt += 3;
        } else if (*fmt == 'L') {
            fmt++;
        }
        wchar16 conv = *fmt;
        if (!conv)
            break;
        fmt++;
        char tmp[80];
        const char *nar = NULL;
        const wchar16 *wid = NULL;
        wchar16 one;
        int len = 0, pad = 1;
        switch (conv) {
        case '%':
            PUT('%');
            continue;
        case 'c': case 'C':
            one = (wchar16)*ap++;
            if (conv == 'C' || size == 'h')
                one &= 0xFF;
            wid = &one;
            len = 1;
            break;
        case 's': case 'S': {
            uint32_t a = *ap++;
            int narrow = conv == 'S' ? size != 'l' : size == 'h';
            if (!a)
                nar = "(null)", narrow = 1;
            else if (narrow)
                nar = (const char *)(uintptr_t)a;
            else
                wid = (const wchar16 *)(uintptr_t)a;
            while ((prec < 0 || len < prec) && (narrow ? nar[len] : wid[len]))
                len++;
            break;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            sl += snprintf(spec + sl, sizeof(spec) - sl, width ? "%d" : "", width);
            if (prec >= 0)
                sl += snprintf(spec + sl, sizeof(spec) - sl, ".%d", prec);
            if (size == 'L') {
                uint64_t v = ap[0] | (uint64_t)ap[1] << 32;
                ap += 2;
                if (!wide_fmt_logged++)
                    say("fmt   first 64-bit %%%c: words 0x%08x 0x%08x\n", (char)conv, (unsigned)ap[-2], (unsigned)ap[-1]);
                snprintf(spec + sl, sizeof(spec) - sl, "ll%c", (char)conv);
                snprintf(tmp, sizeof(tmp), spec, v);
            } else {
                uint32_t v = *ap++;
                if (conv == 'p')
                    snprintf(tmp, sizeof(tmp), "%08X", (unsigned)v);
                else {
                    snprintf(spec + sl, sizeof(spec) - sl, "%s%c", size == 'h' ? "h" : "", (char)conv);
                    snprintf(tmp, sizeof(tmp), spec, v);
                }
            }
            nar = tmp;
            len = strlen(tmp);
            pad = 0;
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            double d;
            memcpy(&d, ap, 8);
            ap += 2;
            if (!wide_fmt_logged++)
                say("fmt   first %%%c: words 0x%08x 0x%08x\n", (char)conv, (unsigned)ap[-2], (unsigned)ap[-1]);
            sl += snprintf(spec + sl, sizeof(spec) - sl, width ? "%d" : "", width);
            if (prec >= 0)
                sl += snprintf(spec + sl, sizeof(spec) - sl, ".%d", prec);
            snprintf(spec + sl, sizeof(spec) - sl, "%c", (char)conv);
            snprintf(tmp, sizeof(tmp), spec, d);
            nar = tmp;
            len = strlen(tmp);
            pad = 0;
            break;
        }
        case 'n':
            *(int32_t *)(uintptr_t)*ap++ = (int32_t)o;
            continue;
        default:
            continue;
        }
        int fill = pad && width > len ? width - len : 0;
        for (; !left && fill; fill--)
            PUT(' ');
        for (int i = 0; i < len; i++)
            PUT(nar ? (uint8_t)nar[i] : wid[i]);
        for (; fill; fill--)
            PUT(' ');
    }
#undef PUT
    if (o < n) {
        buf[o] = 0;
        return (int)o;
    }
    return o == n ? (int)n : -1;
}

static uint32_t rand_seed = 1;

int n_rand(void)
{
    CALLED(rand);
    rand_seed = rand_seed * 214013u + 2531011u;
    return (int)(rand_seed >> 16 & 0x7FFF);
}

void n_GetCurrentFT(uint32_t *ft)
{
    CALLED(GetCurrentFT);
    uint64_t t = (uint64_t)time(NULL) * 10000000u + 116444736000000000ull;
    ft[0] = (uint32_t)t;
    ft[1] = (uint32_t)(t >> 32);
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 584694;
}

int n_FileTimeToSystemTime(const uint32_t *ft, uint16_t *st)
{
    CALLED(FileTimeToSystemTime);
    uint64_t t = ft[0] | (uint64_t)ft[1] << 32;
    uint64_t ms = t / 10000, days = ms / 86400000;
    uint32_t in_day = (uint32_t)(ms % 86400000);
    int64_t z = (int64_t)days + 584694;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = (int64_t)yoe + era * 400 + (m <= 2);
    st[0] = (uint16_t)y; st[1] = (uint16_t)m; st[2] = (uint16_t)((days + 1) % 7); st[3] = (uint16_t)d;
    st[4] = (uint16_t)(in_day / 3600000); st[5] = (uint16_t)(in_day / 60000 % 60);
    st[6] = (uint16_t)(in_day / 1000 % 60); st[7] = (uint16_t)(in_day % 1000);
    return 1;
}

int n_SystemTimeToFileTime(const uint16_t *st, uint32_t *ft)
{
    CALLED(SystemTimeToFileTime);
    if (st[1] < 1 || st[1] > 12 || st[3] < 1 || st[3] > 31)
        return 0;
    int64_t days = days_from_civil(st[0], st[1], st[3]);
    uint64_t ms = (uint64_t)days * 86400000 + st[4] * 3600000u + st[5] * 60000u + st[6] * 1000u + st[7];
    uint64_t t = ms * 10000;
    ft[0] = (uint32_t)t;
    ft[1] = (uint32_t)(t >> 32);
    return 1;
}

uint64_t n_CeGetRandomSeed(void)
{
    CALLED(CeGetRandomSeed);
    return sceKernelGetProcessTimeWide() * 6364136223846793005ull + (uint64_t)time(NULL);
}

static void to_utf8(const wchar16 *w, char *out, size_t cap)
{
    size_t o = 0;
    for (; *w && o + 4 < cap; w++) {
        uint32_t c = *w == '\\' ? '/' : *w;
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

static void from_utf8(const char *s, wchar16 *out, size_t cap)
{
    size_t o = 0;
    const unsigned char *u = (const unsigned char *)s;
    while (*u && o + 1 < cap) {
        uint32_t c = *u++;
        if (c >= 0xE0 && u[0] && u[1]) {
            c = (c & 0x0F) << 12 | (u[0] & 0x3F) << 6 | (u[1] & 0x3F);
            u += 2;
        } else if (c >= 0xC0 && u[0]) {
            c = (c & 0x1F) << 6 | (u[0] & 0x3F);
            u += 1;
        }
        out[o++] = (wchar16)c;
    }
    out[o] = 0;
}

static void to_rel(char *s)
{
    size_t n = sizeof(MODULE_DIR) - 1;
    char *r = s;
    if (strncasecmp(r, MODULE_DIR, n) == 0 && (r[n] == 0 || r[n] == '/'))
        r += n;
    while (*r == '/')
        r++;
    memmove(s, r, strlen(r) + 1);
}

static void rel_of(const wchar16 *w, char *rel, size_t cap)
{
    to_utf8(w, rel, cap);
    to_rel(rel);
}

static void in_layer(char *host, size_t cap, const char *top, const char *rel)
{
    snprintf(host, cap, "%s%s%s", top, *rel ? "/" : "", rel);
}

static int exists(const char *host)
{
    SceIoStat st;
    return sceIoGetstat(host, &st) >= 0;
}

static char missing_dir[HOST_CAP];

static void read_path(const char *rel, char *host, size_t cap)
{
    in_layer(host, cap, DATA_DIR, rel);
    char low[HOST_CAP];
    in_layer(low, sizeof(low), GAME_DIR, rel);
    const char *slash = strrchr(host, '/');
    size_t dir_len = slash ? (size_t)(slash - host) : 0;
    if (missing_dir[0] && strlen(missing_dir) == dir_len && strncmp(missing_dir, host, dir_len) == 0) {
        snprintf(host, cap, "%s", low);
        return;
    }
    if (exists(host))
        return;
    if (dir_len && dir_len < sizeof(missing_dir)) {
        char dir[HOST_CAP];
        snprintf(dir, sizeof(dir), "%.*s", (int)dir_len, host);
        if (!exists(dir))
            snprintf(missing_dir, sizeof(missing_dir), "%s", dir);
    }
    if (exists(low))
        snprintf(host, cap, "%s", low);
}

static void write_path(const char *rel, char *host, size_t cap)
{
    missing_dir[0] = 0;
    in_layer(host, cap, DATA_DIR, rel);
    for (char *c = host + sizeof(DATA_DIR); (c = strchr(c, '/')) != NULL; c++) {
        *c = 0;
        sceIoMkdir(host, 0777);
        *c = '/';
    }
}

static void copy_up(const char *rel, const char *upper)
{
    char low[HOST_CAP];
    in_layer(low, sizeof(low), GAME_DIR, rel);
    SceUID in = sceIoOpen(low, SCE_O_RDONLY, 0);
    if (in < 0)
        return;
    SceUID out = sceIoOpen(upper, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    static char buf[0x10000];
    int n;
    while (out >= 0 && (n = sceIoRead(in, buf, sizeof(buf))) > 0)
        sceIoWrite(out, buf, n);
    say("file  copy-up \"%s\" -> 0x%08x\n", rel, (unsigned)out);
    if (out >= 0)
        sceIoClose(out);
    sceIoClose(in);
}

void guest_path_to_host(const char *guest, int writes, char *host, size_t cap)
{
    char rel[PATH_CAP];
    size_t n = 0;
    for (; guest[n] && n + 1 < sizeof(rel); n++)
        rel[n] = guest[n] == '\\' ? '/' : guest[n];
    rel[n] = 0;
    to_rel(rel);
    if (writes)
        write_path(rel, host, cap);
    else
        read_path(rel, host, cap);
}

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_ALREADY_EXISTS 183
static uint32_t last_error;

uint32_t n_GetLastError(void) { CALLED(GetLastError); return last_error; }

#define INVALID_HANDLE 0xFFFFFFFFu
#define FILE_HANDLE_BASE 0x00F10000u
#define FIND_HANDLE_BASE 0x00F20000u
#define MAX_FILES 64
#define MAX_FINDS 8
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
static SceUID files[MAX_FILES];
static struct { SceUID dir[2]; int layer; char pattern[128], upper[HOST_CAP]; } finds[MAX_FINDS];

#define MAX_HEADS 256
static struct { char *path; uint8_t *data; uint32_t len; } heads[MAX_HEADS];
static volatile unsigned n_heads;
#define FD_LAZY 0x7FFFFFFF
static unsigned short lazy[MAX_FILES];

static int head_of(const char *host)
{
    unsigned n = n_heads;
    __sync_synchronize();
    for (unsigned k = 0; k < n; k++)
        if (strcmp(heads[k].path, host) == 0)
            return (int)k;
    return -1;
}

#define RB_SIZE 0x1000
static struct { uint8_t *buf; uint32_t len, pos; int borrowed; } rb[MAX_FILES];

static int prefetch_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    sceKernelDelayThread(3000000);
    SceUID d = sceIoDopen(GAME_DIR "/Maps");
    SceIoDirent e;
    while (d >= 0 && n_heads < MAX_HEADS && sceIoDread(d, &e) > 0) {
        size_t l = strlen(e.d_name);
        if (l < 4 || strcasecmp(e.d_name + l - 4, ".phm") != 0)
            continue;
        char path[HOST_CAP];
        snprintf(path, sizeof(path), GAME_DIR "/Maps/%s", e.d_name);
        uint8_t *data = malloc(RB_SIZE);
        SceUID fd = data ? sceIoOpen(path, SCE_O_RDONLY, 0) : -1;
        int r = fd >= 0 ? sceIoRead(fd, data, RB_SIZE) : -1;
        if (fd >= 0)
            sceIoClose(fd);
        char *copy = r > 0 ? strdup(path) : NULL;
        if (!copy) {
            free(data);
            continue;
        }
        unsigned k = n_heads;
        heads[k].path = copy;
        heads[k].data = data;
        heads[k].len = (uint32_t)r;
        __sync_synchronize();
        n_heads = k + 1;
    }
    if (d >= 0)
        sceIoDclose(d);
    say("file  prefetched %u map headers\n", n_heads);
    return sceKernelExitDeleteThread(0);
}

void files_prefetch_start(void)
{
    SceUID th = sceKernelCreateThread("ph-prefetch", prefetch_thread, 191, 0x4000, 0, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
}

static SceUID open_lazy(unsigned i)
{
    if (files[i] != FD_LAZY)
        return files[i];
    SceUID fd = sceIoOpen(heads[lazy[i]].path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return fd;
    sceIoLseek(fd, rb[i].len, SCE_SEEK_SET);
    files[i] = fd;
    return fd;
}

static SceUID fd_of(uint32_t h)
{
    uint32_t i = h - FILE_HANDLE_BASE;
    return i < MAX_FILES && files[i] > 0 ? open_lazy(i) : -1;
}

static void rb_drop(unsigned i)
{
    if (rb[i].borrowed) {
        rb[i].buf = NULL;
        rb[i].borrowed = 0;
    }
    rb[i].len = rb[i].pos = 0;
}

static void rb_sync(uint32_t h)
{
    uint32_t i = h - FILE_HANDLE_BASE;
    if (i >= MAX_FILES || files[i] <= 0 || open_lazy(i) < 0)
        return;
    if (rb[i].len > rb[i].pos)
        sceIoLseek(files[i], -(SceOff)(rb[i].len - rb[i].pos), SCE_SEEK_CUR);
    rb_drop(i);
}

static int rb_read(unsigned i, uint8_t *dst, uint32_t n)
{
    uint32_t have = rb[i].len - rb[i].pos, take = n < have ? n : have;
    memcpy(dst, rb[i].buf + rb[i].pos, take);
    rb[i].pos += take;
    if (take == n)
        return (int)n;
    SceUID fd = open_lazy(i);
    rb_drop(i);
    if (fd < 0)
        return take ? (int)take : fd;
    uint32_t rest = n - take;
    if (rest >= RB_SIZE || (!rb[i].buf && !(rb[i].buf = malloc(RB_SIZE)))) {
        int r = sceIoRead(fd, dst + take, rest);
        return r < 0 ? (take ? (int)take : r) : (int)(take + r);
    }
    int r = sceIoRead(fd, rb[i].buf, RB_SIZE);
    if (r <= 0)
        return take ? (int)take : r;
    rb[i].len = (uint32_t)r;
    rb[i].pos = rest < (uint32_t)r ? rest : (uint32_t)r;
    memcpy(dst + take, rb[i].buf, rb[i].pos);
    return (int)(take + rb[i].pos);
}

uint32_t n_CreateFileW(const wchar16 *name, uint32_t access, uint32_t share, void *sa, const uint32_t *stk)
{
    (void)share; (void)sa;
    CALLED(CreateFileW);
    char rel[PATH_CAP], host[HOST_CAP];
    rel_of(name, rel, sizeof(rel));
    uint32_t creation = stk[0];
    if ((access & GENERIC_WRITE) || creation == 1 || creation == 2 || creation == 5) {
        write_path(rel, host, sizeof(host));
        if ((creation == 3 || creation == 4) && !exists(host))
            copy_up(rel, host);
    } else {
        read_path(rel, host, sizeof(host));
        if (strcasecmp(rel, "Data/game.pix") == 0)
            music_stop();
    }
    int flags = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? SCE_O_RDWR : SCE_O_WRONLY) : SCE_O_RDONLY;
    switch (creation) {
    case 1: flags |= SCE_O_CREAT | SCE_O_EXCL; break;
    case 2: flags |= SCE_O_CREAT | SCE_O_TRUNC; break;
    case 4: flags |= SCE_O_CREAT; break;
    case 5: flags |= SCE_O_TRUNC; break;
    default: break;
    }
    unsigned slot = 0;
    while (slot < MAX_FILES && files[slot] > 0)
        slot++;
    int k = !(access & GENERIC_WRITE) && creation == 3 && slot < MAX_FILES ? head_of(host) : -1;
    if (k >= 0) {
        files[slot] = FD_LAZY;
        lazy[slot] = (unsigned short)k;
        rb[slot].buf = heads[k].data;
        rb[slot].len = heads[k].len;
        rb[slot].pos = 0;
        rb[slot].borrowed = 1;
        last_error = 0;
        return FILE_HANDLE_BASE + slot;
    }
    SceUID fd = slot < MAX_FILES ? sceIoOpen(host, flags, 0777) : -1;
    if (fd < 0 || (access & GENERIC_WRITE))
        say("file  open \"%s\" access=0x%08x creation=%u -> fd=0x%08x\n", host, (unsigned)access, (unsigned)creation,
            (unsigned)fd);
    if (fd < 0) {
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE;
    }
    files[slot] = fd;
    last_error = 0;
    return FILE_HANDLE_BASE + slot;
}

int n_ReadFile(uint32_t h, void *buf, uint32_t n, uint32_t *got)
{
    CALLED(ReadFile);
    uint32_t i = h - FILE_HANDLE_BASE;
    int r = i < MAX_FILES && files[i] > 0 ? rb_read(i, buf, n) : -1;
    if (got)
        *got = r > 0 ? (uint32_t)r : 0;
    if (r < 0) {
        say("file  read h=0x%08x n=%u -> 0x%08x\n", (unsigned)h, (unsigned)n, (unsigned)r);
        last_error = ERROR_INVALID_HANDLE;
        return 0;
    }
    return 1;
}

int n_WriteFile(uint32_t h, const void *buf, uint32_t n, uint32_t *put)
{
    CALLED(WriteFile);
    rb_sync(h);
    SceUID fd = fd_of(h);
    int r = fd >= 0 ? sceIoWrite(fd, buf, n) : -1;
    if (put)
        *put = r > 0 ? (uint32_t)r : 0;
    if (r < 0) {
        say("file  write h=0x%08x n=%u -> 0x%08x\n", (unsigned)h, (unsigned)n, (unsigned)r);
        last_error = ERROR_INVALID_HANDLE;
        return 0;
    }
    return 1;
}

uint32_t n_SetFilePointer(uint32_t h, int32_t lo, int32_t *hi, uint32_t method)
{
    CALLED(SetFilePointer);
    rb_sync(h);
    SceUID fd = fd_of(h);
    SceOff off = hi ? ((SceOff)*hi << 32) | (uint32_t)lo : lo;
    SceOff pos = fd >= 0 ? sceIoLseek(fd, off, method == 1 ? SCE_SEEK_CUR : method == 2 ? SCE_SEEK_END : SCE_SEEK_SET) : -1;
    if (pos < 0) {
        last_error = ERROR_INVALID_HANDLE;
        return 0xFFFFFFFFu;
    }
    if (hi)
        *hi = (int32_t)(pos >> 32);
    return (uint32_t)pos;
}

uint32_t n_GetFileSize(uint32_t h, uint32_t *hi)
{
    CALLED(GetFileSize);
    SceIoStat st;
    SceUID fd = fd_of(h);
    if (fd < 0 || sceIoGetstatByFd(fd, &st) < 0) {
        last_error = ERROR_INVALID_HANDLE;
        return 0xFFFFFFFFu;
    }
    if (hi)
        *hi = (uint32_t)(st.st_size >> 32);
    return (uint32_t)st.st_size;
}

int n_CloseHandle(uint32_t h)
{
    CALLED(CloseHandle);
    uint32_t i = h - FILE_HANDLE_BASE;
    if (i < MAX_FILES && files[i] > 0) {
        if (files[i] != FD_LAZY)
            sceIoClose(files[i]);
        files[i] = 0;
        rb_drop(i);
        free(rb[i].buf);
        rb[i].buf = NULL;
    }
    return 1;
}

int n_FlushFileBuffers(uint32_t h) { CALLED(FlushFileBuffers); (void)h; return 1; }

static int match(const char *p, const char *s)
{
    if (strcmp(p, "*.*") == 0)
        return 1;
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (;; s++) {
                if (match(p + 1, s))
                    return 1;
                if (!*s)
                    return 0;
            }
        }
        if (!*s)
            return 0;
        char a = *p >= 'A' && *p <= 'Z' ? *p + 32 : *p, b = *s >= 'A' && *s <= 'Z' ? *s + 32 : *s;
        if (*p != '?' && a != b)
            return 0;
    }
    return *s == 0;
}

static int find_next(unsigned i, uint8_t *fd)
{
    SceIoDirent e;
    for (; finds[i].layer < 2; finds[i].layer++) {
        SceUID dir = finds[i].dir[finds[i].layer];
        while (dir > 0 && sceIoDread(dir, &e) > 0) {
            if (!match(finds[i].pattern, e.d_name))
                continue;
            if (finds[i].layer == 1) {
                char up[HOST_CAP + 256];
                snprintf(up, sizeof(up), "%s/%s", finds[i].upper, e.d_name);
                if (exists(up))
                    continue;
            }
            memset(fd, 0, 40 + 260 * 2);
            uint32_t attr = SCE_S_ISDIR(e.d_stat.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            uint32_t hi = (uint32_t)(e.d_stat.st_size >> 32), lo = (uint32_t)e.d_stat.st_size;
            memcpy(fd, &attr, 4);
            memcpy(fd + 28, &hi, 4);
            memcpy(fd + 32, &lo, 4);
            from_utf8(e.d_name, (wchar16 *)(fd + 40), 260);
            return 1;
        }
    }
    return 0;
}

static void find_close(unsigned i)
{
    for (int l = 0; l < 2; l++) {
        if (finds[i].dir[l] > 0)
            sceIoDclose(finds[i].dir[l]);
        finds[i].dir[l] = 0;
    }
    finds[i].layer = 2;
}

static int find_busy(unsigned i)
{
    return finds[i].dir[0] > 0 || finds[i].dir[1] > 0;
}

uint32_t n_FindFirstFileW(const wchar16 *name, uint8_t *fd)
{
    CALLED(FindFirstFileW);
    char rel[PATH_CAP], low[HOST_CAP];
    rel_of(name, rel, sizeof(rel));
    unsigned i = 0;
    while (i < MAX_FINDS && find_busy(i))
        i++;
    int found = 0;
    if (i < MAX_FINDS) {
        char *slash = strrchr(rel, '/');
        snprintf(finds[i].pattern, sizeof(finds[i].pattern), "%.127s", slash ? slash + 1 : rel);
        if (slash)
            *slash = 0;
        else
            rel[0] = 0;
        in_layer(finds[i].upper, sizeof(finds[i].upper), DATA_DIR, rel);
        in_layer(low, sizeof(low), GAME_DIR, rel);
        finds[i].dir[0] = sceIoDopen(finds[i].upper);
        finds[i].dir[1] = sceIoDopen(low);
        finds[i].layer = 0;
        found = find_next(i, fd);
        say("file  find \"%s/%s\" -> dirs=0x%08x,0x%08x first=%s\n", rel, finds[i].pattern,
            (unsigned)finds[i].dir[0], (unsigned)finds[i].dir[1], found ? "yes" : "none");
        if (!found)
            find_close(i);
    }
    if (!found) {
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE;
    }
    return FIND_HANDLE_BASE + i;
}

int n_FindNextFileW(uint32_t h, uint8_t *fd)
{
    CALLED(FindNextFileW);
    uint32_t i = h - FIND_HANDLE_BASE;
    int more = i < MAX_FINDS && find_busy(i) && find_next(i, fd);
    if (more)
        return 1;
    last_error = ERROR_NO_MORE_FILES;
    return 0;
}

int n_FindClose(uint32_t h)
{
    CALLED(FindClose);
    uint32_t i = h - FIND_HANDLE_BASE;
    if (i < MAX_FINDS)
        find_close(i);
    return 1;
}

uint32_t n_GetFileAttributesW(const wchar16 *name)
{
    CALLED(GetFileAttributesW);
    char rel[PATH_CAP], host[HOST_CAP];
    rel_of(name, rel, sizeof(rel));
    read_path(rel, host, sizeof(host));
    SceIoStat st;
    int r = sceIoGetstat(host, &st);
    uint32_t attr = r < 0 ? 0xFFFFFFFFu : SCE_S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    say("file  attr \"%s\" -> 0x%08x\n", host, (unsigned)attr);
    if (r < 0)
        last_error = ERROR_FILE_NOT_FOUND;
    return attr;
}

int n_DeleteFileW(const wchar16 *name)
{
    CALLED(DeleteFileW);
    char rel[PATH_CAP], host[HOST_CAP];
    rel_of(name, rel, sizeof(rel));
    in_layer(host, sizeof(host), DATA_DIR, rel);
    int r = sceIoRemove(host);
    say("file  delete \"%s\" -> 0x%08x\n", host, (unsigned)r);
    if (r < 0)
        last_error = ERROR_FILE_NOT_FOUND;
    return r >= 0;
}

int n_MoveFileW(const wchar16 *from, const wchar16 *to)
{
    CALLED(MoveFileW);
    char r1[PATH_CAP], h1[HOST_CAP], r2[PATH_CAP], h2[HOST_CAP];
    rel_of(from, r1, sizeof(r1));
    rel_of(to, r2, sizeof(r2));
    in_layer(h1, sizeof(h1), DATA_DIR, r1);
    write_path(r2, h2, sizeof(h2));
    int r = sceIoRename(h1, h2);
    say("file  move \"%s\" -> \"%s\" -> 0x%08x\n", h1, h2, (unsigned)r);
    if (r < 0)
        last_error = ERROR_FILE_NOT_FOUND;
    return r >= 0;
}

int n_CreateDirectoryW(const wchar16 *name, void *sa)
{
    (void)sa;
    CALLED(CreateDirectoryW);
    char rel[PATH_CAP], host[HOST_CAP];
    rel_of(name, rel, sizeof(rel));
    write_path(rel, host, sizeof(host));
    int r = sceIoMkdir(host, 0777);
    say("file  mkdir \"%s\" -> 0x%08x\n", host, (unsigned)r);
    if (r < 0)
        last_error = ERROR_ALREADY_EXISTS;
    return r >= 0;
}

uint32_t n_GetModuleFileNameW(uint32_t module, wchar16 *buf, uint32_t cap)
{
    (void)module;
    CALLED(GetModuleFileNameW);
    from_utf8(MODULE_PATH, buf, cap);
    uint32_t n = 0;
    while (buf[n])
        n++;
    return n;
}
