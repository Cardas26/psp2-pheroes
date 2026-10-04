#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <kubridge.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"

#define TRAP_BASE 0xA0000000u
#define IMPORT_CAP 400
#define DYN_BASE (TRAP_BASE + 0x1000u)
#define DYN_MAX 64
#define OLD_LO 0x00010000u
#define OLD_HI 0x00230000u
#define ATTR_HAS_VBASE 0x1u
#define USER_RX 0x0C20D050u
#define RWX (KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC)
#define PSR_T 0x20u

typedef struct { uint32_t rva, ordinal; char name[40]; } Import;

__attribute__((used)) static const char build_id[] = "PHEROES-BUILD " PHEROES_BUILD_ID;

static uint32_t base, size, entry, n_imports;
static Import *imports;
static unsigned import_calls, faults, kdata_serves, old_serves;
static char dyn_names[DYN_MAX][48];
static unsigned n_dyn;

uint32_t trap_for_name(const char *name)
{
    for (unsigned i = 0; i < n_dyn; i++)
        if (strcmp(dyn_names[i], name) == 0)
            return DYN_BASE + 4 * i;
    if (n_dyn == DYN_MAX)
        return 0;
    strncpy(dyn_names[n_dyn], name, sizeof(dyn_names[0]) - 1);
    return DYN_BASE + 4 * n_dyn++;
}

#ifdef DIAGNOSTICS
static SceUID log_fd = -1;

void say(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 1)
        n = sizeof(line) - 1;
    if (log_fd >= 0 && n > 0)
        sceIoWrite(log_fd, line, n);
}
#endif

static const char *region(uint32_t a)
{
    if (a >= OLD_LO && a < OLD_HI)
        return "old-range";
    if (a >= base && a < base + size)
        return "image";
    if (a >= TRAP_BASE && a < TRAP_BASE + 4 * n_imports)
        return "import-trap";
    if (a >= 0xF0000000u)
        return "ce-kernel";
    if (a < 0x1000u)
        return "null";
    return "other";
}

static void set_pc(KuKernelExceptionContext *c, uint32_t target)
{
    c->pc = target & ~1u;
    c->SPSR = (target & 1) ? (c->SPSR | PSR_T) : (c->SPSR & ~PSR_T);
}

void *native_for(const char *name);
int win_patch_compose(uint32_t image);
int win_patch_pyramid(uint32_t image);
int win_patch_present(uint32_t image);
int win_patch_bars(uint32_t image, uint32_t block);
int win_patch_fog(uint32_t image, uint32_t block);
int win_patch_strips(uint32_t image, uint32_t block);
int win_patch_walk(uint32_t image, uint32_t block);
int win_patch_mapbands(uint32_t image, uint32_t block);
int win_patch_ring_tip(uint32_t image, uint32_t block);
int win_patch_outline(uint32_t image, uint32_t block);
void win_map_scroll_once(void);
void win_set_cycle(uint32_t *cycle_delay);
static inline void say_natives(void) {}
#ifdef DIAGNOSTICS
extern const char *volatile last_native;
#endif

static void guest_stop(void)
{
    say_natives();
    say("stop  import_calls=%u faults=%u kdata_serves=%u old_serves=%u\n", import_calls, faults, kdata_serves,
        old_serves);
    sceKernelExitDeleteThread(0);
}

static void stop_guest(KuKernelExceptionContext *c)
{
    c->sp &= ~7u;
    set_pc(c, (uint32_t)(uintptr_t)guest_stop);
}

#define KDATA_LO 0xFFFFC000u
#define KDATA_HI 0xFFFFD000u
#define KDATA_LOG_CAP 64
static uint32_t kdata[0x400];
static uint32_t tls[64];

static void kdata_init(void)
{
    kdata[0x800 / 4] = (uint32_t)(uintptr_t)tls;
    kdata[0x808 / 4] = 0xC0DE0001u;
    kdata[0x80C / 4] = 0xC0DE0002u;
}

static uint8_t *backing(uint32_t a)
{
    if (a >= KDATA_LO && a < KDATA_HI)
        return (uint8_t *)kdata + (a - KDATA_LO);
    if (a >= OLD_LO && a < OLD_HI)
        return (uint8_t *)(uintptr_t)(a - OLD_LO + base);
    return NULL;
}

static uint32_t *reg(KuKernelExceptionContext *c, unsigned n)
{
    return &c->r0 + n;
}

static uint32_t reg_val(KuKernelExceptionContext *c, unsigned n)
{
    return n == 15 ? c->pc + 8 : *reg(c, n);
}

