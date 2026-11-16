/* Emulator Control Protocol 1: engine-side library. MIT licence. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "ecp.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32")
#endif
typedef SOCKET ecp_sock;
#define ECP_BAD_SOCK INVALID_SOCKET
#define ecp_closesocket closesocket
typedef CRITICAL_SECTION ecp_mutex;
#define mutex_init(m) InitializeCriticalSection(m)
#define mutex_lock(m) EnterCriticalSection(m)
#define mutex_unlock(m) LeaveCriticalSection(m)
#define mutex_destroy(m) DeleteCriticalSection(m)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int ecp_sock;
#define ECP_BAD_SOCK (-1)
#define ecp_closesocket close
typedef pthread_mutex_t ecp_mutex;
#define mutex_init(m) pthread_mutex_init(m, NULL)
#define mutex_lock(m) pthread_mutex_lock(m)
#define mutex_unlock(m) pthread_mutex_unlock(m)
#define mutex_destroy(m) pthread_mutex_destroy(m)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

/* ============================================================================================
 * Logging
 */
static void (*g_log_fn)(void *, const char *);
static void *g_log_user;

void ecp_set_log(void (*fn)(void *user, const char *line), void *user) {
    g_log_fn = fn;
    g_log_user = user;
}

static void ecp_log(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_log_fn)
        g_log_fn(g_log_user, line);
    else
        fprintf(stderr, "ecp: %s\n", line);
}

/* ============================================================================================
 * JSON reader
 */
struct ecp_json {
    ecp_json_type type;
    int boolean;
    double num;
    char *str;
    size_t len; /* string bytes, or item count */
    ecp_json **items;
    char **keys; /* objects only */
};

typedef struct jparser {
    const char *p, *end;
    int depth;
} jparser;

static void jp_ws(jparser *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int jp_hex4(jparser *j, unsigned *out) {
    unsigned v = 0;
    if (j->end - j->p < 4) return -1;
    for (int i = 0; i < 4; i++) {
        int h = hexval(j->p[i]);
        if (h < 0) return -1;
        v = v << 4 | (unsigned)h;
    }
    j->p += 4;
    *out = v;
    return 0;
}

static char *put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | cp >> 6);
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | cp >> 12);
        *o++ = (char)(0x80 | (cp >> 6 & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | cp >> 18);
        *o++ = (char)(0x80 | (cp >> 12 & 0x3F));
        *o++ = (char)(0x80 | (cp >> 6 & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* Parses a string after its opening quote. The decoded text is never longer than the source. */
static char *jp_string(jparser *j, size_t *out_len) {
    const char *start = j->p;
    while (j->p < j->end && *j->p != '"') j->p += (*j->p == '\\' && j->p + 1 < j->end) ? 2 : 1;
    if (j->p >= j->end) return NULL;
    const char *stop = j->p, *end = j->end;
    char *s = (char *)malloc((size_t)(stop - start) + 1), *o = s;
    if (!s) return NULL;
    j->p = start, j->end = stop; /* decode within the quotes */
    while (j->p < stop) {
        unsigned char c = (unsigned char)*j->p++;
        if (c < 0x20) goto fail;
        if (c != '\\') {
            *o++ = (char)c;
            continue;
        }
        switch (*j->p++) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (jp_hex4(j, &cp)) goto fail;
            if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' &&
                j->p[1] == 'u') {
                j->p += 2;
                if (jp_hex4(j, &lo) || lo < 0xDC00 || lo > 0xDFFF) goto fail;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xD800 && cp < 0xE000) {
                cp = 0xFFFD;
            }
            o = put_utf8(o, cp);
            break;
        }
        default: goto fail;
        }
    }
    j->p = stop + 1, j->end = end;
    *o = 0;
    *out_len = (size_t)(o - s);
    return s;
fail:
    j->end = end;
    free(s);
    return NULL;
}

/* Locale-independent number parsing (strtod follows LC_NUMERIC, which emulators may change). */
static int jp_number(jparser *j, double *out) {
    const char *p = j->p;
    double v = 0, sign = 1;
    if (p < j->end && *p == '-') sign = -1, p++;
    if (p >= j->end || *p < '0' || *p > '9') return -1;
    while (p < j->end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (p < j->end && *p == '.') {
        double scale = 0.1;
        p++;
        if (p >= j->end || *p < '0' || *p > '9') return -1;
        while (p < j->end && *p >= '0' && *p <= '9') v += (*p++ - '0') * scale, scale *= 0.1;
    }
    if (p < j->end && (*p == 'e' || *p == 'E')) {
        int esign = 1, e = 0;
        p++;
        if (p < j->end && (*p == '+' || *p == '-')) esign = *p++ == '-' ? -1 : 1;
        if (p >= j->end || *p < '0' || *p > '9') return -1;
        while (p < j->end && *p >= '0' && *p <= '9') e = e < 10000 ? e * 10 + (*p - '0') : e, p++;
        v *= pow(10.0, esign * e);
    }
    j->p = p;
    *out = sign * v;
    return 0;
}

static int jp_push(ecp_json *v, ecp_json *item, char *key, size_t *cap) {
    if (v->len == *cap) {
        size_t n = *cap ? *cap * 2 : 4;
        ecp_json **items = (ecp_json **)realloc(v->items, n * sizeof *items);
        if (!items) return -1;
        v->items = items;
        if (v->type == ECP_JSON_OBJECT) {
            char **keys = (char **)realloc(v->keys, n * sizeof *keys);
            if (!keys) return -1;
            v->keys = keys;
        }
        *cap = n;
    }
    v->items[v->len] = item;
    if (v->type == ECP_JSON_OBJECT) v->keys[v->len] = key;
    v->len++;
    return 0;
}

static ecp_json *jp_value(jparser *j) {
    jp_ws(j);
    if (j->p >= j->end || j->depth > 64) return NULL;
    ecp_json *v = (ecp_json *)calloc(1, sizeof *v);
    if (!v) return NULL;
    char c = *j->p;
    if (c == '{' || c == '[') {
        size_t cap = 0;
        char close = c == '{' ? '}' : ']';
        v->type = c == '{' ? ECP_JSON_OBJECT : ECP_JSON_ARRAY;
        j->p++;
        j->depth++;
        jp_ws(j);
        if (j->p < j->end && *j->p == close) {
            j->p++;
            j->depth--;
            return v;
        }
        for (;;) {
            char *key = NULL;
            if (v->type == ECP_JSON_OBJECT) {
                size_t klen;
                jp_ws(j);
                if (j->p >= j->end || *j->p != '"') goto fail;
                j->p++;
                if (!(key = jp_string(j, &klen))) goto fail;
                jp_ws(j);
                if (j->p >= j->end || *j->p != ':') {
                    free(key);
                    goto fail;
                }
                j->p++;
            }
            ecp_json *item = jp_value(j);
            if (!item || jp_push(v, item, key, &cap)) {
                free(key);
                ecp_json_free(item);
                goto fail;
            }
            jp_ws(j);
            if (j->p < j->end && *j->p == ',') {
                j->p++;
                continue;
            }
            if (j->p < j->end && *j->p == close) {
                j->p++;
                break;
            }
            goto fail;
        }
        j->depth--;
        return v;
    }
    if (c == '"') {
        j->p++;
        v->type = ECP_JSON_STRING;
        if (!(v->str = jp_string(j, &v->len))) goto fail;
        return v;
    }
    if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4)) {
        v->type = ECP_JSON_BOOL, v->boolean = 1, j->p += 4;
        return v;
    }
    if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) {
        v->type = ECP_JSON_BOOL, j->p += 5;
        return v;
    }
    if (j->end - j->p >= 4 && !memcmp(j->p, "null", 4)) {
        j->p += 4;
        return v;
    }
    v->type = ECP_JSON_NUMBER;
    if (jp_number(j, &v->num) == 0) return v;
