/* user/settings.ini -- see settings.h. */
#include "settings.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

static const char* const k_langs[] = {
    "brazilian", "english", "latino", "spanish", "french", "german", "italian",
    "portuguese", "russian", "polish", "dutch", "turkish", "japanese",
    "british", "korean", "chinese",
};

static int lang_known(const char* v)
{
    for (const char* l : k_langs)
        if (!strcmp(v, l)) return 1;
    return 0;
}

/* The last three have text and no voices. */
static int voice_known(const char* v)
{
    return lang_known(v) && strcmp(v, "british") && strcmp(v, "korean") && strcmp(v, "chinese");
}

static port_settings s_file;    /* as read, for the pinned fields when saving */
static unsigned s_pinned;

void port_settings_defaults(port_settings* s)
{
    memset(s, 0, sizeof *s);
    s->scale = 1;
    s->fullscreen = 0;
    s->vsync = 0;
    s->stretch = 0;
    s->display = 0;
    snprintf(s->voice, sizeof s->voice, "brazilian");
    snprintf(s->text, sizeof s->text, "brazilian");
    s->fps_limit = 0;
    s->fps_counter = 0;
    s->record_thread = 1;
    s->skip_launcher = 0;
    s->fullres_particles = 1;
    s->aniso = 16;
}

static std::string trim(const std::string& t)
{
    size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
}

/* An integer in [lo, hi], else the old value. */
static void set_int(int* dst, const std::string& v, int lo, int hi)
{
    char* end = nullptr;
    errno = 0;
    const long n = strtol(v.c_str(), &end, 10);
    if (errno || end == v.c_str() || *end || n < lo || n > hi) return;
    *dst = (int)n;
}

int port_settings_load(port_settings* s, const char* path)
{
    port_settings_defaults(s);
    FILE* f = path ? fopen(path, "r") : nullptr;
    if (!f) return 0;
    std::string section;
    char line[512];
    bool first = true;
    while (fgets(line, sizeof line, f)) {
        std::string l = trim(line);
        /* A byte order mark, from an editor that saves UTF-8 that way. */
        if (first && l.compare(0, 3, "\xEF\xBB\xBF") == 0) l = trim(l.substr(3));
        first = false;
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        if (l[0] == '[') {
            const size_t e = l.find(']');
            section = e == std::string::npos ? std::string() : trim(l.substr(1, e - 1));
            continue;
        }
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(l.substr(0, eq));
        std::string v = trim(l.substr(eq + 1));
        const size_t c = v.find_first_of(";#");
        if (c != std::string::npos) v = trim(v.substr(0, c));
        if (section == "video") {
            if (k == "scale") set_int(&s->scale, v, 1, 3);
            else if (k == "fullscreen") set_int(&s->fullscreen, v, 0, 1);
            else if (k == "vsync") set_int(&s->vsync, v, 0, 1);
            else if (k == "stretch") set_int(&s->stretch, v, 0, 1);
            else if (k == "display") set_int(&s->display, v, 0, 15);
        } else if (section == "language") {
            if (k == "voice" && voice_known(v.c_str())) snprintf(s->voice, sizeof s->voice, "%s", v.c_str());
            else if (k == "text" && lang_known(v.c_str())) snprintf(s->text, sizeof s->text, "%s", v.c_str());
        } else if (section == "performance") {
            if (k == "fps_limit") set_int(&s->fps_limit, v, 0, 1000);
            else if (k == "fps_counter") set_int(&s->fps_counter, v, 0, 1);
            else if (k == "record_thread") set_int(&s->record_thread, v, 0, 1);
        } else if (section == "launcher") {
            if (k == "skip") set_int(&s->skip_launcher, v, 0, 1);
        } else if (section == "mods") {
            if (k == "fullres_particles") set_int(&s->fullres_particles, v, 0, 1);
            else if (k == "aniso") set_int(&s->aniso, v, 0, 16);
        }
    }
    fclose(f);
    return 1;
}