static uint32_t shifted(KuKernelExceptionContext *c, uint32_t insn)
{
    uint32_t v = reg_val(c, insn & 0xF), amt = (insn >> 7) & 0x1F;
    switch ((insn >> 5) & 3) {
    case 0: return v << amt;
    case 1: return amt ? v >> amt : 0;
    case 2: return amt ? (uint32_t)((int32_t)v >> amt) : (uint32_t)((int32_t)v >> 31);
    default: return amt ? (v >> amt) | (v << (32 - amt)) : ((c->SPSR >> 29 & 1) << 31) | (v >> 1);
    }
}

static int emulate(KuKernelExceptionContext *c, uint32_t *addr_out, uint32_t *val_out, int *load_out)
{
    uint32_t insn = *(uint32_t *)(uintptr_t)c->pc;
    unsigned rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    int p = insn >> 24 & 1, u = insn >> 23 & 1, w = insn >> 21 & 1, load = insn >> 20 & 1;
    uint32_t off;
    unsigned width;
    int sign = 0;
    if ((insn & 0x0C000000u) == 0x04000000u) {
        off = (insn >> 25 & 1) ? shifted(c, insn) : insn & 0xFFF;
        width = (insn >> 22 & 1) ? 1 : 4;
    } else if ((insn & 0x0E000090u) == 0x00000090u && (insn & 0x60u)) {
        off = (insn >> 22 & 1) ? ((insn >> 4) & 0xF0) | (insn & 0xF) : reg_val(c, insn & 0xF);
        unsigned sh = (insn >> 5) & 3;
        width = sh == 2 ? 1 : 2;
        sign = sh != 1;
        if (!load && sh != 1)
            return -1;
    } else {
        return -1;
    }
    uint32_t b = reg_val(c, rn);
    uint32_t moved = u ? b + off : b - off;
    uint32_t a = p ? moved : b;
    uint8_t *m = backing(a);
    if (m == NULL || backing(a + width - 1) == NULL)
        return -1;
    uint32_t v;
    if (load) {
        v = width == 4 ? *(uint32_t *)m : width == 2 ? *(uint16_t *)m : *m;
        if (sign)
            v = width == 2 ? (uint32_t)(int32_t)(int16_t)v : (uint32_t)(int32_t)(int8_t)v;
    } else {
        v = reg_val(c, rd);
        if (width == 4) *(uint32_t *)m = v;
        else if (width == 2) *(uint16_t *)m = (uint16_t)v;
        else *m = (uint8_t)v;
    }
    if (!p || w)
        *reg(c, rn) = moved;
    c->pc += 4;
    if (load)
        *reg(c, rd) = v;
    *addr_out = a; *val_out = v; *load_out = load;
    return 0;
}

