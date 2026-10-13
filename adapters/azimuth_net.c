/* FH2Online network layer for the Azimuth adapter.

   Azimuth 0.9.7 binds the game's (and its XSAPI DLL's) CoCreateInstance to one
   host function at RVA 0x6dbe0. For CLSID_FreeThreadedXMLHTTP60 it returns
   azimuth::services::OfflineRequest, an IXMLHTTPRequest2 that always fails with
   INET_E_RESOURCE_NOT_FOUND, so FH2 never reaches the network. We detour that
   function and return NetRequest instead:

     Capture  log every request (method, URL, headers, body) to
              FH2_Network.jsonl, then fail exactly like Azimuth (offline).
     Proxy    also forward it over plain HTTP to a test server and hand the
              server's status, headers and body back to the game.

   The audio device enumerator keeps Azimuth's own factory (RVA 0x144400);
   every other class is refused, as Azimuth would. Freestanding like the rest
   of the mod: Kernel32 only, WinHTTP and OLE32 are loaded on first use. */
#include "azimuth_host.h"
#include "logging.h"
#include "sha256.h"
#include "support.h"
#include <winhttp.h>

#define NET_CLSCTX_INPROC_SERVER 0x1u
#define NET_HOOK_RVA 0x6dbe0u
#define NET_HOOK_HASHED_BYTES 0x20u
#define NET_HOOK_JUMP_BYTES 14u
#define NET_MMDEVICE_FACTORY_RVA 0x144400u
#define NET_OFFLINE_ERROR ((HRESULT)0x800C0005L) /* INET_E_RESOURCE_NOT_FOUND */
#define NET_HEADER_NOT_FOUND ((HRESULT)0x80072F76L)
#define NET_UNEXPECTED ((HRESULT)0x8000FFFFL)
#define NET_MAX_BODY (64u << 20)
#define NET_LOG_NAME L"FH2_Network.jsonl"
#define NET_XHR_PROP_TIMEOUT 2

static const unsigned char net_clsid_xhr[16] = {0x09, 0x6a, 0xd9, 0x88, 0x92, 0xf1, 0xd4, 0x11,
                                                0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5};
static const unsigned char net_clsid_mmdevice[16] = {0x95, 0x03, 0xde, 0xbc, 0x2f, 0xe5,
                                                     0x7c, 0x46, 0x8e, 0x3d, 0xc4, 0x57,
                                                     0x92, 0x91, 0x69, 0x2e};
static const unsigned char net_iid_unknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
static const unsigned char net_iid_xhr2[16] = {0xc0, 0x7d, 0xd3, 0xe5, 0x2a, 0x55, 0x52, 0x4d,
                                               0x9c, 0xc0, 0xa1, 0x4d, 0x54, 0x6f, 0xbd, 0x04};
static const unsigned char net_iid_agile[16] = {0x94, 0x2b, 0xea, 0x94, 0xcc, 0xe9, 0xe0, 0x49,
                                                0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90};
static const unsigned char net_iid_sequential_stream[16] = {0x30, 0x3a, 0x73, 0x0c, 0x1c, 0x2a,
                                                            0xce, 0x11, 0xad, 0xe5, 0x00, 0xaa,
                                                            0x00, 0x44, 0x77, 0x3d};
/* SHA-256 of Azimuth 0.9.7's first 0x20 bytes at RVA 0x6dbe0. */
static const unsigned char net_hook_original_hash[32] = {
    0xbd, 0xac, 0x04, 0x38, 0x9c, 0x8f, 0x5f, 0x0c, 0x9e, 0xe6, 0xc8, 0x04, 0xc9, 0xb5, 0x62, 0x9a,
    0xb0, 0xd4, 0xd7, 0xd4, 0x32, 0xff, 0xc2, 0xed, 0x04, 0x2d, 0x18, 0xdc, 0x79, 0x83, 0xcb, 0xec};

enum { NET_MODE_OFF, NET_MODE_CAPTURE, NET_MODE_PROXY };

static int net_mode;
static wchar_t net_server_host[256];
static INTERNET_PORT net_server_port = 8081;
static wchar_t net_log_path[PATH_CAPACITY];
static unsigned char *net_azimuth_base;
static volatile LONG net_next_id;
static SRWLOCK net_log_lock = SRWLOCK_INIT;
static SRWLOCK net_api_lock = SRWLOCK_INIT;

/* ---------------------------------------------------------------- buffers */

typedef struct {
    unsigned char *data;
    size_t size, capacity;
} Buffer;