int port_settings_save(const port_settings* in, const char* path)
{
    if (!path) return -1;
    port_settings out = *in;
    const port_settings* s = &out;
    if (s_pinned & PORT_PIN_SCALE)         out.scale = s_file.scale;
    if (s_pinned & PORT_PIN_FULLSCREEN)    out.fullscreen = s_file.fullscreen;
    if (s_pinned & PORT_PIN_VSYNC)         out.vsync = s_file.vsync;
    if (s_pinned & PORT_PIN_STRETCH)       out.stretch = s_file.stretch;
    if (s_pinned & PORT_PIN_DISPLAY)       out.display = s_file.display;
    if (s_pinned & PORT_PIN_VOICE)         memcpy(out.voice, s_file.voice, sizeof out.voice);
    if (s_pinned & PORT_PIN_TEXT)          memcpy(out.text, s_file.text, sizeof out.text);
    if (s_pinned & PORT_PIN_FPS_LIMIT)     out.fps_limit = s_file.fps_limit;
    if (s_pinned & PORT_PIN_RECORD_THREAD) out.record_thread = s_file.record_thread;
    if (s_pinned & PORT_PIN_PARTICLES)     out.fullres_particles = s_file.fullres_particles;
    if (s_pinned & PORT_PIN_ANISO)         out.aniso = s_file.aniso;
    std::string dir = path;
    const size_t slash = dir.find_last_of("/\\");
    if (slash != std::string::npos) {
        dir.resize(slash);
#ifdef _WIN32
        if (!dir.empty()) _mkdir(dir.c_str());
#else
        if (!dir.empty()) mkdir(dir.c_str(), 0755);
#endif
    }
    /* Written beside and renamed into place: a crash mid-write leaves the
     * old file, not half of a new one. */
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return -1;
    fprintf(f,
            "; inFAMOUS 2 port settings. A variable set in the environment wins over\n"
            "; this file. The launcher and the in-game menu (F1) write it.\n"
            "\n[video]\n"
            "scale = %d          ; 1 = 720p, 2 = 1440p, 3 = 4K\n"
            "fullscreen = %d\n"
            "vsync = %d\n"
            "stretch = %d        ; 0 = black bars, 1 = stretch\n"
            "display = %d\n"
            "\n[language]\n"
            "voice = %s\n"
            "text = %s\n"
            "\n[performance]\n"
            "fps_limit = %d      ; 0 = none\n"
            "fps_counter = %d\n"
            "record_thread = %d\n"
            "\n[launcher]\n"
            "skip = %d\n"
            "\n[mods]\n"
            "fullres_particles = %d ; fire, sparks, smoke at the picture's resolution\n"
            "aniso = %d             ; 0 = the game's own, 2..16\n",
            s->scale, s->fullscreen, s->vsync, s->stretch, s->display,
            s->voice, s->text, s->fps_limit, s->fps_counter, s->record_thread,
            s->skip_launcher, s->fullres_particles, s->aniso);
    const int ok = fclose(f) == 0;
#ifdef _WIN32
    /* The CRT's rename() refuses an existing target. */
    if (!ok || !MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) { remove(tmp.c_str()); return -1; }
#else
    if (!ok || rename(tmp.c_str(), path)) { remove(tmp.c_str()); return -1; }
#endif
    return 0;
}

static std::string s_exported[32];
static int s_nexported;