static void handler(KuKernelExceptionContext *c)
{
    static const char *kind[] = {"DABT", "PABT", "UNDEF"};
    uint32_t pc = c->pc;
    if (c->exceptionType == KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT && pc >= OLD_LO && pc < OLD_HI) {
        say("old   #%u X 0x%08x lr=0x%08x\n", ++old_serves, (unsigned)pc, (unsigned)c->lr);
        c->pc = pc - OLD_LO + base;
        return;
    }
    if (c->exceptionType == KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT && backing(c->FAR) &&
        pc >= base && pc + 4 <= base + size) {
        uint32_t a, v, far = c->FAR;
        int load;
        if (emulate(c, &a, &v, &load) == 0) {
            int old = a < OLD_HI;
            unsigned n = old ? ++old_serves : ++kdata_serves;
            static uint32_t old_pc[64], old_n[64];
            unsigned k = 0;
            while (old && k < 64 && old_pc[k] && old_pc[k] != pc)
                k++;
            int log_old = old && k < 64 && old_n[k] < 8;
            if (log_old) {
                old_pc[k] = pc;
                old_n[k]++;
            }
            if (log_old || (!old && n <= KDATA_LOG_CAP))
                say("%s #%u %s 0x%08x %s 0x%08x pc=0x%08x old-va=0x%08x lr=0x%08x%s\n", old ? "old  " : "kdata",
                    n, load ? "R" : "W", (unsigned)a, load ? "->" : "<-", (unsigned)v, (unsigned)pc,
                    (unsigned)(pc - base + OLD_LO), (unsigned)c->lr, a == far ? "" : " (FAR differs)");
            return;
        }
        say("emul  unhandled insn 0x%08x at pc=0x%08x FAR=0x%08x\n", (unsigned)*(uint32_t *)(uintptr_t)pc,
            (unsigned)pc, (unsigned)far);
    }
    if (c->exceptionType == KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT && pc >= 0xF0000000u && pc < 0xF0010000u &&
        pc != 0xF000F7F8u) {
        static uint32_t seen[32][2];
        static unsigned n_seen;
        unsigned i = 0;
        while (i < n_seen && !(seen[i][0] == pc && seen[i][1] == c->lr))
            i++;
        if (i == n_seen && n_seen < 32) {
            seen[n_seen][0] = pc; seen[n_seen++][1] = c->lr;
            say("ktrap 0x%08x(0x%08x, 0x%08x, 0x%08x, 0x%08x) lr=0x%08x old-va=0x%08x -> bx lr\n", (unsigned)pc,
                (unsigned)c->r0, (unsigned)c->r1, (unsigned)c->r2, (unsigned)c->r3, (unsigned)c->lr,
                (unsigned)(c->lr - base + OLD_LO));
        }
        set_pc(c, c->lr);
        return;
    }
    int is_dyn = pc >= DYN_BASE && pc < DYN_BASE + 4 * n_dyn;
    if (c->exceptionType == KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT &&
        ((pc >= TRAP_BASE && pc < TRAP_BASE + 4 * n_imports) || is_dyn)) {
        const char *name = is_dyn ? dyn_names[(pc - DYN_BASE) / 4] : imports[(pc - TRAP_BASE) / 4].name;
        import_calls++;
        if (import_calls <= IMPORT_CAP)
            say("imp   #%u %s%s(0x%08x, 0x%08x, 0x%08x, 0x%08x) lr=0x%08x sp=0x%08x\n", import_calls,
                is_dyn ? "dyn " : "", name, (unsigned)c->r0, (unsigned)c->r1, (unsigned)c->r2, (unsigned)c->r3,
                (unsigned)c->lr, (unsigned)c->sp);
        if (import_calls == IMPORT_CAP)
            say("cap   %u import calls logged; later ones are served unlogged\n", IMPORT_CAP);
        c->r0 = 0;
        set_pc(c, c->lr);
        return;
    }
    faults++;
    uint32_t far = c->FAR;
    say("fault %s pc=0x%08x(%s) lr=0x%08x(%s) sp=0x%08x FAR=0x%08x(%s) FSR=0x%08x SPSR=0x%08x\n",
        c->exceptionType < 3 ? kind[c->exceptionType] : "?", (unsigned)pc, region(pc), (unsigned)c->lr,
        region(c->lr), (unsigned)c->sp, (unsigned)far, region(far), (unsigned)c->FSR, (unsigned)c->SPSR);
    say("regs  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x\n"
        "regs  r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x\n",
        (unsigned)c->r0, (unsigned)c->r1, (unsigned)c->r2, (unsigned)c->r3, (unsigned)c->r4, (unsigned)c->r5,
        (unsigned)c->r6, (unsigned)c->r7, (unsigned)c->r8, (unsigned)c->r9, (unsigned)c->r10, (unsigned)c->r11,
        (unsigned)c->r12);
    say("stack");
    for (unsigned i = 0, n = 0; i < 256 && n < 24; i++) {
        uint32_t v = ((const uint32_t *)(uintptr_t)c->sp)[i];
        if ((v >= 0x81000000u && v < 0x81100000u) || (v >= base && v < base + 0x10f000u)) {
            say(" +%x:%08x", i * 4, (unsigned)v);
            n++;
        }
    }
    say("\n");
    if (pc >= base && pc + 4 <= base + size)
        say("insn  [pc]=0x%08x old-va=0x%08x\n", (unsigned)*(uint32_t *)(uintptr_t)pc,
            (unsigned)(pc - base + OLD_LO));
    stop_guest(c);
}

static int guest_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    static const uint16_t cmdline[2] = {0, 0};
    typedef int (*WinMainCRTStartup)(uint32_t, uint32_t, const uint16_t *, int);
    say("run   entry=0x%08x hInstance=0x%08x\n", (unsigned)(base + entry), (unsigned)base);
    int r = ((WinMainCRTStartup)(uintptr_t)(base + entry))(base, 0, cmdline, 5 );
    say_natives();
    say("ret   entry returned %d import_calls=%u faults=%u kdata_serves=%u old_serves=%u\n", r, import_calls,
        faults, kdata_serves, old_serves);
    return 0;
}

uint32_t sample_tramp;
uint32_t replace_tramp;
uint32_t battle_won_back, battle_lost_back;

int32_t sample_reject(uint32_t player, uint32_t sample)
{
    static unsigned n;
    if (n++ < 8)
        say("sound sample %u past the table (player 0x%08x), not played\n", (unsigned)sample, (unsigned)player);
    return -1;
}