static void *net_alloc(size_t size) {
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void net_free(void *pointer) {
    if (pointer) {
        HeapFree(GetProcessHeap(), 0, pointer);
    }
}

/* Keeps two zero bytes after the content so text buffers stay terminated. */
static int buffer_reserve(Buffer *buffer, size_t extra) {
    if (extra > (size_t)-1 - buffer->size - 2) {
        return 0;
    }
    size_t need = buffer->size + extra + 2, capacity = buffer->capacity ? buffer->capacity : 256;
    if (need <= buffer->capacity) {
        return 1;
    }
    while (capacity < need) {
        if (capacity > (size_t)-1 / 2) {
            return 0;
        }
        capacity *= 2;
    }
    unsigned char *data = net_alloc(capacity);
    if (!data) {
        return 0;
    }
    if (buffer->size) {
        memory_copy(data, buffer->data, buffer->size);
    }
    net_free(buffer->data);
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int buffer_append(Buffer *buffer, const void *source, size_t size) {
    if (!buffer_reserve(buffer, size)) {
        return 0;
    }
    if (size) {
        memory_copy(buffer->data + buffer->size, source, size);
    }
    buffer->size += size;
    buffer->data[buffer->size] = 0;
    buffer->data[buffer->size + 1] = 0;
    return 1;
}

static int buffer_append_text(Buffer *buffer, const char *text) {
    return buffer_append(buffer, text, text_length(text));
}

static void buffer_free(Buffer *buffer) {
    net_free(buffer->data);
    buffer->data = 0;
    buffer->size = buffer->capacity = 0;
}

static size_t wide_length(const wchar_t *text) {
    size_t length = 0;
    while (text && text[length]) {
        ++length;
    }
    return length;
}

static int buffer_append_wide(Buffer *buffer, const wchar_t *text) {
    return buffer_append(buffer, text, wide_length(text) * sizeof(wchar_t));
}

static const wchar_t *buffer_wide(const Buffer *buffer) {
    return buffer->data ? (const wchar_t *)buffer->data : L"";
}

static wchar_t wide_lower(wchar_t c) {
    return c >= L'A' && c <= L'Z' ? (wchar_t)(c + (L'a' - L'A')) : c;
}

static int wide_starts_ignore_case(const wchar_t *text, size_t length, const wchar_t *prefix) {
    size_t prefix_length = wide_length(prefix);
    if (prefix_length > length) {
        return 0;
    }
    for (size_t i = 0; i < prefix_length; ++i) {
        if (wide_lower(text[i]) != wide_lower(prefix[i])) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------- lazily loaded APIs */

typedef void *(WINAPI *CoTaskMemAllocFn)(SIZE_T);
typedef HINTERNET(WINAPI *WinHttpOpenFn)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET(WINAPI *WinHttpConnectFn)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI *WinHttpOpenRequestFn)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR,
                                                LPCWSTR *, DWORD);
typedef BOOL(WINAPI *WinHttpAddRequestHeadersFn)(HINTERNET, LPCWSTR, DWORD, DWORD);
typedef BOOL(WINAPI *WinHttpSendRequestFn)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD,
                                           DWORD_PTR);
typedef BOOL(WINAPI *WinHttpReceiveResponseFn)(HINTERNET, LPVOID);
typedef BOOL(WINAPI *WinHttpQueryHeadersFn)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *WinHttpQueryDataAvailableFn)(HINTERNET, LPDWORD);
typedef BOOL(WINAPI *WinHttpReadDataFn)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL(WINAPI *WinHttpCloseHandleFn)(HINTERNET);
typedef BOOL(WINAPI *WinHttpSetTimeoutsFn)(HINTERNET, int, int, int, int);

static int net_ole_state, net_http_state; /* 0 untried, 1 ready, -1 unavailable */
static CoTaskMemAllocFn net_co_task_mem_alloc;
static WinHttpOpenFn http_open;
static WinHttpConnectFn http_connect;
static WinHttpOpenRequestFn http_open_request;
static WinHttpAddRequestHeadersFn http_add_headers;
static WinHttpSendRequestFn http_send;
static WinHttpReceiveResponseFn http_receive;
static WinHttpQueryHeadersFn http_query_headers;
static WinHttpQueryDataAvailableFn http_query_available;
static WinHttpReadDataFn http_read;
static WinHttpCloseHandleFn http_close;
static WinHttpSetTimeoutsFn http_set_timeouts;

/* The game frees response-header strings with OLE32's CoTaskMemFree, which
   Azimuth forwards to the real ole32.dll; allocate from the same place. */
static void *net_task_alloc(size_t size) {
    AcquireSRWLockExclusive(&net_api_lock);
    if (!net_ole_state) {
        HMODULE ole = LoadLibraryExW(L"ole32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
        net_co_task_mem_alloc =
            ole ? (CoTaskMemAllocFn)(uintptr_t)GetProcAddress(ole, "CoTaskMemAlloc") : 0;
        net_ole_state = net_co_task_mem_alloc ? 1 : -1;
    }
    ReleaseSRWLockExclusive(&net_api_lock);
    return net_ole_state > 0 ? net_co_task_mem_alloc(size) : 0;
}

static int net_load_winhttp(void) {
    AcquireSRWLockExclusive(&net_api_lock);
    if (!net_http_state) {
        HMODULE http = LoadLibraryExW(L"winhttp.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (http) {
            http_open = (WinHttpOpenFn)(uintptr_t)GetProcAddress(http, "WinHttpOpen");
            http_connect = (WinHttpConnectFn)(uintptr_t)GetProcAddress(http, "WinHttpConnect");
            http_open_request =
                (WinHttpOpenRequestFn)(uintptr_t)GetProcAddress(http, "WinHttpOpenRequest");
            http_add_headers = (WinHttpAddRequestHeadersFn)(uintptr_t)GetProcAddress(
                http, "WinHttpAddRequestHeaders");
            http_send = (WinHttpSendRequestFn)(uintptr_t)GetProcAddress(http, "WinHttpSendRequest");
            http_receive = (WinHttpReceiveResponseFn)(uintptr_t)GetProcAddress(
                http, "WinHttpReceiveResponse");
            http_query_headers =
                (WinHttpQueryHeadersFn)(uintptr_t)GetProcAddress(http, "WinHttpQueryHeaders");
            http_query_available = (WinHttpQueryDataAvailableFn)(uintptr_t)GetProcAddress(
                http, "WinHttpQueryDataAvailable");
            http_read = (WinHttpReadDataFn)(uintptr_t)GetProcAddress(http, "WinHttpReadData");
            http_close = (WinHttpCloseHandleFn)(uintptr_t)GetProcAddress(http, "WinHttpCloseHandle");
            http_set_timeouts =
                (WinHttpSetTimeoutsFn)(uintptr_t)GetProcAddress(http, "WinHttpSetTimeouts");
        }
        net_http_state = http && http_open && http_connect && http_open_request &&
                                 http_add_headers && http_send && http_receive &&
                                 http_query_headers && http_query_available && http_read &&
                                 http_close && http_set_timeouts
                             ? 1
                             : -1;
    }
    ReleaseSRWLockExclusive(&net_api_lock);
    return net_http_state > 0;
}

/* ------------------------------------------------------------ JSON capture */

static int buffer_append_json_utf8(Buffer *out, const unsigned char *text, size_t size) {
    static const char hex[] = "0123456789abcdef";
    if (!buffer_append(out, "\"", 1)) {
        return 0;
    }
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = text[i];
        char escaped[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0};
        int ok;
        if (c == '"' || c == '\\') {
            char pair[2] = {'\\', (char)c};
            ok = buffer_append(out, pair, 2);
        } else if (c < 0x20) {
            ok = buffer_append(out, escaped, 6);
        } else {
            ok = buffer_append(out, &c, 1);
        }
        if (!ok) {
            return 0;
        }
    }
    return buffer_append(out, "\"", 1);
}