fail:
    ecp_json_free(v);
    return NULL;
}

ecp_json *ecp_json_parse(const char *text, size_t len) {
    jparser j = {text, text + len, 0};
    ecp_json *v = jp_value(&j);
    jp_ws(&j);
    if (v && j.p != j.end) {
        ecp_json_free(v);
        return NULL;
    }
    return v;
}

void ecp_json_free(ecp_json *v) {
    if (!v) return;
    if (v->type == ECP_JSON_ARRAY || v->type == ECP_JSON_OBJECT) {
        for (size_t i = 0; i < v->len; i++) {
            ecp_json_free(v->items[i]);
            if (v->keys) free(v->keys[i]);
        }
        free(v->items);
        free(v->keys);
    }
    free(v->str);
    free(v);
}

ecp_json_type ecp_json_type_of(const ecp_json *v) { return v ? v->type : ECP_JSON_NULL; }

const ecp_json *ecp_json_get(const ecp_json *o, const char *key) {
    if (!o || o->type != ECP_JSON_OBJECT) return NULL;
    for (size_t i = 0; i < o->len; i++)
        if (!strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}

size_t ecp_json_len(const ecp_json *v) {
    return v && (v->type == ECP_JSON_ARRAY || v->type == ECP_JSON_OBJECT) ? v->len : 0;
}

const ecp_json *ecp_json_at(const ecp_json *v, size_t i) {
    return i < ecp_json_len(v) ? v->items[i] : NULL;
}

const char *ecp_json_key_at(const ecp_json *o, size_t i) {
    return o && o->type == ECP_JSON_OBJECT && i < o->len ? o->keys[i] : NULL;
}

const char *ecp_json_str(const ecp_json *v, const char *fallback) {
    return v && v->type == ECP_JSON_STRING ? v->str : fallback;
}

double ecp_json_num(const ecp_json *v, double fallback) {
    return v && v->type == ECP_JSON_NUMBER ? v->num : fallback;
}

int64_t ecp_json_int(const ecp_json *v, int64_t fallback) {
    if (!v || v->type != ECP_JSON_NUMBER || v->num != floor(v->num) || fabs(v->num) > 9.0e15)
        return fallback;
    return (int64_t)v->num;
}

int ecp_json_bool(const ecp_json *v, int fallback) {
    return v && v->type == ECP_JSON_BOOL ? v->boolean : fallback;
}

/* ============================================================================================
 * JSON writer
 */
static void w_put(ecp_writer *w, const char *s, size_t n) {
    if (w->failed) return;
    if (w->len + n + 1 > w->cap) {
        size_t cap = w->cap ? w->cap : 256;
        while (cap < w->len + n + 1) cap *= 2;
        char *buf = (char *)realloc(w->buf, cap);
        if (!buf) {
            w->failed = 1;
            return;
        }
        w->buf = buf, w->cap = cap;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

static void w_sep(ecp_writer *w) {
    if (w->comma) w_put(w, ",", 1);
    w->comma = 1;
}

void ecp_w_free(ecp_writer *w) {
    free(w->buf);
    memset(w, 0, sizeof *w);
}

void ecp_w_object_begin(ecp_writer *w) { w_sep(w), w_put(w, "{", 1), w->comma = 0; }
void ecp_w_object_end(ecp_writer *w) { w_put(w, "}", 1), w->comma = 1; }
void ecp_w_array_begin(ecp_writer *w) { w_sep(w), w_put(w, "[", 1), w->comma = 0; }
void ecp_w_array_end(ecp_writer *w) { w_put(w, "]", 1), w->comma = 1; }

static void w_quoted(ecp_writer *w, const char *s) {
    static const char hex[] = "0123456789abcdef";
    w_put(w, "\"", 1);
    for (const char *run = s;; s++) {
        unsigned char c = (unsigned char)*s;
        if (c && c >= 0x20 && c != '"' && c != '\\') continue;
        w_put(w, run, (size_t)(s - run));
        if (!c) break;
        char esc[6] = {'\\', (char)c, 0};
        size_t n = 2;
        if (c == '\n') esc[1] = 'n';
        else if (c == '\r') esc[1] = 'r';
        else if (c == '\t') esc[1] = 't';
        else if (c < 0x20) esc[1] = 'u', esc[2] = '0', esc[3] = '0', esc[4] = hex[c >> 4],
                           esc[5] = hex[c & 15], n = 6;
        w_put(w, esc, n);
        run = s + 1;
    }
    w_put(w, "\"", 1);
}

void ecp_w_key(ecp_writer *w, const char *key) {
    w_sep(w);
    w_quoted(w, key);
    w_put(w, ":", 1);
    w->comma = 0;
}

void ecp_w_string(ecp_writer *w, const char *s) { w_sep(w), w_quoted(w, s ? s : ""); }

void ecp_w_int(ecp_writer *w, int64_t v) {
    char tmp[24], *p = tmp + sizeof tmp;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do *--p = (char)('0' + u % 10); while (u /= 10);
    if (v < 0) *--p = '-';
    w_sep(w);
    w_put(w, p, (size_t)(tmp + sizeof tmp - p));
}

void ecp_w_number(ecp_writer *w, double v) {
    if (!isfinite(v)) {
        ecp_w_null(w);
        return;
    }
    if (v == floor(v) && fabs(v) < 9.0e15) {
        ecp_w_int(w, (int64_t)v);
        return;
    }
    char tmp[40];
    int n = snprintf(tmp, sizeof tmp, "%.9g", v);
    for (int i = 0; i < n; i++)
        if (tmp[i] == ',') tmp[i] = '.'; /* decimal comma locales */
    w_sep(w);
    w_put(w, tmp, (size_t)n);
}

void ecp_w_bool(ecp_writer *w, int v) { w_sep(w), w_put(w, v ? "true" : "false", v ? 4 : 5); }
void ecp_w_null(ecp_writer *w) { w_sep(w), w_put(w, "null", 4); }
void ecp_w_raw(ecp_writer *w, const char *json) { w_sep(w), w_put(w, json, strlen(json)); }

void ecp_w_json(ecp_writer *w, const ecp_json *v) {
    switch (ecp_json_type_of(v)) {
    case ECP_JSON_NULL: ecp_w_null(w); break;
    case ECP_JSON_BOOL: ecp_w_bool(w, v->boolean); break;
    case ECP_JSON_NUMBER: ecp_w_number(w, v->num); break;
    case ECP_JSON_STRING: ecp_w_string(w, v->str); break;
    case ECP_JSON_ARRAY:
        ecp_w_array_begin(w);
        for (size_t i = 0; i < v->len; i++) ecp_w_json(w, v->items[i]);
        ecp_w_array_end(w);
        break;
    case ECP_JSON_OBJECT:
        ecp_w_object_begin(w);
        for (size_t i = 0; i < v->len; i++) ecp_w_key(w, v->keys[i]), ecp_w_json(w, v->items[i]);
        ecp_w_object_end(w);
        break;
    }
}

void ecp_w_kstring(ecp_writer *w, const char *k, const char *s) { ecp_w_key(w, k), ecp_w_string(w, s); }
void ecp_w_knumber(ecp_writer *w, const char *k, double v) { ecp_w_key(w, k), ecp_w_number(w, v); }
void ecp_w_kint(ecp_writer *w, const char *k, int64_t v) { ecp_w_key(w, k), ecp_w_int(w, v); }
void ecp_w_kbool(ecp_writer *w, const char *k, int v) { ecp_w_key(w, k), ecp_w_bool(w, v); }

void ecp_w_picture_png(ecp_writer *w, const char *key, const void *png, size_t png_len, int width,
                       int height) {
    size_t b64_len;
    char *b64 = ecp_base64_encode(png, png_len, &b64_len);
    if (!b64) {
        w->failed = 1;
        return;
    }
    if (key) ecp_w_key(w, key);
    ecp_w_object_begin(w);
    ecp_w_key(w, "png");
    w_sep(w);
    w_put(w, "\"", 1), w_put(w, b64, b64_len), w_put(w, "\"", 1);
    ecp_w_kint(w, "width", width);
    ecp_w_kint(w, "height", height);
    ecp_w_object_end(w);
    free(b64);
}

void ecp_w_picture_rgba(ecp_writer *w, const char *key, const void *pixels, int width, int height,
                        int stride, int flags) {
    const unsigned char *src = (const unsigned char *)pixels;
    unsigned char *scaled = NULL;
    int step = 1;
    while ((size_t)(width / step) * (size_t)(height / step) * 3 > 8u * 1024 * 1024) step *= 2;
    if (step > 1) {
        int sw = width / step, sh = height / step;
        scaled = (unsigned char *)malloc((size_t)sw * sh * 4);
        if (!scaled) {
            w->failed = 1;
            return;
        }
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++)
                memcpy(scaled + ((size_t)y * sw + x) * 4,
                       src + (size_t)y * step * stride + (size_t)x * step * 4, 4);
        src = scaled, width = sw, height = sh, stride = sw * 4;
    }
    size_t png_len;
    unsigned char *png = ecp_png_encode(src, width, height, stride, flags, &png_len);
    free(scaled);
    if (!png) {
        w->failed = 1;
        return;
    }
    ecp_w_picture_png(w, key, png, png_len, width, height);
    free(png);
}

/* ============================================================================================
 * Base64, PNG, time
 */
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *ecp_base64_encode(const void *data, size_t len, size_t *out_len) {
    const unsigned char *s = (const unsigned char *)data;
    size_t n = (len + 2) / 3 * 4;
    char *o = (char *)malloc(n + 1), *p = o;
    if (!o) return NULL;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8 | s[i + 2];
        *p++ = b64chars[v >> 18], *p++ = b64chars[v >> 12 & 63];
        *p++ = b64chars[v >> 6 & 63], *p++ = b64chars[v & 63];
    }
    if (i < len) {
        uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < len ? (uint32_t)s[i + 1] << 8 : 0);
        *p++ = b64chars[v >> 18], *p++ = b64chars[v >> 12 & 63];
        *p++ = i + 1 < len ? b64chars[v >> 6 & 63] : '=';
        *p++ = '=';
    }
    *p = 0;
    if (out_len) *out_len = n;
    return o;
}

