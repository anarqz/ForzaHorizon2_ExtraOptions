/* FH2Online self-update. When the server's /info names a newer hook (latestHook, with the
   GitHub release it is in: latestRelease), the hook installs it by itself:
   - azimuth-tls.dll and azimuth-tls.dll.sha256 come from that release of the official
     repository (github.com/anarqz/ForzaInfinity), never from the game's server: a server only
     says which release is the newest, so it cannot hand its players code;
   - the DLL must match the published SHA-256, be a PE image and carry "+fh2online.N", N being
     the newer build the server named;
   - it is written beside ours as .new, then ours is renamed to .previous (Windows and Wine let a
     loaded DLL be renamed) and .new takes its name: the game runs it from its next start.
   [Network] AutoUpdate=0 turns that off: the menu then only says that an update is out. */
#include "azimuth_host.h"
#include "extra_options.h"
#include "logging.h"
#include "sha256.h"
#include "support.h"

#define UPDATE_MAX_DLL (8u << 20)
#define UPDATE_PATH 1024

static volatile LONG update_state = UPDATE_NONE;
static volatile LONG update_started;
static char update_latest[48], update_release[24];
static wchar_t update_text[480];

/* The N of the first "+fh2online.N" (a digit after the dot) in `text` (`size` bytes; 0 without
   one). */
static unsigned update_build_in(const char *text, size_t size) {
    static const char marker[] = "+fh2online.";
    size_t m = sizeof(marker) - 1;
    for (size_t i = 0; i + m < size; ++i) {
        if (memory_equal(text + i, marker, m) && text[i + m] >= '0' && text[i + m] <= '9') {
            unsigned n = 0;
            for (size_t j = i + m; j < size && text[j] >= '0' && text[j] <= '9' && n < 100000; ++j) {
                n = n * 10 + (unsigned)(text[j] - '0');
            }
            return n;
        }
    }
    return 0;
}

static unsigned update_build(const char *version) {
    return update_build_in(version, text_length(version));
}

static void text_wide(wchar_t *out, size_t cap, const char *text) {
    size_t n = 0;
    while (out[n] && n + 1 < cap) {
        ++n;
    }
    for (; *text && n + 1 < cap; ++text) {
        out[n++] = (wchar_t)(unsigned char)*text;
    }
    out[n] = 0;
}

static void update_say(const char *a, const char *b, const char *c) {
    update_text[0] = 0;
    text_wide(update_text, ARRAY_COUNT(update_text), a);
    text_wide(update_text, ARRAY_COUNT(update_text), b);
    text_wide(update_text, ARRAY_COUNT(update_text), c);
}

static void text_add_to(char *out, size_t cap, const char *text) {
    size_t n = text_length(out), k = text_length(text);
    if (n + k < cap) {
        memory_copy(out + n, text, k + 1);
    }
}

static void update_log(const char *a, const char *b) {
    static char line[300];
    line[0] = 0;
    text_add_to(line, sizeof(line) - 2, "UPDATE: ");
    text_add_to(line, sizeof(line) - 2, a);
    text_add_to(line, sizeof(line) - 2, b);
    text_add_to(line, sizeof(line), "\r\n");
    log_append(line);
}

/* "/anarqz/ForzaInfinity/releases/download/<release>/<file>" */
static int update_url(wchar_t *out, size_t cap, const char *file) {
    out[0] = 0;
    text_wide(out, cap, "/anarqz/ForzaInfinity/releases/download/");
    text_wide(out, cap, update_release);
    text_wide(out, cap, "/");
    text_wide(out, cap, file);
    size_t n = 0;
    while (out[n]) {
        ++n;
    }
    return n + 2 < cap;
}

