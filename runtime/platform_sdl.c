/* Host platform layer on SDL2: window (Vulkan surface for DXVK), event pump.
 * The same code runs on desktop Linux and on Android. */
#include "game.h"
#include "rt.h"
#include <SDL2/SDL.h>
#ifndef __ANDROID__
#include <fcntl.h>
#endif

static SDL_Window *g_window;
static int g_frames;
static void load_key_script(void);

void platform_init(void) {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
        rt_fatal("SDL_Init: %s", SDL_GetError());
    int w = 1280, h = 720;
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    if (getenv("LB_WINDOW")) sscanf(getenv("LB_WINDOW"), "%dx%d", &w, &h);  /* desktop testing */
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { w = dm.w; h = dm.h; }
#endif
    g_window = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!g_window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    extern void user32_set_screen(int, int);
    user32_set_screen(w, h);
    LOG("platform: window %dx%d", w, h);
    load_key_script();
}

void *platform_window(void) { return g_window; }

/* Scripted input for automated runs: LB_KEYS="frame:Key[:frames],..." holds
 * the SDL-named key (e.g. Return, Space, Left) from that frame on. */
typedef struct { int frame, len; SDL_Scancode sc; int text; } KeyEv;
static KeyEv g_script[256];
static int g_nscript;
static Uint8 g_keys[SDL_NUM_SCANCODES];
static int g_padonly;  /* LB_PADONLY=1: scripted keys drive only the virtual XInput pad */

static void load_key_script(void) {
    const char *s = getenv("LB_KEYS");
    g_padonly = getenv("LB_PADONLY") && *getenv("LB_PADONLY") == '1';
    while (s && *s && g_nscript < 256) {
        char name[32] = "";
        int fr = 0, len = 10, n = 0;
        if (sscanf(s, "%d:%31[^:,]%n", &fr, name, &n) < 2) break;
        s += n;
        if (*s == ':') { s++; len = (int)strtol(s, (char **)&s, 10); }
        SDL_Scancode sc = SDL_GetScancodeFromName(name);
        if (sc != SDL_SCANCODE_UNKNOWN) g_script[g_nscript++] = (KeyEv){fr, len, sc, 0};
        else LOG("LB_KEYS: unknown key %s", name);
        if (*s == ',') s++;
    }
}

/* SDL scancode -> Windows virtual-key code */
static uint32_t vk_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (s - SDL_SCANCODE_F1);
    switch (s) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_RETURN: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    default: return 0;
    }
}

static void key_event(SDL_Scancode sc, int down) {
    extern void user32_key_event(uint32_t vk, uint32_t sc, int down);
    uint32_t vk = vk_of(sc);
    /* PC scan code: approximate with the DirectInput code, which matches set 1 */
    extern uint32_t dinput_dik_of(int sdl_scancode);
    user32_key_event(vk, dinput_dik_of(sc), down);
}

/* Emits key events for scripted keys whose state changed this frame. */
static void script_events(void) {
    static Uint8 prev[SDL_NUM_SCANCODES];
    if (!g_nscript) return;
    Uint8 cur[SDL_NUM_SCANCODES] = {0};
    for (int i = 0; i < g_nscript; i++)
        if (g_frames >= g_script[i].frame && g_frames < g_script[i].frame + g_script[i].len) cur[g_script[i].sc] = 1;
    for (int i = 0; i < g_nscript; i++) {
        SDL_Scancode sc = g_script[i].sc;
        if (cur[sc] != prev[sc]) {
            if (!g_padonly || g_script[i].text) key_event(sc, cur[sc]);
            if (!g_script[i].text) {  /* typed text drives the keyboard only, not the virtual pad */
                SDL_Event e = {0};
                e.type = cur[sc] ? SDL_KEYDOWN : SDL_KEYUP;
                e.key.keysym.scancode = sc;
                e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
                extern void input_handle_event(const SDL_Event *e);
                input_handle_event(&e);
            }
            prev[sc] = cur[sc];
        }
    }
    /* drop typed keys that are over (their release was just sent) */
    int n = 0;
    for (int i = 0; i < g_nscript; i++)
        if (!g_script[i].text || g_frames < g_script[i].frame + g_script[i].len) g_script[n++] = g_script[i];
    g_nscript = n;
}

/* Phone keyboard: the keyboard button (touch.cpp) shows or hides it. The game
 * reads keys only through DirectInput (and turns them into characters with
 * ToUnicode), so each typed character becomes a short scripted key press,
 * with Shift held around capitals and symbols, one after another. */
