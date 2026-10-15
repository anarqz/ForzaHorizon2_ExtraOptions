/* FH2Online free roam: the world this player hosts (PLANO_FREEROAM.md, F1).

   HOSPEDAR MUNDO (INFINITY screen) starts dedicated_server.exe, the world's relay, from this
   folder (data\bin) with its server.cfg. The hook writes a server.cfg with the defaults the
   first time. The relay runs hidden, its console going to dedicated_server.log, inside a Job
   Object with KILL_ON_JOB_CLOSE: the job's only handle is this process's, so when the game
   ends (closed, crashed or killed) Windows closes it and the relay dies with it.

   Two ways in:
   - HOSPEDAR MUNDO (INFINITY screen): a public world, on the server's list and in the
     automatic FREE ROAM ONLINE join;
   - the game's own SESSÃO ONLINE PARTICULAR > PASSEIO LIVRE ONLINE PARTICULAR: the session it
     writes (constants.custom.gameServerPlatform, hopperId 2, no matchmaking) is seen on its way
     to the server (azimuth_world_on_request) and the world opens first, unlisted: only
     ENTRAR NO MUNDO reaches it.
   While the world is on, a thread announces it to the ForzaInfinity server every 15 s
   (POST /infinity/v1/worlds: the world's name, visibility, password, player limit, port,
   whether it is listed and this PC's LAN address); the server sends players to it
   (freeroam.rs). Leaving the online session (HORIZON SOLO: the association's DestroyAsync),
   "Close world", or the relay ending, stops it (DELETE /infinity/v1/worlds). */
#include "azimuth_host.h"
#include "logging.h"
#include "support.h"

#define WORLD_CFG_NAME L"server.cfg"
#define WORLD_EXE_NAME L"dedicated_server.exe"
#define WORLD_LOG_NAME L"dedicated_server.log"
#define WORLD_HEARTBEAT_MS 15000u
#define WORLD_CFG_MAX 16384u

static wchar_t world_directory[PATH_CAPACITY];
static HANDLE world_job, world_process;
/* world_generation: each world's heartbeat thread stops once it changes. */
static volatile LONG world_on, world_generation, world_listed;
static SRWLOCK world_lock = SRWLOCK_INIT;

/* What server.cfg says (UTF-8 values, as they go into the announcement). */
typedef struct {
    char name[96], message[256], visibility[16], password[96];
    unsigned max_players, port;
} WorldConfig;
static WorldConfig world_config;
static char world_lan[64];
static unsigned world_players;

void azimuth_world_set_directory(const wchar_t *directory) {
    size_t n = 0;
    while (directory && directory[n] && n + 1 < ARRAY_COUNT(world_directory)) {
        world_directory[n] = directory[n];
        ++n;
    }
    world_directory[n] = 0;
}

int azimuth_world_hosting(void) {
    return world_on != 0;
}

unsigned azimuth_world_players(void) {
    return world_players;
}

const wchar_t *azimuth_world_config_path(void) {
    static wchar_t path[PATH_CAPACITY];
    if (!path_join(path, world_directory, WORLD_CFG_NAME)) {
        path[0] = 0;
    }
    return path;
}

/* ------------------------------------------------------------ server.cfg */

