#pragma once

#define DATA_DIR "ux0:/data/palmheroes"
#define GAME_DIR "app0:/game"
#define IMG_PATH "app0:/ph.img"

#ifdef DIAGNOSTICS
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
static inline __attribute__((format(printf, 1, 2))) void say(const char *fmt, ...) { (void)fmt; }
#endif
static inline void cpu_track(const char *name, int uid) { (void)name; (void)uid; }

void files_prefetch_start(void);

void music_play(const char *host);
void music_stop(void);
void music_town(const void *castle, int type);
void music_battle(void);
void music_battle_end(int won);
void music_dialog_closed(void);
enum { MUSIC_NONE, MUSIC_ORIGINAL, MUSIC_HOMM2, MUSIC_SETS };
void music_choose(int set);
int music_set(void);
