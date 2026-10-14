/* FH2Online account: "name#12345" on the configured server.

   The player creates it from the game (Settings > Infinity, azimuth_online_menu.c):
   the 5 digits come from a fingerprint of this PC (SHA-256 of the Windows MachineGuid and the
   serial of the volume holding the mod). Only those 5 digits and a per-server device id
   (SHA-256 of the fingerprint + "host:port", so servers cannot match players up) leave the
   PC. The server answers with an XUID, a device token and a web key (for the site); they are
   kept in FH2Online_Accounts.ini, one section per server, with Online=1/0 (whether the game
   starts in the online profile). The network layer sends X-Fh2-Token on every request.

   When the game starts online, it sees the account through Azimuth's LocalUser:
   - XboxUserId (vtable slot 0x24ff88, Azimuth formats a fixed 0x9000000000000) returns the
     account XUID; Azimuth then keeps a separate save (User_<xuid>), so the online profile
     starts from zero and the offline save is left alone;
   - the gamertag / display names (LocalDisplayInfo slots 0x24ffe8, 0x24fff8, 0x250000) are
     read from Azimuth's player-name std::wstring (RVA 0x318c60), which is rewritten in place.
   The token is checked on a thread, which then syncs the cloud save (azimuth_cloud.c); those
   getters wait for it (at most 30 s), so the game never caches the old identity nor opens the
   save before it is synced. Joining or leaving the online profile restarts the game.

   Another PC: Settings > Infinity > "Sign in" takes Name#12345 and the
   site key; the server issues a new token and the PC that had the profile is logged out (its
   next start finds the token refused, forgets it and says so in the menu). */
#include "azimuth_host.h"
#include "logging.h"
#include "sha256.h"
#include "support.h"

#define ACCOUNT_FILE L"FH2Online_Accounts.ini"
#define ACCOUNT_NAME_RVA 0x318c60u
#define ACCOUNT_XUID_SLOT 0x24ff88u
#define ACCOUNT_XUID_TARGET 0x23820u
#define ACCOUNT_HSTRING_CREATE_RVA 0x86110u
#define ACCOUNT_WAIT_MS 30000u

typedef HRESULT (*AccountGetterFn)(void *, void **);
typedef HRESULT (*AccountHstringCreateFn)(const wchar_t *, unsigned, void **);
typedef BOOL(WINAPI *WritePrivateProfileStringWFn)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
typedef BOOL(WINAPI *GetVolumeInformationWFn)(LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD,
                                               LPWSTR, DWORD);
typedef LONG(WINAPI *RegGetValueWFn)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
typedef void(WINAPI *SleepFn)(DWORD);
typedef BOOL(WINAPI *CreateProcessWFn)(LPCWSTR, LPWSTR, void *, void *, BOOL, DWORD, void *, LPCWSTR,
                                       STARTUPINFOW *, PROCESS_INFORMATION *);

static const unsigned account_info_slots[3] = {0x24ffe8u, 0x24fff8u, 0x250000u};
static const unsigned account_info_targets[3] = {0x233d0u, 0x234c0u, 0x234d0u};
static AccountGetterFn account_info_original[3];
static AccountGetterFn account_xuid_original;

static unsigned char *account_host;
static wchar_t account_directory[PATH_CAPACITY], account_file[PATH_CAPACITY];
static const wchar_t *account_server;
static volatile LONG account_ready = 1; /* token check finished (or not needed) */
static volatile LONG account_link;      /* server: 0 not checked yet, 1 answering, 2 not answering */
static int account_available;           /* fingerprint and paths ready: the menu can work */
static int account_active;              /* this session plays as the account */
static wchar_t account_token[72];
static wchar_t account_xuid[24];
static wchar_t account_gamertag[24];
static wchar_t account_web_key[24];
static wchar_t account_online[4];
static char account_discriminator[6];
static wchar_t account_discriminator_wide[6];
static char account_device[65];
static wchar_t account_notice[200]; /* shown by the menu, e.g. "logged in elsewhere" */
static SleepFn account_sleep;

/* ------------------------------------------------------------------ text */

