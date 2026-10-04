#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"

#define RATE 48000
#define GRAIN 1024
#define VOLUME (SCE_AUDIO_VOLUME_0DB / 2)

static SceUID lock = -1, wake = -1;
static char pending[640], asked[640];
static char pending_then[640];
static int has_pending, pending_once;

static struct { uint8_t *buf; size_t len, pos; } track;

static ssize_t track_read(void *h, void *out, size_t n)
{
    (void)h;
    size_t left = track.len - track.pos;
    n = n < left ? n : left;
    memcpy(out, track.buf + track.pos, n);
    track.pos += n;
    return (ssize_t)n;
}

static off_t track_seek(void *h, off_t off, int whence)
{
    (void)h;
    off_t at = whence == SEEK_SET ? off : whence == SEEK_CUR ? (off_t)track.pos + off : (off_t)track.len + off;
    if (at < 0 || at > (off_t)track.len)
        return -1;
    track.pos = (size_t)at;
    return at;
}

static int track_open(mpg123_handle *mh, const char *path)
{
    free(track.buf);
    track.buf = NULL, track.len = track.pos = 0;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    SceOff len = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    track.buf = len > 0 ? malloc((size_t)len) : NULL;
    if (track.buf && sceIoRead(fd, track.buf, (SceSize)len) == (int)len)
        track.len = (size_t)len;
    sceIoClose(fd);
    return track.len && mpg123_open_handle(mh, &track) == MPG123_OK;
}

static int music_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    enum { GRAINS = 4 };
    static int16_t grains[GRAINS][GRAIN * 2] __attribute__((aligned(64)));
    const size_t bytes = sizeof(grains[0]);
    unsigned turn = 0;
    int16_t *grain = grains[0];
    char path[640] = "", next[640], next_then[640] = "", then[640] = "";
    int once = 0, next_once = 0;
    int open = 0, port = -1;
    mpg123_handle *mh = NULL;
    for (;;) {
        int take = 0;
        sceKernelLockMutex(lock, 1, NULL);
        if (has_pending) {
            snprintf(next, sizeof(next), "%s", pending);
            next_once = pending_once;
            snprintf(next_then, sizeof(next_then), "%s", pending_then);
            has_pending = 0;
            take = 1;
        }
        sceKernelUnlockMutex(lock, 1);
        SceIoStat st;
        if (take && !next[0]) {
            if (open)
                mpg123_close(mh);
            say("music stop %s\n", open ? path : "(silence)");
            open = 0;
            path[0] = 0;
        } else if (take && open && strcmp(next, path) == 0) {
        } else if (take && sceIoGetstat(next, &st) < 0) {
            say("music %s missing, keeping %s\n", next, open ? path : "silence");
        } else if (take) {
            if (!mh) {
                int err;
                mpg123_init();
                mh = mpg123_new(NULL, &err);
                port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, RATE, SCE_AUDIO_OUT_MODE_STEREO);
                say("music port=0x%08x mpg123=%s\n", (unsigned)port, mh ? "ok" : mpg123_plain_strerror(err));
                if (!mh || port < 0)
                    return 0;
                int vol[2] = {VOLUME, VOLUME};
                sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
                mpg123_replace_reader_handle(mh, track_read, track_seek, NULL);
                mpg123_format_none(mh);
                mpg123_format(mh, RATE, MPG123_STEREO, MPG123_ENC_SIGNED_16);
            }
            if (open)
                mpg123_close(mh);
            mpg123_param(mh, MPG123_FORCE_RATE, 0, 0);
            open = track_open(mh, next);
            once = next_once;
            snprintf(then, sizeof(then), "%s", next_then);
            long rate = 0;
            int ch = 0, enc = 0;
            if (open && mpg123_getformat(mh, &rate, &ch, &enc) == MPG123_OK && rate != RATE) {
                mpg123_close(mh);
                mpg123_param(mh, MPG123_FORCE_RATE, RATE, 0);
                track.pos = 0;
                open = mpg123_open_handle(mh, &track) == MPG123_OK;
            }
            say("music open %s -> %s (%u bytes, %ld Hz)\n", next, open ? "ok" : mpg123_strerror(mh), (unsigned)track.len, rate);
            snprintf(path, sizeof(path), "%s", next);
        }
        if (!open) {
            sceKernelWaitSema(wake, 1, NULL);
            continue;
        }
        grain = grains[turn++ % GRAINS];
        size_t have = 0;
        while (open && have < bytes) {
            size_t got = 0;
            int r = mpg123_read(mh, (uint8_t *)grain + have, bytes - have, &got);
            have += got;
            if (r == MPG123_DONE) {
                if (once) {
                    mpg123_close(mh);
                    once = 0;
                    open = then[0] && track_open(mh, then);
                    say("music %s ended, %s\n", path, open ? then : "silence");
                    snprintf(path, sizeof(path), "%s", open ? then : "");
                    if (!open)
                        break;
                } else {
                    mpg123_seek(mh, 0, SEEK_SET);
                }
            } else if (r != MPG123_OK && r != MPG123_NEW_FORMAT && got == 0) {
                say("music read %s: %s, closing\n", path, mpg123_strerror(mh));
                mpg123_close(mh);
                open = 0;
            }
        }
        if (have < bytes)
            memset((uint8_t *)grain + have, 0, bytes - have);
        sceAudioOutOutput(port, grain);
    }
    return 0;
}