static int buffer_append_json_wide(Buffer *out, const wchar_t *text, size_t length) {
    if (!length) {
        return buffer_append_text(out, "\"\"");
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, 0, 0, 0, 0);
    if (bytes <= 0) {
        return buffer_append_text(out, "\"\"");
    }
    unsigned char *utf8 = net_alloc((size_t)bytes);
    if (!utf8) {
        return 0;
    }
    WideCharToMultiByte(CP_UTF8, 0, text, (int)length, (char *)utf8, bytes, 0, 0);
    int ok = buffer_append_json_utf8(out, utf8, (size_t)bytes);
    net_free(utf8);
    return ok;
}

static int buffer_append_base64(Buffer *out, const unsigned char *data, size_t size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!buffer_append(out, "\"", 1)) {
        return 0;
    }
    for (size_t i = 0; i < size; i += 3) {
        unsigned value = (unsigned)data[i] << 16;
        size_t remaining = size - i;
        if (remaining > 1) {
            value |= (unsigned)data[i + 1] << 8;
        }
        if (remaining > 2) {
            value |= data[i + 2];
        }
        char quad[4] = {alphabet[(value >> 18) & 63], alphabet[(value >> 12) & 63],
                        remaining > 1 ? alphabet[(value >> 6) & 63] : '=',
                        remaining > 2 ? alphabet[value & 63] : '='};
        if (!buffer_append(out, quad, 4)) {
            return 0;
        }
    }
    return buffer_append(out, "\"", 1);
}