/* The same file and keys as dedicated/server.cfg (UTF-8, CRLF). */
static const char world_default_cfg[] =
    "; ForzaInfinity: o mundo de free roam que você hospeda (HOSPEDAR MUNDO, no menu INFINITY).\r\n"
    "; Vale a cada vez que o mundo é aberto. Toda chave é opcional: sem ela, vale o padrão\r\n"
    "; mostrado aqui. Comentários começam com \";\" (no meio da linha, precisa de um espaço antes).\r\n"
    "\r\n"
    "[World]\r\n"
    "; Nome do mundo na lista (vazio = o seu perfil).\r\n"
    "Name=\r\n"
    "; Mensagem para quem entra.\r\n"
    "Message=\r\n"
    "; Quem acha o mundo: public (todos), friends (só amigos) ou off (ninguém; mundo particular).\r\n"
    "Visibility=public\r\n"
    "; Senha (vazio = sem senha). Um mundo com senha não aparece na lista: só se entra\r\n"
    "; pelas ações do jogador (ENTRAR NO MUNDO), digitando a senha.\r\n"
    "Password=\r\n"
    "; Máximo de jogadores ao mesmo tempo, com você, de 1 a 12 (12 é o limite do jogo).\r\n"
    "MaxPlayers=8\r\n"
    "\r\n"
    "[Network]\r\n"
    "; Porta UDP do mundo. Para jogar pela internet, ela precisa estar acessível\r\n"
    "; (UPnP no roteador, IPv6 ou a porta encaminhada para este PC).\r\n"
    "Port=5101\r\n"
    "\r\n"
    "[Access]\r\n"
    "; Perfis que podem entrar, separados por vírgula (vazio = todos).\r\n"
    "Allow=\r\n"
    "; Perfis que não podem entrar, separados por vírgula.\r\n"
    "Ban=\r\n";

static int world_write_default_cfg(const wchar_t *path) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD written = 0;
    int ok = WriteFile(file, world_default_cfg, (DWORD)(sizeof(world_default_cfg) - 1), &written, 0) &&
             written == sizeof(world_default_cfg) - 1;
    CloseHandle(file);
    if (ok) {
        log_append("WORLD: server.cfg written with the defaults.\r\n");
    }
    return ok;
}

static void world_copy_value(char *out, size_t capacity, const char *value, size_t length) {
    size_t n = length < capacity - 1 ? length : capacity - 1;
    for (size_t i = 0; i < n; ++i) {
        out[i] = value[i];
    }
    out[n] = 0;
}

static unsigned world_number(const char *value, size_t length, unsigned fallback) {
    unsigned n = 0;
    if (!length) {
        return fallback;
    }
    for (size_t i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9' || n > 100000) {
            return fallback;
        }
        n = n * 10 + (unsigned)(value[i] - '0');
    }
    return n;
}

