/* FH2Online account: "name#12345" on the configured server.

   The 5 digits come from a fingerprint of this PC: SHA-256 of the Windows MachineGuid and
   the serial of the volume holding the mod. Only those 5 digits and a per-server device id
   (SHA-256 of the fingerprint + "host:port", so servers cannot match players up) leave the
   PC. The server answers with an XUID and a token, kept in FH2Online_Accounts.ini (one
   section per server); the network layer then sends X-Fh2-Token on every request to it.

   The game sees the account through Azimuth's LocalUser:
   - XboxUserId (vtable slot 0x24ff88, Azimuth formats a fixed 0x9000000000000) returns the
     account XUID;
   - the gamertag / display names (LocalDisplayInfo slots 0x24ffe8, 0x24fff8, 0x250000) are
     read from Azimuth's player-name std::wstring (RVA 0x318c60), which is rewritten in place.
   Registration runs on its own thread; those getters wait for it (at most 10 s), so the
   game never caches the old identity. Without [Account] Name, or when the server refuses,
   the game keeps Azimuth's player as before. */
#include "azimuth_host.h"
#include "logging.h"
#include "sha256.h"
#include "support.h"

#define ACCOUNT_FILE L"FH2Online_Accounts.ini"
#define ACCOUNT_NAME_RVA 0x318c60u
#define ACCOUNT_XUID_SLOT 0x24ff88u
#define ACCOUNT_XUID_TARGET 0x23820u
#define ACCOUNT_HSTRING_CREATE_RVA 0x86110u
#define ACCOUNT_WAIT_MS 10000u

typedef HRESULT (*AccountGetterFn)(void *, void **);
typedef HRESULT (*AccountHstringCreateFn)(const wchar_t *, unsigned, void **);
typedef BOOL(WINAPI *WritePrivateProfileStringWFn)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
typedef BOOL(WINAPI *GetVolumeInformationWFn)(LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD,
                                               LPWSTR, DWORD);
typedef LONG(WINAPI *RegGetValueWFn)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
typedef void(WINAPI *SleepFn)(DWORD);

static const unsigned account_info_slots[3] = {0x24ffe8u, 0x24fff8u, 0x250000u};
static const unsigned account_info_targets[3] = {0x233d0u, 0x234c0u, 0x234d0u};
static AccountGetterFn account_info_original[3];
static AccountGetterFn account_xuid_original;

static unsigned char *account_host;
static wchar_t account_directory[PATH_CAPACITY], account_file[PATH_CAPACITY];
static wchar_t account_name[16];
static const wchar_t *account_server;
static volatile LONG account_ready; /* registration finished (with or without an account) */
static int account_active;          /* the game uses the account below */
static wchar_t account_token[72];
static wchar_t account_xuid[24];
static wchar_t account_gamertag[24];
static char account_discriminator[6];
static char account_device[65];
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

static void ascii_to_wide(const char *in, wchar_t *out, size_t cap) {
    size_t n = 0;
    while (in && in[n] && n + 1 < cap) {
        out[n] = (unsigned char)in[n];
        ++n;
    }
    out[n] = 0;
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
            if (*p == '\\' && p[1]) {
                ++p;
            }
            if (n + 1 < cap) {
                out[n++] = *p;
            }
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

/* 1 to 9 ASCII letters or digits starting with a letter (the server checks it too). */
static int valid_name(const wchar_t *name) {
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
        number /= 10;
    }
    account_discriminator[5] = 0;

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

/* ---------------------------------------------------------- token storage */

static void account_save(void) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    WritePrivateProfileStringWFn write =
        kernel ? (WritePrivateProfileStringWFn)(uintptr_t)GetProcAddress(kernel, "WritePrivateProfileStringW") : 0;
    if (!write || !write(account_server, L"Token", account_token, account_file) ||
        !write(account_server, L"Xuid", account_xuid, account_file) ||
        !write(account_server, L"Gamertag", account_gamertag, account_file)) {
        log_append("ACCOUNT: could not write FH2Online_Accounts.ini; the account works for this session only.\r\n");
    }
}

static void account_forget(void) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    WritePrivateProfileStringWFn write =
        kernel ? (WritePrivateProfileStringWFn)(uintptr_t)GetProcAddress(kernel, "WritePrivateProfileStringW") : 0;
    if (write) {
        write(account_server, L"Token", 0, account_file);
    }
    account_token[0] = 0;
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