static int buffer_append_number(Buffer *out, unsigned long long value) {
    char digits[24];
    unsigned count = 0;
    do {
        digits[sizeof(digits) - 1 - count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    return buffer_append(out, digits + sizeof(digits) - count, count);
}

static int buffer_append_hresult(Buffer *out, HRESULT value) {
    static const char hex[] = "0123456789ABCDEF";
    char text[13] = {'"', '0', 'x'};
    unsigned bits = (unsigned)value;
    for (unsigned i = 0; i < 8; ++i) {
        text[3 + i] = hex[(bits >> (28 - 4 * i)) & 15];
    }
    text[11] = '"';
    return buffer_append(out, text, 12);
}

/* ------------------------------------------------------- response streams */

typedef struct SequentialStream SequentialStream;
typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(SequentialStream *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(SequentialStream *);
    ULONG(STDMETHODCALLTYPE *Release)(SequentialStream *);
    HRESULT(STDMETHODCALLTYPE *Read)(SequentialStream *, void *, ULONG, ULONG *);
    HRESULT(STDMETHODCALLTYPE *Write)(SequentialStream *, const void *, ULONG, ULONG *);
} SequentialStreamVtbl;
struct SequentialStream {
    const SequentialStreamVtbl *lpVtbl;
};

typedef struct {
    const SequentialStreamVtbl *lpVtbl;
    volatile LONG references;
    Buffer data;
    size_t position;
} MemoryStream;

static HRESULT STDMETHODCALLTYPE memory_stream_query(SequentialStream *self, REFIID iid,
                                                     void **out) {
    if (!out) {
        return E_POINTER;
    }
    *out = 0;
    if (memory_equal(iid, net_iid_unknown, 16) || memory_equal(iid, net_iid_sequential_stream, 16)) {
        *out = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    log_append("NETWORK: response stream asked for an interface other than ISequentialStream.\r\n");
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE memory_stream_add_ref(SequentialStream *self) {
    return (ULONG)InterlockedIncrement(&((MemoryStream *)self)->references);
}

static ULONG STDMETHODCALLTYPE memory_stream_release(SequentialStream *self) {
    MemoryStream *stream = (MemoryStream *)self;
    LONG left = InterlockedDecrement(&stream->references);
    if (!left) {
        buffer_free(&stream->data);
        net_free(stream);
    }
    return (ULONG)left;
}

static HRESULT STDMETHODCALLTYPE memory_stream_read(SequentialStream *self, void *output,
                                                    ULONG size, ULONG *read) {
    MemoryStream *stream = (MemoryStream *)self;
    size_t available = stream->data.size - stream->position, count = size;
    if (!output && size) {
        return STG_E_INVALIDPOINTER;
    }
    if (count > available) {
        count = available;
    }
    if (count) {
        memory_copy(output, stream->data.data + stream->position, count);
    }
    stream->position += count;
    if (read) {
        *read = (ULONG)count;
    }
    return count == size ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE memory_stream_write(SequentialStream *self, const void *input,
                                                     ULONG size, ULONG *written) {
    (void)self;
    (void)input;
    (void)size;
    if (written) {
        *written = 0;
    }
    return STG_E_ACCESSDENIED;
}

static const SequentialStreamVtbl memory_stream_vtable = {
    memory_stream_query, memory_stream_add_ref, memory_stream_release, memory_stream_read,
    memory_stream_write};

static SequentialStream *memory_stream_create(const Buffer *body) {
    MemoryStream *stream = net_alloc(sizeof(*stream));
    if (!stream) {
        return 0;
    }
    stream->lpVtbl = &memory_stream_vtable;
    stream->references = 1;
    if (body->size && !buffer_append(&stream->data, body->data, body->size)) {
        net_free(stream);
        return 0;
    }
    return (SequentialStream *)stream;
}

/* ------------------------------------------------------------ the request */

typedef struct NetRequest NetRequest;
typedef struct XhrCallback XhrCallback;
typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(XhrCallback *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(XhrCallback *);
    ULONG(STDMETHODCALLTYPE *Release)(XhrCallback *);
    HRESULT(STDMETHODCALLTYPE *OnRedirect)(XhrCallback *, NetRequest *, const WCHAR *);
    HRESULT(STDMETHODCALLTYPE *OnHeadersAvailable)(XhrCallback *, NetRequest *, DWORD,
                                                   const WCHAR *);
    HRESULT(STDMETHODCALLTYPE *OnDataAvailable)(XhrCallback *, NetRequest *, SequentialStream *);
    HRESULT(STDMETHODCALLTYPE *OnResponseReceived)(XhrCallback *, NetRequest *,
                                                   SequentialStream *);
    HRESULT(STDMETHODCALLTYPE *OnError)(XhrCallback *, NetRequest *, HRESULT);
} XhrCallbackVtbl;
struct XhrCallback {
    const XhrCallbackVtbl *lpVtbl;
};

typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(NetRequest *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(NetRequest *);
    ULONG(STDMETHODCALLTYPE *Release)(NetRequest *);
    HRESULT(STDMETHODCALLTYPE *Open)(NetRequest *, const WCHAR *, const WCHAR *, XhrCallback *,
                                     const WCHAR *, const WCHAR *, const WCHAR *, const WCHAR *);
    HRESULT(STDMETHODCALLTYPE *Send)(NetRequest *, SequentialStream *, ULONGLONG);
    HRESULT(STDMETHODCALLTYPE *Abort)(NetRequest *);
    HRESULT(STDMETHODCALLTYPE *SetCookie)(NetRequest *, const void *, DWORD *);
    HRESULT(STDMETHODCALLTYPE *SetCustomResponseStream)(NetRequest *, SequentialStream *);
    HRESULT(STDMETHODCALLTYPE *SetProperty)(NetRequest *, int, ULONGLONG);
    HRESULT(STDMETHODCALLTYPE *SetRequestHeader)(NetRequest *, const WCHAR *, const WCHAR *);
    HRESULT(STDMETHODCALLTYPE *GetAllResponseHeaders)(NetRequest *, WCHAR **);
    HRESULT(STDMETHODCALLTYPE *GetCookie)(NetRequest *, const WCHAR *, const WCHAR *, DWORD,
                                          DWORD *, void **);
    HRESULT(STDMETHODCALLTYPE *GetResponseHeader)(NetRequest *, const WCHAR *, WCHAR **);
} NetRequestVtbl;

struct NetRequest {
    const NetRequestVtbl *lpVtbl;
    volatile LONG references, sent, aborted;
    LONG id;
    DWORD timeout_ms;
    Buffer method, url, request_headers, request_body;
    XhrCallback *callback;
    SequentialStream *custom_stream;
    DWORD status;
    Buffer status_text, response_headers, response_body;
};

static void net_request_destroy(NetRequest *request) {
    if (request->callback) {
        request->callback->lpVtbl->Release(request->callback);
    }
    if (request->custom_stream) {
        request->custom_stream->lpVtbl->Release(request->custom_stream);
    }
    buffer_free(&request->method);
    buffer_free(&request->url);
    buffer_free(&request->request_headers);
    buffer_free(&request->request_body);
    buffer_free(&request->status_text);
    buffer_free(&request->response_headers);
    buffer_free(&request->response_body);
    net_free(request);
}

static HRESULT STDMETHODCALLTYPE net_request_query(NetRequest *self, REFIID iid, void **out) {
    if (!out) {
        return E_POINTER;
    }
    *out = 0;
    if (memory_equal(iid, net_iid_unknown, 16) || memory_equal(iid, net_iid_xhr2, 16) ||
        memory_equal(iid, net_iid_agile, 16)) {
        *out = self;
        InterlockedIncrement(&self->references);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE net_request_add_ref(NetRequest *self) {
    return (ULONG)InterlockedIncrement(&self->references);
}

static ULONG STDMETHODCALLTYPE net_request_release(NetRequest *self) {
    LONG left = InterlockedDecrement(&self->references);
    if (!left) {
        net_request_destroy(self);
    }
    return (ULONG)left;
}

static HRESULT STDMETHODCALLTYPE net_request_open(NetRequest *self, const WCHAR *method,
                                                  const WCHAR *url, XhrCallback *callback,
                                                  const WCHAR *user, const WCHAR *password,
                                                  const WCHAR *proxy_user,
                                                  const WCHAR *proxy_password) {
    (void)user;
    (void)password;
    (void)proxy_user;
    (void)proxy_password;
    if (!method || !url || !callback) {
        return E_INVALIDARG;
    }
    if (self->sent || self->callback) {
        return NET_UNEXPECTED;
    }
    if (!buffer_append_wide(&self->method, method) || !buffer_append_wide(&self->url, url)) {
        return E_OUTOFMEMORY;
    }
    callback->lpVtbl->AddRef(callback);
    self->callback = callback;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE net_request_set_header(NetRequest *self, const WCHAR *name,
                                                        const WCHAR *value) {
    if (!name || !*name) {
        return E_INVALIDARG;
    }
    if (!buffer_append_wide(&self->request_headers, name) ||
        !buffer_append_wide(&self->request_headers, L": ") ||
        !buffer_append_wide(&self->request_headers, value ? value : L"") ||
        !buffer_append_wide(&self->request_headers, L"\r\n")) {
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

static int net_read_body(NetRequest *request, SequentialStream *body, ULONGLONG size) {
    unsigned char chunk[2048];
    ULONGLONG total = 0;
    while (body && (!size || total < size) && request->request_body.size < NET_MAX_BODY) {
        ULONG want = sizeof(chunk), got = 0;
        if (size && size - total < want) {
            want = (ULONG)(size - total);
        }
        HRESULT result = body->lpVtbl->Read(body, chunk, want, &got);
        if (FAILED(result)) {
            return 0;
        }
        if (got && !buffer_append(&request->request_body, chunk, got)) {
            return 0;
        }
        total += got;
        if (!got || result == S_FALSE) {
            break;
        }
    }
    return 1;
}

static void net_capture(NetRequest *request, int answered, HRESULT error) {
    Buffer line = {0};
    int ok =
        buffer_append_text(&line, "{\"id\":") && buffer_append_number(&line, (ULONGLONG)request->id) &&
        buffer_append_text(&line, ",\"tick_ms\":") && buffer_append_number(&line, GetTickCount64()) &&
        buffer_append_text(&line, net_mode == NET_MODE_PROXY ? ",\"mode\":\"proxy\""
                                                             : ",\"mode\":\"capture\"") &&
        buffer_append_text(&line, ",\"method\":") &&
        buffer_append_json_wide(&line, buffer_wide(&request->method),
                                request->method.size / sizeof(wchar_t)) &&
        buffer_append_text(&line, ",\"url\":") &&
        buffer_append_json_wide(&line, buffer_wide(&request->url),
                                request->url.size / sizeof(wchar_t)) &&
        buffer_append_text(&line, ",\"request_headers\":") &&
        buffer_append_json_wide(&line, buffer_wide(&request->request_headers),
                                request->request_headers.size / sizeof(wchar_t)) &&
        buffer_append_text(&line, ",\"request_body_b64\":") &&
        buffer_append_base64(&line, request->request_body.data, request->request_body.size);
    if (ok && answered) {
        ok = buffer_append_text(&line, ",\"outcome\":\"response\",\"status\":") &&
             buffer_append_number(&line, request->status) &&
             buffer_append_text(&line, ",\"status_text\":") &&
             buffer_append_json_wide(&line, buffer_wide(&request->status_text),
                                     request->status_text.size / sizeof(wchar_t)) &&
             buffer_append_text(&line, ",\"response_headers\":") &&
             buffer_append_json_wide(&line, buffer_wide(&request->response_headers),
                                     request->response_headers.size / sizeof(wchar_t)) &&
             buffer_append_text(&line, ",\"response_body_b64\":") &&
             buffer_append_base64(&line, request->response_body.data, request->response_body.size);
    } else if (ok) {
        ok = buffer_append_text(&line, error == NET_OFFLINE_ERROR ? ",\"outcome\":\"offline\""
                                                                  : ",\"outcome\":\"error\"") &&
             buffer_append_text(&line, ",\"hresult\":") && buffer_append_hresult(&line, error);
    }
    if (ok) {
        ok = buffer_append_text(&line, "}\r\n");
    }
    if (ok && net_log_path[0]) {
        AcquireSRWLockExclusive(&net_log_lock);
        HANDLE file = CreateFileW(net_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(file, line.data, (DWORD)line.size, &written, NULL);
            CloseHandle(file);
        }
        ReleaseSRWLockExclusive(&net_log_lock);
    }
    buffer_free(&line);
}

/* Headers WinHTTP manages itself are logged but not forwarded. */
static int net_header_is_forwarded(const wchar_t *line, size_t length) {
    static const wchar_t *const skipped[] = {L"host:", L"content-length:", L"connection:",
                                             L"transfer-encoding:"};
    for (size_t i = 0; i < ARRAY_COUNT(skipped); ++i) {
        if (wide_starts_ignore_case(line, length, skipped[i])) {
            return 0;
        }
    }
    return 1;
}

static int net_query_text(HINTERNET request, DWORD query, Buffer *out) {
    DWORD size = 0;
    http_query_headers(request, query, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                       &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size || !buffer_reserve(out, size)) {
        return 0;
    }
    if (!http_query_headers(request, query, WINHTTP_HEADER_NAME_BY_INDEX, out->data, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    out->size = size;
    out->data[out->size] = out->data[out->size + 1] = 0;
    return 1;
}

/* Forward to the test server as http://SERVER/<original host + path>. */
static int net_forward(NetRequest *request, HRESULT *error) {
    HINTERNET session = 0, connection = 0, http = 0;
    Buffer path = {0}, headers = {0}, raw = {0};
    int answered = 0;
    *error = NET_OFFLINE_ERROR;
    const wchar_t *url = buffer_wide(&request->url), *rest = url;
    for (const wchar_t *p = url; p[0] && p[1] && p[2]; ++p) {
        if (p[0] == L':' && p[1] == L'/' && p[2] == L'/') {
            rest = p + 3;
            break;
        }
    }
    if (!net_load_winhttp() || !buffer_append_wide(&path, L"/") ||
        !buffer_append_wide(&path, rest)) {
        goto done;
    }
    /* Original headers minus the ones WinHTTP owns, plus where it was going. */
    for (const wchar_t *line = buffer_wide(&request->request_headers); *line;) {
        size_t length = 0;
        while (line[length] && line[length] != L'\n') {
            ++length;
        }
        size_t taken = line[length] ? length + 1 : length;
        if (net_header_is_forwarded(line, length) &&
            !buffer_append(&headers, line, taken * sizeof(wchar_t))) {
            goto done;
        }
        line += taken;
    }
    char id_text[24];
    unsigned count = 0;
    for (ULONG value = (ULONG)request->id; count == 0 || value; value /= 10) {
        id_text[sizeof(id_text) - 1 - count++] = (char)('0' + value % 10);
    }
    wchar_t id_wide[24];
    for (unsigned i = 0; i < count; ++i) {
        id_wide[i] = (wchar_t)id_text[sizeof(id_text) - count + i];
    }
    id_wide[count] = 0;
    if (!buffer_append_wide(&headers, L"X-Fh2-Original-Url: ") ||
        !buffer_append_wide(&headers, url) ||
        !buffer_append_wide(&headers, L"\r\nX-Fh2-Request-Id: ") ||
        !buffer_append_wide(&headers, id_wide) || !buffer_append_wide(&headers, L"\r\n")) {
        goto done;
    }
    session = http_open(L"FH2Online/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        goto done;
    }
    int receive_timeout = request->timeout_ms ? (int)request->timeout_ms : 30000;
    http_set_timeouts(session, 5000, 5000, 30000, receive_timeout);
    connection = http_connect(session, net_server_host, net_server_port, 0);
    http = connection ? http_open_request(connection, buffer_wide(&request->method),
                                          buffer_wide(&path), NULL, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                      : 0;
    if (!http || !http_add_headers(http, buffer_wide(&headers), (DWORD)-1L,
                                   WINHTTP_ADDREQ_FLAG_ADD)) {
        goto done;
    }
    DWORD body_size = (DWORD)request->request_body.size;
    if (!http_send(http, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                   body_size ? request->request_body.data : WINHTTP_NO_REQUEST_DATA, body_size,
                   body_size, 0) ||
        !http_receive(http, NULL)) {
        goto done;
    }
    DWORD status = 0, status_size = sizeof(status);
    if (!http_query_headers(http, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX)) {
        goto done;
    }
    request->status = status;
    net_query_text(http, WINHTTP_QUERY_STATUS_TEXT, &request->status_text);
    /* Raw CRLF headers start with the status line; the game expects only fields. */
    if (net_query_text(http, WINHTTP_QUERY_RAW_HEADERS_CRLF, &raw)) {
        const wchar_t *fields = buffer_wide(&raw);
        while (*fields && *fields != L'\n') {
            ++fields;
        }
        if (*fields) {
            ++fields;
        }
        size_t length = wide_length(fields);
        while (length >= 2 && fields[length - 1] == L'\n' && fields[length - 2] == L'\r' &&
               length >= 4 && fields[length - 3] == L'\n') {
            length -= 2; /* drop the blank line that ends the header block */
        }
        buffer_append(&request->response_headers, fields, length * sizeof(wchar_t));
    }
    for (;;) {
        DWORD available = 0, read = 0;
        if (!http_query_available(http, &available) || !available) {
            break;
        }
        if (request->response_body.size + available > NET_MAX_BODY ||
            !buffer_reserve(&request->response_body, available) ||
            !http_read(http, request->response_body.data + request->response_body.size, available,
                       &read)) {
            break;
        }
        request->response_body.size += read;
        request->response_body.data[request->response_body.size] = 0;
    }
    answered = 1;
done:
    if (http) {
        http_close(http);
    }
    if (connection) {
        http_close(connection);
    }
    if (session) {
        http_close(session);
    }
    buffer_free(&path);
    buffer_free(&headers);
    buffer_free(&raw);
    return answered;
}

static DWORD WINAPI net_worker(void *parameter) {
    NetRequest *request = parameter;
    HRESULT error = NET_OFFLINE_ERROR;
    int answered = net_mode == NET_MODE_PROXY && !request->aborted && net_forward(request, &error);
    net_capture(request, answered, error);
    XhrCallback *callback = request->callback;
    if (!request->aborted && callback) {
        if (!answered) {
            callback->lpVtbl->OnError(callback, request, error);
        } else {
            callback->lpVtbl->OnHeadersAvailable(callback, request, request->status,
                                                 buffer_wide(&request->status_text));
            if (request->custom_stream) {
                SequentialStream *custom = request->custom_stream;
                ULONG written = 0;
                if (request->response_body.size) {
                    custom->lpVtbl->Write(custom, request->response_body.data,
                                          (ULONG)request->response_body.size, &written);
                }
                callback->lpVtbl->OnResponseReceived(callback, request, custom);
            } else {
                SequentialStream *stream = memory_stream_create(&request->response_body);
                if (!stream) {
                    callback->lpVtbl->OnError(callback, request, E_OUTOFMEMORY);
                } else {
                    callback->lpVtbl->OnDataAvailable(callback, request, stream);
                    callback->lpVtbl->OnResponseReceived(callback, request, stream);
                    stream->lpVtbl->Release(stream);
                }
            }
        }
    }
    net_request_release(request);
    return 0;
}

static HRESULT STDMETHODCALLTYPE net_request_send(NetRequest *self, SequentialStream *body,
                                                  ULONGLONG size) {
    if (!self->callback) {
        return NET_UNEXPECTED;
    }
    if (InterlockedCompareExchange(&self->sent, 1, 0)) {
        return NET_UNEXPECTED;
    }
    if (!net_read_body(self, body, size)) {
        return E_FAIL;
    }
    InterlockedIncrement(&self->references);
    HANDLE thread = CreateThread(NULL, 0, net_worker, self, 0, NULL);
    if (!thread) {
        InterlockedDecrement(&self->references);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(thread);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE net_request_abort(NetRequest *self) {
    InterlockedExchange(&self->aborted, 1);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE net_request_set_cookie(NetRequest *self, const void *cookie,
                                                        DWORD *state) {
    (void)self;
    (void)cookie;
    if (state) {
        *state = 0;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE net_request_set_stream(NetRequest *self,
                                                        SequentialStream *stream) {
    if (self->custom_stream) {
        self->custom_stream->lpVtbl->Release(self->custom_stream);
    }
    self->custom_stream = stream;
    if (stream) {
        stream->lpVtbl->AddRef(stream);
        log_append("NETWORK: a request uses a custom response stream.\r\n");
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE net_request_set_property(NetRequest *self, int property,
                                                          ULONGLONG value) {
    if (property == NET_XHR_PROP_TIMEOUT) {
        self->timeout_ms = value > 0x7fffffffu ? 0x7fffffffu : (DWORD)value;
    }
    return S_OK;
}

static HRESULT net_copy_for_caller(const wchar_t *text, size_t length, WCHAR **out) {
    WCHAR *copy = net_task_alloc((length + 1) * sizeof(WCHAR));
    if (!copy) {
        return E_OUTOFMEMORY;
    }
    if (length) {
        memory_copy(copy, text, length * sizeof(WCHAR));
    }
    copy[length] = 0;
    *out = copy;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE net_request_all_headers(NetRequest *self, WCHAR **headers) {
    if (!headers) {
        return E_POINTER;
    }
    *headers = 0;
    return net_copy_for_caller(buffer_wide(&self->response_headers),
                               self->response_headers.size / sizeof(wchar_t), headers);
}

static HRESULT STDMETHODCALLTYPE net_request_get_cookie(NetRequest *self, const WCHAR *url,
                                                        const WCHAR *name, DWORD flags,
                                                        DWORD *count, void **cookies) {
    (void)self;
    (void)url;
    (void)name;
    (void)flags;
    if (count) {
        *count = 0;
    }
    if (cookies) {
        *cookies = 0;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE net_request_header(NetRequest *self, const WCHAR *name,
                                                    WCHAR **value) {
    if (!name || !value) {
        return E_POINTER;
    }
    *value = 0;
    size_t name_length = wide_length(name);
    for (const wchar_t *line = buffer_wide(&self->response_headers); *line;) {
        size_t length = 0;
        while (line[length] && line[length] != L'\n') {
            ++length;
        }
        if (length > name_length && line[name_length] == L':' &&
            wide_starts_ignore_case(line, length, name)) {
            size_t start = name_length + 1, end = length;
            while (start < end && line[start] == L' ') {
                ++start;
            }
            while (end > start && (line[end - 1] == L'\r' || line[end - 1] == L' ')) {
                --end;
            }
            return net_copy_for_caller(line + start, end - start, value);
        }
        line += line[length] ? length + 1 : length;
    }
    return NET_HEADER_NOT_FOUND;
}

static const NetRequestVtbl net_request_vtable = {
    net_request_query,        net_request_add_ref,     net_request_release,
    net_request_open,         net_request_send,        net_request_abort,
    net_request_set_cookie,   net_request_set_stream,  net_request_set_property,
    net_request_set_header,   net_request_all_headers, net_request_get_cookie,
    net_request_header};

/* ------------------------------------------------- CoCreateInstance detour */

typedef HRESULT (*MmDeviceFactoryFn)(REFIID, void **);

static HRESULT net_co_create_instance(REFCLSID clsid, void *outer, DWORD context, REFIID iid,
                                      void **out) {
    if (!out) {
        return E_POINTER;
    }
    *out = 0;
    if (memory_equal(clsid, net_clsid_xhr, 16)) {
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        NetRequest *request = net_alloc(sizeof(*request));
        if (!request) {
            return E_OUTOFMEMORY;
        }
        request->lpVtbl = &net_request_vtable;
        request->references = 1;
        request->id = InterlockedIncrement(&net_next_id);
        HRESULT result = net_request_query(request, iid, out);
        net_request_release(request);
        return result;
    }
    if (memory_equal(clsid, net_clsid_mmdevice, 16)) {
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        if (!(context & NET_CLSCTX_INPROC_SERVER)) {
            return E_INVALIDARG;
        }
        MmDeviceFactoryFn factory =
            (MmDeviceFactoryFn)(uintptr_t)(net_azimuth_base + NET_MMDEVICE_FACTORY_RVA);
        return factory(iid, out);
    }
    log_append("NETWORK: refused CoCreateInstance for a class Azimuth does not provide.\r\n");
    return REGDB_E_CLASSNOTREG;
}

/* 1 = installed, 2 = already installed, 0 = unsupported host, -1 = write failed. */
static int net_install_hook(unsigned char *base) {
    unsigned char current[NET_HOOK_HASHED_BYTES], digest[32];
    unsigned char jump[NET_HOOK_JUMP_BYTES] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    uintptr_t target = (uintptr_t)net_co_create_instance;
    for (unsigned i = 0; i < 8; ++i) {
        jump[6 + i] = (unsigned char)(target >> (8 * i));
    }
    if (azimuth_host_build(base) != AZIMUTH_097 ||
        !memory_read(base + NET_HOOK_RVA, current, sizeof(current))) {
        return 0;
    }
    if (memory_equal(current, jump, sizeof(jump))) {
        return 2;
    }
    Sha256Context hash;
    sha256_initialize(&hash);
    sha256_update(&hash, current, sizeof(current));
    sha256_finalize(&hash, digest);
    if (!memory_equal(digest, net_hook_original_hash, sizeof(digest))) {
        return 0;
    }
    DWORD old_protection, ignored;
    if (!VirtualProtect(base + NET_HOOK_RVA, sizeof(jump), PAGE_EXECUTE_READWRITE,
                        &old_protection)) {
        return -1;
    }
    memory_copy(base + NET_HOOK_RVA, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), base + NET_HOOK_RVA, sizeof(jump));
    VirtualProtect(base + NET_HOOK_RVA, sizeof(jump), old_protection, &ignored);
    return 1;
}

/* ----------------------------------------------------------------- setup */

static void net_read_settings(const wchar_t *directory) {
    static wchar_t ini[PATH_CAPACITY];
    wchar_t value[512];
    net_mode = NET_MODE_OFF;
    if (!path_join(ini, directory, MOD_INI_NAME)) {
        return;
    }
    GetPrivateProfileStringW(L"Network", L"Mode", L"Off", value, ARRAY_COUNT(value), ini);
    if (wide_starts_ignore_case(value, wide_length(value), L"proxy")) {
        net_mode = NET_MODE_PROXY;
    } else if (wide_starts_ignore_case(value, wide_length(value), L"capture")) {
        net_mode = NET_MODE_CAPTURE;
    }
    GetPrivateProfileStringW(L"Network", L"Server", L"127.0.0.1:8081", value, ARRAY_COUNT(value),
                             ini);
    const wchar_t *server = value;
    if (wide_starts_ignore_case(server, wide_length(server), L"http://")) {
        server += 7;
    }
    size_t length = 0, colon = (size_t)-1;
    while (server[length] && server[length] != L'/') {
        if (server[length] == L':') {
            colon = length;
        }
        ++length;
    }
    size_t host_length = colon == (size_t)-1 ? length : colon;
    if (!host_length || host_length >= ARRAY_COUNT(net_server_host)) {
        memory_copy(net_server_host, L"127.0.0.1", sizeof(L"127.0.0.1"));
    } else {
        memory_copy(net_server_host, server, host_length * sizeof(wchar_t));
        net_server_host[host_length] = 0;
    }
    if (colon != (size_t)-1) {
        unsigned port = 0;
        for (size_t i = colon + 1; i < length && server[i] >= L'0' && server[i] <= L'9'; ++i) {
            port = port * 10 + (unsigned)(server[i] - L'0');
        }
        if (port && port < 65536) {
            net_server_port = (INTERNET_PORT)port;
        }
    }
    if (!path_join(net_log_path, directory, NET_LOG_NAME)) {
        net_log_path[0] = 0;
    }
}

void azimuth_apply_network(const wchar_t *directory) {
    net_read_settings(directory);
    if (net_mode == NET_MODE_OFF) {
        log_append("NETWORK: off (set [Network] Mode=Capture or Mode=Proxy to enable).\r\n");
        return;
    }
    net_azimuth_base = (unsigned char *)GetModuleHandleW(NULL);
    int result = net_install_hook(net_azimuth_base);
    if (result > 0) {
        log_append(net_mode == NET_MODE_PROXY
                       ? "NETWORK: HTTP hook installed; requests are captured to "
                         "FH2_Network.jsonl and forwarded to the [Network] Server.\r\n"
                       : "NETWORK: HTTP hook installed; requests are captured to "
                         "FH2_Network.jsonl and answered offline, as Azimuth does.\r\n");
    } else if (result < 0) {
        net_mode = NET_MODE_OFF;
        log_append("NETWORK: HTTP hook could not be written; network layer disabled.\r\n");
    } else {
        net_mode = NET_MODE_OFF;
        log_append("NETWORK: skipped; host is not the inspected Azimuth 0.9.7 build.\r\n");
    }
}