unsigned char *ecp_base64_decode(const char *text, size_t len, size_t *out_len) {
    unsigned char *o = (unsigned char *)malloc(len / 4 * 3 + 3), *p = o;
    uint32_t acc = 0;
    int bits = 0;
    if (!o) return NULL;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        const char *at = c ? strchr(b64chars, c) : NULL;
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        if (!at) {
            free(o);
            return NULL;
        }
        acc = acc << 6 | (uint32_t)(at - b64chars);
        if ((bits += 6) >= 8) *p++ = (unsigned char)(acc >> (bits -= 8));
    }
    if (out_len) *out_len = (size_t)(p - o);
    return o;
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n, const uint32_t *table) {
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ crc >> 8;
    return ~crc;
}

static void put_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24), p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8), p[3] = (unsigned char)v;
}

unsigned char *ecp_png_encode(const void *pixels, int width, int height, int stride, int flags,
                              size_t *out_len) {
    if (width <= 0 || height <= 0) return NULL;
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ c >> 1 : c >> 1;
        table[i] = c;
    }
    size_t row = (size_t)width * 3 + 1, raw = row * (size_t)height;
    size_t blocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + blocks * 5 + 4;
    size_t total = 8 + 25 + 12 + zlen + 12;
    unsigned char *png = (unsigned char *)malloc(total), *p = png;
    unsigned char *line = (unsigned char *)malloc(row);
    if (!png || !line) {
        free(png), free(line);
        return NULL;
    }
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    memcpy(p, sig, 8), p += 8;

    put_be32(p, 13), memcpy(p + 4, "IHDR", 4);
    put_be32(p + 8, (uint32_t)width), put_be32(p + 12, (uint32_t)height);
    p[16] = 8, p[17] = 2, p[18] = 0, p[19] = 0, p[20] = 0; /* 8-bit RGB */
    put_be32(p + 21, crc32_update(0, p + 4, 17, table)), p += 25;

    put_be32(p, (uint32_t)zlen), memcpy(p + 4, "IDAT", 4);
    unsigned char *idat = p + 4, *z = p + 8;
    *z++ = 0x78, *z++ = 0x01;
    uint32_t a = 1, b = 0;
    size_t left_in_block = 0, remaining = raw;
    const unsigned char *src = (const unsigned char *)pixels;
    int r = flags & ECP_PNG_BGRA ? 2 : 0, bl = 2 - r;
    for (int y = 0; y < height; y++) {
        const unsigned char *s = src + (size_t)y * (size_t)stride;
        line[0] = 0;
        for (int x = 0; x < width; x++) {
            line[1 + x * 3] = s[x * 4 + r];
            line[2 + x * 3] = s[x * 4 + 1];
            line[3 + x * 3] = s[x * 4 + bl];
        }
        for (size_t i = 0; i < row;) {
            if (!left_in_block) {
                size_t n = remaining < 65535 ? remaining : 65535;
                *z++ = remaining == n ? 1 : 0;
                *z++ = (unsigned char)n, *z++ = (unsigned char)(n >> 8);
                *z++ = (unsigned char)~n, *z++ = (unsigned char)(~n >> 8);
                left_in_block = n;
            }
            size_t n = row - i < left_in_block ? row - i : left_in_block;
            for (size_t k = 0; k < n; k++) {
                a = (a + line[i + k]) % 65521;
                b = (b + a) % 65521;
            }
            memcpy(z, line + i, n);
            z += n, i += n, left_in_block -= n, remaining -= n;
        }
    }
    put_be32(z, b << 16 | a), z += 4;
    put_be32(z, crc32_update(0, idat, (size_t)(z - idat), table)), z += 4;
    p = z;

    put_be32(p, 0), memcpy(p + 4, "IEND", 4);
    put_be32(p + 8, crc32_update(0, p + 4, 4, table)), p += 12;
    free(line);
    *out_len = (size_t)(p - png);
    return png;
}

