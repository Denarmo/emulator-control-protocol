/* Emulator Control Protocol 1: engine-side library.
 *
 * MIT licence, see LICENSE in the repository root. Portable C11, no dependencies beyond the C
 * runtime (plus ws2_32 on Windows). Builds on Windows, Linux, macOS and Android (NDK).
 *
 * Typical use from an emulator's main loop:
 *
 *     ecp_config cfg;
 *     if (ecp_config_parse(&cfg, &argc, argv) != 0) { fprintf(stderr, "%s\n", cfg.error); return 2; }
 *     if (cfg.describe) return ecp_print_descriptor(&desc);
 *     ecp_conn *c = cfg.transport ? ecp_open(&cfg, &desc, &handlers, app, err, sizeof err) : NULL;
 *     while (running) {
 *         if (c && ecp_poll(c, 0) < 0) break;      // dispatches requests to the handlers
 *         run_one_frame();
 *     }
 *
 * ecp_poll() and the handlers run on the thread that calls ecp_poll(). Everything that sends
 * (results, events) may be called from any thread.
 */
#ifndef ECP_H
#define ECP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECP_PROTOCOL 1
#define ECP_MAX_FRAME (16u * 1024u * 1024u)

/* ---------------------------------------------------------------------------------------------
 * JSON reader. ecp_json_parse() builds a read-only tree; every accessor accepts NULL and returns
 * the fallback, so lookups chain without checks: ecp_json_int(ecp_json_get(msg, "slot"), -1).
 */
typedef enum ecp_json_type {
    ECP_JSON_NULL, ECP_JSON_BOOL, ECP_JSON_NUMBER, ECP_JSON_STRING, ECP_JSON_ARRAY, ECP_JSON_OBJECT
} ecp_json_type;

typedef struct ecp_json ecp_json;

ecp_json *ecp_json_parse(const char *text, size_t len);
void ecp_json_free(ecp_json *v);

ecp_json_type ecp_json_type_of(const ecp_json *v); /* ECP_JSON_NULL for NULL */
const ecp_json *ecp_json_get(const ecp_json *object, const char *key);
size_t ecp_json_len(const ecp_json *v);                       /* array items or object members */
const ecp_json *ecp_json_at(const ecp_json *v, size_t index); /* array item or member value */
const char *ecp_json_key_at(const ecp_json *object, size_t index);
const char *ecp_json_str(const ecp_json *v, const char *fallback);
double ecp_json_num(const ecp_json *v, double fallback);
int64_t ecp_json_int(const ecp_json *v, int64_t fallback);
int ecp_json_bool(const ecp_json *v, int fallback);

/* ---------------------------------------------------------------------------------------------
 * JSON writer into a growing buffer. Zero-initialise, or use ecp_msg_*() below. Allocation
 * failures are sticky (failed = 1) and make the send fail.
 */
typedef struct ecp_writer {
    char *buf;
    size_t len, cap;
    int comma; /* a value was written at this level */
    int failed;
} ecp_writer;

void ecp_w_free(ecp_writer *w);
void ecp_w_object_begin(ecp_writer *w);
void ecp_w_object_end(ecp_writer *w);
void ecp_w_array_begin(ecp_writer *w);
void ecp_w_array_end(ecp_writer *w);
void ecp_w_key(ecp_writer *w, const char *key);
void ecp_w_string(ecp_writer *w, const char *s);
void ecp_w_number(ecp_writer *w, double v);
void ecp_w_int(ecp_writer *w, int64_t v);
void ecp_w_bool(ecp_writer *w, int v);
void ecp_w_null(ecp_writer *w);
void ecp_w_raw(ecp_writer *w, const char *json); /* an already encoded value */
void ecp_w_json(ecp_writer *w, const ecp_json *v); /* copy a parsed value */
/* key + value shortcuts */
void ecp_w_kstring(ecp_writer *w, const char *key, const char *s);
void ecp_w_knumber(ecp_writer *w, const char *key, double v);
void ecp_w_kint(ecp_writer *w, const char *key, int64_t v);
void ecp_w_kbool(ecp_writer *w, const char *key, int v);