static int load(void)
{
    SceUID fd = sceIoOpen(IMG_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        say("load  open %s: 0x%08x\n", IMG_PATH, (unsigned)fd);
        return -1;
    }
    char magic[4];
    uint32_t hdr[4];
    sceIoRead(fd, magic, 4);
    sceIoRead(fd, hdr, sizeof(hdr));
    base = hdr[0]; size = hdr[1]; entry = hdr[2]; n_imports = hdr[3];
    say("load  magic=%.4s base=0x%08x size=0x%x entry=0x%x imports=%u\n", magic, (unsigned)base,
        (unsigned)size, (unsigned)entry, (unsigned)n_imports);
    if (memcmp(magic, "PHE5", 4) != 0 || n_imports > 1024)
        return -1;
    imports = malloc(n_imports * sizeof(Import));
    void *buf = malloc(size);
    int ri = sceIoRead(fd, imports, n_imports * sizeof(Import));
    int rb = sceIoRead(fd, buf, size);
    sceIoClose(fd);
    if (ri != (int)(n_imports * sizeof(Import)) || rb != (int)size) {
        say("load  short read imports=%d image=%d\n", ri, rb);
        return -1;
    }

    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = ATTR_HAS_VBASE;
    opt.field_C = base;
    uint32_t block = (size + 0xFFFFFu) & ~0xFFFFFu;
    SceUID uid = kuKernelAllocMemBlock("ph-image", USER_RX, block, &opt);
    void *got = NULL;
    if (uid >= 0)
        sceKernelGetMemBlockBase(uid, &got);
    int pr = got ? kuKernelMemProtect(got, block, RWX) : -1;
    say("map   uid=0x%08x base=0x%08x block=0x%x protect=0x%08x\n", (unsigned)uid, (unsigned)(uintptr_t)got,
        (unsigned)block, (unsigned)pr);
    if (uid < 0 || got != (void *)(uintptr_t)base || pr < 0)
        return -1;
    memcpy(got, buf, size);
    free(buf);
    unsigned n_native = 0;
    for (uint32_t i = 0; i < n_imports; i++) {
        void *fn = native_for(imports[i].name);
        n_native += fn != NULL;
        *(uint32_t *)(uintptr_t)(base + imports[i].rva) = fn ? (uint32_t)(uintptr_t)fn : TRAP_BASE + 4 * i;
    }
    uint32_t *cycle = (uint32_t *)(uintptr_t)(base + 0xc9888);
    if (*cycle == 0xE3A0201Eu)
        *cycle = 0xE3A0200Fu;
    say("patch cycle 0x%08x\n", (unsigned)*cycle);
    if (cycle[2] == 0xE586200Cu)
        win_set_cycle((uint32_t *)(uintptr_t)(*(uint32_t *)(uintptr_t)(base + 0xc9a14) + 0xc));
    say("patch compose %s\n", win_patch_compose(base) ? "native" : "not found");
    say("patch bars %s\n", win_patch_bars(base, block) ? "kept" : "not found");
    say("patch pyramid %s\n", win_patch_pyramid(base) ? "native" : "not found");
    say("patch present %s\n", win_patch_present(base) ? "native" : "not found");
    say("patch fog %s\n", win_patch_fog(base, block) ? "grid" : "not found");
    say("patch strips %s\n", win_patch_strips(base, block) ? "banded" : "not found");
    say("patch walk %s\n", win_patch_walk(base, block) ? "bounded" : "not found");
    say("patch mapbands %s\n", win_patch_mapbands(base, block) ? "banded" : "not found");
    extern void sample_guard(void);
    uint32_t *play = (uint32_t *)(uintptr_t)(base + 0xda5f4 - 0x10000);
    if (play[0] == 0xE92D47F0u && play[1] == 0xE1A09002u && play[17] == 0xE5972038u) {
        uint32_t *t = (uint32_t *)(uintptr_t)(base + block - 0x1000 + 0x240);
        t[0] = play[0], t[1] = play[1], t[2] = 0xE51FF004u, t[3] = (uint32_t)(uintptr_t)(play + 2);
        sample_tramp = (uint32_t)(uintptr_t)t;
        play[0] = 0xE51FF004u;
        play[1] = (uint32_t)(uintptr_t)sample_guard;
    }
    say("patch sample guard %s\n", sample_tramp ? "bounded" : "not found");
    extern void replace_guard(void);
    uint32_t *repl = (uint32_t *)(uintptr_t)(base + 0xda6e0 - 0x10000);
    if (repl[0] == 0xE92D47F0u && repl[1] == 0xE1A06002u && repl[13] == 0xE5983038u) {
        uint32_t *t = (uint32_t *)(uintptr_t)(base + block - 0x1000 + 0x270);
        t[0] = repl[0], t[1] = repl[1], t[2] = 0xE51FF004u, t[3] = (uint32_t)(uintptr_t)(repl + 2);
        replace_tramp = (uint32_t)(uintptr_t)t;
        repl[0] = 0xE51FF004u;
        repl[1] = (uint32_t)(uintptr_t)replace_guard;
    }
    say("patch replace guard %s\n", replace_tramp ? "bounded" : "not found");
    say("patch outline %s\n", win_patch_outline(base, block) ? "hooked" : "not found");
    {
        extern void battle_won_thunk(void), battle_lost_thunk(void);
        uint32_t *won = (uint32_t *)(uintptr_t)(base + 0x66704 - 0x10000), *lost = (uint32_t *)(uintptr_t)(base + 0x66808 - 0x10000);
        int ok = won[0] == 0xE59B3010u && lost[0] == 0xE3500000u && lost[-1] == 0xE59A0010u &&
                 *(uint32_t *)(uintptr_t)(base + 0x666e8 - 0x10000) == 0x1A000045u;
        if (ok) {
            uint32_t *v = (uint32_t *)(uintptr_t)(base + block - 0x1000 + 0x250);
            v[0] = 0xE51FF004u, v[1] = (uint32_t)(uintptr_t)battle_won_thunk;
            v[2] = 0xE51FF004u, v[3] = (uint32_t)(uintptr_t)battle_lost_thunk;
            battle_won_back = (uint32_t)(uintptr_t)(won + 1);
            battle_lost_back = (uint32_t)(uintptr_t)(lost + 1);
            won[0] = 0xEA000000u | ((((uint32_t)(uintptr_t)v) - ((uint32_t)(uintptr_t)won + 8)) >> 2 & 0xFFFFFFu);
            lost[0] = 0xEA000000u | ((((uint32_t)(uintptr_t)(v + 2)) - ((uint32_t)(uintptr_t)lost + 8)) >> 2 & 0xFFFFFFu);
        }
        say("patch battle end %s\n", ok ? "noted" : "not found");
    }
    int32_t *scroll = (int32_t *)(uintptr_t)(base + 0x12a018 - 0x10000);
    if (scroll[-6] == 0 && scroll[-5] == 10 && scroll[-4] == 5 && scroll[0] == 0 && scroll[1] == 4 && scroll[2] == 2)
        scroll[2] = 4;
    say("patch map scroll default %s\n", scroll[2] == 4 ? "very fast" : "not found");
    say("patch ring tip %s\n", win_patch_ring_tip(base, block) ? "hooked" : "not found");
    kuKernelFlushCaches(got, block);
    say("iat   %u slots, %u native, the rest -> 0x%08x..0x%08x\n", (unsigned)n_imports, n_native, TRAP_BASE,
        (unsigned)(TRAP_BASE + 4 * n_imports));
    return 0;
}