static void put(const char* name, const char* value)
{
    if (getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
    if (s_nexported < 32) s_exported[s_nexported++] = name;
}

/* The backend's reading of a switch: set, not empty, not "0". */
static int env_on(const char* v) { return v[0] && v[0] != '0'; }

unsigned port_settings_from_env(port_settings* s)
{
    s_file = *s;
    unsigned pin = 0;
    const char* v;
    if ((v = getenv("PS3RECOMP_RES_SCALE"))) {
        const int n = atoi(v);
        if (n >= 1 && n <= 3) { s->scale = n; pin |= PORT_PIN_SCALE; }
    }
    if ((v = getenv("PS3RECOMP_VK_FULLSCREEN"))) { s->fullscreen = env_on(v); pin |= PORT_PIN_FULLSCREEN; }
    if ((v = getenv("PS3RECOMP_VK_PRESENT")))    { s->vsync = !strcmp(v, "fifo"); pin |= PORT_PIN_VSYNC; }
    if ((v = getenv("PS3RECOMP_VK_STRETCH")))    { s->stretch = env_on(v); pin |= PORT_PIN_STRETCH; }
    if ((v = getenv("PS3RECOMP_VK_DISPLAY")))    { s->display = atoi(v) < 0 ? 0 : atoi(v); pin |= PORT_PIN_DISPLAY; }
    if ((v = getenv("PS3_VOICE_LANG")) && voice_known(v)) {
        snprintf(s->voice, sizeof s->voice, "%s", v); pin |= PORT_PIN_VOICE;
    }
    if ((v = getenv("PS3_TEXT_LANG")) && lang_known(v)) {
        snprintf(s->text, sizeof s->text, "%s", v); pin |= PORT_PIN_TEXT;
    }
    if ((v = getenv("PS3RECOMP_FPS_LIMIT")))     { s->fps_limit = atoi(v) < 0 ? 0 : atoi(v); pin |= PORT_PIN_FPS_LIMIT; }
    if ((v = getenv("PS3RECOMP_BE_THREAD")))     { s->record_thread = env_on(v); pin |= PORT_PIN_RECORD_THREAD; }
    if ((v = getenv("PS3RECOMP_FULLRES_PARTICLES"))) { s->fullres_particles = env_on(v); pin |= PORT_PIN_PARTICLES; }
    if ((v = getenv("PS3RECOMP_ANISO")))         { s->aniso = atoi(v) < 0 ? 0 : atoi(v) > 16 ? 16 : atoi(v); pin |= PORT_PIN_ANISO; }
    s_pinned = pin;
    return pin;
}

unsigned port_settings_pinned(void) { return s_pinned; }

extern char** environ;

char** port_settings_restart_env(const char* extra)
{
    size_t n = 0;
    while (environ[n]) n++;
    char** env = (char**)calloc(n + 3, sizeof *env);
    if (!env) return nullptr;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char* e = environ[i];
        const char* eq = strchr(e, '=');
        const size_t len = eq ? (size_t)(eq - e) : strlen(e);
        bool drop = len == 17 && !strncmp(e, "PS3_SKIP_LAUNCHER", 17);
        for (int j = 0; j < s_nexported && !drop; j++)
            drop = s_exported[j].size() == len && !strncmp(e, s_exported[j].c_str(), len);
        if (!drop) env[k++] = (char*)e;
    }
    env[k++] = (char*)"PS3_SKIP_LAUNCHER=1";
    if (extra) env[k++] = (char*)extra;
    env[k] = nullptr;
    return env;
}

void port_settings_export(const port_settings* s)
{
    char b[32];
    snprintf(b, sizeof b, "%d", s->scale);         put("PS3RECOMP_RES_SCALE", b);
    put("PS3RECOMP_VK_FULLSCREEN", s->fullscreen ? "1" : "0");
    put("PS3RECOMP_VK_PRESENT", s->vsync ? "fifo" : "mailbox");
    put("PS3RECOMP_VK_STRETCH", s->stretch ? "1" : "0");
    snprintf(b, sizeof b, "%d", s->display);       put("PS3RECOMP_VK_DISPLAY", b);
    put("PS3_VOICE_LANG", s->voice);
    put("PS3_TEXT_LANG", s->text);
    /* The console language the game picks its pair by: the text's where the
     * North American list has it (english, french, latino, brazilian, through
     * cellSysutil's numbers -- docs/NOTES.md, "Language"), so the game's own
     * logic matches the text; Portuguese otherwise. The packs themselves come
     * from the redirect above whatever the game picks. */
    const char* t = s->text;
    put("PS3_SYS_LANG", !strcmp(t, "english") || !strcmp(t, "british") ? "en"
                      : !strcmp(t, "french") ? "fr"
                      : !strcmp(t, "latino") || !strcmp(t, "spanish") ? "es" : "pt");
    snprintf(b, sizeof b, "%d", s->fps_limit);     put("PS3RECOMP_FPS_LIMIT", b);
    put("PS3RECOMP_BE_THREAD", s->record_thread ? "1" : "0");
    /* The GPU reads the guest's vertex bytes itself: the CPU fetch it
     * replaces was what made turning the camera drop to ~40 fps in the city,
     * where most geometry is rebuilt by the SPUs every frame. */
    put("PS3RECOMP_ENG_VPULL", "1");
    snprintf(b, sizeof b, "%d", s->aniso);         put("PS3RECOMP_ANISO", b);
    put("PS3RECOMP_FULLRES_PARTICLES", s->fullres_particles ? "1" : "0");
    if (s->skip_launcher) put("PS3_SKIP_LAUNCHER", "1");
}

const char* port_settings_path(void)
{
    static std::string p;
    if (p.empty()) {
        const char* d = getenv("PS3_USER_DIR");
        p = std::string(d && *d ? d : "user") + "/settings.ini";
    }
    return p.c_str();
}

port_settings* port_settings_get(void)
{
    static port_settings s;
    static int loaded;
    if (!loaded) { loaded = 1; port_settings_defaults(&s); }
    return &s;
}