static int g_text_on, g_text_hidden_frames, g_text_next;
int platform_text_input(int toggle) {
    if (toggle < 0) return g_text_on;
    g_text_on = !g_text_on;
    g_text_hidden_frames = 0;
    if (g_text_on) SDL_StartTextInput(); else SDL_StopTextInput();
    LOG("keyboard %s", g_text_on ? "shown" : "hidden");
    return g_text_on;
}
static void type_key(SDL_Scancode sc, int shift) {
    if (g_nscript > 250) return;
    if (g_text_next < g_frames + 1) g_text_next = g_frames + 1;
    if (shift) {
        g_script[g_nscript++] = (KeyEv){g_text_next, 4, SDL_SCANCODE_LSHIFT, 1};
        g_script[g_nscript++] = (KeyEv){g_text_next + 1, 2, sc, 1};
        g_text_next += 5;
    } else {
        g_script[g_nscript++] = (KeyEv){g_text_next, 2, sc, 1};
        g_text_next += 3;
    }
}
/* US layout */
static void type_char(unsigned ch) {
    static const char shifted_digits[] = ")!@#$%^&*(";
    static const struct { char c, s; SDL_Scancode sc; } sym[] = {
        {' ', 0, SDL_SCANCODE_SPACE}, {'-', 0, SDL_SCANCODE_MINUS}, {'_', 1, SDL_SCANCODE_MINUS},
        {'=', 0, SDL_SCANCODE_EQUALS}, {'+', 1, SDL_SCANCODE_EQUALS}, {'\'', 0, SDL_SCANCODE_APOSTROPHE},
        {'"', 1, SDL_SCANCODE_APOSTROPHE}, {',', 0, SDL_SCANCODE_COMMA}, {'<', 1, SDL_SCANCODE_COMMA},
        {'.', 0, SDL_SCANCODE_PERIOD}, {'>', 1, SDL_SCANCODE_PERIOD}, {'/', 0, SDL_SCANCODE_SLASH},
        {'?', 1, SDL_SCANCODE_SLASH}, {';', 0, SDL_SCANCODE_SEMICOLON}, {':', 1, SDL_SCANCODE_SEMICOLON},
        {'[', 0, SDL_SCANCODE_LEFTBRACKET}, {']', 0, SDL_SCANCODE_RIGHTBRACKET}, {'`', 0, SDL_SCANCODE_GRAVE},
        {'~', 1, SDL_SCANCODE_GRAVE}, {'\n', 0, SDL_SCANCODE_RETURN},
    };
    if (ch >= 'a' && ch <= 'z') { type_key((SDL_Scancode)(SDL_SCANCODE_A + ch - 'a'), 0); return; }
    if (ch >= 'A' && ch <= 'Z') { type_key((SDL_Scancode)(SDL_SCANCODE_A + ch - 'A'), 1); return; }
    if (ch == '0') { type_key(SDL_SCANCODE_0, 0); return; }
    if (ch >= '1' && ch <= '9') { type_key((SDL_Scancode)(SDL_SCANCODE_1 + ch - '1'), 0); return; }
    for (int i = 0; i < 10; i++)
        if (ch == (unsigned char)shifted_digits[i]) { type_key(i ? (SDL_Scancode)(SDL_SCANCODE_1 + i - 1) : SDL_SCANCODE_0, 1); return; }
    for (unsigned i = 0; i < sizeof sym / sizeof sym[0]; i++)
        if (ch == (unsigned char)sym[i].c) { type_key(sym[i].sc, sym[i].s); return; }
}
/* While the phone keyboard is up: characters arrive as SDL_TEXTINPUT (the
 * key events SDL makes up for them are pressed and released at once, too
 * fast for DirectInput polling), Backspace and Enter as key presses. Returns
 * 1 when the event was used. */
static int text_event(const SDL_Event *e) {
    if (!g_text_on) return 0;
    if (e->type == SDL_TEXTINPUT) {
        for (const unsigned char *p = (const unsigned char *)e->text.text; *p; p++)
            if (*p < 0x80) type_char(*p);
        return 1;
    }
    if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) {
        SDL_Scancode sc = e->key.keysym.scancode;
        int special = sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_ESCAPE ||
                      sc == SDL_SCANCODE_TAB || sc == SDL_SCANCODE_DELETE ||
                      (sc >= SDL_SCANCODE_RIGHT && sc <= SDL_SCANCODE_UP);
        if (special && e->type == SDL_KEYDOWN && !e->key.repeat) type_key(sc, 0);
        return special || SDL_GetKeyFromScancode(sc) < 0x80 || sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT;
    }
    return 0;
}
const Uint8 *platform_keys(int *n);
/* Shift as the game should see it (ToUnicode gets the game's own key state). */
int platform_shift_down(void) {
    const Uint8 *ks = platform_keys(NULL);
    return ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
}