static int world_key_is(const char *key, size_t length, const char *name) {
    size_t n = text_length(name);
    if (n != length) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        char a = key[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* A small INI reader: [Section], Key=Value, ";" comments (mid-line after a space or tab). */
static int world_read_cfg(WorldConfig *config) {
    static char text[WORLD_CFG_MAX];
    world_copy_value(config->name, sizeof(config->name), "", 0);
    world_copy_value(config->message, sizeof(config->message), "", 0);
    world_copy_value(config->visibility, sizeof(config->visibility), "public", 6);
    world_copy_value(config->password, sizeof(config->password), "", 0);
    config->max_players = 8;
    config->port = 5101;
    HANDLE file = CreateFileW(azimuth_world_config_path(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD size = 0;
    int ok = ReadFile(file, text, sizeof(text) - 1, &size, 0);
    CloseHandle(file);
    if (!ok) {
        return 0;
    }
    text[size] = 0;
    char section[16];
    section[0] = 0;
    size_t at = 0;
    if (size >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf) {
        at = 3; /* UTF-8 BOM */
    }
    while (at < size) {
        size_t start = at, end;
        while (at < size && text[at] != '\n') {
            ++at;
        }
        end = at++;
        if (end > start && text[end - 1] == '\r') {
            --end;
        }
        while (start < end && (text[start] == ' ' || text[start] == '\t')) {
            ++start;
        }
        if (start >= end || text[start] == ';' || text[start] == '#') {
            continue;
        }
        if (text[start] == '[') {
            size_t close = start + 1;
            while (close < end && text[close] != ']') {
                ++close;
            }
            world_copy_value(section, sizeof(section), text + start + 1, close - start - 1);
            continue;
        }
        size_t equal = start;
        while (equal < end && text[equal] != '=') {
            ++equal;
        }
        if (equal >= end) {
            continue;
        }
        size_t key_end = equal;
        while (key_end > start && (text[key_end - 1] == ' ' || text[key_end - 1] == '\t')) {
            --key_end;
        }
        size_t value = equal + 1, value_end = end;
        for (size_t i = value; i + 1 < end; ++i) {
            if ((text[i] == ' ' || text[i] == '\t') && text[i + 1] == ';') {
                value_end = i;
                break;
            }
        }
        while (value < value_end && (text[value] == ' ' || text[value] == '\t')) {
            ++value;
        }
        while (value_end > value && (text[value_end - 1] == ' ' || text[value_end - 1] == '\t')) {
            --value_end;
        }
        const char *key = text + start, *v = text + value;
        size_t key_length = key_end - start, length = value_end - value;
        if (world_key_is(section, text_length(section), "World")) {
            if (world_key_is(key, key_length, "Name")) {
                world_copy_value(config->name, sizeof(config->name), v, length);
            } else if (world_key_is(key, key_length, "Message")) {
                world_copy_value(config->message, sizeof(config->message), v, length);
            } else if (world_key_is(key, key_length, "Visibility")) {
                world_copy_value(config->visibility, sizeof(config->visibility), v, length);
            } else if (world_key_is(key, key_length, "Password")) {
                world_copy_value(config->password, sizeof(config->password), v, length);
            } else if (world_key_is(key, key_length, "MaxPlayers")) {
                config->max_players = world_number(v, length, 8);
            }
        } else if (world_key_is(section, text_length(section), "Network") &&
                   world_key_is(key, key_length, "Port")) {
            config->port = world_number(v, length, 5101);
        }
    }
    if (config->max_players < 1 || config->max_players > 12) {
        config->max_players = 8;
    }
    if (config->port < 1 || config->port > 65535 || config->port == 5100) {
        config->port = 5101;
    }
    if (!world_key_is(config->visibility, text_length(config->visibility), "public") &&
        !world_key_is(config->visibility, text_length(config->visibility), "friends") &&
        !world_key_is(config->visibility, text_length(config->visibility), "off")) {
        world_copy_value(config->visibility, sizeof(config->visibility), "public", 6);
    }
    return 1;
}

/* ------------------------------------------------------------- LAN address */

typedef int(WINAPI *WorldStartupFn)(WORD, void *);
typedef UINT_PTR(WINAPI *WorldSocketFn)(int, int, int);
typedef int(WINAPI *WorldConnectFn)(UINT_PTR, const void *, int);
typedef int(WINAPI *WorldNameFn)(UINT_PTR, void *, int *);
typedef int(WINAPI *WorldCloseFn)(UINT_PTR);

/* This PC's address on the network the default route leaves by: a UDP socket "connected" to
   a public address sends nothing, and its local name is that address. */
static void world_find_lan(void) {
    static unsigned char wsa[512];
    world_lan[0] = 0;
    HMODULE ws2 = LoadLibraryExW(L"ws2_32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ws2) {
        return;
    }
    WorldStartupFn startup = (WorldStartupFn)(uintptr_t)GetProcAddress(ws2, "WSAStartup");
    WorldSocketFn make = (WorldSocketFn)(uintptr_t)GetProcAddress(ws2, "socket");
    WorldConnectFn join = (WorldConnectFn)(uintptr_t)GetProcAddress(ws2, "connect");
    WorldNameFn name = (WorldNameFn)(uintptr_t)GetProcAddress(ws2, "getsockname");
    WorldCloseFn close = (WorldCloseFn)(uintptr_t)GetProcAddress(ws2, "closesocket");
    if (!startup || !make || !join || !name || !close || startup(0x0202, wsa)) {
        return;
    }
    UINT_PTR s = make(2 /* AF_INET */, 2 /* SOCK_DGRAM */, 17 /* IPPROTO_UDP */);
    if (s == (UINT_PTR)~(UINT_PTR)0) {
        return;
    }
    unsigned char to[16] = {2, 0, 0, 53, 8, 8, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0}, mine[16];
    int length = sizeof(mine);
    if (!join(s, to, sizeof(to)) && !name(s, mine, &length) && mine[4]) {
        char *o = world_lan;
        for (int i = 0; i < 4; ++i) {
            unsigned v = mine[4 + i];
            if (v >= 100) *o++ = (char)('0' + v / 100);
            if (v >= 10) *o++ = (char)('0' + (v / 10) % 10);
            *o++ = (char)('0' + v % 10);
            if (i < 3) *o++ = '.';
        }
        *o = 0;
    }
    close(s);
}

/* ------------------------------------------------------------ announcement */

typedef struct {
    char *data;
    size_t size, capacity;
} WorldText;

static void world_add(WorldText *t, const char *text) {
    while (text && *text && t->size + 1 < t->capacity) {
        t->data[t->size++] = *text++;
    }
    t->data[t->size] = 0;
}

static void world_add_json(WorldText *t, const char *text) {
    static const char hex[] = "0123456789abcdef";
    world_add(t, "\"");
    for (; text && *text && t->size + 8 < t->capacity; ++text) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            t->data[t->size++] = '\\';
            t->data[t->size++] = (char)c;
        } else if (c < 0x20) {
            const char esc[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0};
            world_add(t, esc);
        } else {
            t->data[t->size++] = (char)c;
        }
    }
    t->data[t->size] = 0;
    world_add(t, "\"");
}

static void world_add_number(WorldText *t, unsigned value) {
    char digits[12], text[12];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    for (unsigned i = 0; i < n; ++i) {
        text[i] = digits[n - 1 - i];
    }
    text[n] = 0;
    world_add(t, text);
}

static const wchar_t *world_token_header(void) {
    static wchar_t header[160];
    const wchar_t *token = azimuth_account_token();
    size_t n = 0;
    static const wchar_t prefix[] = L"X-Fh2-Token: ";
    if (!token) {
        return 0;
    }
    for (size_t i = 0; prefix[i]; ++i) header[n++] = prefix[i];
    for (size_t i = 0; token[i] && n + 1 < ARRAY_COUNT(header); ++i) header[n++] = token[i];
    header[n] = 0;
    return header;
}

/* POST /infinity/v1/worlds (start and heartbeat). Returns the HTTP status. */
static DWORD world_announce(void) {
    static char body[1024], reply[1024];
    WorldText t = {body, 0, sizeof(body)};
    body[0] = 0;
    AcquireSRWLockExclusive(&world_lock);
    world_add(&t, "{\"name\":");
    world_add_json(&t, world_config.name);
    world_add(&t, ",\"message\":");
    world_add_json(&t, world_config.message);
    world_add(&t, ",\"visibility\":");
    world_add_json(&t, world_config.visibility);
    world_add(&t, ",\"password\":");
    world_add_json(&t, world_config.password);
    world_add(&t, ",\"maxPlayers\":");
    world_add_number(&t, world_config.max_players);
    world_add(&t, ",\"port\":");
    world_add_number(&t, world_config.port);
    world_add(&t, ",\"lan\":[");
    if (world_lan[0]) {
        world_add_json(&t, world_lan);
    }
    world_add(&t, world_listed ? "],\"listed\":true,\"version\":" : "],\"listed\":false,\"version\":");
    world_add_json(&t, MOD_VERSION);
    world_add(&t, "}");
    ReleaseSRWLockExclusive(&world_lock);
    DWORD status = azimuth_net_call(L"POST", L"/infinity/v1/worlds", world_token_header(), body, reply, sizeof(reply));
    if (status == 200) {
        /* "players": n */
        const char *p = text_find(reply, text_length(reply), "\"players\":");
        if (p) {
            unsigned n = 0;
            for (p += 10; *p == ' '; ++p) {
            }
            for (; *p >= '0' && *p <= '9'; ++p) n = n * 10 + (unsigned)(*p - '0');
            world_players = n;
        }
    }
    return status;
}

static void world_log_status(const char *what, DWORD value) {
    char text[96];
    WorldText t = {text, 0, sizeof(text)};
    text[0] = 0;
    world_add(&t, "WORLD: ");
    world_add(&t, what);
    world_add_number(&t, (unsigned)value);
    world_add(&t, ".\r\n");
    log_append(text);
}

static void world_end(const char *why) {
    if (!InterlockedExchange(&world_on, 0)) {
        return;
    }
    InterlockedIncrement(&world_generation);
    if (world_job) {
        TerminateJobObject(world_job, 0);
    }
    if (world_process) {
        CloseHandle(world_process);
        world_process = 0;
    }
    static char reply[256];
    azimuth_net_call(L"DELETE", L"/infinity/v1/worlds", world_token_header(), 0, reply, sizeof(reply));
    log_append("WORLD: ended (");
    log_append(why);
    log_append(").\r\n");
}

static DWORD WINAPI world_heartbeat(void *argument) {
    LONG generation = (LONG)(intptr_t)argument;
    for (;;) {
        for (unsigned waited = 0; waited < WORLD_HEARTBEAT_MS && world_generation == generation; waited += 500) {
            Sleep(500);
        }
        if (!world_on || world_generation != generation) {
            break;
        }
        DWORD code = 0;
        if (world_process && GetExitCodeProcess(world_process, &code) && code != STILL_ACTIVE) {
            world_log_status("the relay stopped, exit code ", code);
            world_end("the relay stopped");
            break;
        }
        DWORD status = world_announce();
        if (status != 200) {
            world_log_status("heartbeat answered ", status);
        }
    }
    return 0;
}

/* --------------------------------------------------------------- start/stop */

static int world_job_ready(void) {
    static JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    if (world_job) {
        return 1;
    }
    world_job = CreateJobObjectW(0, 0);
    if (!world_job) {
        return 0;
    }
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(world_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(world_job);
        world_job = 0;
        return 0;
    }
    return 1;
}

/* 1 started, 0 failed (`why` says it in English, for azimuth_tr). */
static int world_open(int listed, const wchar_t **why) {
    static wchar_t exe[PATH_CAPACITY], log_path[PATH_CAPACITY], command[PATH_CAPACITY + 64];
    static STARTUPINFOW startup;
    static PROCESS_INFORMATION process;
    static SECURITY_ATTRIBUTES inherit;
    *why = L"";
    if (world_on) {
        return 1;
    }
    if (!azimuth_account_token()) {
        *why = L"Hosting a world needs your online profile.";
        return 0;
    }
    if (!path_join(exe, world_directory, WORLD_EXE_NAME) || GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        *why = L"dedicated_server.exe is missing from the game's data\\bin folder. Reinstall ForzaInfinity.";
        return 0;
    }
    const wchar_t *cfg = azimuth_world_config_path();
    if (GetFileAttributesW(cfg) == INVALID_FILE_ATTRIBUTES) {
        world_write_default_cfg(cfg);
    }
    AcquireSRWLockExclusive(&world_lock);
    world_read_cfg(&world_config);
    ReleaseSRWLockExclusive(&world_lock);
    world_find_lan();
    if (!world_job_ready()) {
        *why = L"The world could not be started (job object).";
        return 0;
    }
    /* "<exe>" --config "<cfg>" */
    size_t n = 0;
    command[n++] = L'"';
    for (size_t i = 0; exe[i] && n + 2 < ARRAY_COUNT(command); ++i) command[n++] = exe[i];
    static const wchar_t option[] = L"\" --config \"";
    for (size_t i = 0; option[i]; ++i) command[n++] = option[i];
    for (size_t i = 0; cfg[i] && n + 2 < ARRAY_COUNT(command); ++i) command[n++] = cfg[i];
    command[n++] = L'"';
    command[n] = 0;
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE log_file = INVALID_HANDLE_VALUE;
    if (path_join(log_path, world_directory, WORLD_LOG_NAME)) {
        log_file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, 0);
    }
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (log_file != INVALID_HANDLE_VALUE) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = 0;
        startup.hStdOutput = log_file;
        startup.hStdError = log_file;
    }
    BOOL created = CreateProcessW(exe, command, 0, 0, log_file != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                  0, world_directory, &startup, &process);
    if (log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(log_file);
    }
    if (!created) {
        world_log_status("CreateProcess failed, error ", GetLastError());
        *why = L"The world could not be started (dedicated_server.exe did not run).";
        return 0;
    }
    if (!AssignProcessToJobObject(world_job, process.hProcess)) {
        /* Without the job it would outlive the game: do not run it. */
        world_log_status("AssignProcessToJobObject failed, error ", GetLastError());
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        *why = L"The world could not be started (job object).";
        return 0;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    world_process = process.hProcess;
    world_players = 1;
    InterlockedExchange(&world_listed, listed);
    InterlockedExchange(&world_on, 1);
    log_append("WORLD: dedicated_server.exe started (killed with the game).\r\n");
    DWORD status = world_announce();
    if (status != 200) {
        world_log_status("announcement answered ", status);
        world_end("the server refused the announcement");
        *why = status == 404 ? L"This server does not take hosted worlds yet."
                             : L"The server did not take the world. Try again in a moment.";
        return 0;
    }
    HANDLE thread = CreateThread(0, 0, world_heartbeat, (void *)(intptr_t)world_generation, 0, 0);
    if (thread) {
        CloseHandle(thread);
    }
    return 1;
}

int azimuth_world_start(const wchar_t **why) {
    return world_open(1, why);
}

int azimuth_world_listed(void) {
    return world_listed != 0;
}

void azimuth_world_stop(void) {
    world_end("Close world");
}

/* The association with the world was destroyed: the game left the online session (HORIZON
   SOLO), or a join failed. The world this PC hosts goes with it. */
void azimuth_world_left(void) {
    if (world_on) {
        world_end("left the online session");
    }
}

static int world_has(const unsigned char *data, size_t size, const char *needle) {
    return data && text_find((const char *)data, size, needle) != 0;
}

static int world_wide_has(const wchar_t *text, const wchar_t *needle) {
    for (; text && *text; ++text) {
        size_t i = 0;
        while (needle[i] && text[i] == needle[i]) {
            ++i;
        }
        if (!needle[i]) {
            return 1;
        }
    }
    return 0;
}

/* Every request of the game, before it is forwarded (azimuth_net.c, a worker thread): a
   private free roam session being written opens this PC's world first, so the server's
   answer already points the game at it. */
void azimuth_world_on_request(const wchar_t *method, const wchar_t *url, const unsigned char *body, size_t size) {
    if (!world_directory[0] || world_on || !method || method[0] != L'P' || method[1] != L'U' ||
        !world_wide_has(url, L"sessiondirectory.xboxlive.com") || !world_has(body, size, "\"gameServerPlatform\"") ||
        !world_has(body, size, "\"hopperId\":2") || world_has(body, size, "\"matchmaking\"")) {
        return;
    }
    const wchar_t *why = L"";
    log_append(world_open(0, &why) ? "WORLD: private free roam session: this PC hosts it (unlisted).\r\n"
                                   : "WORLD: private free roam session, but the world did not open.\r\n");
}

/* What server.cfg says, for the popup. */
void azimuth_world_describe(char *visibility, size_t visibility_capacity, int *has_password,
                            unsigned *max_players, unsigned *port) {
    AcquireSRWLockExclusive(&world_lock);
    world_copy_value(visibility, visibility_capacity, world_config.visibility, text_length(world_config.visibility));
    *has_password = world_config.password[0] != 0;
    *max_players = world_config.max_players;
    *port = world_config.port;
    ReleaseSRWLockExclusive(&world_lock);
}
