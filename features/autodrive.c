#include "features.h"
#include "autodrive_game.h"
#include "autodrive_notice.h"
#include "autodrive_ui.h"
#include "logging.h"
#include "support.h"

#define CINEMATIC_DELAY 3.0f

typedef void (*UpdateFn)(void *, float);
typedef void (*ActionFn)(void *);
typedef void *(*DestroyFn)(void *, unsigned);
typedef SHORT(WINAPI *KeyStateFn)(int);
typedef HWND(WINAPI *ForegroundFn)(void);
typedef DWORD(WINAPI *WindowProcessFn)(HWND, LPDWORD);

static unsigned char *game_image;
static int cinematic_enabled;
static KeyStateFn key_state;
static ForegroundFn foreground_window;
static WindowProcessFn window_process;
static int input_initialized, f2_was_down;
static int pad_was_down, pad_blocked = 1;
static void *previous_controller;
static void *available_tracker;

typedef struct {
    void *satnav, *tracker, *car, *route;
    float elapsed;
    int cinematic, tried_camera, event;
    AutodriveCamera camera;
} AutodriveState;

static int events_enabled;

static AutodriveState drive;

#ifdef FH2_TWEAKS_TEST
static int test_input, test_f2, test_escape, test_focus;
#endif

static void input_read(int *f2, int *escape, int *focused) {
    *f2 = *escape = *focused = 0;
#ifdef FH2_TWEAKS_TEST
    if (test_input) {
        *f2 = test_f2;
        *escape = test_escape;
        *focused = test_focus;
        return;
    }
#endif
    if (!input_initialized) {
        input_initialized = 1;
        HMODULE module = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) {
            key_state = (KeyStateFn)(uintptr_t)GetProcAddress(module, "GetAsyncKeyState");
            foreground_window =
                (ForegroundFn)(uintptr_t)GetProcAddress(module, "GetForegroundWindow");
            window_process =
                (WindowProcessFn)(uintptr_t)GetProcAddress(module, "GetWindowThreadProcessId");
        }
        if (!key_state || !foreground_window || !window_process) {
            log_append("AUTODRIVE: keyboard API unavailable; F2 is inactive.\r\n");
        }
    }
    if (!key_state || !foreground_window || !window_process) {
        return;
    }
    HWND window = foreground_window();
    DWORD process = 0;
    if (window) {
        window_process(window, &process);
    }
    *focused = process == GetCurrentProcessId();
    *f2 = (key_state(VK_F2) & 0x8000) != 0;
    *escape = (key_state(VK_ESCAPE) & 0x8000) != 0;
}

static void stop_driving(const char *reason, int flash) {
    if (!drive.car) {
        return;
    }
    void *camera = drive.camera.camera;
    autodrive_notice_clear();
    autodrive_ui_set_hidden(0);
    int restored = autodrive_game_restore_camera(&drive.camera);
    autodrive_game_set_ai(drive.car, 0);
    autodrive_game_end_route(drive.tracker);
    autodrive_game_release_car(drive.car);
    if (flash && restored) {
        autodrive_game_flash(camera);
    }
    drive = (AutodriveState){0};
    log_append(reason);
}

/* InEvents: in a race the native AI driver follows the event's own route, so only the
   driver handoff is needed (no GPS route tracking, no cinematic camera). */
static void start_event_driving(void *satnav) {
    void *car = autodrive_game_get_car();
    if (!car) {
        return;
    }
    if (autodrive_game_driver_mode(car) != 1) {
        autodrive_game_release_car(car);
        log_append("AUTODRIVE: toggle ignored while another flow controls the car.\r\n");
        return;
    }
    if (!autodrive_game_set_ai(car, 1)) {
        autodrive_game_release_car(car);
        log_append("AUTODRIVE: native AI driver handoff failed.\r\n");
        return;
    }
    drive.satnav = satnav;
    drive.car = car;
    drive.event = 1;
    drive.tried_camera = 1;
    log_append("AUTODRIVE: event driving started; F2 or D-pad Down cancels.\r\n");
}