double ecp_time_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* ============================================================================================
 * Descriptor
 */
static const char *const k_caps[] = {
    "pause", "menu", "states", "state_thumbnails", "reset", "fast_forward", "slow_motion",
    "rewind", "settings", "screenshot", "perf", "hud", "notify", "volume", "cheats", "discs",
    "applets", "oneshot", "configure", NULL};

static int in_list(const char *const *list, const char *s) {
    for (; list && *list; list++)
        if (!strcmp(*list, s)) return 1;
    return 0;
}

int ecp_descriptor_has(const ecp_descriptor *d, const char *cap) { return in_list(d->capabilities, cap); }

static void w_list(ecp_writer *w, const char *key, const char *const *list) {
    ecp_w_key(w, key);
    ecp_w_array_begin(w);
    for (; list && *list; list++) ecp_w_string(w, *list);
    ecp_w_array_end(w);
}

void ecp_w_descriptor(ecp_writer *w, const ecp_descriptor *d) {
    ecp_w_kint(w, "ecp", ECP_PROTOCOL);
    ecp_w_kstring(w, "id", d->id);
    ecp_w_kstring(w, "name", d->name);
    ecp_w_kstring(w, "version", d->version);
    ecp_w_kint(w, "level", d->level);
    w_list(w, "systems", d->systems);
    w_list(w, "extensions", d->extensions);
    w_list(w, "capabilities", d->capabilities);
    w_list(w, "needs", d->needs);
    if (d->settings_schema) ecp_w_kstring(w, "settings", "schema");
}

