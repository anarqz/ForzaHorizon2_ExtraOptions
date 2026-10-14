/* Settings > INFINITY: the FH2Online profile from the game's own UI.

   azimuth_ui.c adds the INFINITY tile (PART_Button_Infinity) below CREDITS on the
   pause-options page. When the page initializes (vtable slot 0x2e15940 -> FUN_140dc8a60, which
   binds each PART_Button_* to its handler with FUN_140d9fc10(page, name, &handler)), the
   INFINITY button is bound the same way to menu_click.
   Everything shown uses the game's own popups and virtual keyboard:
     popup = FUN_140c963a0(page); title/message FUN_140e1bd90/FUN_140e1b650; buttons
     FUN_140db3d00(popup, index, &label, &std::function<void()>, result, 1);
     keyboard FUN_140de0400(page, &default, descriptor, title, description, scope, filter,
     max, min): when it closes the game calls the function at descriptor+0x30 as
     fn(descriptor, page, text) (after page->vtable[0x5b8], guarded below),
     text being a string object with its wchar_t* at +0x28 (NULL when empty).
   Texts are our own ITextSource objects (the CLocString interface: AddRef +0x10,
   Release +0x18, GetText +0x38). Server calls are synchronous (short timeouts). */
#include "azimuth_host.h"
#include "logging.h"
#include "support.h"

#define MENU_INIT_SLOT_RVA 0x2e15940u
#define MENU_INIT_RVA 0xdc8a60u
#define MENU_BIND_BUTTON_RVA 0xd9fc10u
#define MENU_POPUP_CREATE_RVA 0xc963a0u
#define MENU_POPUP_TITLE_RVA 0xe1bd90u
#define MENU_POPUP_MESSAGE_RVA 0xe1b650u
#define MENU_POPUP_BUTTON_RVA 0xdb3d00u
#define MENU_POPUP_CANCELABLE_RVA 0xd0d5a0u
#define MENU_KEYBOARD_RVA 0xde0400u
#define MENU_TEXT_SLOT5_RVA 0xc47af0u
#define MENU_TEXT_NOP_RVA 0x1db440u
#define MENU_TEXT_COPYN_RVA 0x7434a0u
#define MENU_TEXT_COPY_RVA 0x743470u
/* PauseOptionsMenu vtable slot +0x5b8: the keyboard's completion calls it with the text length
   in rdx; the club pages ignore it, this page writes through it. */
#define MENU_PAGE_KEYBOARD_SLOT_RVA (0x2e154b8u + 0x5b8u)
#define MENU_PAGE_KEYBOARD_RVA 0x155c3f0u

typedef void (*MenuPageFn)(void *);
typedef void (*MenuBindButtonFn)(void *, const char *, void *const *);
typedef void *(*MenuPopupCreateFn)(void *);
typedef void (*MenuPopupTextFn)(void *, void **);
typedef void (*MenuPopupButtonFn)(void *, int, void **, void *, int, char);
typedef void (*MenuPopupIntFn)(void *, int);
typedef void (*MenuKeyboardFn)(void *, void **, void *, const wchar_t *, const wchar_t *, int, char, int, int);
typedef void (*MenuPageArgFn)(void *, void *);

static unsigned char *menu_game;
static MenuPageFn menu_init_original;
static void *menu_page;

/* ------------------------------------------------------- text objects */

typedef struct {
    const void *const *vtable;
    wchar_t *text;
    volatile LONG references; /* +0x10, as CLocString */
    LONG pad;
} MenuText;

static const void *menu_text_vtable[11];

