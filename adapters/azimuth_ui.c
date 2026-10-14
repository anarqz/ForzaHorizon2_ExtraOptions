/* Game pages edited in memory as the game reads them from media/UI.zip:
   - Configurações (Scenes/Anthem/A_PauseOptions.xaml): its right column becomes two tiles,
     CRÉDITOS (the game's own, now 1x1) on top and INFINITY (PART_Button_Infinity, bound to
     the FH2Online menu by azimuth_online_menu.c) below;
   - the pause menu (Scenes/Anthem/A_PauseMenu.xaml): a line centered at the top says whether
     the Infinity server is connected (rebuilt every time the page is read).

   Every zip entry the game reads is opened by FUN_1411ecbd0(archive, &stream, entry, mode),
   which returns an IOSys stream (vtable: +0x10 AddRef, +0x18 Release, +0x38 Seek(offset,
   origin), +0x50 bytes remaining, +0x90 Read(buffer, size)). That function is detoured: when
   a stream is exactly one of these pages (size and x:Class), it is read, edited, and replaced
   by the game's own CMemoryStream (FUN_1411e0640) over the edited text. Nothing on disk
   changes and no copy of a game file ships in the mod; other content is left untouched. */
#include "azimuth_host.h"
#include "logging.h"
#include "support.h"

#define UI_ZIP_OPEN_RVA 0x11ecbd0u
#define UI_MEMORY_STREAM_RVA 0x11e0640u
#define UI_GAME_ALLOC_RVA 0x3229c0u
#define UI_OPTIONS_SIZE 17949u
#define UI_PAUSE_SIZE 143644u
#define UI_SOURCE_MAX (UI_PAUSE_SIZE + 1u)
#define UI_OPTIONS_MAX 24576u
#define UI_PAUSE_MAX (UI_PAUSE_SIZE + 2048u)

static const unsigned char ui_zip_open_bytes[16] = {0x48, 0x8b, 0xc4, 0x44, 0x89, 0x48, 0x20, 0x48,
                                                    0x89, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54};

typedef void **(*UiZipOpenFn)(void *, void **, void *, unsigned);
typedef void *(*UiAllocFn)(size_t);
typedef void *(*UiMemoryStreamFn)(void *, const void *, int);
typedef unsigned long (*UiRefFn)(void *);
typedef char (*UiSeekFn)(void *, int, int);
typedef long long (*UiRemainingFn)(void *);
typedef int (*UiReadFn)(void *, void *, unsigned);

typedef struct {
    char *data;
    unsigned length, capacity;
} UiText;

static unsigned char *ui_game;
static UiZipOpenFn ui_zip_open_original;
static char ui_source[UI_SOURCE_MAX];
static char ui_options_buffer[UI_OPTIONS_MAX];
static UiText ui_options = {ui_options_buffer, 0, UI_OPTIONS_MAX};
/* the pause page alternates between two buffers, so a page still being parsed is never
   rewritten under it */
static char ui_pause_buffers[2][UI_PAUSE_MAX];
static UiText ui_pause[2] = {{ui_pause_buffers[0], 0, UI_PAUSE_MAX}, {ui_pause_buffers[1], 0, UI_PAUSE_MAX}};
static unsigned ui_pause_current;
static char ui_pause_banner[256];
static int ui_options_failed, ui_pause_failed;
static volatile LONG ui_busy, ui_logged_options, ui_logged_pause;

static void *ui_slot(void *object, unsigned offset) {
    return (*(void ***)object)[offset / sizeof(void *)];
}

/* ------------------------------------------------------------ text edits */

static const char *ui_find(const char *text, unsigned length, const char *needle, unsigned from) {
    size_t n = text_length(needle);
    for (unsigned i = from; i + n <= length; ++i) {
        if (text[i] == needle[0] && memory_equal(text + i, needle, n)) {
            return text + i;
        }
    }
    return 0;
}

static int ui_append(UiText *out, const char *text, size_t n) {
    if (out->length + n >= out->capacity) {
        return 0;
    }
    memory_copy(out->data + out->length, text, n);
    out->length += (unsigned)n;
    return 1;
}

/* Copies the source from `*at` up to `until`, skips `skip` bytes of it and adds `insert`. */
static int ui_copy(UiText *out, const char **at, const char *until, size_t skip, const char *insert) {
    if (!ui_append(out, *at, (size_t)(until - *at)) || (insert && !ui_append(out, insert, text_length(insert)))) {
        return 0;
    }
    *at = until + skip;
    return 1;
}

#define UI_INFINITY_ITEM                                                                                      \
    "\r\n            <local:TileMenuDataItem x:Key=\"A_MenuDataItem_Infinity\" Heading=\"INFINITY\" "        \
    "Icon=\"/ForzaUI;component/Textures/Anthem/Icons/TiledMenus/Options.png\" />"
