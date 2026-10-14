/* Configurações > MULTIPLAYER: the FH2Online profile from the game's own UI.

   The pause-options page (A_PauseOptions.xaml) has a "Créditos da equipe" tile,
   PART_Button_TeamCredits, whose click handler FUN_140dcc6c0 only opens the credits scene.
   We take that tile over:
   - its heading (ScreenTitles.IDS_TeamCredits) is rewritten in the loaded string table to
     "MULTIPLAYER" when the page initializes (vtable slot 0x2e15940 -> FUN_140dc8a60);
   - the handler is detoured to online_menu_click.
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

#define MENU_CREDITS_HANDLER_RVA 0xdcc6c0u
#define MENU_INIT_SLOT_RVA 0x2e15940u
#define MENU_INIT_RVA 0xdc8a60u
#define MENU_LOC_ID_RVA 0x123d4b0u
#define MENU_LOC_MAKE_RVA 0x123a570u
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

static const unsigned char menu_credits_bytes[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b,
                                                     0xd9, 0x48, 0x8b, 0x89, 0x10, 0x04, 0x00, 0x00};

typedef void (*MenuPageFn)(void *);
typedef unsigned (*MenuLocIdFn)(const char *);
typedef void **(*MenuLocMakeFn)(void **, const char *, unsigned);
typedef void *(*MenuPopupCreateFn)(void *);
typedef void (*MenuPopupTextFn)(void *, void **);
typedef void (*MenuPopupButtonFn)(void *, int, void **, void *, int, char);
typedef void (*MenuPopupIntFn)(void *, int);
typedef void (*MenuKeyboardFn)(void *, void **, void *, const wchar_t *, const wchar_t *, int, char, int, int);
typedef wchar_t *(*MenuGetTextFn)(void *);
typedef void (*MenuReleaseFn)(void *);
typedef void (*MenuPageArgFn)(void *, void *);

static unsigned char *menu_game;
static MenuPageFn menu_init_original;
static void *menu_page;
static int menu_relabeled;

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
    if (count > 1) {
        ((MenuPopupIntFn)(uintptr_t)(menu_game + MENU_POPUP_CANCELABLE_RVA))(popup, 1);
    }
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
    static const MenuButton restart[1] = {{L"Reiniciar agora", menu_restart}};
    line_start();
    line_add(L"Seu perfil online: ");
    line_add(azimuth_account_gamertag());
    line_add(L"\nChave do site: ");
    line_add(azimuth_account_web_key());
    line_add(L"\n\nUse o nome e a chave para entrar no site do servidor. A chave fica guardada em "
             L"Configurações > Multiplayer.\n\nO jogo vai fechar e abrir de novo no perfil online, que começa do "
             L"zero. Seu progresso offline continua salvo.");
    menu_popup(L"PERFIL CRIADO", menu_line, restart, 1);
}

static void menu_name_typed(const wchar_t *name) {
    static wchar_t error[400];
    if (!name[0]) {
        return; /* keyboard cancelled */
    }
    if (!azimuth_account_register(name, error, ARRAY_COUNT(error))) {
        menu_info(L"MULTIPLAYER", error);
        return;
    }
    menu_show_created();
}

static void menu_ask_name(void) {
    line_start();
    line_add(L"De 1 a 9 letras ou números, começando com uma letra. Ficará como Nome#");
    line_add(azimuth_account_discriminator());
    line_add(L".");
    menu_keyboard(L"Nome do perfil online", menu_line, 9, menu_name_typed);
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
    static const MenuButton buttons[2] = {{L"Sair e reiniciar", menu_leave}, {L"Cancelar", menu_nothing}};
    menu_popup(L"SAIR DO MULTIPLAYER",
               L"O jogo vai reiniciar no seu perfil offline. Seu perfil online continua guardado e você pode "
               L"voltar a ele aqui quando quiser.",
               buttons, 2);
}

static void menu_new_key(void) {
    static wchar_t error[400];
    if (!azimuth_account_new_web_key(error, ARRAY_COUNT(error))) {
        menu_info(L"CHAVE DO SITE", error);
        return;
    }
    line_start();
    line_add(L"Nova chave do site: ");
    line_add(azimuth_account_web_key());
    line_add(L"\n\nA chave anterior deixou de funcionar.");
    menu_info(L"CHAVE DO SITE", menu_line);
}

static void menu_friend_typed(const wchar_t *gamertag) {
    static wchar_t message[400];
    if (!gamertag[0]) {
        return;
    }
    azimuth_account_add_friend(gamertag, message, ARRAY_COUNT(message));
    menu_info(L"ADICIONAR AMIGO", message);
}

static void menu_add_friend(void) {
    menu_keyboard(L"Adicionar amigo",
                  L"Digite o perfil do seu amigo no formato Nome#12345. Quando os dois se adicionarem, vocês viram "
                  L"amigos e rivais nas corridas de fantasma.",
                  15, menu_friend_typed);
}

