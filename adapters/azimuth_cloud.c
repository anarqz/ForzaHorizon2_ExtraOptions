/* FH2Online cloud save: while the game plays an online profile, the server keeps the newest
   save revision of the account, so the profile continues on any PC it logs in from.

   Azimuth keeps a profile in data\saves\player-1\User_<XUID in hex>\revisions\<16 hex
   digits>\: a "commit" file (the SHA-256, size and name of each file under data\, checked when
   the game loads it) and data\*. A revision is bundled as is, each file compressed by
   azimuth_codec.c ("FH2SAVE1", see server/src/db/saves.rs), and sent to PUT /fh2online/v1/save.
   Uploads use method 2; downloads also take method 1, the XPRESS Huffman of ntdll that older
   hooks sent from Windows (Wine has neither, so the hook decodes both itself).

   At start (on the account thread, before the game asks for the XUID and opens the save):
   - the server has the save this PC has: nothing to do;
   - the local save is the one last synced (SaveCommit in FH2Online_Accounts.ini) and the
     server's is another: the profile was played elsewhere, the server's revision is written
     here as a new, higher revision;
   - the server still has the last synced save and the local one is newer: it is uploaded;
   - both changed: the server wins; the local revision is moved to saves\fh2online-backup.
   While playing, a thread uploads each new revision (checked every minute).
   A downloaded bundle is installed only if every file matches the commit's SHA-256 and size,
   and only the fixed file names below are ever written. */
#include "azimuth_codec.h"
#include "azimuth_host.h"
#include "logging.h"
#include "sha256.h"
#include "support.h"

#define CLOUD_PATH 1024
#define CLOUD_MAX_BUNDLE (2u << 20)
#define CLOUD_MAX_FILE (32u << 20)
#define CLOUD_POLL_MS 60000u

typedef HANDLE(WINAPI *FindFirstFileWFn)(LPCWSTR, WIN32_FIND_DATAW *);
typedef BOOL(WINAPI *FindNextFileWFn)(HANDLE, WIN32_FIND_DATAW *);
typedef BOOL(WINAPI *FindCloseFn)(HANDLE);
typedef BOOL(WINAPI *CreateDirectoryWFn)(LPCWSTR, void *);
typedef BOOL(WINAPI *MoveFileExWFn)(LPCWSTR, LPCWSTR, DWORD);
typedef BOOL(WINAPI *WritePrivateProfileStringWFn)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
typedef void(WINAPI *SleepFn)(DWORD);

static const char *const cloud_files[] = {"commit",          "data/BinarySaveState", "data/PlayerDatabase",
                                          "data/ProfileData", "data/SaveState",       "data/TransactionLogFile",
                                          "data/VersionFlags"};
#define CLOUD_FILE_COUNT (sizeof(cloud_files) / sizeof(cloud_files[0]))

static FindFirstFileWFn cloud_find_first;
static FindNextFileWFn cloud_find_next;
static FindCloseFn cloud_find_close;
static CreateDirectoryWFn cloud_create_directory;
static MoveFileExWFn cloud_move;
static WritePrivateProfileStringWFn cloud_write_profile;
static SleepFn cloud_sleep;

static wchar_t cloud_saves[CLOUD_PATH];    /* data\saves */
static wchar_t cloud_profile[CLOUD_PATH];  /* data\saves\player-1\User_<hex> */
static wchar_t cloud_ini[CLOUD_PATH];
static wchar_t cloud_section[300];
static wchar_t cloud_headers[200];         /* X-Fh2-Token: ... */
static wchar_t cloud_synced[72];           /* SaveCommit: what server and PC last agreed on */
static void *cloud_workspace;              /* codec_lz_encode's hash table */

/* ------------------------------------------------------------- helpers */

static void *cloud_alloc(size_t size) {
    return size ? HeapAlloc(GetProcessHeap(), 0, size) : 0;
}

static void cloud_free(void *p) {
    if (p) {
        HeapFree(GetProcessHeap(), 0, p);
    }
}

static size_t wlen(const wchar_t *t) {
    size_t n = 0;
    while (t && t[n]) {
        ++n;
    }
    return n;
}

/* out = a + b (+ c), within CLOUD_PATH. */
static int wjoin(wchar_t *out, const wchar_t *a, const wchar_t *b, const wchar_t *c) {
    size_t n = 0;
    const wchar_t *parts[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        for (const wchar_t *p = parts[i]; p && *p; ++p) {
            if (n + 1 >= CLOUD_PATH) {
                return 0;
            }
            out[n++] = *p;
        }
    }
    out[n] = 0;
    return 1;
}