#define UI_INFINITY_BUTTON                                                                                    \
    "\r\n                                <Button x:Name=\"PART_Button_Infinity\"\r\n"                        \
    "                                        local:TileMenuNavigationManager.TileState=\"Enabled\"\r\n"      \
    "                                        DataContext=\"{StaticResource A_MenuDataItem_Infinity}\"\r\n"   \
    "                                        local:TileMenuNavigationManager.GridX=\"4\"\r\n"                \
    "                                        local:TileMenuNavigationManager.GridY=\"1\"\r\n"                \
    "                                        Style=\"{DynamicResource A_Style_Button_Tile_1x1}\"\r\n"         \
    "                                        Canvas.Left=\"{StaticResource A_Double_TileCoord_X_4}\"\r\n"     \
    "                                        Canvas.Top=\"{StaticResource A_Double_TileCoord_Y_1}\"\r\n"      \
    "                                        Template=\"{StaticResource A_ControlTemplate_IconTile}\"\r\n"    \
    "                                        local:UIPageTransitionManager.TransitionIndex=\"6\"\r\n"        \
    "                                        local:BindingProxy.C=\"DisableMotionBlur\"/>"

/* Configurações: credits 1x1 on top, INFINITY below. 0 when the text is not the one we know. */
static int ui_edit_options(const char *page, unsigned length, UiText *out) {
    const char *item = ui_find(page, length, "x:Key=\"A_MenuDataItem_TeamCredits\"", 0);
    const char *item_end = item ? ui_find(page, length, "/>", (unsigned)(item - page)) : 0;
    const char *button = ui_find(page, length, "x:Name=\"PART_Button_TeamCredits\"", 0);
    if (!item_end || !button || item_end > button || ui_find(page, length, "PART_Button_Infinity", 0)) {
        return 0;
    }
    unsigned from = (unsigned)(button - page);
    const char *size = ui_find(page, length, "A_Style_Button_Tile_1x2", from);
    const char *tile = ui_find(page, length, "A_ControlTemplate_VerticalImageTile", from);
    const char *end = ui_find(page, length, "/>", from);
    if (!size || !tile || !end || size > end || tile > end) {
        return 0;
    }
    const char *at = page;
    out->length = 0;
    return ui_copy(out, &at, item_end + 2, 0, UI_INFINITY_ITEM) &&
           ui_copy(out, &at, size, text_length("A_Style_Button_Tile_1x2"), "A_Style_Button_Tile_1x1") &&
           ui_copy(out, &at, tile, text_length("A_ControlTemplate_VerticalImageTile"), "A_ControlTemplate_IconTile") &&
           ui_copy(out, &at, end + 2, 0, UI_INFINITY_BUTTON) && ui_copy(out, &at, page + length, 0, 0);
}

/* The pause menu with the status line after its player bar (same top margin as the bar). */
static int ui_edit_pause(const char *page, unsigned length, const char *text, const char *color, UiText *out) {
    const char *bar = ui_find(page, length, "x:Name=\"TopBar\" />", 0);
    if (!bar || ui_find(page, length, "x:Name=\"TopBar\" />", (unsigned)(bar - page) + 1)) {
        return 0;
    }
    const char *at = page;
    out->length = 0;
    return ui_copy(out, &at, bar + text_length("x:Name=\"TopBar\" />"), 0,
                   "\r\n        <StackPanel Orientation=\"Horizontal\" HorizontalAlignment=\"Center\" "
                   "VerticalAlignment=\"Top\" Margin=\"0,40,0,0\">\r\n"
                   "            <local:FZMaterial Width=\"12\" Height=\"12\" VerticalAlignment=\"Center\" "
                   "Margin=\"0,0,12,0\" Color=\"") &&
           ui_copy(out, &at, at, 0, color) && ui_copy(out, &at, at, 0, "\" />\r\n            <local:FZTextBlock Text=\"") &&
           ui_copy(out, &at, at, 0, text) &&
           ui_copy(out, &at, at, 0,
                   "\" Style=\"{StaticResource A_Style_FZTextBlock_AnthemA}\" Color=\"#f5f5f5\" FontSize=\"24\" "
                   "VerticalAlignment=\"Center\" />\r\n        </StackPanel>") &&
           ui_copy(out, &at, page + length, 0, 0);
}

/* ------------------------------------------------------------- the hook */

/* Reads a stream of exactly `size` bytes into ui_source; 1 when it holds `marker`. */
static int ui_read(void *stream, unsigned size, const char *marker) {
    unsigned got = 0;
    while (got < size) {
        int n = ((UiReadFn)ui_slot(stream, 0x90))(stream, ui_source + got, size - got);
        if (n <= 0) {
            break;
        }
        got += (unsigned)n;
    }
    ui_source[got] = 0;
    return got == size && ui_find(ui_source, got, marker, 0);
}

static void ui_rewind(void *stream) {
    ((UiSeekFn)ui_slot(stream, 0x38))(stream, 0, 0);
}