const Uint8 *platform_keys(int *n) {
    int nk;
    const Uint8 *ks = SDL_GetKeyboardState(&nk);
    if (!g_nscript || g_padonly) { if (n) *n = nk; return ks; }
    memcpy(g_keys, ks, nk < SDL_NUM_SCANCODES ? nk : SDL_NUM_SCANCODES);
    for (int i = 0; i < g_nscript; i++)
        if (g_frames >= g_script[i].frame && g_frames < g_script[i].frame + g_script[i].len) g_keys[g_script[i].sc] = 1;
    if (n) *n = SDL_NUM_SCANCODES;
    return g_keys;
}

/* Interactive test input: LB_KEYFIFO=path, lines "Key[:frames]" are queued
 * at the current frame (desktop testing only). */
static void poll_key_fifo(void) {
#ifndef __ANDROID__
    static FILE *fifo;
    static int tried;
    if (!tried) {
        tried = 1;
        const char *p = getenv("LB_KEYFIFO");
        if (p) {
            int fd = open(p, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) fifo = fdopen(fd, "r");
        }
    }
    if (!fifo) return;
    char line[64];
    while (fgets(line, sizeof line, fifo)) {
        char name[32] = "";
        int len = 8;
        if (!strncmp(line, "dump ", 5)) {  /* "dump <file>": save the exe's data sections */
            line[strcspn(line, "\n")] = 0;
            FILE *d = fopen(line + 5, "wb");
            if (d) { fwrite(MEM + 0x934000, 1, 0x20E2490, d); fclose(d); }
            LOG("keyfifo: dumped data to %s at frame %d", line + 5, g_frames);
            continue;
        }
        if (sscanf(line, "%31[^:\n]:%d", name, &len) < 1) continue;
        SDL_Scancode sc = SDL_GetScancodeFromName(name);
        if (sc == SDL_SCANCODE_UNKNOWN || g_nscript >= 256) continue;
        g_script[g_nscript++] = (KeyEv){g_frames + 1, len, sc, 0};
        LOG("keyfifo: %s at frame %d", name, g_frames + 1);
    }
    clearerr(fifo);
#endif
}

void platform_present_hook(void) {
    SDL_Event e;
    poll_key_fifo();
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            LOG("quit requested");
            fflush(NULL);
            exit(0);
        }
        if (text_event(&e)) continue;
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)
            key_event(e.key.keysym.scancode, e.type == SDL_KEYDOWN);
        extern void input_handle_event(const SDL_Event *e);
        input_handle_event(&e);
        extern void touch_handle_event(const SDL_Event *e);
        touch_handle_event(&e);
    }
    script_events();
#ifdef __ANDROID__
    /* closed with the phone's Back key or its own button: follow it */
    if (g_text_on) {
        g_text_hidden_frames = SDL_IsScreenKeyboardShown(g_window) ? 0 : g_text_hidden_frames + 1;
        if (g_text_hidden_frames > 60) platform_text_input(1);
    }
#endif
    g_frames++;
#ifdef GAME_SKYRIM
    /* bGamepadEnable (Controls menu toggle, not read from the INI at boot):
     * always on, the phone's pad and touch overlay are XInput pads. */
    M8(0x12C3650) = 1;
#endif
    if (g_frames % 300 == 0) LOG("frame %d", g_frames);
    {   /* canary: the main thread's static TLS pointer (TEB slot 0 +0x2C) */
        static uint32_t last;
        uint32_t v = M32(GUEST_TEB_BASE + 0x2C);
        if (last && v != last)
            fprintf(stderr, "frame %d: main TEB+0x2C changed %08x -> %08x (TEB+0x24 tid %u, +0x1000 %08x)\n",
                    g_frames, last, v, M32(GUEST_TEB_BASE + 0x24), M32(GUEST_TEB_BASE + 0x1000));
        last = v;
    }
}
void platform_window_size(int *w, int *h) { if (g_window) SDL_GetWindowSize(g_window, w, h); }
int platform_frame_count(void) { return g_frames; }