static int hex_digit(char c) {
    return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

/* Writes `dll` as the new azimuth-tls.dll (this module's file), keeping ours as .previous. */
static const char *update_install(const unsigned char *dll, size_t size) {
    static wchar_t path[UPDATE_PATH], fresh[UPDATE_PATH + 16], previous[UPDATE_PATH + 16];
    static const wchar_t name[] = L"azimuth-tls.dll";
    HMODULE self = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(uintptr_t)update_install, &self)) {
        return "this file's place not found";
    }
    DWORD n = GetModuleFileNameW(self, path, UPDATE_PATH);
    size_t k = ARRAY_COUNT(name) - 1;
    if (!n || n >= UPDATE_PATH || n < k) {
        return "this file's place not found";
    }
    for (size_t i = 0; i < k; ++i) {
        wchar_t c = path[n - k + i];
        if ((c >= L'A' && c <= L'Z' ? c + 32 : c) != name[i]) {
            return "this file is not named azimuth-tls.dll";
        }
    }
    memory_copy(fresh, path, n * sizeof(wchar_t));
    memory_copy(fresh + n, L".new", sizeof(L".new"));
    memory_copy(previous, path, n * sizeof(wchar_t));
    memory_copy(previous + n, L".previous", sizeof(L".previous"));
    HANDLE file = CreateFileW(fresh, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return "could not write in data\\bin";
    }
    size_t done = 0;
    while (done < size) {
        DWORD wrote = 0, chunk = size - done > (1u << 20) ? (1u << 20) : (DWORD)(size - done);
        if (!WriteFile(file, dll + done, chunk, &wrote, 0) || !wrote) {
            break;
        }
        done += wrote;
    }
    int flushed = FlushFileBuffers(file);
    CloseHandle(file);
    if (done != size || !flushed) {
        DeleteFileW(fresh);
        return "could not write in data\\bin";
    }
    if (!MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(fresh);
        return "could not move the current file aside";
    }
    if (!MoveFileExW(fresh, path, MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(previous, path, MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(fresh);
        return "could not put the new file in place";
    }
    return 0;
}

static const char *update_download(void) {
    static wchar_t url[200];
    static char sha_text[96];
    size_t got = 0;
    if (!update_url(url, ARRAY_COUNT(url), "azimuth-tls.dll.sha256") ||
        azimuth_net_fetch(L"github.com", url, sha_text, sizeof(sha_text) - 1, &got) != 200 || got < 64) {
        return "GitHub did not answer";
    }
    unsigned char want[32], have[32];
    for (int i = 0; i < 32; ++i) {
        int hi = hex_digit(sha_text[2 * i]), lo = hex_digit(sha_text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return "the release's SHA-256 is not readable";
        }
        want[i] = (unsigned char)(hi * 16 + lo);
    }
    unsigned char *dll = HeapAlloc(GetProcessHeap(), 0, UPDATE_MAX_DLL);
    if (!dll) {
        return "out of memory";
    }
    const char *why = 0;
    if (!update_url(url, ARRAY_COUNT(url), "azimuth-tls.dll") ||
        azimuth_net_fetch(L"github.com", url, dll, UPDATE_MAX_DLL, &got) != 200 || got < 4096 ||
        got >= UPDATE_MAX_DLL) {
        why = "the download failed";
    } else {
        Sha256Context hash;
        sha256_initialize(&hash);
        sha256_update(&hash, dll, got);
        sha256_finalize(&hash, have);
        if (!memory_equal(want, have, 32)) {
            why = "the download does not match the release's SHA-256";
        } else if (dll[0] != 'M' || dll[1] != 'Z' ||
                   update_build_in((const char *)dll, got) != update_build(update_latest)) {
            why = "the download is not the version the server named";
        } else {
            why = update_install(dll, got);
        }
    }
    HeapFree(GetProcessHeap(), 0, dll);
    return why;
}

static DWORD WINAPI update_thread(void *unused) {
    (void)unused;
    const char *why = update_download();
    if (!why) {
        update_say("ForzaInfinity ", update_release, " is installed (" MOD_VERSION " before). Restart the game to "
                   "use it; your server and settings stay as they are.");
        InterlockedExchange(&update_state, UPDATE_INSTALLED);
        update_log("installed ", update_latest);
        return 0;
    }
    update_log("not installed: ", why);
    update_say("ForzaInfinity ", update_release, " is out, but it could not be installed by itself (");
    text_wide(update_text, ARRAY_COUNT(update_text), why);
    text_wide(update_text, ARRAY_COUNT(update_text), ").\n\nClose the game and run ForzaInfinity-Update in the Azimuth "
                                                     "folder (next to azimuth.exe).");
    InterlockedExchange(&update_state, UPDATE_AVAILABLE);
    return 0;
}

/* From the server's /info: `latest` its latestHook, `release` its latestRelease. */
void azimuth_update_offer(const char *latest, const char *release) {
    size_t r = text_length(release), l = text_length(latest);
    if (update_started || !l || l >= sizeof(update_latest) || update_build(latest) <= update_build(MOD_VERSION)) {
        return;
    }
    /* a release tag: v and digits and dots */
    int tag = r >= 2 && r < sizeof(update_release) && release[0] == 'v';
    for (size_t i = 1; tag && i < r; ++i) {
        tag = (release[i] >= '0' && release[i] <= '9') || release[i] == '.';
    }
    if (InterlockedExchange(&update_started, 1)) {
        return;
    }
    memory_copy(update_latest, latest, l + 1);
    if (tag) {
        memory_copy(update_release, release, r + 1);
    } else {
        memory_copy(update_release, latest, l + 1);
    }
    update_log("newer version on the server: ", latest);
    if (!tag || !azimuth_net_auto_update()) {
        update_say("ForzaInfinity ", update_release, " is out (this is " MOD_VERSION ").\n\nClose the game and run "
                   "ForzaInfinity-Update in the Azimuth folder (next to azimuth.exe): it installs the new version "
                   "and keeps your server and settings.");
        InterlockedExchange(&update_state, UPDATE_AVAILABLE);
        return;
    }
    update_say("Downloading ForzaInfinity ", update_release, "...");
    InterlockedExchange(&update_state, UPDATE_DOWNLOADING);
    HANDLE thread = CreateThread(0, 0, update_thread, 0, 0, 0);
    if (thread) {
        CloseHandle(thread);
    } else {
        update_thread(0);
    }
}

int azimuth_update_state(void) {
    return (int)update_state;
}

const wchar_t *azimuth_update_text(void) {
    return update_text;
}