int ecp_print_descriptor(const ecp_descriptor *d) {
    ecp_writer w = {0};
    ecp_w_object_begin(&w);
    ecp_w_descriptor(&w, d);
    return ecp_oneshot_print(&w);
}

/* ============================================================================================
 * Command line
 */
static void cfg_error(ecp_config *cfg, const char *msg, const char *arg) {
    snprintf(cfg->error, sizeof cfg->error, "%s%s%s", msg, arg ? ": " : "", arg ? arg : "");
}

static int parse_transport(ecp_config *cfg, const char *v) {
    if (!strcmp(v, "stdio")) {
        cfg->transport = ECP_STDIO;
        return 0;
    }
    if (!strncmp(v, "tcp:", 4)) {
        const char *host = v + 4, *colon = strrchr(host, ':');
        size_t n = colon ? (size_t)(colon - host) : 0;
        if (!colon || n == 0 || n >= sizeof cfg->host) return cfg_error(cfg, "bad --ecp address", v), -1;
        memcpy(cfg->host, host, n);
        cfg->host[n] = 0;
        if (cfg->host[0] == '[' && cfg->host[n - 1] == ']')
            memmove(cfg->host, cfg->host + 1, n - 2), cfg->host[n - 2] = 0;
        if (strcmp(cfg->host, "127.0.0.1") && strcmp(cfg->host, "localhost") && strcmp(cfg->host, "::1"))
            return cfg_error(cfg, "--ecp tcp must be a loopback address", v), -1;
        cfg->port = atoi(colon + 1);
        if (cfg->port <= 0 || cfg->port > 65535) return cfg_error(cfg, "bad --ecp port", v), -1;
        cfg->transport = ECP_TCP;
        return 0;
    }
    return cfg_error(cfg, "unknown --ecp transport", v), -1;
}

int ecp_config_parse(ecp_config *cfg, int *argc, char **argv) {
    const char *token_env = "ECP_TOKEN";
    int out = 1;
    memset(cfg, 0, sizeof *cfg);
    for (int i = 1; i < *argc; i++) {
        const char *a = argv[i];
        const char *val = NULL;
        size_t n;
        if (!strcmp(a, "--ecp-describe")) {
            cfg->describe = 1;
            continue;
        }
        if (!strncmp(a, "--ecp", 5) && (a[5] == 0 || a[5] == '=' || a[5] == '-')) {
            const char *name = a;
            const char *eq = strchr(a, '=');
            n = eq ? (size_t)(eq - a) : strlen(a);
            if (eq) {
                val = eq + 1;
            } else if (i + 1 < *argc) {
                val = argv[++i];
            } else {
                return cfg_error(cfg, "missing value", a), -1;
            }
            if (n == 5 && !strncmp(name, "--ecp", 5)) {
                if (parse_transport(cfg, val)) return -1;
            } else if (n == 15 && !strncmp(name, "--ecp-token-env", 15)) {
                token_env = val;
            } else if (n == 13 && !strncmp(name, "--ecp-oneshot", 13)) {
                snprintf(cfg->oneshot, sizeof cfg->oneshot, "%s", val);
            } else {
                return cfg_error(cfg, "unknown option", a), -1;
            }
            continue;
        }
        argv[out++] = argv[i];
    }
    *argc = out;
    argv[out] = NULL;
    if (cfg->transport == ECP_TCP) {
        const char *t = getenv(token_env);
        if (!t || strlen(t) >= sizeof cfg->token || !*t)
            return cfg_error(cfg, "no session token in the environment variable", token_env), -1;
        snprintf(cfg->token, sizeof cfg->token, "%s", t);
    }
    return 0;
}

void ecp_config_tcp(ecp_config *cfg, int port, const char *token, const char *session) {
    memset(cfg, 0, sizeof *cfg);
    cfg->transport = ECP_TCP;
    snprintf(cfg->host, sizeof cfg->host, "127.0.0.1");
    cfg->port = port;
    snprintf(cfg->token, sizeof cfg->token, "%s", token ? token : "");
    snprintf(cfg->session, sizeof cfg->session, "%s", session ? session : "");
}

ecp_json *ecp_oneshot_read_request(void) {
    size_t len = 0, cap = 4096;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len == cap) {
            char *nb = (char *)realloc(buf, cap *= 2);
            if (!nb) break;
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len, stdin);
        if (!n) break;
        len += n;
    }
    ecp_json *v = len ? ecp_json_parse(buf, len) : NULL;
    free(buf);
    return v;
}