static void music_request_once(const char *host, int once, const char *then)
{
    if (lock < 0) {
        lock = sceKernelCreateMutex("ph-music", 0, 0, NULL);
        wake = sceKernelCreateSema("ph-music", 0, 0, 1, NULL);
        SceUID th = sceKernelCreateThread("ph-music", music_thread, 64, 0x10000, 0, 0, NULL);
        cpu_track("music", th);
        sceKernelStartThread(th, 0, NULL);
    }
    sceKernelLockMutex(lock, 1, NULL);
    if (once || strcmp(host, asked) != 0) {
        snprintf(asked, sizeof(asked), "%s", host);
        snprintf(pending, sizeof(pending), "%s", host);
        snprintf(pending_then, sizeof(pending_then), "%s", then ? then : "");
        pending_once = once;
        has_pending = 1;
        sceKernelSignalSema(wake, 1);
    }
    sceKernelUnlockMutex(lock, 1);
}

static void music_request(const char *host) { music_request_once(host, 0, NULL); }

#define HOMM2 DATA_DIR "/Music/HoMM2/Heroes of Might and Magic II OST - "
#define ORIGINAL DATA_DIR "/Music/Original/"
static const struct { const char *game, *ost, *none; } terrain_ost[] = {
    {"", "16 - Terrain - Ocean", NULL},
    {"Colossus", "17 - Terrain - Dirt", NULL},
    {"Arid Foothills", "18 - Terrain - Grass", NULL},
    {"Opium", "15 - Terrain - Swamp", NULL},
    {"Supernatural", "11 - Terrain - Lava", NULL},
    {"Willow and the Light", "12 - Terrain - Wasteland", NULL},
    {"Desert City", "13 - Terrain - Desert", NULL},
    {"Temple of the Manes", "14 - Terrain - Snow", NULL},
    {"Cambodean Odyssey", "13 - Terrain - Desert", NULL},
    {"Phantasm", "12 - Terrain - Wasteland", NULL},
    {"Expeditionary", NULL, "13 - Terrain - Desert"},
    {"Celtic Impulse", NULL, "18 - Terrain - Grass"},
};
static const char *const town_ost[6][2] = {
    {"08 - Town - Knight", "47 - Town - Knight (expansion)"},
    {"09 - Town - Barbarian", "48 - Town - Barbarian (expansion)"},
    {"10 - Town - Wizard", "49 - Town - Wizard (expansion)"},
    {"06 - Town - Warlock", "45 - Town - Warlock (expansion)"},
    {"05 - Town - Sorceress", "44 - Town - Sorceress (expansion)"},
    {"07 - Town - Necromancer", "46 - Town - Necromancer (expansion)"},
};
static int music_set_ = MUSIC_ORIGINAL;
#define set_homm2 (music_set_ == MUSIC_HOMM2)
static char last_name[128];
static int have_name;
static char last_terrain[640];
static const void *town_castle;
static int town_type, battle_on, result_up;