static size_t wide_len(const wchar_t *text) {
    size_t n = 0;
    while (text && text[n]) {
        ++n;
    }
    return n;
}

static void hex_text(const unsigned char *data, size_t size, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    out[2 * size] = 0;
}

/* Appends ASCII `text` to `out` (capacity `cap`, kept NUL-terminated). */
static void text_add(char *out, size_t cap, const char *text) {
    size_t n = text_length(out);
    while (*text && n + 1 < cap) {
        out[n++] = *text++;
    }
    out[n] = 0;
}

static void wide_to_ascii(const wchar_t *in, char *out, size_t cap) {
    size_t n = 0;
    while (in && in[n] && n + 1 < cap) {
        out[n] = in[n] < 128 ? (char)in[n] : '?';
        ++n;
    }
    out[n] = 0;
}

/* UTF-8 (server messages) to UTF-16. */
static void utf8_to_wide(const char *in, wchar_t *out, size_t cap) {
    out[0] = 0;
    if (in && *in) {
        int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, (int)cap);
        out[n > 0 ? n - 1 : 0] = 0;
    }
}

/* The string value of "key" in a flat JSON object (no nesting), escapes \" and \\ only. */
static int json_string(const char *json, const char *key, char *out, size_t cap) {
    size_t key_length = text_length(key);
    out[0] = 0;
    for (const char *p = json; p && *p; ++p) {
        if (*p != '"' || !memory_equal(p + 1, key, key_length) || p[1 + key_length] != '"') {
            continue;
        }
        p += key_length + 2;
        while (*p == ' ' || *p == ':') {
            ++p;
        }
        if (*p != '"') {
            return 0;
        }
        size_t n = 0;
        for (++p; *p && *p != '"'; ++p) {
            char c = *p;
            if (c == '\\' && p[1]) {
                c = *++p == 'n' ? '\n' : *p;
            }
            if (n + 1 < cap) {
                out[n++] = c;
            }
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

/* 1 to 9 ASCII letters or digits starting with a letter (the server checks it too). */
int azimuth_account_valid_name(const wchar_t *name) {
    size_t n = wide_len(name);
    if (n < 1 || n > 9 || !((name[0] >= L'A' && name[0] <= L'Z') || (name[0] >= L'a' && name[0] <= L'z'))) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        wchar_t c = name[i];
        if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))) {
            return 0;
        }
    }
    return 1;
}

static void account_log(const char *prefix, const char *detail) {
    static char line[640];
    line[0] = 0;
    text_add(line, sizeof(line), "ACCOUNT: ");
    text_add(line, sizeof(line), prefix);
    text_add(line, sizeof(line), detail);
    text_add(line, sizeof(line), "\r\n");
    log_append(line);
}

/* ----------------------------------------------------------- fingerprint */