/* "data/X" -> L"data\\X" */
static void file_name_wide(const char *name, wchar_t *out, size_t cap) {
    size_t n = 0;
    for (; name[n] && n + 1 < cap; ++n) {
        out[n] = name[n] == '/' ? L'\\' : (wchar_t)name[n];
    }
    out[n] = 0;
}

static void hex16(unsigned long long value, wchar_t out[17]) {
    static const wchar_t digits[] = L"0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        out[i] = digits[value & 15];
        value >>= 4;
    }
    out[16] = 0;
}

static void sha_hex(const unsigned char *data, size_t size, char out[65]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[32];
    Sha256Context hash;
    sha256_initialize(&hash);
    sha256_update(&hash, data, size);
    sha256_finalize(&hash, digest);
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = digits[digest[i] >> 4];
        out[2 * i + 1] = digits[digest[i] & 15];
    }
    out[64] = 0;
}

static void cloud_log(const char *a, const char *b) {
    static char line[400];
    size_t n = 0;
    const char *parts[3] = {"CLOUD: ", a, b};
    for (int i = 0; i < 3; ++i) {
        for (const char *p = parts[i]; p && *p && n + 3 < sizeof(line); ++p) {
            line[n++] = *p;
        }
    }
    line[n++] = '\r';
    line[n++] = '\n';
    line[n] = 0;
    log_append(line);
}

/* Whole file into a new buffer (NULL when missing or larger than `max`). */
static unsigned char *read_file(const wchar_t *path, size_t max, size_t *size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    LARGE_INTEGER length;
    unsigned char *data = 0;
    *size = 0;
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (GetFileSizeEx(file, &length) && length.QuadPart >= 0 && (unsigned long long)length.QuadPart <= max &&
        (data = cloud_alloc((size_t)length.QuadPart + 1)) != 0) {
        size_t done = 0;
        while (done < (size_t)length.QuadPart) {
            DWORD got = 0;
            if (!ReadFile(file, data + done, (DWORD)((size_t)length.QuadPart - done), &got, 0) || !got) {
                break;
            }
            done += got;
        }
        if (done == (size_t)length.QuadPart) {
            *size = done;
        } else {
            cloud_free(data);
            data = 0;
        }
    }
    CloseHandle(file);
    return data;
}

static int write_file(const wchar_t *path, const unsigned char *data, size_t size) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    size_t done = 0;
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    while (done < size) {
        DWORD wrote = 0;
        if (!WriteFile(file, data + done, (DWORD)(size - done), &wrote, 0) || !wrote) {
            break;
        }
        done += wrote;
    }
    CloseHandle(file);
    return done == size;
}

/* ------------------------------------------------------------ revisions */

typedef struct {
    int found;
    unsigned long long number;
    wchar_t path[CLOUD_PATH]; /* ...\revisions\<hex> */
    char commit[65];          /* SHA-256 of its commit file */
} CloudRevision;

static int parse_hex16(const wchar_t *name, unsigned long long *value) {
    unsigned long long v = 0;
    for (int i = 0; i < 16; ++i) {
        wchar_t c = name[i];
        int d = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10
                : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (d < 0) {
            return 0;
        }
        v = v << 4 | (unsigned)d;
    }
    *value = v;
    return name[16] == 0;
}