static HRESULT account_get_info_0(void *self, void **out) {
    account_wait();
    return account_info_original[0](self, out);
}
static HRESULT account_get_info_1(void *self, void **out) {
    account_wait();
    return account_info_original[1](self, out);
}
static HRESULT account_get_info_2(void *self, void **out) {
    account_wait();
    return account_info_original[2](self, out);
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

/* -------------------------------------------------------- registration */

static void account_log(const char *prefix, const char *detail) {
    static char line[640];
    line[0] = 0;
    text_add(line, sizeof(line), "ACCOUNT: ");
    text_add(line, sizeof(line), prefix);
    text_add(line, sizeof(line), detail);
    text_add(line, sizeof(line), "\r\n");
    log_append(line);
}

/* Takes xuid/gamertag (and token, if present) from a server reply. */
static int account_take(const char *reply) {
    char value[96];
    if (!json_string(reply, "xuid", value, sizeof(value)) || !value[0]) {
        return 0;
    }
    ascii_to_wide(value, account_xuid, ARRAY_COUNT(account_xuid));
    if (!json_string(reply, "gamertag", value, sizeof(value)) || !value[0]) {
        return 0;
    }
    ascii_to_wide(value, account_gamertag, ARRAY_COUNT(account_gamertag));
    if (json_string(reply, "token", value, sizeof(value)) && value[0]) {
        ascii_to_wide(value, account_token, ARRAY_COUNT(account_token));
    }
    return 1;
}

static DWORD WINAPI account_thread(void *unused) {
    static char reply[2048], body[256], gamertag[32];
    static wchar_t header[96];
    (void)unused;
    if (account_token[0]) {
        /* known account: check the token is still good */
        header[0] = 0;
        memory_copy(header, L"X-Fh2-Token: ", sizeof(L"X-Fh2-Token: "));
        size_t n = wide_len(header), t = wide_len(account_token);
        if (n + t < ARRAY_COUNT(header)) {
            memory_copy(header + n, account_token, (t + 1) * sizeof(wchar_t));
        }
        DWORD status = azimuth_net_call(L"GET", L"/fh2online/v1/me", header, 0, reply, sizeof(reply));
        if (status == 200 && account_take(reply)) {
            account_active = 1;
        } else if (status == 401) {
            account_log("the server no longer knows this PC's token; registering again.", "");
            account_forget();
        } else if (status == 403) {
            json_string(reply, "error", body, sizeof(body));
            account_log("account refused by the server: ", body);
        } else {
            /* server unreachable: keep the stored identity, the token is sent anyway */
            account_active = account_xuid[0] && account_gamertag[0];
            account_log("server unreachable; using the stored account.", "");
        }
    }
    if (!account_active && !account_token[0] && account_name[0]) {
        char name[16];
        wide_to_ascii(account_name, name, sizeof(name));
        body[0] = 0;
        text_add(body, sizeof(body), "{\"name\":\"");
        text_add(body, sizeof(body), name);
        text_add(body, sizeof(body), "\",\"discriminator\":\"");
        text_add(body, sizeof(body), account_discriminator);
        text_add(body, sizeof(body), "\",\"device\":\"");
        text_add(body, sizeof(body), account_device);
        text_add(body, sizeof(body), "\"}");
        DWORD status = azimuth_net_call(L"POST", L"/fh2online/v1/register", 0, body, reply, sizeof(reply));
        if (status == 200 && account_take(reply) && account_token[0]) {
            account_active = 1;
            account_save();
            wide_to_ascii(account_gamertag, gamertag, sizeof(gamertag));
            account_log("created ", gamertag);
        } else if (status) {
            json_string(reply, "error", body, sizeof(body));
            account_log("registration refused: ", body);
        } else {
            account_log("registration failed: server unreachable.", "");
        }
    }
    if (account_active) {
        wide_to_ascii(account_gamertag, gamertag, sizeof(gamertag));
        account_log(account_set_player_name() ? "playing as " : "playing as (name not applied) ", gamertag);
    }
    InterlockedExchange(&account_ready, 1);
    return 0;
}

const wchar_t *azimuth_account_token(void) {
    return account_ready && account_token[0] ? account_token : 0;
}

void azimuth_apply_account(const wchar_t *directory) {
    static wchar_t ini[PATH_CAPACITY];
    account_ready = 1;
    if (!azimuth_net_proxy_enabled() || !(account_server = azimuth_net_server_name())) {
        return;
    }
    account_host = (unsigned char *)GetModuleHandleW(NULL);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    account_sleep = kernel ? (SleepFn)(uintptr_t)GetProcAddress(kernel, "Sleep") : 0;
    size_t n = wide_len(directory);
    if (!account_sleep || azimuth_host_build(account_host) != AZIMUTH_097 || n >= PATH_CAPACITY ||
        !path_join(ini, directory, MOD_INI_NAME) || !path_join(account_file, directory, ACCOUNT_FILE)) {
        log_append("ACCOUNT: unsupported host or paths; playing without an FH2Online account.\r\n");
        return;
    }
    memory_copy(account_directory, directory, (n + 1) * sizeof(wchar_t));
    GetPrivateProfileStringW(L"Account", L"Name", L"", account_name, ARRAY_COUNT(account_name), ini);
    GetPrivateProfileStringW(account_server, L"Token", L"", account_token, ARRAY_COUNT(account_token), account_file);
    GetPrivateProfileStringW(account_server, L"Xuid", L"", account_xuid, ARRAY_COUNT(account_xuid), account_file);
    GetPrivateProfileStringW(account_server, L"Gamertag", L"", account_gamertag, ARRAY_COUNT(account_gamertag),
                             account_file);
    if (!account_token[0] && !account_name[0]) {
        log_append("ACCOUNT: no account on this server; set [Account] Name to create one.\r\n");
        return;
    }
    if (account_name[0] && !valid_name(account_name)) {
        log_append("ACCOUNT: [Account] Name must be 1-9 letters or digits starting with a letter.\r\n");
        account_name[0] = 0;
        if (!account_token[0]) {
            return;
        }
    }
    if (!account_fingerprint()) {
        log_append("ACCOUNT: no fingerprint for this PC; playing without an FH2Online account.\r\n");
        return;
    }
    int patched = account_patch_slot(ACCOUNT_XUID_SLOT, ACCOUNT_XUID_TARGET, account_get_xuid, &account_xuid_original);
    for (unsigned i = 0; i < 3; ++i) {
        patched += account_patch_slot(account_info_slots[i], account_info_targets[i], account_info_hooks[i],
                                      &account_info_original[i]);
    }
    if (patched != 4) {
        log_append("ACCOUNT: LocalUser getters not where expected; playing without an FH2Online account.\r\n");
        return;
    }
    account_ready = 0;
    HANDLE thread = CreateThread(0, 0, account_thread, 0, 0, 0);
    if (!thread) {
        account_ready = 1;
        return;
    }
    CloseHandle(thread);
    account_log("fingerprint #", account_discriminator);
}