int ecp_oneshot_print(ecp_writer *w) {
    ecp_w_object_end(w);
    if (!w->failed) {
        fwrite(w->buf, 1, w->len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    ecp_w_free(w);
    return 0;
}

/* ============================================================================================
 * Connection
 */
struct ecp_conn {
    ecp_transport transport;
    ecp_sock sock;
#ifdef _WIN32
    HANDLE in, out;
    int in_is_pipe;
#else
    int in_fd, out_fd;
#endif
    ecp_mutex send_lock, id_lock;
    int open, closed_reported;
    int paused_reported; /* the last of paused/resumed sent was paused */
    unsigned char *rbuf;
    size_t rlen, rcap;
    const ecp_descriptor *desc;
    ecp_handlers h;
    void *user;
    int fe_level;
    unsigned fe_caps;
    char fe_name[64];
    int64_t next_id;
};

static int cap_bit(const char *cap) {
    for (int i = 0; k_caps[i]; i++)
        if (!strcmp(k_caps[i], cap)) return i;
    return -1;
}

static int raw_write(ecp_conn *c, const unsigned char *p, size_t n) {
    while (n) {
#ifdef _WIN32
        int k;
        if (c->transport == ECP_TCP) {
            k = send(c->sock, (const char *)p, n > 1u << 30 ? 1 << 30 : (int)n, 0);
        } else {
            DWORD wrote = 0;
            k = WriteFile(c->out, p, (DWORD)(n > 1u << 30 ? 1u << 30 : n), &wrote, NULL) ? (int)wrote : -1;
        }
#else
        ssize_t k = c->transport == ECP_TCP ? send(c->sock, p, n, MSG_NOSIGNAL) : write(c->out_fd, p, n);
        if (k < 0 && errno == EINTR) continue;
#endif
        if (k <= 0) return -1;
        p += k, n -= (size_t)k;
    }
    return 0;
}

static int send_frame(ecp_conn *c, const char *json, size_t len) {
    unsigned char hdr[4] = {(unsigned char)len, (unsigned char)(len >> 8),
                            (unsigned char)(len >> 16), (unsigned char)(len >> 24)};
    int rc = -1;
    if (len > ECP_MAX_FRAME) {
        ecp_log("not sending a frame of %zu bytes: over the 16 MiB limit", len);
        return -1;
    }
    mutex_lock(&c->send_lock);
    if (c->open) {
        rc = raw_write(c, hdr, 4) || raw_write(c, (const unsigned char *)json, len) ? -1 : 0;
        if (rc) c->open = 0;
    }
    mutex_unlock(&c->send_lock);
    return rc;
}

int ecp_send(ecp_conn *c, ecp_writer *w) {
    ecp_w_object_end(w);
    int rc = w->failed ? -1 : send_frame(c, w->buf, w->len);
    ecp_w_free(w);
    return rc;
}

void ecp_msg_event(ecp_writer *w, const char *type) {
    memset(w, 0, sizeof *w);
    ecp_w_object_begin(w);
    ecp_w_kstring(w, "type", type);
}

void ecp_msg_result(ecp_writer *w, int64_t id) {
    ecp_msg_event(w, "result");
    ecp_w_kint(w, "id", id);
    ecp_w_kbool(w, "ok", 1);
}

int64_t ecp_msg_request(ecp_conn *c, ecp_writer *w, const char *type) {
    mutex_lock(&c->id_lock);
    int64_t id = ++c->next_id;
    mutex_unlock(&c->id_lock);
    ecp_msg_event(w, type);
    ecp_w_kint(w, "id", id);
    return id;
}

int ecp_reply_ok(ecp_conn *c, int64_t id) {
    ecp_writer w;
    ecp_msg_result(&w, id);
    return ecp_send(c, &w);
}

void ecp_w_error(ecp_writer *w, const char *code, const char *message, const char *detail) {
    ecp_w_key(w, "error");
    ecp_w_object_begin(w);
    ecp_w_kstring(w, "code", code);
    if (message) ecp_w_kstring(w, "message", message);
    if (detail) ecp_w_kstring(w, "detail", detail);
    ecp_w_object_end(w);
}

int ecp_reply_error(ecp_conn *c, int64_t id, const char *code, const char *message,
                    const char *detail) {
    ecp_writer w;
    ecp_msg_event(&w, "result");
    ecp_w_kint(&w, "id", id);
    ecp_w_kbool(&w, "ok", 0);
    ecp_w_error(&w, code, message, detail);
    return ecp_send(c, &w);
}

static int send_simple(ecp_conn *c, const char *type) {
    ecp_writer w;
    ecp_msg_event(&w, type);
    return ecp_send(c, &w);
}

int ecp_send_loading(ecp_conn *c, double progress, const char *text) {
    ecp_writer w;
    ecp_msg_event(&w, "loading");
    if (progress >= 0) ecp_w_knumber(&w, "progress", progress > 1 ? 1 : progress);
    if (text) ecp_w_kstring(&w, "text", text);
    return ecp_send(c, &w);
}

int ecp_send_first_frame(ecp_conn *c) { return send_simple(c, "first_frame"); }
int ecp_send_running(ecp_conn *c) { return send_simple(c, "running"); }
int ecp_send_resumed(ecp_conn *c) {
    c->paused_reported = 0;
    return send_simple(c, "resumed");
}

int ecp_send_stopped(ecp_conn *c, const char *reason, const char *code, const char *message,
                     const char *detail) {
    ecp_writer w;
    ecp_msg_event(&w, "stopped");
    ecp_w_kstring(&w, "reason", reason);
    if (code) ecp_w_error(&w, code, message, detail);
    return ecp_send(c, &w);
}

int ecp_send_paused(ecp_conn *c, const char *reason) {
    ecp_writer w;
    c->paused_reported = 1;
    ecp_msg_event(&w, "paused");
    ecp_w_kstring(&w, "reason", reason);
    return ecp_send(c, &w);
}

int ecp_send_menu_request(ecp_conn *c, const void *pixels, int width, int height, int stride,
                          int flags) {
    ecp_writer w;
    if (!c->paused_reported && ecp_send_paused(c, "menu")) return -1;
    ecp_msg_event(&w, "menu_request");
    ecp_w_picture_rgba(&w, "frame", pixels, width, height, stride, flags);
    return ecp_send(c, &w);
}

int ecp_send_perf(ecp_conn *c, const ecp_perf *p) {
    ecp_writer w;
    ecp_msg_event(&w, "perf");
    ecp_w_knumber(&w, "fps", p->fps);
    ecp_w_knumber(&w, "target_fps", p->target_fps);
    ecp_w_knumber(&w, "speed", p->speed);
    ecp_w_knumber(&w, "frame_ms", p->frame_ms);
    if (p->cpu_ms >= 0) ecp_w_knumber(&w, "cpu_ms", p->cpu_ms);
    if (p->gpu_ms >= 0) ecp_w_knumber(&w, "gpu_ms", p->gpu_ms);
    if (p->shaders_building >= 0) ecp_w_kbool(&w, "shaders_building", p->shaders_building);
    return ecp_send(c, &w);
}

#ifdef _WIN32
static int g_wsa_started;
#endif

static ecp_sock tcp_connect(const ecp_config *cfg, char *err, size_t errlen) {
    char port[8];
    struct addrinfo hints, *res = NULL, *ai;
    ecp_sock s = ECP_BAD_SOCK;
#ifdef _WIN32
    if (!g_wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
            snprintf(err, errlen, "WSAStartup failed");
            return ECP_BAD_SOCK;
        }
        g_wsa_started = 1;
    }
#endif
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%d", cfg->port);
    if (getaddrinfo(cfg->host, port, &hints, &res)) {
        snprintf(err, errlen, "cannot resolve %s", cfg->host);
        return ECP_BAD_SOCK;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == ECP_BAD_SOCK) continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        ecp_closesocket(s);
        s = ECP_BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s == ECP_BAD_SOCK) {
        snprintf(err, errlen, "cannot connect to %s:%d", cfg->host, cfg->port);
        return s;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return s;
}

/* Takes the process's stdin/stdout for the channel; stdout then points at stderr. */
static int stdio_take(ecp_conn *c, char *err, size_t errlen) {
#ifdef _WIN32
    HANDLE self = GetCurrentProcess();
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE || !in || out == INVALID_HANDLE_VALUE || !out ||
        !DuplicateHandle(self, in, self, &c->in, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(self, out, self, &c->out, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        snprintf(err, errlen, "no standard input/output to use as the channel");
        return -1;
    }
    c->in_is_pipe = GetFileType(c->in) == FILE_TYPE_PIPE;
    fflush(stdout);
    if (_fileno(stdout) >= 0 && _fileno(stderr) >= 0) _dup2(_fileno(stderr), _fileno(stdout));
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
#else
    fflush(stdout);
    c->in_fd = dup(0);
    c->out_fd = dup(1);
    if (c->in_fd < 0 || c->out_fd < 0) {
        snprintf(err, errlen, "no standard input/output to use as the channel");
        return -1;
    }
    dup2(2, 1);
#endif
    return 0;
}

ecp_conn *ecp_open(const ecp_config *cfg, const ecp_descriptor *desc, const ecp_handlers *h,
                   void *user, char *err, size_t errlen) {
    char dummy[8];
    if (!err) err = dummy, errlen = sizeof dummy;
    ecp_conn *c = (ecp_conn *)calloc(1, sizeof *c);
    if (!c) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    c->transport = cfg->transport;
    c->sock = ECP_BAD_SOCK;
    c->desc = desc;
    if (h) c->h = *h;
    c->user = user;
    c->fe_level = -1;
    mutex_init(&c->send_lock);
    mutex_init(&c->id_lock);
    if (cfg->transport == ECP_TCP) {
        if ((c->sock = tcp_connect(cfg, err, errlen)) == ECP_BAD_SOCK) goto fail;
    } else if (cfg->transport == ECP_STDIO) {
        if (stdio_take(c, err, errlen)) goto fail;
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
    } else {
        snprintf(err, errlen, "no ECP transport");
        goto fail;
    }
    c->open = 1;
    if (cfg->session[0]) ecp_log("session %s", cfg->session);
    ecp_writer w;
    if (cfg->transport == ECP_TCP) {
        ecp_msg_event(&w, "auth");
        ecp_w_kstring(&w, "token", cfg->token);
        if (ecp_send(c, &w)) goto send_fail;
    }
    ecp_msg_event(&w, "hello");
    ecp_w_kint(&w, "protocol", ECP_PROTOCOL);
    ecp_w_descriptor(&w, desc);
    if (ecp_send(c, &w) == 0) return c;
send_fail:
    snprintf(err, errlen, "the channel closed during hello");
fail:
    ecp_close(c);
    return NULL;
}

int ecp_is_open(const ecp_conn *c) { return c && c->open; }
int ecp_frontend_level(const ecp_conn *c) { return c->fe_level; }
const char *ecp_frontend_name(const ecp_conn *c) { return c->fe_name; }

int ecp_frontend_has(const ecp_conn *c, const char *cap) {
    int bit = cap_bit(cap);
    return bit >= 0 && (c->fe_caps >> bit & 1);
}

void ecp_close(ecp_conn *c) {
    if (!c) return;
    mutex_lock(&c->send_lock);
    c->open = 0;
    if (c->sock != ECP_BAD_SOCK) ecp_closesocket(c->sock);
#ifdef _WIN32
    if (c->in) CloseHandle(c->in);
    if (c->out) CloseHandle(c->out);
#else
    if (c->transport == ECP_STDIO) {
        if (c->in_fd > 0) close(c->in_fd);
        if (c->out_fd > 0) close(c->out_fd);
    }
#endif
    mutex_unlock(&c->send_lock);
    mutex_destroy(&c->send_lock);
    mutex_destroy(&c->id_lock);
    free(c->rbuf);
    free(c);
}

/* Reads once; returns bytes read, 0 if nothing arrived in time, -1 when the channel is gone. */
static int raw_read(ecp_conn *c, int timeout_ms) {
    if (c->rcap - c->rlen < 65536) {
        size_t cap = c->rcap ? c->rcap * 2 : 131072;
        unsigned char *nb = (unsigned char *)realloc(c->rbuf, cap);
        if (!nb) return -1;
        c->rbuf = nb, c->rcap = cap;
    }
    unsigned char *dst = c->rbuf + c->rlen;
    size_t room = c->rcap - c->rlen;
    if (c->transport == ECP_TCP) {
#ifdef _WIN32
        fd_set set;
        struct timeval tv = {timeout_ms / 1000, timeout_ms % 1000 * 1000};
        FD_ZERO(&set);
        FD_SET(c->sock, &set);
        int r = select(0, &set, NULL, NULL, &tv);
        if (r < 0) return -1;
        if (r == 0) return 0;
        int k = recv(c->sock, (char *)dst, room > 1u << 30 ? 1 << 30 : (int)room, 0);
        return k > 0 ? k : -1;
#else
        struct pollfd pfd = {c->sock, POLLIN, 0};
        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0) return errno == EINTR ? 0 : -1;
        if (r == 0) return 0;
        ssize_t k = recv(c->sock, dst, room, 0);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
        return k > 0 ? (int)k : -1;
#endif
    }
#ifdef _WIN32
    DWORD avail = 0, got = 0;
    if (c->in_is_pipe) {
        double until = ecp_time_ms() + timeout_ms;
        for (;;) {
            if (!PeekNamedPipe(c->in, NULL, 0, NULL, &avail, NULL)) return -1;
            if (avail) break;
            if (ecp_time_ms() >= until) return 0;
            Sleep(1);
        }
        if (avail > room) avail = (DWORD)room;
    } else {
        avail = room > 65536 ? 65536 : (DWORD)room; /* a file: blocking read */
    }
    if (!ReadFile(c->in, dst, avail, &got, NULL) || !got) return -1;
    return (int)got;
#else
    struct pollfd pfd = {c->in_fd, POLLIN, 0};
    int r = poll(&pfd, 1, timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    ssize_t k = read(c->in_fd, dst, room);
    if (k < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
    return k > 0 ? (int)k : -1;
#endif
}

typedef struct route {
    const char *type;
    size_t offset; /* of the handler in ecp_handlers */
    const char *cap, *cap2;
} route;

#define R(t, c, c2) {#t, offsetof(ecp_handlers, t), c, c2}
static const route k_routes[] = {
    R(launch, NULL, NULL),          R(stop, NULL, NULL),
    R(pause, "pause", NULL),        R(resume, "pause", "menu"), /* resume also ends the menu */
    R(save_state, "states", NULL),  R(load_state, "states", NULL),
    R(list_states, "states", NULL), R(delete_state, "states", NULL),
    R(get_schema, "settings", NULL), R(get_settings, "settings", NULL),
    R(set_setting, "settings", NULL), R(reset, "reset", NULL),
    R(set_speed, "fast_forward", "slow_motion"), R(set_rewind, "rewind", NULL),
    R(screenshot, "screenshot", NULL), R(set_volume, "volume", NULL),
    R(hud_level, "hud", NULL),      R(notify, "notify", NULL),
    R(set_cheats, "cheats", NULL),  R(list_discs, "discs", NULL),
    R(set_disc, "discs", NULL),
};
#undef R

static void on_hello(ecp_conn *c, const ecp_json *msg) {
    if (ecp_json_int(ecp_json_get(msg, "protocol"), 0) != ECP_PROTOCOL) {
        ecp_log("the frontend speaks protocol %d; closing",
                (int)ecp_json_int(ecp_json_get(msg, "protocol"), 0));
        c->open = 0;
        return;
    }
    const ecp_json *caps = ecp_json_get(msg, "capabilities");
    c->fe_level = (int)ecp_json_int(ecp_json_get(msg, "level"), 1);
    c->fe_caps = 0;
    for (size_t i = 0; i < ecp_json_len(caps); i++) {
        const char *cap = ecp_json_str(ecp_json_at(caps, i), "");
        int bit = cap_bit(cap);
        if (bit >= 0 && ecp_descriptor_has(c->desc, cap)) c->fe_caps |= 1u << bit;
    }
    snprintf(c->fe_name, sizeof c->fe_name, "%s", ecp_json_str(ecp_json_get(msg, "frontend"), ""));
    if (c->h.hello) c->h.hello(c->user, c, msg);
}

static void dispatch(ecp_conn *c, const ecp_json *msg) {
    const char *type = ecp_json_str(ecp_json_get(msg, "type"), NULL);
    const ecp_json *idv = ecp_json_get(msg, "id");
    if (!type) {
        ecp_log("ignoring a message without type");
        return;
    }
    if (!strcmp(type, "hello")) {
        on_hello(c, msg);
        return;
    }
    if (!strcmp(type, "result")) {
        if (c->h.result) c->h.result(c->user, c, ecp_json_int(idv, -1), msg);
        return;
    }
    if (ecp_json_type_of(idv) != ECP_JSON_NUMBER) return; /* an event: none go this way yet */
    ecp_request req = {ecp_json_int(idv, -1), type, msg};
    for (size_t i = 0; i < sizeof k_routes / sizeof *k_routes; i++) {
        const route *r = &k_routes[i];
        if (strcmp(r->type, type)) continue;
        ecp_handler fn = *(const ecp_handler *)((const char *)&c->h + r->offset);
        int allowed = !r->cap || ecp_descriptor_has(c->desc, r->cap) ||
                      (r->cap2 && ecp_descriptor_has(c->desc, r->cap2));
        if (fn && allowed) {
            fn(c->user, c, &req);
            return;
        }
        break;
    }
    ecp_reply_error(c, req.id, "unsupported", NULL, type);
}

int ecp_poll(ecp_conn *c, int timeout_ms) {
    int dispatched = 0;
    if (c->open) {
        int r = raw_read(c, timeout_ms);
        while (r > 0) {
            c->rlen += (size_t)r;
            r = raw_read(c, 0);
        }
        if (r < 0) c->open = 0;
    }
    size_t at = 0;
    while (c->rlen - at >= 4) {
        const unsigned char *p = c->rbuf + at;
        uint32_t len = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        if (len > ECP_MAX_FRAME) {
            ecp_log("frame of %u bytes is over the limit; closing", len);
            c->open = 0, at = c->rlen;
            break;
        }
        if (c->rlen - at - 4 < len) break;
        ecp_json *msg = ecp_json_parse((const char *)p + 4, len);
        at += 4 + (size_t)len;
        if (!msg || ecp_json_type_of(msg) != ECP_JSON_OBJECT) {
            ecp_log("ignoring a frame that is not a JSON object");
        } else {
            dispatch(c, msg);
            dispatched++;
        }
        ecp_json_free(msg);
    }
    if (at) memmove(c->rbuf, c->rbuf + at, c->rlen - at), c->rlen -= at;
    if (!c->open) {
        if (!c->closed_reported) {
            c->closed_reported = 1;
            if (c->h.closed) c->h.closed(c->user, c);
        }
        return -1;
    }
    return dispatched;
}
