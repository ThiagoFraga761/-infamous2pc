/* The port's screens over the game's window, drawn with Dear ImGui through the
 * toolkit's rsx_ui (libs/video/rsx_ui.h): the in-game menu (F1, or Select+Start
 * held half a second) and the FPS counter. Every change is written to
 * user/settings.ini at once (settings.h); the options marked live apply now,
 * the others at the next start. */
#include "settings.h"
#include "ui.h"

#ifdef PS3RECOMP_UI

#include "imgui.h"
#include "rsx_ui.h"

#include <SDL2/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <unistd.h>

extern "C" void rsx_vulkan_backend_set_fullscreen(int on);
extern "C" void rsx_vulkan_backend_set_vsync(int on);
extern "C" void rsx_vulkan_backend_set_stretch(int on);
extern "C" void rsx_vulkan_backend_set_fps_limit(int fps);
extern "C" int rsx_vulkan_backend_fullscreen(void);
extern "C" unsigned ppu_boot_frames_presented(void);

namespace {

struct Lang { const char* pack; const char* name; bool voice; };
const Lang k_langs[] = {
    { "brazilian",  "Português (Brasil)",          true },
    { "english",    "Inglês",                       true },
    { "latino",     "Espanhol (América Latina)",    true },
    { "spanish",    "Espanhol (Espanha)",           true },
    { "french",     "Francês",                      true },
    { "german",     "Alemão",                       true },
    { "italian",    "Italiano",                     true },
    { "portuguese", "Português (Portugal)",         true },
    { "russian",    "Russo",                        true },
    { "polish",     "Polonês",                      true },
    { "dutch",      "Holandês",                     true },
    { "turkish",    "Turco",                        true },
    { "japanese",   "Japonês",                      true },
    { "british",    "Inglês (Reino Unido)",         false },
    { "korean",     "Coreano (experimental)",       false },
    { "chinese",    "Chinês (experimental)",        false },
};
const char* const k_scales[] = { "720p (1x)", "1440p (2x)", "4K (3x)" };
const int k_fps_limits[] = { 0, 30, 60, 120, -1 };
const char* const k_fps_names[] = { "Sem limite", "30", "60", "120", "Taxa do monitor" };

port_settings g_boot;          /* as the game started: what a restart would change */
int g_argc; char** g_argv;
std::atomic<int> g_go{1};      /* the main thread may boot the guest */
bool g_want_start = false;     /* the start screen was asked for */
std::atomic<bool> g_start{false};  /* ...and is showing */
double g_fps = 0.0, g_fps_t = 0.0;
unsigned g_fps_frames = 0;
bool g_styled = false;

void style()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10.0f; s.FrameRounding = 6.0f; s.TabRounding = 6.0f;
    s.GrabRounding = 6.0f; s.PopupRounding = 6.0f;
    s.WindowPadding = ImVec2(22, 18); s.FramePadding = ImVec2(12, 7);
    s.ItemSpacing = ImVec2(12, 12); s.WindowBorderSize = 0.0f;
    ImVec4* c = s.Colors;
    const ImVec4 accent(0.27f, 0.78f, 1.00f, 1.00f);                 /* #46c8ff */
    c[ImGuiCol_WindowBg]        = ImVec4(0.04f, 0.06f, 0.10f, 0.94f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.06f, 0.08f, 0.13f, 0.98f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.11f, 0.14f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.15f, 0.20f, 0.29f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.17f, 0.24f, 0.35f, 1.00f);
    c[ImGuiCol_Tab]             = ImVec4(0.10f, 0.13f, 0.19f, 1.00f);
    c[ImGuiCol_TabHovered]      = ImVec4(0.18f, 0.40f, 0.55f, 1.00f);
    c[ImGuiCol_TabSelected]     = ImVec4(0.13f, 0.42f, 0.60f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.13f, 0.18f, 0.27f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.18f, 0.40f, 0.55f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.20f, 0.52f, 0.72f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.13f, 0.18f, 0.27f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.18f, 0.40f, 0.55f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.20f, 0.52f, 0.72f, 1.00f);
    c[ImGuiCol_CheckMark]       = accent;
    c[ImGuiCol_SliderGrab]      = accent;
    c[ImGuiCol_NavHighlight]    = accent;
    c[ImGuiCol_Separator]       = ImVec4(0.20f, 0.25f, 0.33f, 1.00f);
    c[ImGuiCol_Text]            = ImVec4(0.90f, 0.93f, 0.97f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.55f, 0.60f, 0.68f, 1.00f);
}