static int account_fingerprint(void) {
    static wchar_t guid[64];
    static char material[256];
    unsigned char full[32], device[32];
    DWORD size = sizeof(guid), serial = 0;
    HMODULE advapi = LoadLibraryExW(L"advapi32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    RegGetValueWFn reg_get = advapi ? (RegGetValueWFn)(uintptr_t)GetProcAddress(advapi, "RegGetValueW") : 0;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    GetVolumeInformationWFn volume =
        kernel ? (GetVolumeInformationWFn)(uintptr_t)GetProcAddress(kernel, "GetVolumeInformationW") : 0;
    guid[0] = 0;
    /* RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY */
    if (!reg_get || reg_get(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                            0x00000002 | 0x00010000, 0, guid, &size) != ERROR_SUCCESS) {
        log_append("ACCOUNT: MachineGuid unavailable; the fingerprint uses the volume only.\r\n");
        guid[0] = 0;
    }
    wchar_t root[4] = {account_directory[0], L':', L'\\', 0};
    if (!volume || !volume(root, 0, 0, &serial, 0, 0, 0, 0)) {
        serial = 0;
    }
    if (!guid[0] && !serial) {
        return 0;
    }
    char guid_ascii[64], serial_hex[9];
    wide_to_ascii(guid, guid_ascii, sizeof(guid_ascii));
    unsigned char serial_bytes[4] = {(unsigned char)(serial >> 24), (unsigned char)(serial >> 16),
                                     (unsigned char)(serial >> 8), (unsigned char)serial};
    hex_text(serial_bytes, 4, serial_hex);
    material[0] = 0;
    text_add(material, sizeof(material), "FH2Online/v1|");
    text_add(material, sizeof(material), guid_ascii);
    text_add(material, sizeof(material), "|");
    text_add(material, sizeof(material), serial_hex);
    Sha256Context hash;
    sha256_initialize(&hash);
    sha256_update(&hash, (const unsigned char *)material, text_length(material));
    sha256_finalize(&hash, full);

    unsigned long long number = 0;
    for (int i = 0; i < 8; ++i) {
        number = number << 8 | full[i];
    }
    number %= 100000u;
    for (int i = 4; i >= 0; --i) {
        account_discriminator[i] = (char)('0' + number % 10);
        account_discriminator_wide[i] = (wchar_t)account_discriminator[i];
        number /= 10;
    }
    account_discriminator[5] = 0;
    account_discriminator_wide[5] = 0;

    /* per-server device id: SHA-256(hex(fingerprint) | host:port) */
    char full_hex[65], server[300];
    hex_text(full, 32, full_hex);
    wide_to_ascii(account_server, server, sizeof(server));
    material[0] = 0;
    text_add(material, sizeof(material), full_hex);
    text_add(material, sizeof(material), "|");
    text_add(material, sizeof(material), server);
    sha256_initialize(&hash);
    sha256_update(&hash, (const unsigned char *)material, text_length(material));
    sha256_finalize(&hash, device);
    hex_text(device, 32, account_device);
    return 1;
}

/* ---------------------------------------------------------- local storage */

static int account_write(const wchar_t *key, const wchar_t *value) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    WritePrivateProfileStringWFn write =
        kernel ? (WritePrivateProfileStringWFn)(uintptr_t)GetProcAddress(kernel, "WritePrivateProfileStringW") : 0;
    return write && write(account_server, key, value, account_file);
}

static void account_save(void) {
    if (!account_write(L"Token", account_token) || !account_write(L"Xuid", account_xuid) ||
        !account_write(L"Gamertag", account_gamertag) || !account_write(L"WebKey", account_web_key) ||
        !account_write(L"Online", account_online)) {
        log_append("ACCOUNT: could not write FH2Online_Accounts.ini.\r\n");
    }
}

/* -------------------------------------------------------- identity patch */

/* Rewrites Azimuth's player-name std::wstring (ptr/inline buffer, size +0x10, capacity
   +0x18) in place; the gamertag fits whenever "Player 1"-style names already did. */
static int account_set_player_name(void) {
    unsigned char *string = account_host + ACCOUNT_NAME_RVA;
    unsigned long long capacity = 0;
    size_t length = wide_len(account_gamertag);
    if (!memory_read(string + 0x18, &capacity, 8) || capacity < length || capacity > 4096) {
        return 0;
    }
    wchar_t *text = capacity > 7 ? *(wchar_t **)string : (wchar_t *)string;
    if (!text || !memory_is_writable(text, (length + 1) * sizeof(wchar_t))) {
        return 0;
    }
    memory_copy(text, account_gamertag, (length + 1) * sizeof(wchar_t));
    *(unsigned long long *)(string + 0x10) = length;
    return 1;
}

static void account_wait(void) {
    for (unsigned waited = 0; !account_ready && waited < ACCOUNT_WAIT_MS; waited += 20) {
        account_sleep(20);
    }
}

static HRESULT account_get_xuid(void *self, void **out) {
    account_wait();
    if (!account_active || !out) {
        return account_xuid_original(self, out);
    }
    AccountHstringCreateFn create = (AccountHstringCreateFn)(uintptr_t)(account_host + ACCOUNT_HSTRING_CREATE_RVA);
    return create(account_xuid, (unsigned)wide_len(account_xuid), out);
}

/* Gamertag / display names: answered from the account directly. Azimuth rewrites its
   player-name string (e.g. when a controller is paired again), so patching it once is not
   enough. */