int main(void)
{
    sceIoMkdir(DATA_DIR, 0777);
#ifdef DIAGNOSTICS
    log_fd = sceIoOpen(DATA_DIR "/debug.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
#endif
    say("%s pid=0x%08x\n", build_id, (unsigned)sceKernelGetProcessId());
    if (load() == 0) {
        kdata_init();
        for (uint32_t t = 0; t < 3; t++) {
            int r = kuKernelRegisterExceptionHandler(t, handler, NULL, NULL);
            say("excp  type=%u register=0x%08x\n", (unsigned)t, (unsigned)r);
        }
        files_prefetch_start();
        win_map_scroll_once();
        SceUID th = sceKernelCreateThread("ph-guest", guest_main, 170, 0x100000, 0, 0, NULL);
        cpu_track("guest", th);
        cpu_track("main", sceKernelGetThreadId());
        int started = sceKernelStartThread(th, 0, NULL);
        say("thread uid=0x%08x start=0x%08x\n", (unsigned)th, (unsigned)started);
        int status = 0, w = -1;
        for (int t = 5; w < 0; t += 5) {
            SceUInt timeout = 5 * 1000 * 1000;
            w = sceKernelWaitThreadEnd(th, &status, &timeout);
#if defined(DIAGNOSTICS)
            if (w < 0)
                say("beat  t=%ds import_calls=%u faults=%u last_native=%s\n", t, import_calls, faults, last_native);
#endif
        }
        say("wait  0x%08x\n", (unsigned)w);
    }
    say("done\n");
#ifdef DIAGNOSTICS
    if (log_fd >= 0)
        sceIoClose(log_fd);
#endif
    sceKernelExitProcess(0);
    return 0;
}