static void *menu_alloc(size_t size) {
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void menu_free(void *p) {
    if (p) {
        HeapFree(GetProcessHeap(), 0, p);
    }
}

static MenuText *menu_text_destroy(MenuText *self, unsigned flags) {
    if (flags & 1) {
        menu_free(self);
    }
    return self;
}
static void **menu_text_none(MenuText *self, void **out) {
    (void)self;
    *out = 0;
    return out;
}
static LONG menu_text_add_ref(MenuText *self) {
    return InterlockedIncrement(&self->references);
}
static LONG menu_text_release(MenuText *self) {
    LONG left = InterlockedDecrement(&self->references);
    if (!left) {
        menu_text_destroy(self, 1);
    }
    return left;
}
static LONG menu_text_count(MenuText *self) {
    return self->references;
}
static wchar_t *menu_text_get(MenuText *self) {
    return self->text;
}
static void **menu_text_clone(MenuText *self, void **out) {
    menu_text_add_ref(self);
    *out = self;
    return out;
}

/* A text object holding a copy of `text` (one reference, ours). */
static MenuText *menu_text(const wchar_t *text) {
    size_t n = 0;
    while (text[n]) {
        ++n;
    }
    MenuText *t = menu_alloc(sizeof(MenuText) + (n + 1) * sizeof(wchar_t));
    if (!t) {
        return 0;
    }
    t->vtable = menu_text_vtable;
    t->text = (wchar_t *)(t + 1);
    memory_copy(t->text, text, (n + 1) * sizeof(wchar_t));
    t->references = 1;
    return t;
}

/* -------------------------------------------- std::function<void()> objects */

typedef void (*MenuAction)(void);

typedef struct {
    const void *const *vtable;
    MenuAction action;
    void *unused;
} MenuFn;

/* MSVC std::function: 3 words of small storage, then the callable's pointer. */
typedef struct {
    void *storage[3];
    MenuFn *impl;
} MenuStdFunction;

static const void *menu_fn_vtable[6];

static MenuFn *menu_fn_copy(MenuFn *self, MenuFn *where) {
    if (!where) {
        where = menu_alloc(sizeof(MenuFn));
        if (!where) {
            return 0;
        }
    }
    where->vtable = menu_fn_vtable;
    where->action = self->action;
    where->unused = 0;
    return where;
}
static void menu_fn_call(MenuFn *self) {
    if (self->action) {
        self->action();
    }
}
static void *menu_fn_type(MenuFn *self) {
    (void)self;
    return 0;
}
static void menu_fn_delete(MenuFn *self, char deallocate) {
    if (deallocate) {
        menu_free(self);
    }
}
static MenuFn *menu_fn_destroy(MenuFn *self, unsigned flags) {
    (void)flags;
    return self;
}

static void menu_function(MenuStdFunction *f, MenuAction action) {
    MenuFn *impl = (MenuFn *)f->storage;
    impl->vtable = menu_fn_vtable;
    impl->action = action;
    impl->unused = 0;
    f->impl = impl;
}

/* ------------------------------------------------------------- popups */

typedef struct {
    const wchar_t *label;
    MenuAction action;
} MenuButton;

static void menu_popup(const wchar_t *title, const wchar_t *message, const MenuButton *buttons, int count) {
    if (!menu_page) {
        return;
    }
    void *popup = ((MenuPopupCreateFn)(uintptr_t)(menu_game + MENU_POPUP_CREATE_RVA))(menu_page);
    if (!popup) {
        return;
    }
    MenuText *t = menu_text(title), *m = menu_text(message);
    if (t) {
        ((MenuPopupTextFn)(uintptr_t)(menu_game + MENU_POPUP_TITLE_RVA))(popup, (void **)&t);
        menu_text_release(t);
    }
    if (m) {
        ((MenuPopupTextFn)(uintptr_t)(menu_game + MENU_POPUP_MESSAGE_RVA))(popup, (void **)&m);
        menu_text_release(m);
    }
    /* the type FUN_140dd8820 would store: 0 one button, 1 two, 7 three */
    *(int *)((unsigned char *)popup + 0x418) = count == 1 ? 0 : count == 2 ? 1 : 7;
    for (int i = 0; i < count; ++i) {
        MenuText *label = menu_text(buttons[i].label);
        MenuStdFunction fn = {{0}, 0};
        menu_function(&fn, buttons[i].action);
        if (label) {
            ((MenuPopupButtonFn)(uintptr_t)(menu_game + MENU_POPUP_BUTTON_RVA))(popup, i, (void **)&label, &fn, i, 1);
            menu_text_release(label);
        }
    }
    /* B always backs out of an Infinity popup */
    ((MenuPopupIntFn)(uintptr_t)(menu_game + MENU_POPUP_CANCELABLE_RVA))(popup, 1);
}

static void menu_nothing(void) {
}

static void menu_info(const wchar_t *title, const wchar_t *message) {
    static const MenuButton ok[1] = {{L"OK", menu_nothing}};
    menu_popup(title, message, ok, 1);
}

/* ------------------------------------------------------------ keyboard */

typedef void (*MenuTextDone)(const wchar_t *);

/* As the game's keyboard descriptors (FUN_140debdd0): the completion calls the function
   pointer at +0x30 as fn(descriptor, page, text). */
typedef struct {
    void *fields[6];
    void (*on_text)(void *, void *, void *);
    void *more[8];
} MenuDescriptor;

static MenuDescriptor menu_descriptor;
static MenuTextDone menu_keyboard_done;
static MenuPageArgFn menu_page_keyboard_original;

/* Our keyboard's completion passes a small integer (the length), never a pointer. */
static void menu_page_keyboard(void *self, void *argument) {
    if (menu_keyboard_done && (uintptr_t)argument < 0x10000u) {
        return;
    }
    menu_page_keyboard_original(self, argument);
}
static wchar_t menu_typed[64];

static void menu_descriptor_fire(void *self, void *page, void *text) {
    (void)self;
    menu_page = page ? page : menu_page;
    menu_typed[0] = 0;
    if (text) {
        const wchar_t *typed = *(const wchar_t **)((unsigned char *)text + 0x28);
        size_t n = 0;
        while (typed && typed[n] && n + 1 < ARRAY_COUNT(menu_typed)) {
            menu_typed[n] = typed[n];
            ++n;
        }
        menu_typed[n] = 0;
    }
    MenuTextDone done = menu_keyboard_done;
    menu_keyboard_done = 0;
    if (done) {
        done(menu_typed);
    }
}

static void menu_keyboard(const wchar_t *title, const wchar_t *description, int max_length, MenuTextDone done) {
    void *no_default = 0;
    menu_keyboard_done = done;
    ((MenuKeyboardFn)(uintptr_t)(menu_game + MENU_KEYBOARD_RVA))(menu_page, &no_default, &menu_descriptor, title,
                                                                 description, 0, 0, max_length, 1);
}

/* --------------------------------------------------------------- text */

static wchar_t menu_line[1024];

static void line_start(void) {
    menu_line[0] = 0;
}

static void line_add(const wchar_t *text) {
    size_t n = 0;
    while (menu_line[n]) {
        ++n;
    }
    while (text && *text && n + 1 < ARRAY_COUNT(menu_line)) {
        menu_line[n++] = *text++;
    }
    menu_line[n] = 0;
}

/* --------------------------------------------------------------- flows */

static void menu_restart(void) {
    azimuth_account_restart();
}

static void menu_show_created(void) {
    static const MenuButton restart[1] = {{L"Restart now", menu_restart}};
    line_start();
    line_add(L"Your online profile: ");
    line_add(azimuth_account_gamertag());
    line_add(L"\nSite key: ");
    line_add(azimuth_account_web_key());
    line_add(L"\n\nUse your profile and this key to sign in to the server's site. The key is kept in "
             L"Settings > Infinity.\n\nThe game will close and open again on the online profile, straight in "
             L"the open world with your car: the intro and first championship are done, and online, clubs and "
             L"rivals are open. Your offline progress stays saved.");
    menu_popup(L"PROFILE CREATED", menu_line, restart, 1);
}

/* Create profile: the first car (one of the game's own three), then the name. */
static int menu_starter_car;

static void menu_name_typed(const wchar_t *name) {
    static wchar_t error[400];
    if (!name[0]) {
        return; /* keyboard cancelled */
    }
    if (!azimuth_account_register(name, menu_starter_car, error, ARRAY_COUNT(error))) {
        menu_info(L"INFINITY", error);
        return;
    }
    menu_show_created();
}

static void menu_ask_name(void) {
    line_start();
    line_add(L"1 to 9 letters or digits, starting with a letter. It will read Name#");
    line_add(azimuth_account_discriminator());
    line_add(L".");
    menu_keyboard(L"Online profile name", menu_line, 9, menu_name_typed);
}

static void menu_car_camaro(void) {
    menu_starter_car = 289;
    menu_ask_name();
}
static void menu_car_supra(void) {
    menu_starter_car = 461;
    menu_ask_name();
}
static void menu_car_z4(void) {
    menu_starter_car = 1218;
    menu_ask_name();
}

static void menu_pick_car(void) {
    static const MenuButton cars[3] = {{L"1969 Chevrolet Camaro SS", menu_car_camaro},
                                       {L"1998 Toyota Supra RZ", menu_car_supra},
                                       {L"2011 BMW Z4 sDrive35is", menu_car_z4}};
    menu_popup(L"CHOOSE YOUR CAR",
               L"Your new profile starts in the open world, past the intro and the first championship, with "
               L"online, clubs and rivals open.\n\nPick the car you start with:",
               cars, 3);
}

/* Entrar com perfil: Name#12345, then the site key (a popup in between: a keyboard is not
   opened from another keyboard's completion). */
static wchar_t menu_login_tag[20];

static void menu_show_logged_in(void) {
    static const MenuButton restart[1] = {{L"Restart now", menu_restart}};
    line_start();
    line_add(L"Signed in as ");
    line_add(azimuth_account_gamertag());
    line_add(L".\nNew site key: ");
    line_add(azimuth_account_web_key());
    line_add(L"\n\nThe game will restart and download the profile's save from the server. The PC that was "
             L"signed in to this profile has been signed out, and the previous key no longer works.");
    menu_popup(L"PROFILE SIGNED IN", menu_line, restart, 1);
}

static void menu_key_typed(const wchar_t *key) {
    static wchar_t error[400];
    if (!key[0]) {
        return;
    }
    if (!azimuth_account_log_in(menu_login_tag, key, error, ARRAY_COUNT(error))) {
        menu_info(L"SIGN IN", error);
        return;
    }
    menu_show_logged_in();
}

static void menu_ask_key(void) {
    menu_keyboard(L"Site key",
                  L"The key is shown in Settings > Infinity on the PC that has the profile, like "
                  L"K7QM-4XPA-9RT2.",
                  14, menu_key_typed);
}

static void menu_login_tag_typed(const wchar_t *tag) {
    static const MenuButton next[1] = {{L"Enter key", menu_ask_key}};
    size_t n = 0;
    if (!tag[0]) {
        return;
    }
    while (tag[n] && n + 1 < ARRAY_COUNT(menu_login_tag)) {
        menu_login_tag[n] = tag[n];
        ++n;
    }
    menu_login_tag[n] = 0;
    line_start();
    line_add(L"Profile: ");
    line_add(menu_login_tag);
    line_add(L"\n\nNow enter that profile's site key.");
    menu_popup(L"SIGN IN", menu_line, next, 1);
}

static void menu_log_in(void) {
    menu_keyboard(L"Sign in",
                  L"Enter the profile as Name#12345. The PC it is signed in on will be signed out.", 15,
                  menu_login_tag_typed);
}

static void menu_join(void) {
    azimuth_account_set_online(1);
    azimuth_account_restart();
}

static void menu_leave(void) {
    azimuth_account_set_online(0);
    azimuth_account_restart();
}

static void menu_confirm_leave(void) {
    static const MenuButton buttons[2] = {{L"Leave and restart", menu_leave}, {L"Back", menu_nothing}};
    menu_popup(L"LEAVE INFINITY",
               L"The game will restart on your offline profile. Your online profile stays saved and you can "
               L"come back to it here anytime.",
               buttons, 2);
}

static void menu_new_key(void) {
    static wchar_t error[400];
    if (!azimuth_account_new_web_key(error, ARRAY_COUNT(error))) {
        menu_info(L"SITE KEY", error);
        return;
    }
    line_start();
    line_add(L"New site key: ");
    line_add(azimuth_account_web_key());
    line_add(L"\n\nThe previous key no longer works.");
    menu_info(L"SITE KEY", menu_line);
}

/* The current key, and a new one only on request. */
static void menu_show_key(void) {
    static const MenuButton buttons[2] = {{L"New key", menu_new_key}, {L"Back", menu_nothing}};
    line_start();
    if (azimuth_account_web_key()[0]) {
        line_add(L"Your site key: ");
        line_add(azimuth_account_web_key());
    } else {
        line_add(L"This PC has no site key saved.");
    }
    line_add(L"\n\nUse your profile (");
    line_add(azimuth_account_gamertag());
    line_add(L") and this key to sign in to the server's site or to continue this profile on another PC.\n\n"
             L"A new key makes the current one stop working.");
    menu_popup(L"SITE KEY", menu_line, buttons, 2);
}

static void menu_friend_typed(const wchar_t *gamertag) {
    static wchar_t message[400];
    if (!gamertag[0]) {
        return;
    }
    azimuth_account_add_friend(gamertag, message, ARRAY_COUNT(message));
    menu_info(L"ADD FRIEND", message);
}

static void menu_add_friend(void) {
    menu_keyboard(L"Add friend",
                  L"Enter your friend's profile as Name#12345. Once you both add each other, you become friends "
                  L"and rivals in ghost races.",
                  15, menu_friend_typed);
}


static void menu_test_connection(void) {
    static wchar_t report[600];
    azimuth_account_test(report, ARRAY_COUNT(report));
    menu_info(L"TEST CONNECTION", report);
}

static void menu_more(void) {
    static const MenuButton buttons[3] = {{L"New site key", menu_new_key},
                                          {L"Sign in to another profile", menu_log_in},
                                          {L"Leave Infinity", menu_confirm_leave}};
    menu_popup(L"INFINITY",
               L"The site key signs you in to the server's site and lets you use this profile on another PC. "
               L"A new one makes the previous one stop working.\n\nSigning in to another profile switches this "
               L"PC's profile (the current one stays on the server). Leaving Infinity restarts the game on your "
               L"offline profile.",
               buttons, 3);
}

static void menu_profile(void) {
    line_start();
    if (azimuth_account_state() == 2) {
        line_add(L"Online profile: ");
        line_add(azimuth_account_gamertag());
        line_add(L"\nServer: ");
        line_add(azimuth_account_server());
        line_add(L"\nSite key: ");
        line_add(azimuth_account_web_key()[0] ? azimuth_account_web_key() : L"(make a new one in Site key)");
        line_add(L"\n\nYour progress is saved on the server: sign in with your profile and site key on another "
                 L"PC to pick up where you left off.");
    } else if (azimuth_account_state() == 1) {
        line_add(L"You have the online profile ");
        line_add(azimuth_account_gamertag());
        line_add(L" on this server, but you are playing the offline profile.\n\nUse Join and restart to go back to it.");
    } else {
        if (azimuth_account_notice()[0]) {
            line_add(azimuth_account_notice());
            line_add(L"\n\n");
        }
        line_add(L"No online profile on this PC.\n\nA new profile will read Name#");
        line_add(azimuth_account_discriminator());
        line_add(L". Already have one? Use Sign in, with your profile and site key.");
    }
    menu_info(L"PROFILE", menu_line);
}

static void menu_about(void) {
    line_start();
    line_add(L"ForzaInfinity: ghost races between friends.\n\nAdd a friend by their profile (Name#12345). "
             L"Once you both add each other you become rivals: your best times and ghosts show up in each "
             L"other's races.\n\nServer: ");
    line_add(azimuth_account_server());
    menu_info(L"ABOUT INFINITY", menu_line);
}

/* ------------------------------------------------------- INFINITY screen */

static InfinityTile menu_tiles[7];
static MenuAction menu_tile_actions[7];

static void menu_set_tile(int i, const char *heading, const char *icon, int photo, MenuAction action) {
    menu_tiles[i].heading = heading;
    menu_tiles[i].icon = icon;
    menu_tiles[i].photo = photo;
    menu_tile_actions[i] = action;
}

/* The tiles for the profile's state (slots: 0 tall photo, 1-2 wide, 3-6 small icons). The
   actions are kept with them, so a click does what its tile said. */
const InfinityTile *azimuth_infinity_tiles(void) {
    int state = azimuth_account_state();
    if (state < 0) {
        return 0;
    }
    for (int i = 0; i < 7; ++i) {
        menu_set_tile(i, 0, 0, 0, 0);
    }
    if (state == 2) {
        menu_set_tile(0, "PROFILE", "PauseMenuTileBackgrounds/StartingGrid.png", 1, menu_profile);
        menu_set_tile(1, "ADD FRIEND", "PauseMenuTileBackgrounds/MyRivals.png", 1, menu_add_friend);
        menu_set_tile(2, "TEST CONNECTION", "PauseMenuTileBackgrounds/OnlineFreeroam.png", 1, menu_test_connection);
        menu_set_tile(3, "SITE KEY", "Icons/TiledMenus/Options.png", 0, menu_show_key);
        menu_set_tile(4, "SWITCH PROFILE", "Icons/TiledMenus/Garage.png", 0, menu_log_in);
        menu_set_tile(5, "ABOUT", "Icons/TiledMenus/TeamCredits.png", 0, menu_about);
        menu_set_tile(6, "LEAVE INFINITY", "Icons/TiledMenus/Difficulty.png", 0, menu_confirm_leave);
    } else if (state == 1) {
        menu_set_tile(0, "PROFILE", "PauseMenuTileBackgrounds/StartingGrid.png", 1, menu_profile);
        menu_set_tile(1, "JOIN AND RESTART", "PauseMenuTileBackgrounds/OnlineRoadtrip.png", 1, menu_join);
        menu_set_tile(2, "TEST CONNECTION", "PauseMenuTileBackgrounds/OnlineFreeroam.png", 1, menu_test_connection);
        menu_set_tile(3, "SWITCH PROFILE", "Icons/TiledMenus/Garage.png", 0, menu_log_in);
        menu_set_tile(4, "ABOUT", "Icons/TiledMenus/Options.png", 0, menu_about);
    } else {
        menu_set_tile(0, "PROFILE", "PauseMenuTileBackgrounds/StartingGrid.png", 1, menu_profile);
        menu_set_tile(1, "CREATE PROFILE", "PauseMenuTileBackgrounds/MyRivals.png", 1, menu_pick_car);
        menu_set_tile(2, "SIGN IN", "PauseMenuTileBackgrounds/OnlineRoadtrip.png", 1, menu_log_in);
        menu_set_tile(3, "TEST CONNECTION", "Icons/TiledMenus/Options.png", 0, menu_test_connection);
        menu_set_tile(4, "ABOUT", "Icons/TiledMenus/TeamCredits.png", 0, menu_about);
    }
    return menu_tiles;
}

static void menu_tile(int i, void *page) {
    menu_page = page;
    if (menu_tile_actions[i]) {
        menu_tile_actions[i]();
    }
}
static void menu_tile_0(void *page) { menu_tile(0, page); }
static void menu_tile_1(void *page) { menu_tile(1, page); }
static void menu_tile_2(void *page) { menu_tile(2, page); }
static void menu_tile_3(void *page) { menu_tile(3, page); }
static void menu_tile_4(void *page) { menu_tile(4, page); }
static void menu_tile_5(void *page) { menu_tile(5, page); }
static void menu_tile_6(void *page) { menu_tile(6, page); }
static MenuPageFn const menu_tile_handlers[7] = {menu_tile_0, menu_tile_1, menu_tile_2, menu_tile_3,
                                                  menu_tile_4, menu_tile_5, menu_tile_6};

/* The popup version of the menu: used when the INFINITY screen cannot be opened. */
static void menu_click(void *page) {
    menu_page = page;
    switch (azimuth_account_state()) {
    case 0: {
        static const MenuButton buttons[3] = {{L"Create profile", menu_pick_car},
                                              {L"Sign in", menu_log_in},
                                              {L"Test connection", menu_test_connection}};
        line_start();
        if (azimuth_account_notice()[0]) {
            line_add(azimuth_account_notice());
            line_add(L"\n\n");
        }
        line_add(L"ForzaInfinity: ghost races between friends.\n\nServer: ");
        line_add(azimuth_account_server());
        line_add(L"\nA new profile will read Name#");
        line_add(azimuth_account_discriminator());
        line_add(L". Already have a profile? Sign in with your profile and site key and continue from the save "
                 L"kept on the server.\n\nYour offline progress stays saved.");
        menu_popup(L"INFINITY", menu_line, buttons, 3);
        break;
    }
    case 1: {
        static const MenuButton buttons[2] = {{L"Join and restart", menu_join},
                                              {L"Test connection", menu_test_connection}};
        line_start();
        line_add(L"You have the online profile ");
        line_add(azimuth_account_gamertag());
        line_add(L" on this server.\n\nJoining restarts the game on the online profile. Your offline progress "
                 L"stays saved.");
        menu_popup(L"INFINITY", menu_line, buttons, 2);
        break;
    }
    case 2: {
        static const MenuButton buttons[3] = {{L"Add friend", menu_add_friend},
                                              {L"Test connection", menu_test_connection},
                                              {L"More options", menu_more}};
        line_start();
        line_add(L"Online profile: ");
        line_add(azimuth_account_gamertag());
        line_add(L"\nServer: ");
        line_add(azimuth_account_server());
        line_add(L"\nSite key: ");
        line_add(azimuth_account_web_key()[0] ? azimuth_account_web_key() : L"(make a new one)");
        menu_popup(L"INFINITY", menu_line, buttons, 3);
        break;
    }
    default:
        menu_info(L"INFINITY",
                  L"Infinity needs [Network] Mode=Proxy and a server in FH2_ExtraOptions.ini.");
    }
}

/* ------------------------------------------------------------- install */

/* A named element of a page, as FUN_140dc6980 looks one up: a name object built by
   FUN_141663220 (reference count at +0x10), FUN_140cbdc40(&name, page), FUN_1416650a0. */
#define MENU_NAME_MAKE_RVA 0x1663220u
#define MENU_NAME_FREE_RVA 0x16650a0u
#define MENU_FIND_ELEMENT_RVA 0xcbdc40u
#define MENU_FOCUS_SLOT_RVA (0x2e154b8u + 0x498u)
#define MENU_FOCUS_RVA 0xdc6980u

typedef void (*MenuNameMakeFn)(void *, const char *, int);
typedef void (*MenuNameFreeFn)(void *);
typedef void *(*MenuFindElementFn)(void **, void *);
typedef void (*MenuFocusFn)(void *);

static void *menu_find_element(void *page, const char *name) {
    unsigned char key[96] __attribute__((aligned(16))) = {0}; /* the game keeps it in 0x58 bytes */
    void *reference = key;
    ((MenuNameMakeFn)(uintptr_t)(menu_game + MENU_NAME_MAKE_RVA))(key, name, -1);
    InterlockedIncrement((volatile LONG *)(key + 0x10));
    void *element = ((MenuFindElementFn)(uintptr_t)(menu_game + MENU_FIND_ELEMENT_RVA))(&reference, page);
    ((MenuNameFreeFn)(uintptr_t)(menu_game + MENU_NAME_FREE_RVA))(key);
    return element;
}

/* The page's first focus (vtable +0x498, FUN_140dc6980) goes to PART_Button_Assists without
   checking it exists: on the INFINITY screen (whose Assists is a collapsed stand-in) it is
   PART_Button_I0. */
static void menu_default_focus(void *page) {
    unsigned char *element = menu_find_element(page, "PART_Button_I0");
    if (!element) {
        element = menu_find_element(page, "PART_Button_Assists");
    }
    if (element) {
        void *focusable = element + 0xb8;
        ((MenuFocusFn)(*(void ***)focusable)[3])(focusable);
    }
}

/* Settings' INFINITY tile opens the INFINITY screen (or the popup menu without it). */
static int menu_infinity_screen_off;

static void menu_infinity_click(void *page) {
    menu_page = page;
    if (menu_infinity_screen_off || azimuth_account_state() < 0 || !azimuth_ui_open_infinity(page)) {
        menu_click(page);
    }
}

/* After the game binds its own tiles, ours: PART_Button_Infinity on Settings and
   PART_Button_I0..I6 on the INFINITY screen (the same page class; a page has only its own
   buttons, binding a missing name does nothing). A handler is a member-function pointer pair
   {function, this adjustment}. */
static void menu_init(void *page) {
    static void *handlers[8][2];
    static const char *const names[8] = {"PART_Button_Infinity", "PART_Button_I0", "PART_Button_I1",
                                         "PART_Button_I2",       "PART_Button_I3", "PART_Button_I4",
                                         "PART_Button_I5",       "PART_Button_I6"};
    MenuBindButtonFn bind = (MenuBindButtonFn)(uintptr_t)(menu_game + MENU_BIND_BUTTON_RVA);
    menu_init_original(page);
    for (int i = 0; i < 8; ++i) {
        handlers[i][0] = (void *)(uintptr_t)(i ? menu_tile_handlers[i - 1] : menu_infinity_click);
        bind(page, names[i], (void *const *)handlers[i]);
    }
}

void azimuth_apply_online_menu(unsigned char *game_image) {
    menu_game = game_image;
    if (azimuth_account_state() < 0) {
        return;
    }
    void **slot = (void **)(game_image + MENU_INIT_SLOT_RVA);
    void *slot_value = 0;
    if (!memory_read(slot, &slot_value, sizeof(slot_value)) || slot_value != game_image + MENU_INIT_RVA) {
        log_append("MENU: pause-options code not as expected; no INFINITY menu.\r\n");
        return;
    }
    menu_text_vtable[0] = (const void *)(uintptr_t)menu_text_destroy;
    menu_text_vtable[1] = (const void *)(uintptr_t)menu_text_none;
    menu_text_vtable[2] = (const void *)(uintptr_t)menu_text_add_ref;
    menu_text_vtable[3] = (const void *)(uintptr_t)menu_text_release;
    menu_text_vtable[4] = (const void *)(uintptr_t)menu_text_count;
    menu_text_vtable[5] = game_image + MENU_TEXT_SLOT5_RVA;
    menu_text_vtable[6] = game_image + MENU_TEXT_NOP_RVA;
    menu_text_vtable[7] = (const void *)(uintptr_t)menu_text_get;
    menu_text_vtable[8] = (const void *)(uintptr_t)menu_text_clone;
    menu_text_vtable[9] = game_image + MENU_TEXT_COPYN_RVA;
    menu_text_vtable[10] = game_image + MENU_TEXT_COPY_RVA;
    menu_fn_vtable[0] = (const void *)(uintptr_t)menu_fn_copy;
    menu_fn_vtable[1] = (const void *)(uintptr_t)menu_fn_copy;
    menu_fn_vtable[2] = (const void *)(uintptr_t)menu_fn_call;
    menu_fn_vtable[3] = (const void *)(uintptr_t)menu_fn_type;
    menu_fn_vtable[4] = (const void *)(uintptr_t)menu_fn_delete;
    menu_fn_vtable[5] = (const void *)(uintptr_t)menu_fn_destroy;
    menu_descriptor.on_text = menu_descriptor_fire;

    void **keyboard_slot = (void **)(game_image + MENU_PAGE_KEYBOARD_SLOT_RVA);
    void *keyboard_value = 0;
    if (!memory_read(keyboard_slot, &keyboard_value, sizeof(keyboard_value)) ||
        keyboard_value != game_image + MENU_PAGE_KEYBOARD_RVA) {
        log_append("MENU: pause-options vtable not as expected; no INFINITY menu.\r\n");
        return;
    }

    /* page init: vtable slot */
    DWORD old_protection, ignored;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protection)) {
        return;
    }
    menu_init_original = (MenuPageFn)(uintptr_t)slot_value;
    *slot = (void *)(uintptr_t)menu_init;
    VirtualProtect(slot, sizeof(*slot), old_protection, &ignored);
    if (!VirtualProtect(keyboard_slot, sizeof(*keyboard_slot), PAGE_READWRITE, &old_protection)) {
        return;
    }
    menu_page_keyboard_original = (MenuPageArgFn)(uintptr_t)keyboard_value;
    *keyboard_slot = (void *)(uintptr_t)menu_page_keyboard;
    VirtualProtect(keyboard_slot, sizeof(*keyboard_slot), old_protection, &ignored);
    /* first focus: also on the INFINITY screen */
    void **focus_slot = (void **)(game_image + MENU_FOCUS_SLOT_RVA);
    void *focus_value = 0;
    if (memory_read(focus_slot, &focus_value, sizeof(focus_value)) && focus_value == game_image + MENU_FOCUS_RVA &&
        VirtualProtect(focus_slot, sizeof(*focus_slot), PAGE_READWRITE, &old_protection)) {
        *focus_slot = (void *)(uintptr_t)menu_default_focus;
        VirtualProtect(focus_slot, sizeof(*focus_slot), old_protection, &ignored);
    } else {
        log_append("MENU: page focus slot not as expected; the INFINITY screen stays off.\r\n");
        menu_infinity_screen_off = 1;
    }
    log_append("MENU: INFINITY menu bound in Settings.\r\n");
}