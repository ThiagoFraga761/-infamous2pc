/* The player's settings: user/settings.ini (beside disc/, outside git), read
 * at start and turned into the environment variables the port already reads.
 * A variable already set wins over the file, so play.sh, the tests and the
 * benchmarks keep working as they did. The UI (launcher and in-game menu)
 * changes the values and saves them. */
#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct port_settings {
    /* [video] */
    int scale;          /* render scale: 1 (720p), 2 (1440p), 3 (4K) */
    int fullscreen;     /* 0 window, 1 full screen */
    int vsync;          /* 1 fifo, 0 mailbox */
    int stretch;        /* 1 stretch to the window, 0 black bars */
    int display;        /* which monitor the window opens on */
    /* [language] */
    char voice[24];     /* the voice pack: cache/psarc/<voice>.psarc */
    char text[24];      /* the text pack: cache/psarc/<text>_text.psarc */
    /* [performance] */
    int fps_limit;      /* 0 none, else frames a second */
    int fps_counter;    /* the counter drawn by the UI */
    int record_thread;  /* PS3RECOMP_BE_THREAD */
    /* [launcher] */
    int skip_launcher;  /* open the game without the start screen */
} port_settings;

/* The defaults: today's play.sh -- 720p, a window, mailbox, black bars,
 * Brazilian Portuguese voices and text, the recording thread on. */
void port_settings_defaults(port_settings* s);

/* Read `path` (missing or unreadable: the defaults). Returns 1 when the file
 * was read, 0 when the defaults were used. Unknown keys and bad values are
 * ignored, each falling back to its default. */
int port_settings_load(port_settings* s, const char* path);

/* Write `path`, creating its directory. 0 on success. A field pinned by the
 * environment (port_settings_from_env) is written as the file had it. */
int port_settings_save(const port_settings* s, const char* path);

/* The fields a variable already in the environment decides. */
enum {
    PORT_PIN_SCALE = 1 << 0, PORT_PIN_FULLSCREEN = 1 << 1, PORT_PIN_VSYNC = 1 << 2,
    PORT_PIN_STRETCH = 1 << 3, PORT_PIN_DISPLAY = 1 << 4, PORT_PIN_VOICE = 1 << 5,
    PORT_PIN_TEXT = 1 << 6, PORT_PIN_FPS_LIMIT = 1 << 7, PORT_PIN_RECORD_THREAD = 1 << 8,
};

/* Before port_settings_export: each field whose variable is set takes the
 * environment's value, which is what the game will run with, so the UI shows
 * that. Returns the PORT_PIN_* bits (kept, see port_settings_pinned): the UI
 * greys those out and saving leaves the file's own values for them. */
unsigned port_settings_from_env(port_settings* s);
unsigned port_settings_pinned(void);

/* Each value as the environment variable the port reads, unless that
 * variable is already set. */
void port_settings_export(const port_settings* s);

/* The environment for starting the process again with new settings: this
 * one without what port_settings_export set (the new process reads the file
 * again), plus PS3_SKIP_LAUNCHER=1 and `extra` ("NAME=value") when not NULL.
 * Built aside rather than by unsetting: other threads read the environment,
 * and a failed exec keeps it whole. NULL-terminated, from malloc. */
char** port_settings_restart_env(const char* extra);

/* user/settings.ini, or $PS3_USER_DIR/settings.ini. */
const char* port_settings_path(void);

/* The process-wide settings, loaded once by main(). */
port_settings* port_settings_get(void);

#ifdef __cplusplus
}
#endif

#endif