static HRESULT account_get_name(int slot, void *self, void **out) {
    account_wait();
    if (!account_active || !out) {
        return account_info_original[slot](self, out);
    }
    AccountHstringCreateFn create = (AccountHstringCreateFn)(uintptr_t)(account_host + ACCOUNT_HSTRING_CREATE_RVA);
    return create(account_gamertag, (unsigned)wide_len(account_gamertag), out);
}
static HRESULT account_get_info_0(void *self, void **out) {
    return account_get_name(0, self, out);
}
static HRESULT account_get_info_1(void *self, void **out) {
    return account_get_name(1, self, out);
}
static HRESULT account_get_info_2(void *self, void **out) {
    return account_get_name(2, self, out);
}
static const AccountGetterFn account_info_hooks[3] = {account_get_info_0, account_get_info_1,
                                                       account_get_info_2};

static int account_patch_slot(unsigned slot_rva, unsigned target_rva, AccountGetterFn hook, AccountGetterFn *original) {
    void **slot = (void **)(account_host + slot_rva);
    void *expected = account_host + target_rva, *current = 0;
    DWORD old_protection, ignored;
    if (!memory_read(slot, &current, sizeof(current)) || current != expected ||
        !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protection)) {
        return 0;
    }
    *original = (AccountGetterFn)(uintptr_t)expected;
    *slot = (void *)(uintptr_t)hook;
    VirtualProtect(slot, sizeof(*slot), old_protection, &ignored);
    return 1;
}

/* ------------------------------------------------------------ server calls */

/* Takes xuid/gamertag (and token, web key when present) from a server reply. */
static int account_take(const char *reply) {
    char value[96];
    if (!json_string(reply, "xuid", value, sizeof(value)) || !value[0]) {
        return 0;
    }
    utf8_to_wide(value, account_xuid, ARRAY_COUNT(account_xuid));
    if (!json_string(reply, "gamertag", value, sizeof(value)) || !value[0]) {
        return 0;
    }
    utf8_to_wide(value, account_gamertag, ARRAY_COUNT(account_gamertag));
    if (json_string(reply, "token", value, sizeof(value)) && value[0]) {
        utf8_to_wide(value, account_token, ARRAY_COUNT(account_token));
    }
    if (json_string(reply, "webKey", value, sizeof(value)) && value[0]) {
        utf8_to_wide(value, account_web_key, ARRAY_COUNT(account_web_key));
    }
    return 1;
}

static const wchar_t *account_token_header(void) {
    static wchar_t header[96];
    size_t n = sizeof(L"X-Fh2-Token: ") / sizeof(wchar_t) - 1, t = wide_len(account_token);
    memory_copy(header, L"X-Fh2-Token: ", (n + 1) * sizeof(wchar_t));
    if (n + t < ARRAY_COUNT(header)) {
        memory_copy(header + n, account_token, (t + 1) * sizeof(wchar_t));
    }
    return header;
}

/* The server's {"error": "..."} as text for the player, or a fallback. */
static void account_error(DWORD status, const char *reply, wchar_t *message, size_t cap) {
    static char text[400];
    if (status && json_string(reply, "error", text, sizeof(text)) && text[0]) {
        utf8_to_wide(text, message, cap);
    } else if (!status) {
        utf8_to_wide("Could not reach the server. Check its address and that it is running.",
                     message, cap);
    } else {
        utf8_to_wide("The server refused the request.", message, cap);
    }
}