/* The newest revision with a commit file; `highest` gets the highest number seen at all. */
static void newest_revision(CloudRevision *out, unsigned long long *highest) {
    static wchar_t pattern[CLOUD_PATH], commit_path[CLOUD_PATH];
    static WIN32_FIND_DATAW entry;
    out->found = 0;
    if (highest) {
        *highest = 0;
    }
    if (!wjoin(pattern, cloud_profile, L"\\revisions\\*", 0)) {
        return;
    }
    HANDLE find = cloud_find_first(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    unsigned long long best = 0;
    int have = 0;
    do {
        unsigned long long number;
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !parse_hex16(entry.cFileName, &number)) {
            continue;
        }
        if (highest && number > *highest) {
            *highest = number;
        }
        wchar_t name[17];
        hex16(number, name);
        if (!wjoin(commit_path, cloud_profile, L"\\revisions\\", name) || !wjoin(commit_path, commit_path, L"\\commit", 0)) {
            continue;
        }
        HANDLE file = CreateFileW(commit_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        if (file == INVALID_HANDLE_VALUE) {
            continue;
        }
        CloseHandle(file);
        if (!have || number > best) {
            best = number;
            have = 1;
        }
    } while (cloud_find_next(find, &entry));
    cloud_find_close(find);
    if (!have) {
        return;
    }
    wchar_t name[17];
    hex16(best, name);
    if (!wjoin(out->path, cloud_profile, L"\\revisions\\", name) || !wjoin(commit_path, out->path, L"\\commit", 0)) {
        return;
    }
    size_t size;
    unsigned char *commit = read_file(commit_path, 1 << 16, &size);
    if (!commit) {
        return;
    }
    sha_hex(commit, size, out->commit);
    cloud_free(commit);
    out->number = best;
    out->found = 1;
}

/* ---------------------------------------------------------------- bundle */

static void put32(unsigned char *p, unsigned v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

static unsigned get32(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/* The revision as an FH2SAVE1 bundle (caller frees), or NULL. */
static unsigned char *bundle_revision(const CloudRevision *revision, size_t *size) {
    static wchar_t path[CLOUD_PATH], name[64];
    unsigned char *files[CLOUD_FILE_COUNT] = {0};
    size_t sizes[CLOUD_FILE_COUNT] = {0}, total = 20;
    unsigned char *bundle = 0;
    unsigned count = 0;
    *size = 0;
    for (unsigned i = 0; i < CLOUD_FILE_COUNT; ++i) {
        file_name_wide(cloud_files[i], name, ARRAY_COUNT(name));
        if (wjoin(path, revision->path, L"\\", name)) {
            files[i] = read_file(path, CLOUD_MAX_FILE, &sizes[i]);
        }
        if (files[i]) {
            total += 2 + text_length(cloud_files[i]) + 9 + sizes[i];
            ++count;
        } else if (i == 0) {
            goto done; /* no commit: not a complete revision */
        }
    }
    bundle = cloud_alloc(total);
    if (!bundle) {
        goto done;
    }
    memory_copy(bundle, "FH2SAVE1", 8);
    put32(bundle + 8, (unsigned)revision->number);
    put32(bundle + 12, (unsigned)(revision->number >> 32));
    put32(bundle + 16, count);
    size_t at = 20;
    for (unsigned i = 0; i < CLOUD_FILE_COUNT; ++i) {
        if (!files[i]) {
            continue;
        }
        size_t name_length = text_length(cloud_files[i]);
        bundle[at] = (unsigned char)name_length;
        bundle[at + 1] = 0;
        memory_copy(bundle + at + 2, cloud_files[i], name_length);
        at += 2 + name_length;
        unsigned char *header = bundle + at;
        at += 9;
        size_t packed = 0;
        int method = 0;
        if (sizes[i] >= 64 && cloud_workspace &&
            (packed = codec_lz_encode(files[i], sizes[i], bundle + at, sizes[i] - 1, cloud_workspace)) != 0) {
            method = 2;
        } else {
            memory_copy(bundle + at, files[i], sizes[i]);
            packed = sizes[i];
        }
        put32(header, (unsigned)sizes[i]);
        put32(header + 4, (unsigned)packed);
        header[8] = (unsigned char)method;
        at += packed;
    }
    *size = at;
done:
    for (unsigned i = 0; i < CLOUD_FILE_COUNT; ++i) {
        cloud_free(files[i]);
    }
    return bundle;
}

/* "<sha256> <size> <name>" lines of a commit: does `data` match the entry for `name`? */
static int commit_lists(const unsigned char *commit, size_t commit_size, const char *name, const unsigned char *data,
                        size_t size) {
    char want[65];
    sha_hex(data, size, want);
    size_t name_length = text_length(name);
    for (size_t at = 0; at < commit_size;) {
        size_t end = at;
        while (end < commit_size && commit[end] != '\n') {
            ++end;
        }
        /* 64 hex, space, decimal size, space, name */
        if (end - at > 66 && memory_equal(commit + at, want, 64) && commit[at + 64] == ' ') {
            size_t p = at + 65, listed = 0;
            while (p < end && commit[p] >= '0' && commit[p] <= '9') {
                listed = listed * 10 + (size_t)(commit[p++] - '0');
            }
            if (p < end && commit[p] == ' ' && end - p - 1 == name_length && memory_equal(commit + p + 1, name, name_length) &&
                listed == size) {
                return 1;
            }
        }
        at = end + 1;
    }
    return 0;
}

/* Unpacks a server bundle into `directory` (a new folder). Every data file must be listed in
   the bundle's commit with its SHA-256 and size. Returns the revision number, or 0. */
static unsigned long long install_bundle(const unsigned char *bundle, size_t size, const wchar_t *directory) {
    static wchar_t path[CLOUD_PATH], name[64];
    unsigned char *out[CLOUD_FILE_COUNT] = {0};
    size_t out_size[CLOUD_FILE_COUNT] = {0};
    unsigned long long revision = 0, result = 0;
    if (size < 20 || !memory_equal(bundle, "FH2SAVE1", 8)) {
        return 0;
    }
    revision = (unsigned long long)get32(bundle + 8) | (unsigned long long)get32(bundle + 12) << 32;
    unsigned count = get32(bundle + 16);
    size_t at = 20;
    if (!count || count > CLOUD_FILE_COUNT) {
        return 0;
    }
    for (unsigned k = 0; k < count; ++k) {
        if (at + 2 > size) {
            goto done;
        }
        size_t name_length = bundle[at] | (size_t)bundle[at + 1] << 8;
        at += 2;
        if (at + name_length + 9 > size) {
            goto done;
        }
        int index = -1;
        for (unsigned i = 0; i < CLOUD_FILE_COUNT; ++i) {
            if (text_length(cloud_files[i]) == name_length && memory_equal(cloud_files[i], bundle + at, name_length)) {
                index = (int)i;
            }
        }
        at += name_length;
        unsigned raw = get32(bundle + at), stored = get32(bundle + at + 4);
        int method = bundle[at + 8];
        at += 9;
        if (index < 0 || out[index] || raw > CLOUD_MAX_FILE || stored > size - at || method > 2 ||
            (method == 0 && stored != raw)) {
            goto done;
        }
        out[index] = cloud_alloc(raw ? raw : 1);
        if (!out[index]) {
            goto done;
        }
        if (method == 0) {
            memory_copy(out[index], bundle + at, raw);
        } else if (!(method == 1 ? codec_xpress_huffman_decode(bundle + at, stored, out[index], raw)
                                 : codec_lz_decode(bundle + at, stored, out[index], raw))) {
            goto done;
        }
        out_size[index] = raw;
        at += stored;
    }
    if (at != size || !out[0]) {
        goto done;
    }
    /* integrity: each data file is the one the commit names */
    for (unsigned i = 1; i < CLOUD_FILE_COUNT; ++i) {
        if (out[i] && !commit_lists(out[0], out_size[0], cloud_files[i] + 5, out[i], out_size[i])) {
            cloud_log("downloaded save does not match its commit: ", cloud_files[i]);
            goto done;
        }
    }
    if (!wjoin(path, directory, L"\\data", 0) || !cloud_create_directory(directory, 0) || !cloud_create_directory(path, 0)) {
        goto done;
    }
    for (unsigned i = 0; i < CLOUD_FILE_COUNT; ++i) {
        if (!out[i]) {
            continue;
        }
        file_name_wide(cloud_files[i], name, ARRAY_COUNT(name));
        if (!wjoin(path, directory, L"\\", name) || !write_file(path, out[i], out_size[i])) {
            goto done;
        }
    }
    result = revision ? revision : 1;
done:
    for (unsigned i = 0; i < CLOUD_FILE_COUNT; ++i) {
        cloud_free(out[i]);
    }
    return result;
}

/* ---------------------------------------------------------------- server */

static void cloud_set_synced(const char *commit) {
    for (int i = 0; i < 64; ++i) {
        cloud_synced[i] = (wchar_t)commit[i];
    }
    cloud_synced[64] = 0;
    if (cloud_write_profile) {
        cloud_write_profile(cloud_section, L"SaveCommit", cloud_synced, cloud_ini);
    }
}

static int synced_is(const char *commit) {
    for (int i = 0; i < 64; ++i) {
        if (cloud_synced[i] != (wchar_t)commit[i]) {
            return 0;
        }
    }
    return cloud_synced[64] == 0;
}

/* The server's save: 1 found (commit filled), 0 none, -1 unreachable or refused. */
static int server_info(char commit[65]) {
    static char reply[512];
    size_t got = 0;
    DWORD status = azimuth_net_request(L"GET", L"/fh2online/v1/save/info", cloud_headers, L"application/json", 0, 0,
                                       reply, sizeof(reply) - 1, &got);
    reply[got] = 0;
    if (status == 404) {
        return 0;
    }
    if (status != 200) {
        return -1;
    }
    const char *p = reply;
    while (*p && !(p[0] == '"' && memory_equal(p + 1, "commit\"", 7))) {
        ++p;
    }
    if (!*p) {
        return -1;
    }
    p += 8;
    while (*p == ':' || *p == ' ' || *p == '"') {
        ++p;
    }
    for (int i = 0; i < 64; ++i) {
        if (!p[i]) {
            return -1;
        }
        commit[i] = p[i];
    }
    commit[64] = 0;
    return 1;
}

static int upload(const CloudRevision *revision) {
    static wchar_t headers[300];
    size_t size;
    unsigned char *bundle = bundle_revision(revision, &size);
    if (!bundle) {
        cloud_log("could not read the save revision.", "");
        return 0;
    }
    if (size > CLOUD_MAX_BUNDLE) {
        cloud_free(bundle);
        cloud_log("save too big for the server.", "");
        return 0;
    }
    size_t n = wlen(cloud_headers);
    memory_copy(headers, cloud_headers, n * sizeof(wchar_t));
    static const wchar_t name[] = L"\r\nX-Fh2-Save-Commit: ";
    memory_copy(headers + n, name, sizeof(name));
    n += ARRAY_COUNT(name) - 1;
    for (int i = 0; i < 64; ++i) {
        headers[n++] = (wchar_t)revision->commit[i];
    }
    headers[n] = 0;
    char reply[256];
    size_t got = 0;
    DWORD status = azimuth_net_request(L"PUT", L"/fh2online/v1/save", headers, L"application/octet-stream", bundle,
                                       size, reply, sizeof(reply) - 1, &got);
    cloud_free(bundle);
    if (status != 200) {
        reply[got] = 0;
        cloud_log("upload refused: ", reply);
        return 0;
    }
    cloud_set_synced(revision->commit);
    cloud_log("save uploaded, commit ", revision->commit);
    return 1;
}

/* Downloads the server's save as revision `number`. */
static int download(unsigned long long number) {
    static wchar_t incoming[CLOUD_PATH], target[CLOUD_PATH], revisions[CLOUD_PATH];
    unsigned char *bundle = cloud_alloc(CLOUD_MAX_BUNDLE);
    size_t size = 0;
    int ok = 0;
    if (!bundle) {
        return 0;
    }
    DWORD status = azimuth_net_request(L"GET", L"/fh2online/v1/save", cloud_headers, L"application/json", 0, 0, bundle,
                                       CLOUD_MAX_BUNDLE, &size);
    wchar_t name[17], stamp[17];
    hex16(number, name);
    hex16(GetTickCount64(), stamp); /* a fresh folder even after a failed attempt */
    if (status == 200 && size < CLOUD_MAX_BUNDLE && wjoin(incoming, cloud_saves, L"\\fh2online-incoming-", stamp) &&
        wjoin(revisions, cloud_profile, L"\\revisions", 0) && wjoin(target, revisions, L"\\", name)) {
        cloud_create_directory(cloud_profile, 0);
        cloud_create_directory(revisions, 0);
        ok = install_bundle(bundle, size, incoming) && cloud_move(incoming, target, 0);
    }
    cloud_free(bundle);
    return ok;
}

/* Moves a local revision out of the way (kept in saves\fh2online-backup). */
static void back_up(const CloudRevision *revision) {
    static wchar_t backup[CLOUD_PATH], target[CLOUD_PATH];
    wchar_t name[17];
    hex16(revision->number, name);
    const wchar_t *profile = cloud_profile + wlen(cloud_profile);
    while (profile > cloud_profile && profile[-1] != L'\\') {
        --profile;
    }
    if (wjoin(backup, cloud_saves, L"\\fh2online-backup", 0) && wjoin(target, backup, L"\\", profile) &&
        wjoin(target, target, L"-", name)) {
        cloud_create_directory(backup, 0);
        if (cloud_move(revision->path, target, 0)) {
            cloud_log("local save kept in saves\\fh2online-backup (the server's was newer).", "");
        }
    }
}

/* ----------------------------------------------------------------- entry */

static DWORD WINAPI cloud_watch(void *unused) {
    static CloudRevision revision;
    (void)unused;
    for (;;) {
        cloud_sleep(CLOUD_POLL_MS);
        newest_revision(&revision, 0);
        if (!revision.found || synced_is(revision.commit)) {
            continue;
        }
        char server[65];
        int state = server_info(server);
        /* someone else's newer save is on the server (this PC was logged out elsewhere) */
        if (state < 0 || (state > 0 && !synced_is(server) && !memory_equal(server, revision.commit, 64))) {
            continue;
        }
        upload(&revision);
    }
}

static int cloud_resolve(void) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) {
        return 0;
    }
    cloud_find_first = (FindFirstFileWFn)(uintptr_t)GetProcAddress(kernel, "FindFirstFileW");
    cloud_find_next = (FindNextFileWFn)(uintptr_t)GetProcAddress(kernel, "FindNextFileW");
    cloud_find_close = (FindCloseFn)(uintptr_t)GetProcAddress(kernel, "FindClose");
    cloud_create_directory = (CreateDirectoryWFn)(uintptr_t)GetProcAddress(kernel, "CreateDirectoryW");
    cloud_move = (MoveFileExWFn)(uintptr_t)GetProcAddress(kernel, "MoveFileExW");
    cloud_write_profile = (WritePrivateProfileStringWFn)(uintptr_t)GetProcAddress(kernel, "WritePrivateProfileStringW");
    cloud_sleep = (SleepFn)(uintptr_t)GetProcAddress(kernel, "Sleep");
    if (!cloud_find_first || !cloud_find_next || !cloud_find_close || !cloud_create_directory || !cloud_move ||
        !cloud_write_profile || !cloud_sleep) {
        return 0;
    }
    return (cloud_workspace = cloud_alloc(CODEC_LZ_WORK_SIZE)) != 0;
}

/* Called on the account thread once the server accepted the token, before the game opens the
   save. `bin` is data\bin (the mod's folder), `xuid` the account's decimal XUID, `token` its
   device token, `ini`/`section` where SaveCommit is kept. Then watches for new revisions. */
void azimuth_cloud_start(const wchar_t *bin, const wchar_t *xuid, const wchar_t *token, const wchar_t *ini,
                         const wchar_t *section) {
    static wchar_t joined[CLOUD_PATH];
    static CloudRevision local;
    unsigned long long value = 0, highest = 0;
    if (!cloud_resolve()) {
        log_append("CLOUD: system functions missing; no cloud save.\r\n");
        return;
    }
    for (const wchar_t *p = xuid; *p; ++p) {
        if (*p < L'0' || *p > L'9') {
            return;
        }
        value = value * 10 + (unsigned)(*p - L'0');
    }
    /* User_<XUID in upper-case hex, no leading zeros> */
    wchar_t hex[17], *h = hex + 16;
    *h = 0;
    do {
        *--h = L"0123456789ABCDEF"[value & 15];
        value >>= 4;
    } while (value);
    if (!wjoin(joined, bin, L"\\..\\saves", 0) || !GetFullPathNameW(joined, CLOUD_PATH, cloud_saves, 0) ||
        !wjoin(cloud_profile, cloud_saves, L"\\player-1\\User_", h) || wlen(ini) >= CLOUD_PATH ||
        wlen(section) >= ARRAY_COUNT(cloud_section) || wlen(token) > 100) {
        return;
    }
    wjoin(cloud_ini, ini, 0, 0);
    wjoin(cloud_section, section, 0, 0);
    wjoin(cloud_headers, L"X-Fh2-Token: ", token, 0);
    GetPrivateProfileStringW(cloud_section, L"SaveCommit", L"", cloud_synced, ARRAY_COUNT(cloud_synced), cloud_ini);

    newest_revision(&local, &highest);
    char server[65];
    int state = server_info(server);
    if (state < 0) {
        log_append("CLOUD: server unreachable; the save syncs later.\r\n");
    } else if (state == 0) {
        if (local.found) {
            upload(&local);
        }
    } else if (local.found && memory_equal(server, local.commit, 64)) {
        if (!synced_is(server)) {
            cloud_set_synced(server);
        }
        log_append("CLOUD: save in sync with the server.\r\n");
    } else if (local.found && synced_is(server)) {
        upload(&local); /* played here since the last sync */
    } else {
        if (local.found && !synced_is(local.commit)) {
            back_up(&local); /* both changed: the server wins */
        }
        if (download(highest + 1)) {
            cloud_set_synced(server);
            cloud_log("save downloaded from the server, commit ", server);
        } else {
            log_append("CLOUD: could not download the server's save; playing the local one.\r\n");
        }
    }
    HANDLE thread = CreateThread(0, 0, cloud_watch, 0, 0, 0);
    if (thread) {
        CloseHandle(thread);
    }
}