static int ost_path(const char *name, char *out, size_t cap)
{
    SceIoStat st;
    snprintf(out, cap, HOMM2 "%s.mp3", name);
    return sceIoGetstat(out, &st) >= 0;
}

static void terrain(const char *name)
{
    char ost[640];
    if (music_set_ == MUSIC_NONE) {
        music_stop();
        return;
    }
    if (set_homm2)
        for (unsigned i = 0; i < sizeof(terrain_ost) / sizeof(terrain_ost[0]); i++) {
            if (strcmp(name, terrain_ost[i].game) != 0)
                continue;
            if (!terrain_ost[i].ost) {
                if (strstr(last_terrain, "/Music/HoMM2/")) {
                    music_request(last_terrain);
                    return;
                }
                if (ost_path(terrain_ost[i].none, ost, sizeof(ost))) {
                    snprintf(last_terrain, sizeof(last_terrain), "%s", ost);
                    music_request(ost);
                    return;
                }
                break;
            }
            if (ost_path(terrain_ost[i].ost, ost, sizeof(ost))) {
                snprintf(last_terrain, sizeof(last_terrain), "%s", ost);
                music_request(ost);
                return;
            }
            break;
        }
    snprintf(last_terrain, sizeof(last_terrain), ORIGINAL "%s.mp3", name);
    music_request(last_terrain);
}

void music_play(const char *host)
{
    const char *slash = strrchr(host, '/'), *base = slash ? slash + 1 : host;
    size_t n = strlen(base);
    snprintf(last_name, sizeof(last_name), "%.*s", (int)(n >= 4 ? n - 4 : n), base);
    have_name = 1;
    town_castle = NULL;
    battle_on = 0;
    terrain(last_name);
}

void music_town(const void *castle, int type)
{
    if (castle == town_castle || type < 0 || type > 5)
        return;
    town_castle = castle;
    town_type = type;
    char ost[640];
    int v = (int)(sceKernelGetProcessTimeWide() & 1);
    if (set_homm2 && (ost_path(town_ost[type][v], ost, sizeof(ost)) || ost_path(town_ost[type][!v], ost, sizeof(ost))))
        music_request(ost);
}

static void battle_track(void)
{
    static const char *const battle_ost[3] = {"02 - Battle I", "03 - Battle II", "04 - Battle III"};
    char ost[640];
    int k = (int)(sceKernelGetProcessTimeWide() % 3);
    for (int i = 0; set_homm2 && i < 3; i++)
        if (ost_path(battle_ost[(k + i) % 3], ost, sizeof(ost))) {
            music_request(ost);
            return;
        }
}

void music_battle(void)
{
    town_castle = NULL;
    battle_on = 1;
    battle_track();
}

void music_battle_end(int won)
{
    char ost[640];
    battle_on = 0;
    result_up = 1;
    if (set_homm2 && ost_path(won ? "29 - Hero Victory" : "30 - Hero Defeat", ost, sizeof(ost)))
        music_request_once(ost, 1, NULL);
}

void music_dialog_closed(void)
{
    if (!result_up)
        return;
    result_up = 0;
    if (have_name)
        terrain(last_name);
}

int music_set(void) { return music_set_; }

void music_choose(int set)
{
    if (set == music_set_ || set < 0 || set >= MUSIC_SETS)
        return;
    music_set_ = set;
    if (music_set_ == MUSIC_NONE) {
        music_stop();
    } else if (battle_on) {
        if (set_homm2)
            battle_track();
        else
            music_stop();
    } else if (result_up) {
    } else if (town_castle && set_homm2) {
        const void *c = town_castle;
        town_castle = NULL;
        music_town(c, town_type);
    } else if (have_name) {
        terrain(last_name);
    }
}

void music_stop(void)
{
    if (lock < 0)
        return;
    sceKernelLockMutex(lock, 1, NULL);
    asked[0] = pending[0] = 0;
    has_pending = 1;
    sceKernelSignalSema(wake, 1);
    sceKernelUnlockMutex(lock, 1);
}