static DWORD WINAPI account_thread(void *unused) {
    static char reply[2048], text[400], gamertag[32];
    (void)unused;
    DWORD status = azimuth_net_call(L"GET", L"/fh2online/v1/me", account_token_header(), 0, reply, sizeof(reply));
    if (status == 200 && account_take(reply)) {
        account_active = 1;
        InterlockedExchange(&account_link, 1);
        azimuth_cloud_start(account_directory, account_xuid, account_token, account_file, account_server);
    } else if (status == 401) {
        /* the profile logged in on another PC (or the server forgot it): this token is dead */
        utf8_to_wide("The profile ", account_notice, ARRAY_COUNT(account_notice));
        size_t n = wide_len(account_notice), g = wide_len(account_gamertag);
        if (n + g + 1 < ARRAY_COUNT(account_notice)) {
            memory_copy(account_notice + n, account_gamertag, (g + 1) * sizeof(wchar_t));
        }
        n = wide_len(account_notice);
        utf8_to_wide(" was signed in on another PC. To play it here, sign in again with the profile and its site key.",
                     account_notice + n, ARRAY_COUNT(account_notice) - n);
        account_token[0] = 0;
        account_write(L"Token", L"");
        account_log("the server no longer knows this PC's token (logged in elsewhere); forgotten.", "");
    } else if (status == 403) {
        json_string(reply, "error", text, sizeof(text));
        account_log("the server refused this PC's account; playing offline: ", text);
    } else {
        /* server unreachable: keep the stored identity, the token is sent anyway */
        account_active = account_xuid[0] && account_gamertag[0];
        account_log("server unreachable; using the stored account.", "");
    }
    if (account_active) {
        wide_to_ascii(account_gamertag, gamertag, sizeof(gamertag));
        account_log(account_set_player_name() ? "playing as " : "playing as (name not applied) ", gamertag);
    }
    InterlockedExchange(&account_ready, 1);
    return 0;
}

/* ------------------------------------------------------- menu interface */

int azimuth_account_state(void) {
    if (!account_available) {
        return -1;
    }
    if (account_active) {
        return 2;
    }
    return account_token[0] ? 1 : 0;
}

const wchar_t *azimuth_account_gamertag(void) {
    return account_gamertag;
}

const wchar_t *azimuth_account_web_key(void) {
    return account_web_key;
}

const wchar_t *azimuth_account_discriminator(void) {
    return account_discriminator_wide;
}

const wchar_t *azimuth_account_server(void) {
    return account_server ? account_server : L"";
}

/* Creates "name#12345" on the server (one account per PC). On success the account is kept
   with Online=1, so the next start plays as it. */
int azimuth_account_register(const wchar_t *name, int starter_car, wchar_t *message, size_t cap) {
    static char reply[2048], body[256];
    char ascii[16];
    if (!account_available || !azimuth_account_valid_name(name)) {
        utf8_to_wide("The name must have 1 to 9 letters or digits and start with a letter.", message, cap);
        return 0;
    }
    wide_to_ascii(name, ascii, sizeof(ascii));
    body[0] = 0;
    text_add(body, sizeof(body), "{\"name\":\"");
    text_add(body, sizeof(body), ascii);
    text_add(body, sizeof(body), "\",\"discriminator\":\"");
    text_add(body, sizeof(body), account_discriminator);
    text_add(body, sizeof(body), "\",\"device\":\"");
    text_add(body, sizeof(body), account_device);
    /* the first car (the server takes only the game's three) */
    char car[12];
    int digits = 0;
    for (unsigned value = starter_car > 0 ? (unsigned)starter_car : 0; digits == 0 || value; value /= 10) {
        car[digits++] = (char)('0' + value % 10);
    }
    text_add(body, sizeof(body), "\",\"starterCar\":");
    while (digits) {
        char one[2] = {car[--digits], 0};
        text_add(body, sizeof(body), one);
    }
    text_add(body, sizeof(body), "}");
    DWORD status = azimuth_net_call(L"POST", L"/fh2online/v1/register", 0, body, reply, sizeof(reply));
    if (status != 200 || !account_take(reply) || !account_token[0]) {
        account_error(status, reply, message, cap);
        account_token[0] = 0;
        return 0;
    }
    memory_copy(account_online, L"1", sizeof(L"1"));
    account_save();
    wide_to_ascii(account_gamertag, ascii, sizeof(ascii));
    account_log("created ", ascii);
    return 1;
}

/* Logs this PC in to an existing profile (Name#12345 + site key). The server issues a new
   token and a new site key; the PC that had the profile is logged out. The next start
   downloads the profile's cloud save. */