/* "error":{"code":...,"message":...,"detail":...}; message and detail may be NULL. */
void ecp_w_error(ecp_writer *w, const char *code, const char *message, const char *detail);
/* A picture value: {"png":"<base64>","width":w,"height":h}. */
void ecp_w_picture_png(ecp_writer *w, const char *key, const void *png, size_t png_len, int width,
                       int height);
/* Encodes 8-bit RGBA (or BGRA with ECP_PNG_BGRA) pixels; alpha is dropped. Frames over 8 MiB of
 * RGB are halved until they fit, so a picture always stays well under the frame limit. */
void ecp_w_picture_rgba(ecp_writer *w, const char *key, const void *pixels, int width, int height,
                        int stride, int flags);

/* ---------------------------------------------------------------------------------------------
 * Helpers.
 */
#define ECP_PNG_BGRA 1
/* A PNG of 8-bit RGB from RGBA/BGRA pixels, stored (uncompressed) deflate. Free with free(). */
unsigned char *ecp_png_encode(const void *pixels, int width, int height, int stride, int flags,
                              size_t *out_len);
/* Base64 (standard alphabet, padded). Both return malloc'd buffers; free with free(). */
char *ecp_base64_encode(const void *data, size_t len, size_t *out_len);
unsigned char *ecp_base64_decode(const char *text, size_t len, size_t *out_len);
/* Monotonic milliseconds. */
double ecp_time_ms(void);

/* ---------------------------------------------------------------------------------------------
 * Descriptor (section 2). Lists are NULL-terminated; the strings must outlive the connection.
 */
typedef struct ecp_descriptor {
    const char *id;
    const char *name;
    const char *version;
    int level;
    const char *const *systems;
    const char *const *extensions;
    const char *const *capabilities;
    const char *const *needs;
    int settings_schema; /* 1: answers get_schema and the schema one-shot */
} ecp_descriptor;

int ecp_descriptor_has(const ecp_descriptor *d, const char *capability);
void ecp_w_descriptor(ecp_writer *w, const ecp_descriptor *d); /* the fields, inside an object */
/* Prints the descriptor as one JSON object on standard output; returns 0 (an exit code). */
int ecp_print_descriptor(const ecp_descriptor *d);

/* ---------------------------------------------------------------------------------------------
 * Command line (section 3, section 7).
 */
typedef enum ecp_transport { ECP_NONE = 0, ECP_STDIO, ECP_TCP } ecp_transport;

typedef struct ecp_config {
    ecp_transport transport;
    char host[64];
    int port;
    char token[65];
    char session[64];
    int describe;        /* --ecp-describe */
    char oneshot[32];    /* --ecp-oneshot <command>, empty if none */
    char error[128];
} ecp_config;

/* Reads --ecp, --ecp-token-env, --ecp-describe and --ecp-oneshot and removes them from argv, so
 * the emulator's own parser never sees them. Returns 0, or -1 with cfg->error set. */
int ecp_config_parse(ecp_config *cfg, int *argc, char **argv);
/* Android: the port and token from the intent extras (passed down through JNI). */
void ecp_config_tcp(ecp_config *cfg, int port, const char *token, const char *session);

/* One-shot commands: reads the request object from standard input (NULL if there is none) and
 * prints a result object on standard output. */
ecp_json *ecp_oneshot_read_request(void);
int ecp_oneshot_print(ecp_writer *w); /* closes the object, prints it, frees w; returns 0 */

/* ---------------------------------------------------------------------------------------------
 * Connection and dispatch.
 */
typedef struct ecp_conn ecp_conn;

typedef struct ecp_request {
    int64_t id;
    const char *type;
    const ecp_json *msg; /* the whole message; valid only during the callback */
} ecp_request;

/* A handler answers with ecp_reply_ok(), ecp_reply_error() or ecp_msg_result() + ecp_send(),
 * now or later from any thread (keep req->id). A NULL handler, or one for a capability the
 * descriptor does not list, is answered with `unsupported` by the library. */
typedef void (*ecp_handler)(void *user, ecp_conn *c, const ecp_request *req);