static void start_driving(void *satnav) {
    if (!autodrive_game_is_freeroam()) {
        if (events_enabled && autodrive_game_is_unpaused()) {
            start_event_driving(satnav);
            return;
        }
        log_append("AUTODRIVE: toggle ignored outside unpaused single-player free roam.\r\n");
        return;
    }
    int arrived;
    void *route = autodrive_game_route(satnav, &arrived);
    if (!route || arrived) {
        log_append("AUTODRIVE: set a GPS destination before pressing F2 or D-pad Down.\r\n");
        return;
    }
    if (!autodrive_game_tracker_matches(available_tracker, satnav)) {
        log_append("AUTODRIVE: the local GPS route tracker is not ready.\r\n");
        return;
    }
    void *car = autodrive_game_get_car();
    if (!car) {
        return;
    }
    if (autodrive_game_driver_mode(car) != 1) {
        autodrive_game_release_car(car);
        log_append("AUTODRIVE: toggle ignored while another flow controls the car.\r\n");
        return;
    }
    if (!autodrive_game_begin_route(available_tracker)) {
        autodrive_game_release_car(car);
        log_append("AUTODRIVE: GPS event-route tracking could not start, or is already owned.\r\n");
        return;
    }
    if (!autodrive_game_set_ai(car, 1)) {
        autodrive_game_end_route(available_tracker);
        autodrive_game_release_car(car);
        log_append("AUTODRIVE: native AI driver handoff failed.\r\n");
        return;
    }
    drive.satnav = satnav;
    drive.tracker = available_tracker;
    drive.car = car;
    drive.route = route;
    log_append(autodrive_notice_show()
                   ? "AUTODRIVE: activation notice submitted to the XP-award HUD.\r\n"
                   : "AUTODRIVE: activation notice unavailable; driving continues.\r\n");
    log_append("AUTODRIVE: GPS route tracking started; F2 or D-pad Down cancels.\r\n");
}

static void enter_cinematic(void) {
    drive.tried_camera = 1;
    if (!autodrive_game_slow_spin(&drive.camera)) {
        log_append(
            "AUTODRIVE: Slow Spin is unavailable; continuing with the normal HUD and camera.\r\n");
        return;
    }
    drive.cinematic = 1;
    autodrive_ui_set_hidden(1);
    autodrive_game_flash(drive.camera.camera);
    log_append("AUTODRIVE: Slow Spin selected; UI rendering disabled.\r\n");
}

static void autodrive_tick(void *satnav, float dt) {
    int f2, escape, focused;
    input_read(&f2, &escape, &focused);
    int pressed = f2 && !f2_was_down;
    f2_was_down = f2;
    void *controller;
    int down = autodrive_game_dpad_down(&controller);
    if (down < 0 || controller != previous_controller || !focused) {
        pad_blocked = 1;
    } else if (!down) {
        pad_blocked = 0;
    }
    pressed |= down > 0 && !pad_was_down && !pad_blocked;
    if (down > 0 && !pad_was_down) {
        log_append(
            pad_blocked
                ? "AUTODRIVE: D-pad Down ignored until a neutral release after input recovery.\r\n"
                : "AUTODRIVE: D-pad Down toggle received.\r\n");
    }
    pad_was_down = down > 0;
    previous_controller = controller;
    if (drive.car) {
        if (satnav != drive.satnav) {
            return;
        }
        if (pressed || escape || !focused) {
            stop_driving("AUTODRIVE: cancelled by toggle, Escape, or focus loss.\r\n",
                         !drive.event && pressed && focused && !escape &&
                             autodrive_game_is_freeroam());
            return;
        }
        if (drive.event) {
            void *current_car = autodrive_game_get_car();
            int same_car = current_car == drive.car;
            autodrive_game_release_car(current_car);
            if (!autodrive_game_is_unpaused() || autodrive_game_driving_input(controller) ||
                !same_car || autodrive_game_driver_mode(drive.car) != 0) {
                stop_driving("AUTODRIVE: event driving stopped (pause, input, car or control "
                             "change).\r\n",
                             0);
            }
            return;
        }
        if (!autodrive_game_is_freeroam()) {
            stop_driving("AUTODRIVE: stopped when free roam was paused or left.\r\n", 0);
            return;
        }
        if (autodrive_game_driving_input(controller)) {
            stop_driving("AUTODRIVE: cancelled by steering, throttle or braking.\r\n", 1);
            return;
        }
        void *current_car = autodrive_game_get_car();
        int same_car = current_car == drive.car;
        autodrive_game_release_car(current_car);
        if (!same_car || autodrive_game_driver_mode(drive.car) != 0) {
            stop_driving("AUTODRIVE: stopped on car change or a native control change.\r\n", 0);
            return;
        }
        int arrived;
        void *route = autodrive_game_route(satnav, &arrived);
        if (arrived || route != drive.route) {
            stop_driving(
                arrived ? "AUTODRIVE: GPS destination reached; player control restored.\r\n"
                        : "AUTODRIVE: GPS route changed or cleared; player control restored.\r\n",
                1);
            return;
        }
        if (dt > 0.0f && dt <= 1.0f) {
            drive.elapsed += dt;
        }
        autodrive_notice_update(drive.elapsed);
        if (cinematic_enabled && !drive.tried_camera && drive.elapsed >= CINEMATIC_DELAY) {
            enter_cinematic();
        } else if (drive.cinematic) {
            autodrive_game_cycle_camera(&drive.camera);
        }
    } else if (focused && pressed && !escape) {
        start_driving(satnav);
    }
}

static void satnav_update(void *self, float dt) {
    ((UpdateFn)(game_image + 0x6b4d30))(self, dt);
    autodrive_tick(self, dt);
}