int azimuth_account_log_in(const wchar_t *gamertag, const wchar_t *web_key, wchar_t *message, size_t cap) {
    static char reply[2048], body[160];
    char tag[32], key[32];
    wide_to_ascii(gamertag, tag, sizeof(tag));
    wide_to_ascii(web_key, key, sizeof(key));
    /* Name#12345 and K7QM-4XPA-9RT2: letters, digits, '#' and '-' only (no JSON escaping) */
    for (const char *p = tag; *p; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '#')) {
            utf8_to_wide("Use the format Name#12345.", message, cap);
            return 0;
        }
    }
    for (const char *p = key; *p; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) {
            utf8_to_wide("A site key has letters, digits and dashes, like K7QM-4XPA-9RT2.", message, cap);
            return 0;
        }
    }
    if (!account_available) {
        utf8_to_wide("Infinity is not available on this PC.", message, cap);
        return 0;
    }
    body[0] = 0;
    text_add(body, sizeof(body), "{\"gamertag\":\"");
    text_add(body, sizeof(body), tag);
    text_add(body, sizeof(body), "\",\"webKey\":\"");
    text_add(body, sizeof(body), key);
    text_add(body, sizeof(body), "\"}");
    static wchar_t old_token[72];
    memory_copy(old_token, account_token, sizeof(account_token));
    DWORD status = azimuth_net_call(L"POST", L"/fh2online/v1/login", 0, body, reply, sizeof(reply));
    if (status != 200 || !account_take(reply) || !account_token[0] || wide_len(account_token) != 64) {
        account_error(status, reply, message, cap);
        memory_copy(account_token, old_token, sizeof(account_token));
        return 0;
    }
    memory_copy(account_online, L"1", sizeof(L"1"));
    account_save();
    account_write(L"SaveCommit", L""); /* this PC never synced this profile */
    account_notice[0] = 0;
    wide_to_ascii(account_gamertag, tag, sizeof(tag));
    account_log("logged in as ", tag);
    return 1;
}

static void report_add(wchar_t *out, size_t cap, const wchar_t *text) {
    size_t n = wide_len(out);
    while (text && *text && n + 1 < cap) {
        out[n++] = *text++;
    }
    out[n] = 0;
}

static void report_number(wchar_t *out, size_t cap, unsigned long long value) {
    wchar_t digits[24];
    int n = 0;
    do {
        digits[n++] = (wchar_t)(L'0' + value % 10);
        value /= 10;
    } while (value && n < 23);
    wchar_t text[24];
    for (int i = 0; i < n; ++i) {
        text[i] = digits[n - 1 - i];
    }
    text[n] = 0;
    report_add(out, cap, text);
}

/* Settings > Infinity > Testar conexão: is the server up, how fast, and does it still
   accept this PC's profile. */
void azimuth_account_test(wchar_t *out, size_t cap) {
    static char reply[1024], text[400];
    static wchar_t wide[400];
    out[0] = 0;
    report_add(out, cap, L"Server: ");
    report_add(out, cap, azimuth_account_server());
    unsigned long long start = GetTickCount64();
    DWORD status = azimuth_net_call(L"GET", L"/fh2online/v1/info", 0, 0, reply, sizeof(reply));
    unsigned long long took = GetTickCount64() - start;
    if (status != 200) {
        report_add(out, cap, status ? L"\nThe server answered with an error (HTTP " : L"\nNo answer from the server.");
        if (status) {
            report_number(out, cap, status);
            report_add(out, cap, L").");
        }
        report_add(out, cap, L"\n\nCheck the address in FH2_ExtraOptions.ini ([Network] Server), that the server is "
                             L"running and that its PC's firewall allows the port.");
        return;
    }
    report_add(out, cap, L"\nConnected: answered in ");
    report_number(out, cap, took);
    report_add(out, cap, L" ms.");
    if (!account_token[0]) {
        report_add(out, cap, L"\nProfile: none on this server yet.");
        return;
    }
    status = azimuth_net_call(L"GET", L"/fh2online/v1/me", account_token_header(), 0, reply, sizeof(reply));
    if (status == 200) {
        report_add(out, cap, L"\nProfile ");
        report_add(out, cap, account_gamertag);
        report_add(out, cap, account_active ? L": accepted, playing online." : L": accepted (restart to play online).");
    } else {
        json_string(reply, "error", text, sizeof(text));
        utf8_to_wide(text[0] ? text : "refused.", wide, ARRAY_COUNT(wide));
        report_add(out, cap, L"\nProfile: ");
        report_add(out, cap, wide);
    }
}