void save()
{
    if (port_settings_save(port_settings_get(), port_settings_path()))
        fprintf(stderr, "[ui] could not write %s\n", port_settings_path());
}

int lang_index(const char* pack)
{
    for (int i = 0; i < (int)(sizeof k_langs / sizeof k_langs[0]); i++)
        if (!strcmp(k_langs[i].pack, pack)) return i;
    return 0;
}

/* A combo over the language list; only voice packs when `voice`. */
bool lang_combo(const char* label, char* pack, size_t cap, bool voice)
{
    const Lang& cur = k_langs[lang_index(pack)];
    bool changed = false;
    if (ImGui::BeginCombo(label, cur.name)) {
        for (const Lang& l : k_langs) {
            if (voice && !l.voice) continue;
            const bool sel = !strcmp(l.pack, pack);
            if (ImGui::Selectable(l.name, sel) && !sel) {
                snprintf(pack, cap, "%s", l.pack);
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void hint(const char* t)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor();
}

/* A row an environment variable decides (settings.h): shown, not editable. */
bool pinned(unsigned bit) { return (port_settings_pinned() & bit) != 0; }
void pin_begin(unsigned bit) { ImGui::BeginDisabled(pinned(bit)); }
void pin_end(unsigned bit)
{
    ImGui::EndDisabled();
    if (pinned(bit) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Definido por uma variável de ambiente (play.sh ou o terminal)");
}

/* `t` inside single quotes for the shell. */
std::string sh_quote(const std::string& t)
{
    std::string q = "'";
    for (char c : t) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

const char* saves_dir()
{
    const char* e = getenv("PS3_SAVEDATA_ROOT");
    return (e && *e) ? e : "gamedata/dev_hdd0/home/00000001/savedata";
}

void open_folder(const char* path)
{
    const std::string cmd = "xdg-open " + sh_quote(path) + " >/dev/null 2>&1 &";
    if (system(cmd.c_str()) != 0) fprintf(stderr, "[ui] could not open %s\n", path);
}

std::string g_last_backup;

void backup_saves()
{
    char ts[32];
    const time_t t = time(nullptr);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
    const char* u = getenv("PS3_USER_DIR");
    const std::string dst = std::string(u && *u ? u : "user") + "/backups/" + ts;
    const std::string cmd = "mkdir -p " + sh_quote(dst) + " && cp -a " + sh_quote(std::string(saves_dir()) + "/.") +
                            " " + sh_quote(dst + "/");
    g_last_backup = system(cmd.c_str()) == 0 ? "Backup em " + dst : "O backup falhou";
}

void page_video(port_settings& s)
{
    int sc = s.scale - 1;
    pin_begin(PORT_PIN_SCALE);
    if (ImGui::Combo("Resolução", &sc, k_scales, 3)) { s.scale = sc + 1; save(); }
    pin_end(PORT_PIN_SCALE);
    /* The window as it is: F11 changes it without the menu. */
    bool b = rsx_vulkan_backend_fullscreen();
    if (ImGui::Checkbox("Tela cheia  (F11)", &b)) { s.fullscreen = b; rsx_vulkan_backend_set_fullscreen(b); save(); }
    b = s.vsync;
    if (ImGui::Checkbox("VSync", &b)) { s.vsync = b; rsx_vulkan_backend_set_vsync(b); save(); }
    b = s.stretch;
    if (ImGui::Checkbox("Esticar a imagem (sem barras pretas)", &b)) {
        s.stretch = b; rsx_vulkan_backend_set_stretch(b); save();
    }
    const int nd = SDL_GetNumVideoDisplays();
    if (nd > 1 && !pinned(PORT_PIN_DISPLAY)) {
        const char* cur = SDL_GetDisplayName(s.display < nd ? s.display : 0);
        if (ImGui::BeginCombo("Monitor", cur ? cur : "?")) {
            for (int i = 0; i < nd; i++) {
                const char* n = SDL_GetDisplayName(i);
                char label[160]; snprintf(label, sizeof label, "%d: %s", i + 1, n ? n : "?");
                if (ImGui::Selectable(label, i == s.display)) { s.display = i; save(); }
            }
            ImGui::EndCombo();
        }
    }
    hint("Resolução e monitor valem a partir da próxima vez que o jogo abrir.");
}

void page_language(port_settings& s)
{
    pin_begin(PORT_PIN_VOICE);
    if (lang_combo("Dublagem", s.voice, sizeof s.voice, true)) save();
    pin_end(PORT_PIN_VOICE);
    pin_begin(PORT_PIN_TEXT);
    if (lang_combo("Legendas e textos", s.text, sizeof s.text, false)) save();
    pin_end(PORT_PIN_TEXT);
    hint("Dá para misturar, por exemplo vozes em inglês e legendas em português. "
         "Ligar ou desligar as legendas fica no menu de opções do próprio jogo. "
         "O idioma vale a partir da próxima vez que o jogo abrir.");
}

void page_performance(port_settings& s)
{
    ImGui::TextUnformatted("Predefinição");
    const char* names[] = { "Desempenho (720p)", "Equilibrado (1440p)", "Qualidade (4K)" };
    pin_begin(PORT_PIN_SCALE | PORT_PIN_RECORD_THREAD | PORT_PIN_FPS_LIMIT);
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(names[i])) { s.scale = i + 1; s.record_thread = 1; s.fps_limit = 0;
                                       rsx_vulkan_backend_set_fps_limit(0); save(); }
    }
    pin_end(PORT_PIN_SCALE | PORT_PIN_RECORD_THREAD | PORT_PIN_FPS_LIMIT);
    int li = 0;
    for (int i = 0; i < 5; i++) if (k_fps_limits[i] == s.fps_limit) li = i;
    if (s.fps_limit > 0 && k_fps_limits[li] != s.fps_limit) li = 4;
    pin_begin(PORT_PIN_FPS_LIMIT);
    if (ImGui::Combo("Limite de FPS", &li, k_fps_names, 5)) {
        int v = k_fps_limits[li];
        if (v < 0) {
            SDL_DisplayMode dm;
            v = SDL_GetCurrentDisplayMode(rsx_ui_display_index(), &dm) == 0 && dm.refresh_rate > 0
                ? dm.refresh_rate : 60;
        }
        s.fps_limit = v; rsx_vulkan_backend_set_fps_limit(v); save();
    }
    pin_end(PORT_PIN_FPS_LIMIT);
    bool b = s.fps_counter;
    if (ImGui::Checkbox("Contador de FPS na tela", &b)) { s.fps_counter = b; rsx_ui_set_visible(b); save(); }
    b = s.record_thread;
    pin_begin(PORT_PIN_RECORD_THREAD);
    if (ImGui::Checkbox("Thread de gravação Vulkan (+12% nas áreas pesadas)", &b)) { s.record_thread = b; save(); }
    pin_end(PORT_PIN_RECORD_THREAD);
    hint("A thread de gravação vale a partir da próxima vez que o jogo abrir.");
}

void page_controller(port_settings& s)
{
    SDL_GameController* gc = nullptr;
    for (int i = 0; i < SDL_NumJoysticks() && !gc; i++)
        if (SDL_IsGameController(i)) gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
    const char* name = gc ? SDL_GameControllerName(gc) : nullptr;
    ImGui::Text("Controle: %s", name ? name : "nenhum encontrado (conecte antes de abrir o jogo)");
    if (gc) {
        static const struct { SDL_GameControllerButton b; const char* n; } k[] = {
            { SDL_CONTROLLER_BUTTON_A, "Xis/A" }, { SDL_CONTROLLER_BUTTON_B, "Bola/B" },
            { SDL_CONTROLLER_BUTTON_X, "Quadrado/X" }, { SDL_CONTROLLER_BUTTON_Y, "Triângulo/Y" },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L1" }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R1" },
            { SDL_CONTROLLER_BUTTON_BACK, "Select" }, { SDL_CONTROLLER_BUTTON_START, "Start" },
            { SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3" }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3" },
        };
        std::string held;
        for (const auto& e : k)
            if (SDL_GameControllerGetButton(gc, e.b)) { held += held.empty() ? "" : "  "; held += e.n; }
        ImGui::Text("Apertados: %s", held.empty() ? "nenhum" : held.c_str());
    }
    ImGui::Separator();
    if (ImGui::Button("Abrir pasta dos saves")) open_folder(saves_dir());
    ImGui::SameLine();
    if (ImGui::Button("Fazer backup dos saves")) backup_saves();
    ImGui::SameLine();
    if (ImGui::Button("Abrir pasta dos logs")) open_folder("work/runs");
    if (!g_last_backup.empty()) hint(g_last_backup.c_str());
    hint("O jogo não tem menu principal: depois do primeiro save ele sempre continua do mais recente. "
         "Novo jogo e carregar outro save ficam no próprio jogo, em Start > Dados salvos.");
    ImGui::Separator();
    bool b = s.skip_launcher;
    if (ImGui::Checkbox("Pular a tela de opções ao abrir o jogo (F1 abre no jogo)", &b)) { s.skip_launcher = b; save(); }
}

/* What changed since the game started and needs a restart. */
std::string restart_note(const port_settings& s)
{
    std::string n;
    auto add = [&](const char* w) { n += n.empty() ? "" : ", "; n += w; };
    if (s.scale != g_boot.scale) add("resolução");
    if (s.display != g_boot.display) add("monitor");
    if (strcmp(s.voice, g_boot.voice) || strcmp(s.text, g_boot.text)) add("idioma");
    if (s.record_thread != g_boot.record_thread) add("thread de gravação");
    return n;
}

void draw_menu()
{
    port_settings& s = *port_settings_get();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fs * 38.0f < io.DisplaySize.x * 0.92f ? fs * 38.0f : io.DisplaySize.x * 0.92f,
                                    fs * 26.0f < io.DisplaySize.y * 0.9f ? fs * 26.0f : io.DisplaySize.y * 0.9f),
                             ImGuiCond_Always);
    ImGui::Begin("inFAMOUS 2  ·  Opções", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTabBar("pages")) {
        if (ImGui::BeginTabItem("Vídeo")) { page_video(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Idioma")) { page_language(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Desempenho")) { page_performance(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Controle e saves")) { page_controller(s); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    const std::string note = restart_note(s);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - fs * 3.2f);
    ImGui::Separator();
    if (!note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
        ImGui::Text("Reinicie o jogo para aplicar: %s.", note.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - fs * 7.5f);
    if (ImGui::Button("Fechar  (F1)")) rsx_ui_set_menu_open(0);
    ImGui::End();
}

void draw_fps()
{
    const ImGuiIO& io = ImGui::GetIO();
    const char* t = nullptr;
    char b[32];
    snprintf(b, sizeof b, "%.0f fps", g_fps);
    t = b;
    const ImVec2 sz = ImGui::CalcTextSize(t);
    const ImVec2 p(io.DisplaySize.x - sz.x - 18.0f, 12.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(p.x - 8, p.y - 4), ImVec2(p.x + sz.x + 8, p.y + sz.y + 4), IM_COL32(0, 0, 0, 150), 6.0f);
    dl->AddText(p, IM_COL32(120, 220, 255, 255), t);
}

/* "Play": boot, or start the process again when an option that only takes
 * effect at start changed here. */
void play()
{
    port_settings& s = *port_settings_get();
    save();
    if (!restart_note(s).empty() && g_argv) {
        fprintf(stderr, "[ui] starting again with the new settings\n");
        fflush(stderr); fflush(stdout);
        if (char** env = port_settings_restart_env()) {
            /* The binary's own path, not /proc/self/exe: the process keeps
             * its name (MyGameRecomp), which play.sh and the scripts look for. */
            char self[4096];
            const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
            if (n > 0) { self[n] = 0; execve(self, g_argv, env); }
            execve("/proc/self/exe", g_argv, env);
            free(env);
        }
        fprintf(stderr, "[ui] could not start again: the old settings stay for this run\n");
    }
    g_start = false;
    rsx_ui_set_modal(0);
    rsx_ui_set_menu_open(0);
    rsx_ui_set_visible(s.fps_counter);
    g_go = 1;
}

void draw_start()
{
    port_settings& s = *port_settings_get();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 d = io.DisplaySize;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilledMultiColor(ImVec2(0, 0), d, IM_COL32(14, 22, 38, 255), IM_COL32(8, 12, 22, 255),
                                IM_COL32(2, 3, 6, 255), IM_COL32(4, 6, 12, 255));
    const float fs = ImGui::GetFontSize();
    /* the title, top left */
    ImFont* tf = (ImFont*)rsx_ui_title_font();
    const ImVec2 tp(d.x * 0.06f, d.y * 0.06f);
    const float th = fs * 3.0f;
    bg->AddText(tf, th, tp, IM_COL32(255, 255, 255, 255), "inFAMOUS 2");
    bg->AddRectFilled(ImVec2(tp.x + 4, tp.y + th * 1.08f), ImVec2(tp.x + 4 + fs * 5.0f, tp.y + th * 1.08f + 5),
                      IM_COL32(70, 200, 255, 255));
    bg->AddText(ImVec2(tp.x + 4, tp.y + th * 1.08f + fs * 0.9f), IM_COL32(70, 200, 255, 255),
                "PORT NATIVO PARA PC   ·   Linux   ·   Vulkan");

    /* the options and "Play", below the title, as much of the window as is left */
    const float top = tp.y + th * 1.08f + fs * 2.6f;
    const ImVec2 sz(fs * 38.0f < d.x * 0.88f ? fs * 38.0f : d.x * 0.88f,
                    fs * 24.0f < d.y - top - d.y * 0.05f ? fs * 24.0f : d.y - top - d.y * 0.05f);
    ImGui::SetNextWindowPos(ImVec2(tp.x, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    ImGui::Begin("##start", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTabBar("pages")) {
        if (ImGui::BeginTabItem("Vídeo")) { page_video(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Idioma")) { page_language(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Desempenho")) { page_performance(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Controle e saves")) { page_controller(s); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::SetCursorPosY(sz.y - fs * 3.4f);
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.55f, 0.80f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.70f, 0.95f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.78f, 1.00f, 1.0f));
    static bool focused;
    if (!focused) { ImGui::SetKeyboardFocusHere(); focused = true; }
    const bool go = ImGui::Button("JOGAR", ImVec2(fs * 9.0f, fs * 2.0f));
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    const std::string note = restart_note(s);
    hint(note.empty() ? "O jogo continua do último save. Novo jogo e outros saves: Start > Dados salvos, "
                        "dentro do jogo. F1 (ou Select+Start segurados) abre estas opções lá."
                      : "As mudanças de resolução, monitor ou idioma reabrem o jogo ao clicar em Jogar.");
    ImGui::End();
    if (go) play();
}

void draw(void*)
{
    if (!g_styled) { style(); g_styled = true; }
    /* Frames presented over half a second. */
    g_fps_t += rsx_ui_frame_dt();
    if (g_fps_t >= 0.5) {
        const unsigned f = ppu_boot_frames_presented();
        g_fps = (double)(f - g_fps_frames) / g_fps_t;
        g_fps_frames = f; g_fps_t = 0.0;
    }
    if (g_start) { draw_start(); return; }
    if (port_settings_get()->fps_counter) draw_fps();
    if (rsx_ui_menu_open()) draw_menu();
}

} /* namespace */

void port_ui_init(int argc, char** argv)
{
    g_boot = *port_settings_get();
    g_argc = argc; g_argv = argv;
    rsx_ui_set_draw(draw, nullptr);
    rsx_ui_set_visible(port_settings_get()->fps_counter);
    bool play_arg = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--play")) play_arg = true;
    const char* skip = getenv("PS3_SKIP_LAUNCHER");
    g_want_start = !play_arg && !(skip && skip[0] == '1') && !port_settings_get()->skip_launcher;
    if (g_want_start) g_go = 0;            /* port_ui_backend_ready decides */
}

void port_ui_backend_ready(void)
{
    if (!g_want_start) return;
    if (!rsx_ui_available()) {
        fprintf(stderr, "[ui] no UI: the game starts without its start screen\n");
        g_go = 1;
        return;
    }
    g_start = true;
    rsx_ui_set_modal(1);
    rsx_ui_set_visible(1);
}

void port_ui_wait_start(void)
{
    while (!g_go.load()) {
        /* The window went away under the start screen: nothing can press Play. */
        if (g_start.load() && !rsx_ui_available()) {
            fprintf(stderr, "[ui] the start screen is gone: the game starts\n");
            g_start = false;
            g_go = 1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int port_ui_start_screen(void) { return g_start.load() ? 1 : 0; }

#else

void port_ui_init(int, char**) {}
void port_ui_backend_ready(void) {}
void port_ui_wait_start(void) {}
int port_ui_start_screen(void) { return 0; }

#endif