static void satnav_deactivate(void *self) {
    if (self == drive.satnav) {
        stop_driving("AUTODRIVE: stopped before GPS deactivation.\r\n", 0);
    }
    ((ActionFn)(game_image + 0x6c0c70))(self);
}

static void tracker_update(void *self, float dt) {
    available_tracker = self;
    ((UpdateFn)(game_image + 0x6b4e40))(self, dt);
}

static void *tracker_destroy(void *self, unsigned flags) {
    if (self == drive.tracker) {
        stop_driving("AUTODRIVE: stopped before route-tracker destruction.\r\n", 0);
    }
    if (self == available_tracker) {
        available_tracker = 0;
    }
    return ((DestroyFn)(game_image + 0x6a26e0))(self, flags);
}

static void pause_enter(void *self) {
    stop_driving("AUTODRIVE: stopped before the game's pause flow.\r\n", 0);
    ((ActionFn)(game_image + 0x8e98e0))(self);
}

static void event_context_enter(void *self) {
    stop_driving("AUTODRIVE: stopped before an event-context transition.\r\n", 0);
    ((ActionFn)(game_image + 0x912860))(self);
}

static unsigned char original_pointers[12][8], hook_pointers[12][8];
static const unsigned char *accepted_pointers[12][2];
PatchDefinition autodrive_hook_patches[12];

void autodrive_prepare_hooks(unsigned char *image, const ModOptions *options) {
    static const unsigned slots[] = {0x2cc3198, 0x2cc31b0, 0x2cc3288, 0x2cc3278,
                                     0x2d16698, 0x2d1ea50, 0x2e7f040, 0x2ecfaa8,
                                     0x2ecfaa0, 0x2e832a8, 0x2f09088, 0x2f09080};
    static const unsigned originals[] = {0x6b4d30,  0x6c0c70,  0x6b4e40,  0x6a26e0,
                                         0x8e98e0,  0x912860,  0x1140ed0, 0x15b51a0,
                                         0x15b5110, 0x1140ed0, 0x15b51a0, 0x15b5110};
    static const char *names[] = {"autodrive_gps_update",     "autodrive_gps_deactivation",
                                  "autodrive_tracker_update", "autodrive_tracker_destruction",
                                  "autodrive_pause_entry",    "autodrive_event_context_entry",
                                  "autodrive_ui_wait_render", "autodrive_ui_draw",
                                  "autodrive_ui_present",     "autodrive_ui_wait_render_plain",
                                  "autodrive_ui_draw_base",   "autodrive_ui_present_base"};
    uintptr_t hooks[] = {(uintptr_t)satnav_update,
                         (uintptr_t)satnav_deactivate,
                         (uintptr_t)tracker_update,
                         (uintptr_t)tracker_destroy,
                         (uintptr_t)pause_enter,
                         (uintptr_t)event_context_enter,
                         (uintptr_t)autodrive_ui_wait_render,
                         (uintptr_t)autodrive_ui_draw,
                         (uintptr_t)autodrive_ui_present,
                         (uintptr_t)autodrive_ui_wait_render,
                         (uintptr_t)autodrive_ui_draw,
                         (uintptr_t)autodrive_ui_present};
    game_image = image;
    autodrive_game_bind(image);
    autodrive_notice_prepare(image);
    autodrive_ui_prepare(image);
    cinematic_enabled = options->autodrive_cinematic;
    events_enabled = options->autodrive_events;
    for (unsigned i = 0; i < 12; ++i) {
        uintptr_t original = (uintptr_t)(image + originals[i]);
        memory_copy(original_pointers[i], &original, 8);
        memory_copy(hook_pointers[i], &hooks[i], 8);
        accepted_pointers[i][0] = original_pointers[i];
        accepted_pointers[i][1] = hook_pointers[i];
        autodrive_hook_patches[i] = (PatchDefinition){slots[i],
                                                      0,
                                                      8,
                                                      GROUP_AUTODRIVE,
                                                      names[i],
                                                      original_pointers[i],
                                                      hook_pointers[i],
                                                      hook_pointers[i],
                                                      accepted_pointers[i],
                                                      2};
    }
}

#ifdef FH2_TWEAKS_TEST
__declspec(dllexport) void Fh2TestAutodriveInput(int f2, int escape, int focus) {
    test_input = 1;
    test_f2 = f2;
    test_escape = escape;
    test_focus = focus;
}

__declspec(dllexport) int Fh2TestAutodriveState(void) {
    return drive.car ? (drive.cinematic ? 2 : 1) : 0;
}

__declspec(dllexport) void *Fh2TestAutodriveHookAddress(unsigned index) {
    uintptr_t address = 0;
    if (index < 12) {
        memory_copy(&address, hook_pointers[index], 8);
    }
    return (void *)address;
}
#endif