/* Pings the server every 15 s for the pause menu's status line. */
static DWORD WINAPI account_link_thread(void *unused) {
    static char reply[512];
    (void)unused;
    for (;;) {
        DWORD status = azimuth_net_call(L"GET", L"/fh2online/v1/info", 0, 0, reply, sizeof(reply));
        InterlockedExchange(&account_link, status == 200 ? 1 : 2);
        account_sleep(15000);
    }
}

/* The pause menu's status line (ASCII/UTF-8, no XML specials) and its dot color; 0 when the
   Infinity is not configured. */
int azimuth_account_banner(char *out, size_t cap, const char **color) {
    char tag[32];
    out[0] = 0;
    if (!account_available) {
        return 0;
    }
    /* checked now, every time the pause menu opens (a short wait, so the menu never hangs) */
    InterlockedExchange(&account_link, azimuth_net_ping(1000) ? 1 : 2);
    LONG link = account_link;
    if (account_active && link != 2) {
        wide_to_ascii(account_gamertag, tag, sizeof(tag));
        text_add(out, cap, "INFINITY  |  CONNECTED  |  ");
        text_add(out, cap, tag);
        *color = "#3ddc84";
    } else if (link == 2) {
        text_add(out, cap, "INFINITY  |  NO CONNECTION TO THE SERVER");
        *color = "#ff4f4f";
    } else if (account_token[0]) {
        text_add(out, cap, "INFINITY  |  ONLINE PROFILE OFF");
        *color = "#9a9a9a";
    } else {
        text_add(out, cap, link == 1 ? "INFINITY  |  SERVER ONLINE  |  CREATE YOUR PROFILE IN SETTINGS"
                                     : "INFINITY  |  CHECKING THE SERVER...");
        *color = link == 1 ? "#f5c400" : "#9a9a9a";
    }
    return 1;
}

const wchar_t *azimuth_account_notice(void) {
    return account_notice;
}

/* A new site key (the old one stops working). */
int azimuth_account_new_web_key(wchar_t *message, size_t cap) {
    static char reply[1024], key[64];
    DWORD status = azimuth_net_call(L"POST", L"/fh2online/v1/webkey", account_token_header(), "{}", reply, sizeof(reply));
    if (status != 200 || !json_string(reply, "webKey", key, sizeof(key)) || !key[0]) {
        account_error(status, reply, message, cap);
        return 0;
    }
    utf8_to_wide(key, account_web_key, ARRAY_COUNT(account_web_key));
    account_write(L"WebKey", account_web_key);
    return 1;
}

/* Asks the server to befriend "Name#12345"; `message` is the server's answer for the
   player either way (friends once both added each other). */
int azimuth_account_add_friend(const wchar_t *gamertag, wchar_t *message, size_t cap) {
    static char reply[1024], body[96], text[400];
    char tag[32];
    wide_to_ascii(gamertag, tag, sizeof(tag));
    /* Name#12345 only: letters, digits and one '#' (no JSON escaping needed) */
    for (const char *p = tag; *p; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '#')) {
            utf8_to_wide("Use the format Name#12345.", message, cap);
            return 0;
        }
    }
    body[0] = 0;
    text_add(body, sizeof(body), "{\"gamertag\":\"");
    text_add(body, sizeof(body), tag);
    text_add(body, sizeof(body), "\"}");
    DWORD status = azimuth_net_call(L"POST", L"/fh2online/v1/friends", account_token_header(), body, reply, sizeof(reply));
    if (status == 200 && json_string(reply, "message", text, sizeof(text)) && text[0]) {
        utf8_to_wide(text, message, cap);
        return 1;
    }
    account_error(status, reply, message, cap);
    return 0;
}

/* Whether the next start plays as the account (1) or as the offline profile (0). */
void azimuth_account_set_online(int online) {
    memory_copy(account_online, online ? L"1" : L"0", sizeof(L"1"));
    account_write(L"Online", account_online);
}