static void menu_more(void) {
    static const MenuButton buttons[3] = {{L"Nova chave do site", menu_new_key},
                                          {L"Voltar", menu_nothing},
                                          {L"Sair do multiplayer", menu_confirm_leave}};
    menu_popup(L"MULTIPLAYER",
               L"A chave do site serve para entrar no site do servidor com o seu perfil. Gerar uma nova "
               L"invalida a anterior.\n\nSair do multiplayer reinicia o jogo no seu perfil offline.",
               buttons, 3);
}

static void menu_click(void *page) {
    menu_page = page;
    switch (azimuth_account_state()) {
    case 0: {
        static const MenuButton buttons[2] = {{L"Criar perfil", menu_ask_name}, {L"Cancelar", menu_nothing}};
        line_start();
        line_add(L"ForzaInfinity: corridas de fantasma entre amigos.\n\nServidor: ");
        line_add(azimuth_account_server());
        line_add(L"\nSeu perfil terá o formato Nome#");
        line_add(azimuth_account_discriminator());
        line_add(L".\n\nAo criar o perfil, o jogo reinicia em um perfil zerado. Seu progresso offline continua "
                 L"salvo.");
        menu_popup(L"MULTIPLAYER", menu_line, buttons, 2);
        break;
    }
    case 1: {
        static const MenuButton buttons[2] = {{L"Entrar e reiniciar", menu_join}, {L"Cancelar", menu_nothing}};
        line_start();
        line_add(L"Você tem o perfil online ");
        line_add(azimuth_account_gamertag());
        line_add(L" neste servidor.\n\nEntrar nele reinicia o jogo no perfil online. Seu progresso offline "
                 L"continua salvo.");
        menu_popup(L"MULTIPLAYER", menu_line, buttons, 2);
        break;
    }
    case 2: {
        static const MenuButton buttons[3] = {{L"Adicionar amigo", menu_add_friend},
                                              {L"Voltar", menu_nothing},
                                              {L"Mais opções", menu_more}};
        line_start();
        line_add(L"Perfil online: ");
        line_add(azimuth_account_gamertag());
        line_add(L"\nServidor: ");
        line_add(azimuth_account_server());
        line_add(L"\nChave do site: ");
        line_add(azimuth_account_web_key()[0] ? azimuth_account_web_key() : L"(gere uma nova)");
        menu_popup(L"MULTIPLAYER", menu_line, buttons, 3);
        break;
    }
    default:
        menu_info(L"MULTIPLAYER",
                  L"O multiplayer precisa de [Network] Mode=Proxy e de um servidor em FH2_ExtraOptions.ini.");
    }
}

/* ------------------------------------------------------------- install */

static void menu_relabel(void) {
    if (menu_relabeled) {
        return;
    }
    unsigned id = ((MenuLocIdFn)(uintptr_t)(menu_game + MENU_LOC_ID_RVA))("IDS_TeamCredits");
    void *loc = 0;
    ((MenuLocMakeFn)(uintptr_t)(menu_game + MENU_LOC_MAKE_RVA))(&loc, "ScreenTitles", id);
    if (!loc) {
        return;
    }
    const void *const *vtable = *(const void *const **)loc;
    wchar_t *text = ((MenuGetTextFn)(uintptr_t)vtable[7])(loc);
    static const wchar_t label[] = L"MULTIPLAYER";
    size_t n = 0;
    while (text && text[n]) {
        ++n;
    }
    if (text && n + 1 >= ARRAY_COUNT(label) && memory_is_writable(text, sizeof(label))) {
        memory_copy(text, label, sizeof(label));
        menu_relabeled = 1;
        log_append("MENU: Configurações tile relabeled MULTIPLAYER.\r\n");
    }
    ((MenuReleaseFn)(uintptr_t)vtable[3])(loc);
}

static void menu_init(void *page) {
    menu_relabel();
    menu_init_original(page);
}

void azimuth_apply_online_menu(unsigned char *game_image) {
    unsigned char current[16];
    menu_game = game_image;
    if (azimuth_account_state() < 0) {
        return;
    }
    unsigned char *handler = game_image + MENU_CREDITS_HANDLER_RVA;
    void **slot = (void **)(game_image + MENU_INIT_SLOT_RVA);
    void *slot_value = 0;
    if (!memory_read(handler, current, sizeof(current)) || !memory_equal(current, menu_credits_bytes, sizeof(current)) ||
        !memory_read(slot, &slot_value, sizeof(slot_value)) || slot_value != game_image + MENU_INIT_RVA) {
        log_append("MENU: pause-options code not as expected; no MULTIPLAYER tile.\r\n");
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
        log_append("MENU: pause-options vtable not as expected; no MULTIPLAYER tile.\r\n");
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

    /* click handler: jmp [rip+0]; dq menu_click */
    unsigned char jump[14] = {0xff, 0x25, 0, 0, 0, 0};
    uintptr_t target = (uintptr_t)menu_click;
    memory_copy(jump + 6, &target, sizeof(target));
    if (!VirtualProtect(handler, sizeof(jump), PAGE_EXECUTE_READWRITE, &old_protection)) {
        return;
    }
    memory_copy(handler, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), handler, sizeof(jump));
    VirtualProtect(handler, sizeof(jump), old_protection, &ignored);
    log_append("MENU: MULTIPLAYER tile installed in Configurações.\r\n");
}
