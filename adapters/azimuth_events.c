/* FH2Online free roam: events (races) are not run by the world's relay yet, and entering one
   left the game stuck: its state NetworkGetGameOptionsOwnership (vtable game+0x2d0e2a0) asks the
   server for the GameOptions object (ObjectRequestOwnership), takes it locally as pending
   (owner +0x38 = 0xfffffffe, pending +0x70) and waits for the server's answer, which the relay
   never gives.

   A relay with races on ([Events] Races=1) answers it: the event is granted or denied. Otherwise
   (an older relay, races off) the answer is given here after EV_ANSWER_WAIT_MS, as the game's own
   "someone else has it": the GameOptions' previous owner (the server, saved as the state starts)
   is set back with the game's network owner setter (FUN_1417b24c0, which
   for a locally owned object runs FUN_1417a7290: the owner set, the pending request dropped, the
   change notified). The state then leaves by its OwnershipDenied branch, as it does when the
   server refuses, and the player is back in free roam; the INFINITY screen says why. */
#include "azimuth_host.h"
#include "logging.h"
#include "support.h"

#define EV_STATE_VTABLE_RVA 0x2d0e2a0u
#define EV_ENTER_SLOT 4
#define EV_DECIDE_SLOT 14
#define EV_ENTER_RVA 0x8c5570u
#define EV_DECIDE_RVA 0x8c8030u
#define EV_SET_OWNER_RVA 0x17b24c0u
#define EV_SESSION_RVA 0x34d6348u
#define EV_ANSWER_WAIT_MS 4000u

typedef void (*EvStateFn)(void *);
typedef void (*EvSetOwnerFn)(void *, const unsigned long long *);

static unsigned char *ev_game;
static EvStateFn ev_enter_original, ev_decide_original;
static unsigned long long ev_previous_owner[2];
static int ev_have_previous, ev_denied_logged;
static ULONGLONG ev_entered_at;

/* An online session (the game's own test in this state: its session object with +0xd0 set and
   +0xa8 clear). */
static int ev_online(void) {
    unsigned char *session = 0;
    unsigned long long running = 0;
    unsigned flag = 1;
    return memory_read_pointer(ev_game + EV_SESSION_RVA, (void **)&session) && session &&
           memory_read(session + 0xd0, &running, sizeof(running)) && running &&
           memory_read(session + 0xa8, &flag, sizeof(flag)) && flag == 0;
}

/* The session's GameOptions object (FUN_140a2ae90 reads it the same way: session +0xb8). */
static unsigned char *ev_game_options(void) {
    unsigned char *session = 0, *options = 0;
    return memory_read_pointer(ev_game + EV_SESSION_RVA, (void **)&session) && session &&
                   memory_read_pointer(session + 0xb8, (void **)&options)
               ? options
               : 0;
}

static void ev_enter(void *state) {
    ev_have_previous = 0;
    ev_entered_at = GetTickCount64();
    if (ev_online()) {
        unsigned char *options = ev_game_options();
        unsigned owner;
        if (options && memory_read(options + 0x38, &owner, sizeof(owner)) && owner < 0xfffffffeu &&
            memory_read(options + 0x38, ev_previous_owner, sizeof(ev_previous_owner))) {
            ev_have_previous = 1;
        }
    }
    ev_enter_original(state);
}

static void ev_decide(void *state) {
    if (ev_have_previous && GetTickCount64() - ev_entered_at >= EV_ANSWER_WAIT_MS && ev_online()) {
        unsigned char *options = ev_game_options();
        unsigned owner = 0;
        void *pending = 0;
        if (options && memory_read(options + 0x38, &owner, sizeof(owner)) && owner >= 0xfffffffeu &&
            memory_read_pointer(options + 0x70, &pending) && pending) {
            ev_have_previous = 0;
            ((EvSetOwnerFn)(uintptr_t)(ev_game + EV_SET_OWNER_RVA))(options, ev_previous_owner);
            azimuth_account_set_notice("Online events (races, Road Trip) are not available in free roam yet. "
                                       "Play them in HORIZON SOLO; free roam with your friends works.");
            if (!ev_denied_logged) {
                ev_denied_logged = 1;
                log_append("EVENTS: an event was entered in free roam online and the relay did not answer, "
                           "so the game was told the event is taken (back to free roam).\r\n");
            }
        }
    }
    ev_decide_original(state);
}

static int ev_patch_slot(unsigned slot, unsigned expected_rva, void *hook, EvStateFn *original) {
    void **at = (void **)(ev_game + EV_STATE_VTABLE_RVA + slot * sizeof(void *));
    void *current = 0;
    DWORD old_protection, ignored;
    if (!memory_read(at, &current, sizeof(current)) || current != ev_game + expected_rva ||
        !VirtualProtect(at, sizeof(*at), PAGE_READWRITE, &old_protection)) {
        return 0;
    }
    *original = (EvStateFn)(uintptr_t)current;
    *at = hook;
    VirtualProtect(at, sizeof(*at), old_protection, &ignored);
    return 1;
}

void azimuth_apply_events(unsigned char *game_image) {
    if (!azimuth_net_proxy_enabled() || !game_image) {
        return;
    }
    ev_game = game_image;
    if (!ev_patch_slot(EV_ENTER_SLOT, EV_ENTER_RVA, (void *)(uintptr_t)ev_enter, &ev_enter_original)) {
        log_append("EVENTS: the event ownership state is not where expected; events in free roam online are not "
                   "guarded.\r\n");
        return;
    }
    if (!ev_patch_slot(EV_DECIDE_SLOT, EV_DECIDE_RVA, (void *)(uintptr_t)ev_decide, &ev_decide_original)) {
        log_append("EVENTS: the event ownership state is not where expected (decide); not guarded.\r\n");
        return;
    }
    log_append("EVENTS: entering an event in free roam online goes back to free roam (no races on the relay yet).\r\n");
}