typedef struct ecp_handlers {
    /* level 1 */
    ecp_handler launch, stop;
    /* level 2, by capability */
    ecp_handler pause, resume;                                     /* pause; resume also menu */
    ecp_handler save_state, load_state, list_states, delete_state; /* states */
    ecp_handler get_schema, get_settings, set_setting;             /* settings */
    ecp_handler reset;                                             /* reset */
    ecp_handler set_speed;                                         /* fast_forward, slow_motion */
    ecp_handler set_rewind;                                        /* rewind */
    ecp_handler screenshot;                                        /* screenshot */
    ecp_handler set_volume;                                        /* volume */
    ecp_handler hud_level;                                         /* hud */
    ecp_handler notify;                                            /* notify */
    ecp_handler set_cheats;                                        /* cheats */
    ecp_handler list_discs, set_disc;                              /* discs */
    /* The frontend's hello; its level and capabilities are also kept, see ecp_frontend_has(). */
    void (*hello)(void *user, ecp_conn *c, const ecp_json *msg);
    /* A result to a request the engine sent (applet_request). */
    void (*result)(void *user, ecp_conn *c, int64_t id, const ecp_json *msg);
    /* The channel closed or failed. The engine saves and exits (section 4, End). */
    void (*closed)(void *user, ecp_conn *c);
} ecp_handlers;

/* Connects (TCP: to the loopback port, then auth), sends hello and returns. On stdio the
 * process's standard output is moved to standard error, so stray printf()s land in the log
 * instead of corrupting the channel. NULL on failure, with the reason in err. */
ecp_conn *ecp_open(const ecp_config *cfg, const ecp_descriptor *desc, const ecp_handlers *h,
                   void *user, char *err, size_t errlen);
/* Reads what has arrived, waiting up to timeout_ms (0: do not wait), and dispatches every
 * complete message. Returns how many were dispatched, or -1 once the channel is closed. */
int ecp_poll(ecp_conn *c, int timeout_ms);
int ecp_is_open(const ecp_conn *c);
void ecp_close(ecp_conn *c); /* closes the channel and frees c; not from inside a handler */

int ecp_frontend_level(const ecp_conn *c);                     /* -1 before its hello */
int ecp_frontend_has(const ecp_conn *c, const char *capability); /* listed in its hello */
const char *ecp_frontend_name(const ecp_conn *c);

/* Log sink for the library's own messages; default standard error. */
void ecp_set_log(void (*fn)(void *user, const char *line), void *user);

/* ---------------------------------------------------------------------------------------------
 * Sending. Build a message with ecp_msg_*(), add fields, then ecp_send(), which closes the
 * object, frames and writes it, and frees the writer. All return 0, or -1 if the channel failed.
 */
void ecp_msg_event(ecp_writer *w, const char *type);
void ecp_msg_result(ecp_writer *w, int64_t id); /* {"type":"result","id":id,"ok":true ... */
int64_t ecp_msg_request(ecp_conn *c, ecp_writer *w, const char *type); /* returns the new id */
int ecp_send(ecp_conn *c, ecp_writer *w);

int ecp_reply_ok(ecp_conn *c, int64_t id);
int ecp_reply_error(ecp_conn *c, int64_t id, const char *code, const char *message,
                    const char *detail);

/* Events (sections 5 and 6). */
int ecp_send_loading(ecp_conn *c, double progress /* < 0: none */, const char *text /* or NULL */);
int ecp_send_first_frame(ecp_conn *c);
int ecp_send_running(ecp_conn *c);
int ecp_send_stopped(ecp_conn *c, const char *reason, const char *error_code /* reason "error" */,
                     const char *message, const char *detail);
int ecp_send_paused(ecp_conn *c, const char *reason);
int ecp_send_resumed(ecp_conn *c);
/* Sends paused (reason "menu") unless paused was already reported, then menu_request with the
 * paused frame. Do not report your internal pauses (state save/load) with ecp_send_paused(). */
int ecp_send_menu_request(ecp_conn *c, const void *pixels, int width, int height, int stride,
                          int flags);

typedef struct ecp_perf {
    double fps, target_fps, speed, frame_ms;
    double cpu_ms, gpu_ms; /* < 0: not sent */
    int shaders_building;  /* < 0: not sent */
} ecp_perf;
int ecp_send_perf(ecp_conn *c, const ecp_perf *p);

#ifdef __cplusplus
}
#endif
#endif