/* `*out` becomes a memory stream over `text`. */
static int ui_serve(void **out, void *stream, const UiText *text) {
    void *memory = ((UiAllocFn)(uintptr_t)(ui_game + UI_GAME_ALLOC_RVA))(0x60);
    if (!memory) {
        return 0;
    }
    ((UiMemoryStreamFn)(uintptr_t)(ui_game + UI_MEMORY_STREAM_RVA))(memory, text->data, (int)text->length);
    ((UiRefFn)ui_slot(memory, 0x10))(memory);
    *out = memory;
    ((UiRefFn)ui_slot(stream, 0x18))(stream);
    return 1;
}

static void ui_replace(void **out, void *stream) {
    long long size = ((UiRemainingFn)ui_slot(stream, 0x50))(stream);
    if (size == UI_OPTIONS_SIZE && !ui_options_failed) {
        if (!ui_read(stream, UI_OPTIONS_SIZE, "x:Class=\"ForzaUI.A_PauseOptions\"")) {
            ui_rewind(stream);
            return;
        }
        if (!ui_options.length && !ui_edit_options(ui_source, UI_OPTIONS_SIZE, &ui_options)) {
            ui_options.length = 0;
            ui_options_failed = 1;
            log_append("UI: A_PauseOptions.xaml not as expected; Configurações left as is.\r\n");
        }
        if (!ui_options.length || !ui_serve(out, stream, &ui_options)) {
            ui_rewind(stream);
        } else if (!InterlockedExchange(&ui_logged_options, 1)) {
            log_append("UI: Configurações: CRÉDITOS + INFINITY tiles.\r\n");
        }
    } else if (size == UI_PAUSE_SIZE && !ui_pause_failed) {
        static char text[200];
        const char *color = 0;
        if (!azimuth_account_banner(text, sizeof(text), &color) ||
            !ui_read(stream, UI_PAUSE_SIZE, "x:Class=\"ForzaUI.A_PauseMenu\"")) {
            ui_rewind(stream);
            return;
        }
        /* a new buffer only when the line changes */
        UiText *page = &ui_pause[ui_pause_current];
        if (!page->length || !memory_equal(ui_pause_banner, text, text_length(text) + 1)) {
            ui_pause_current ^= 1;
            page = &ui_pause[ui_pause_current];
            if (!ui_edit_pause(ui_source, UI_PAUSE_SIZE, text, color, page)) {
                page->length = 0;
                ui_pause_failed = 1;
                log_append("UI: A_PauseMenu.xaml not as expected; no Infinity status line.\r\n");
                ui_rewind(stream);
                return;
            }
            memory_copy(ui_pause_banner, text, text_length(text) + 1);
        }
        if (!ui_serve(out, stream, page)) {
            ui_rewind(stream);
        } else if (InterlockedIncrement(&ui_logged_pause) <= 3) {
            log_append("UI: pause menu read with the Infinity status line.\r\n");
        }
    }
}

static void **ui_zip_open(void *archive, void **out, void *entry, unsigned mode) {
    void **result = ui_zip_open_original(archive, out, entry, mode);
    void *stream = out ? *out : 0;
    /* zip entries open on several threads; the page buffers serve one at a time */
    if (!stream || InterlockedCompareExchange(&ui_busy, 1, 0)) {
        return result;
    }
    ui_replace(out, stream);
    InterlockedExchange(&ui_busy, 0);
    return result;
}

void azimuth_apply_ui(unsigned char *game_image) {
    unsigned char *target = game_image + UI_ZIP_OPEN_RVA, current[16];
    ui_game = game_image;
    if (!memory_read(target, current, sizeof(current)) || !memory_equal(current, ui_zip_open_bytes, sizeof(current))) {
        log_append("UI: zip reader not as expected; game pages left as is.\r\n");
        return;
    }
    /* trampoline: the 16 original bytes (position independent), then jmp back */
    unsigned char *trampoline = VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) {
        return;
    }
    uintptr_t back = (uintptr_t)(target + sizeof(current));
    memory_copy(trampoline, current, sizeof(current));
    trampoline[16] = 0xff;
    trampoline[17] = 0x25;
    trampoline[18] = trampoline[19] = trampoline[20] = trampoline[21] = 0;
    memory_copy(trampoline + 22, &back, sizeof(back));
    ui_zip_open_original = (UiZipOpenFn)(uintptr_t)trampoline;

    unsigned char jump[14] = {0xff, 0x25, 0, 0, 0, 0};
    uintptr_t hook = (uintptr_t)ui_zip_open;
    memory_copy(jump + 6, &hook, sizeof(hook));
    DWORD old_protection, ignored;
    if (!VirtualProtect(target, sizeof(jump), PAGE_EXECUTE_READWRITE, &old_protection)) {
        return;
    }
    memory_copy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), old_protection, &ignored);
    log_append("UI: zip reader hooked for the Configurações and pause pages.\r\n");
}
