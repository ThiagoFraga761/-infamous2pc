/* settings.cpp: round trip, defaults, bad values, the environment winning,
 * pinned fields, the restart environment.
 *   c++ -std=c++17 -I.. ../settings.cpp settings_test.cpp -o /tmp/st && /tmp/st */
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main()
{
    char dir[] = "/tmp/settings_test.XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string path = std::string(dir) + "/sub/settings.ini";

    port_settings s, r;
    CHECK(port_settings_load(&s, path.c_str()) == 0);          /* missing: defaults */
    CHECK(s.scale == 1 && s.record_thread == 1 && !strcmp(s.voice, "brazilian"));

    s.scale = 3; s.fullscreen = 1; s.vsync = 1; s.stretch = 1; s.display = 2;
    snprintf(s.voice, sizeof s.voice, "english"); snprintf(s.text, sizeof s.text, "brazilian");
    s.fps_limit = 60; s.fps_counter = 1; s.record_thread = 0; s.skip_launcher = 1;
    CHECK(port_settings_save(&s, path.c_str()) == 0);            /* creates sub/ */
    CHECK(port_settings_load(&r, path.c_str()) == 1);
    CHECK(!memcmp(&s, &r, sizeof s));

    /* bad values fall back, unknown keys and sections are ignored */
    FILE* f = fopen(path.c_str(), "w");
    fputs("[video]\nscale = 9\nvsync = yes\n[language]\nvoice = klingon\ntext = french ; c\n"
          "[nope]\nx = 1\ngarbage line\n[performance]\nfps_limit = 120\n", f);
    fclose(f);
    CHECK(port_settings_load(&r, path.c_str()) == 1);
    CHECK(r.scale == 1 && r.vsync == 0 && !strcmp(r.voice, "brazilian") && !strcmp(r.text, "french"));
    CHECK(r.fps_limit == 120);

    /* export: the environment wins; the console language follows the text */
    unsetenv("PS3RECOMP_RES_SCALE"); unsetenv("PS3_SYS_LANG"); setenv("PS3_VOICE_LANG", "japanese", 1);
    r.scale = 2;
    port_settings_export(&r);
    CHECK(!strcmp(getenv("PS3RECOMP_RES_SCALE"), "2"));
    CHECK(!strcmp(getenv("PS3_VOICE_LANG"), "japanese"));
    CHECK(!strcmp(getenv("PS3_TEXT_LANG"), "french"));
    CHECK(!strcmp(getenv("PS3_SYS_LANG"), "fr"));

    /* the environment pins what it sets: the struct shows it, the file keeps its own */
    {
        const std::string p2 = std::string(dir) + "/pin.ini";
        port_settings a; port_settings_defaults(&a);
        a.scale = 1; snprintf(a.voice, sizeof a.voice, "english");
        CHECK(port_settings_save(&a, p2.c_str()) == 0);
        setenv("PS3RECOMP_RES_SCALE", "3", 1); unsetenv("PS3_VOICE_LANG"); unsetenv("PS3_TEXT_LANG");
        unsetenv("PS3RECOMP_VK_FULLSCREEN"); unsetenv("PS3RECOMP_VK_PRESENT"); unsetenv("PS3RECOMP_VK_STRETCH");
        unsetenv("PS3RECOMP_VK_DISPLAY"); unsetenv("PS3RECOMP_FPS_LIMIT"); setenv("PS3RECOMP_BE_THREAD", "0", 1);
        port_settings b;
        CHECK(port_settings_load(&b, p2.c_str()) == 1);
        const unsigned pin = port_settings_from_env(&b);
        CHECK(pin == (PORT_PIN_SCALE | PORT_PIN_RECORD_THREAD));
        CHECK(port_settings_pinned() == pin);
        CHECK(b.scale == 3 && b.record_thread == 0 && !strcmp(b.voice, "english"));
        b.fps_counter = 1;                                    /* the UI changes something else */
        CHECK(port_settings_save(&b, p2.c_str()) == 0);
        port_settings c;
        CHECK(port_settings_load(&c, p2.c_str()) == 1);
        CHECK(c.scale == 1 && c.record_thread == 1 && c.fps_counter == 1);
        unsetenv("PS3RECOMP_RES_SCALE"); unsetenv("PS3RECOMP_BE_THREAD");
        port_settings_from_env(&c);                           /* nothing pinned again */
        CHECK(port_settings_pinned() == 0);
        remove(p2.c_str());
    }

    /* voices: text-only languages are refused */
    f = fopen(path.c_str(), "w");
    fputs("\xEF\xBB\xBF[language]\nvoice = korean\ntext = korean\n", f);   /* with a BOM */
    fclose(f);
    CHECK(port_settings_load(&r, path.c_str()) == 1);
    CHECK(!strcmp(r.voice, "brazilian") && !strcmp(r.text, "korean"));

    /* the restart environment: without what export set, with the skip */
    {
        unsetenv("PS3RECOMP_RES_SCALE"); unsetenv("PS3_SKIP_LAUNCHER");
        setenv("SETTINGS_TEST_KEEP", "1", 1); setenv("PS3_VOICE_LANG", "english", 1);
        port_settings e; port_settings_defaults(&e);
        port_settings_export(&e);                             /* RES_SCALE is ours now */
        char** env = port_settings_restart_env("SETTINGS_TEST_EXTRA=1");
        CHECK(env != nullptr);
        bool scale = false, keep = false, skip = false, voice = false, extra = false;
        for (char** p = env; p && *p; p++) {
            if (!strncmp(*p, "PS3RECOMP_RES_SCALE=", 20)) scale = true;
            if (!strcmp(*p, "SETTINGS_TEST_KEEP=1")) keep = true;
            if (!strcmp(*p, "PS3_SKIP_LAUNCHER=1")) skip = true;
            if (!strncmp(*p, "PS3_VOICE_LANG=", 15)) voice = true;   /* the caller's, not ours */
            if (!strcmp(*p, "SETTINGS_TEST_EXTRA=1")) extra = true;
        }
        CHECK(!scale && keep && skip && voice && extra);
        CHECK(getenv("PS3RECOMP_RES_SCALE") != nullptr);      /* this process keeps its own */
        free(env);
    }

    remove(path.c_str());
    printf(fails ? "settings_test: %d FAILED\n" : "settings_test: ok\n", fails);
    return fails != 0;
}