/* Closes the game and opens azimuth.exe again (its launcher shows Start). */
void azimuth_account_restart(void) {
    static wchar_t exe[PATH_CAPACITY], folder[PATH_CAPACITY];
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    CreateProcessWFn create = kernel ? (CreateProcessWFn)(uintptr_t)GetProcAddress(kernel, "CreateProcessW") : 0;
    DWORD n = GetModuleFileNameW(0, exe, PATH_CAPACITY);
    if (create && n && n < PATH_CAPACITY) {
        memory_copy(folder, exe, (n + 1) * sizeof(wchar_t));
        while (n && folder[n - 1] != L'\\' && folder[n - 1] != L'/') {
            --n;
        }
        folder[n] = 0;
        STARTUPINFOW startup = {0};
        PROCESS_INFORMATION process = {0};
        startup.cb = sizeof(startup);
        if (create(exe, 0, 0, 0, FALSE, 0, 0, folder, &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
    log_append("ACCOUNT: restarting the game for the profile change.\r\n");
    ExitProcess(0);
}

const wchar_t *azimuth_account_token(void) {
    return account_ready && account_active && account_token[0] ? account_token : 0;
}

void azimuth_apply_account(const wchar_t *directory) {
    if (!azimuth_net_proxy_enabled() || !(account_server = azimuth_net_server_name())) {
        return;
    }
    account_host = (unsigned char *)GetModuleHandleW(NULL);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    account_sleep = kernel ? (SleepFn)(uintptr_t)GetProcAddress(kernel, "Sleep") : 0;
    size_t n = wide_len(directory);
    if (!account_sleep || azimuth_host_build(account_host) != AZIMUTH_097 || n >= PATH_CAPACITY ||
        !path_join(account_file, directory, ACCOUNT_FILE)) {
        log_append("ACCOUNT: unsupported host or paths; FH2Online accounts unavailable.\r\n");
        return;
    }
    memory_copy(account_directory, directory, (n + 1) * sizeof(wchar_t));
    if (!account_fingerprint()) {
        log_append("ACCOUNT: no fingerprint for this PC; FH2Online accounts unavailable.\r\n");
        return;
    }
    account_available = 1;
    HANDLE link = CreateThread(0, 0, account_link_thread, 0, 0, 0);
    if (link) {
        CloseHandle(link);
    }
    GetPrivateProfileStringW(account_server, L"Token", L"", account_token, ARRAY_COUNT(account_token), account_file);
    GetPrivateProfileStringW(account_server, L"Xuid", L"", account_xuid, ARRAY_COUNT(account_xuid), account_file);
    GetPrivateProfileStringW(account_server, L"Gamertag", L"", account_gamertag, ARRAY_COUNT(account_gamertag),
                             account_file);
    GetPrivateProfileStringW(account_server, L"WebKey", L"", account_web_key, ARRAY_COUNT(account_web_key),
                             account_file);
    GetPrivateProfileStringW(account_server, L"Online", L"1", account_online, ARRAY_COUNT(account_online),
                             account_file);
    account_log("fingerprint #", account_discriminator);
    if (!account_token[0]) {
        log_append("ACCOUNT: no account on this server yet (Settings > Infinity creates one).\r\n");
        return;
    }
    if (account_online[0] != L'1') {
        log_append("ACCOUNT: account kept but Online=0; playing the offline profile.\r\n");
        return;
    }
    int patched = account_patch_slot(ACCOUNT_XUID_SLOT, ACCOUNT_XUID_TARGET, account_get_xuid, &account_xuid_original);
    for (unsigned i = 0; i < 3; ++i) {
        patched += account_patch_slot(account_info_slots[i], account_info_targets[i], account_info_hooks[i],
                                      &account_info_original[i]);
    }
    if (patched != 4) {
        log_append("ACCOUNT: LocalUser getters not where expected; playing the offline profile.\r\n");
        return;
    }
    account_ready = 0;
    HANDLE thread = CreateThread(0, 0, account_thread, 0, 0, 0);
    if (!thread) {
        account_ready = 1;
        return;
    }
    CloseHandle(thread);
}
