/* ECP test engine: a fake emulator for testing frontends.
 *
 * It "plays" any file by drawing a moving pattern into an off-screen framebuffer. Its whole game
 * state is the frame counter, which the pattern shows, so a loaded state is easy to verify.
 *
 *   test-engine --ecp stdio [--menu-after <s>] [--fail <code>] [--data <dir>] [--without <cap>]
 *   test-engine --ecp tcp:127.0.0.1:<port>      (token in ECP_TOKEN)
 *   test-engine --ecp-describe | --ecp-oneshot describe|schema|status|verify
 *   test-engine <file> [--seconds <n>]           (level 0: plays headless for n seconds)
 */
#include "ecp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

#define BASE_W 320
#define BASE_H 240

static const char *const k_systems[] = {"nes", "snes", "gb", "gbc", "gba", "nds", "n64", "psx",
                                        "ps2", "psp", "gamecube", "wii", "switch", NULL};
static const char *const k_extensions[] = {"ecptest", "bin", "iso", "rom", NULL};
static const char *const k_caps[] = {
    "pause", "menu", "states", "state_thumbnails", "reset", "fast_forward", "slow_motion",
    "rewind", "settings", "screenshot", "perf", "hud", "notify", "volume", "cheats", "discs",
    "oneshot", NULL};
static const char *const k_needs[] = {NULL};

static const char *g_caps[sizeof k_caps / sizeof *k_caps]; /* k_caps minus --without */
static ecp_descriptor k_desc = {
    "io.github.ecp.testengine", "ECP Test Engine", "1.0.0", 2,
    k_systems, k_extensions, k_caps, k_needs, 1};

/* --without <capability>: leave one out, to test how a frontend copes. */
static void drop_capability(const char *cap) {
    size_t n = 0;
    for (const char *const *c = k_desc.capabilities; *c; c++)
        if (strcmp(*c, cap)) g_caps[n++] = *c;
    g_caps[n] = NULL;
    k_desc.capabilities = g_caps;
}

/* ---- settings -------------------------------------------------------------------------- */

typedef enum { TOGGLE, CHOICE, RANGE } setting_type;

typedef struct setting {
    const char *key, *label, *group, *applies;
    setting_type type;
    double min, max, def;            /* RANGE; TOGGLE uses def */
    const char *const *choices;      /* CHOICE: value, label, value, label, ..., NULL */
    const char *def_choice;
} setting;

static const char *const k_aspects[] = {"auto", "Automatic", "4:3", "4:3", "16:9", "16:9",
                                        "stretch", "Stretch", NULL};
static const char *const k_patterns[] = {"bars", "Bars", "checker", "Checkerboard", NULL};
static const char *const k_regions[] = {"ntsc", "NTSC (60 Hz)", "pal", "PAL (50 Hz)", NULL};

static const setting k_settings[] = {
    {"video.internal_scale", "Internal resolution", "Video", "now", RANGE, 1, 8, 1, NULL, NULL},
    {"video.aspect", "Aspect ratio", "Video", "now", CHOICE, 0, 0, 0, k_aspects, "auto"},
    {"audio.volume", "Volume", "Audio", "now", RANGE, 0, 100, 100, NULL, NULL},
    {"speed.limit", "Frame limiter", "Speed", "now", TOGGLE, 0, 0, 1, NULL, NULL},
    {"speed.fast_forward_factor", "Fast-forward speed", "Speed", "now", RANGE, 2, 10, 4, NULL, NULL},
    {"test.pattern", "Pattern", "Test", "now", CHOICE, 0, 0, 0, k_patterns, "bars"},
    {"test.region", "Region", "Test", "restart", CHOICE, 0, 0, 0, k_regions, "ntsc"},
};
#define N_SETTINGS (sizeof k_settings / sizeof *k_settings)

typedef struct value {
    double num;      /* RANGE, TOGGLE */
    char text[16];   /* CHOICE */
} value;

/* ---- engine state ----------------------------------------------------------------------- */

typedef enum { IDLE, LOADING, RUNNING } phase;

typedef struct app {
    ecp_conn *c;
    int quit;
    phase phase;
    int paused, menu_sent;
    int64_t frame; /* the game state */
    value values[N_SETTINGS];
    int fast_forward, rewind, hud, muted;
    double slow_factor;
    int disc, cheats;
    char content[1024], game_id[128], data_dir[1024];
    double menu_after, played_ms, load_started, next_frame, perf_at;
    int perf_frames, load_step;
    char fail_code[32];
} app;

static int setting_index(const char *key) {
    for (size_t i = 0; i < N_SETTINGS; i++)
        if (!strcmp(k_settings[i].key, key)) return (int)i;
    return -1;
}

static double num_setting(const app *a, const char *key) { return a->values[setting_index(key)].num; }
static const char *text_setting(const app *a, const char *key) { return a->values[setting_index(key)].text; }

static void reset_settings(app *a) {
    for (size_t i = 0; i < N_SETTINGS; i++) {
        a->values[i].num = k_settings[i].def;
        snprintf(a->values[i].text, sizeof a->values[i].text, "%s",
                 k_settings[i].def_choice ? k_settings[i].def_choice : "");
    }
}

/* Validates and applies one value. Returns NULL, or why it was refused. */
static const char *apply_setting(app *a, const char *key, const ecp_json *v) {
    int i = setting_index(key);
    if (i < 0) return "unknown key";
    const setting *s = &k_settings[i];
    switch (s->type) {
    case TOGGLE:
        if (ecp_json_type_of(v) != ECP_JSON_BOOL) return "expected a boolean";
        a->values[i].num = ecp_json_bool(v, 0);
        return NULL;
    case RANGE: {
        double n = ecp_json_num(v, -1e300);
        if (ecp_json_type_of(v) != ECP_JSON_NUMBER || n < s->min || n > s->max) return "out of range";
        a->values[i].num = n;
        return NULL;
    }
    case CHOICE:
        for (const char *const *c = s->choices; *c; c += 2)
            if (!strcmp(*c, ecp_json_str(v, ""))) {
                snprintf(a->values[i].text, sizeof a->values[i].text, "%s", *c);
                return NULL;
            }
        return "not one of the choices";
    }
    return "unknown type";
}

static void write_schema(ecp_writer *w) {
    ecp_w_key(w, "settings");
    ecp_w_array_begin(w);
    for (size_t i = 0; i < N_SETTINGS; i++) {
        const setting *s = &k_settings[i];
        ecp_w_object_begin(w);
        ecp_w_kstring(w, "key", s->key);
        ecp_w_kstring(w, "label", s->label);
        ecp_w_kstring(w, "group", s->group);
        ecp_w_kstring(w, "applies", s->applies);
        ecp_w_kstring(w, "type", s->type == TOGGLE ? "toggle" : s->type == RANGE ? "range" : "choice");
        if (s->type == TOGGLE) ecp_w_kbool(w, "default", s->def != 0);
        if (s->type == RANGE) {
            ecp_w_knumber(w, "min", s->min);
            ecp_w_knumber(w, "max", s->max);
            ecp_w_knumber(w, "step", 1);
            ecp_w_knumber(w, "default", s->def);
        }
        if (s->type == CHOICE) {
            ecp_w_key(w, "choices");
            ecp_w_array_begin(w);
            for (const char *const *c = s->choices; *c; c += 2) {
                ecp_w_array_begin(w);
                ecp_w_string(w, c[0]);
                ecp_w_string(w, c[1]);
                ecp_w_array_end(w);
            }
            ecp_w_array_end(w);
            ecp_w_kstring(w, "default", s->def_choice);
        }
        ecp_w_object_end(w);
    }
    ecp_w_array_end(w);
}

/* ---- drawing ---------------------------------------------------------------------------- */

static const unsigned short k_digits[10] = {/* 3x5 font, one bit per pixel, top row first */
    075557, 022222, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717};

static unsigned char *render(const app *a, int64_t frame, int scale, int *w, int *h) {
    *w = BASE_W * scale, *h = BASE_H * scale;
    unsigned char *px = (unsigned char *)malloc((size_t)*w * *h * 4);
    if (!px) return NULL;
    int checker = !strcmp(text_setting(a, "test.pattern"), "checker");
    unsigned t = (unsigned)frame;
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++) {
            unsigned char *p = px + ((size_t)y * *w + x) * 4;
            unsigned bx = (unsigned)x / scale, by = (unsigned)y / scale;
            if (checker) {
                unsigned on = ((bx + t) / 16 + by / 16) & 1;
                p[0] = on ? 230 : 30, p[1] = on ? 200 : 40, p[2] = (unsigned char)(by + t);
            } else {
                p[0] = (unsigned char)(bx + t * 2), p[1] = (unsigned char)(by + t);
                p[2] = (unsigned char)((bx ^ by) + t * 3);
            }
            p[3] = 255;
        }
    /* The frame counter in the top-left corner, 8 digits. */
    char digits[16];
    snprintf(digits, sizeof digits, "%08lld", (long long)(frame % 100000000));
    int cell = 3 * scale;
    for (int d = 0; d < 8; d++)
        for (int gy = 0; gy < 5; gy++)
            for (int gx = 0; gx < 3; gx++) {
                int on = k_digits[digits[d] - '0'] >> ((4 - gy) * 3 + (2 - gx)) & 1;
                for (int yy = 0; yy < cell; yy++)
                    for (int xx = 0; xx < cell; xx++) {
                        int x = (4 + d * 4 + gx) * cell + xx, y = (4 + gy) * cell + yy;
                        if (x >= *w || y >= *h) continue;
                        memset(px + ((size_t)y * *w + x) * 4, on ? 255 : 0, 3);
                    }
            }
    return px;
}

/* ---- states ----------------------------------------------------------------------------- */

static void state_path(const app *a, int slot, char *out, size_t n) {
    snprintf(out, n, "%s/%s/slot%02d.state", a->data_dir, a->game_id, slot);
}

static int read_state(const app *a, int slot, int64_t *frame, int64_t *when) {
    char path[1200];
    long long f, t;
    state_path(a, slot, path, sizeof path);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    int ok = fscanf(fp, "ecp-test-state 1 %lld %lld", &f, &t) == 2;
    fclose(fp);
    if (!ok) return -2;
    *frame = f, *when = t;
    return 0;
}

static void write_thumbnail(ecp_writer *w, const app *a, int64_t frame) {
    int tw, th;
    unsigned char *px = render(a, frame, 1, &tw, &th);
    if (px) ecp_w_picture_rgba(w, "thumbnail", px, tw, th, tw * 4, 0);
    free(px);
}

/* ---- request handlers ------------------------------------------------------------------- */

static int ready(app *a, ecp_conn *c, const ecp_request *r) {
    if (a->phase == RUNNING) return 1;
    ecp_reply_error(c, r->id, "busy", "The game is not running yet.", NULL);
    return 0;
}

static void on_launch(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    if (a->phase != IDLE) {
        ecp_reply_error(c, r->id, "busy", "A game is already running.", NULL);
        return;
    }
    if (a->fail_code[0]) {
        ecp_reply_error(c, r->id, a->fail_code, "The test engine was told to fail.", "--fail");
        return;
    }
    const char *content = ecp_json_str(ecp_json_get(r->msg, "content"), "");
    FILE *fp = fopen(content, "rb");
    if (!fp) {
        ecp_reply_error(c, r->id, "content_missing", "The game file was not found.", content);
        return;
    }
    fclose(fp);
    snprintf(a->content, sizeof a->content, "%s", content);
    const char *gid = ecp_json_str(ecp_json_get(r->msg, "game_id"), NULL);
    if (!gid || !*gid) {
        const char *slash = strrchr(content, '/'), *bslash = strrchr(content, '\\');
        gid = slash > bslash ? slash + 1 : bslash ? bslash + 1 : content;
    }
    size_t n = 0;
    for (; gid[n] && n + 1 < sizeof a->game_id; n++) {
        char ch = gid[n];
        int safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                   ch == '-' || ch == '.';
        a->game_id[n] = safe ? ch : '_';
    }
    a->game_id[n] = 0;
    const ecp_json *settings = ecp_json_get(r->msg, "settings");
    for (size_t i = 0; i < ecp_json_len(settings); i++) {
        const char *why = apply_setting(a, ecp_json_key_at(settings, i), ecp_json_at(settings, i));
        if (why) fprintf(stderr, "test-engine: launch setting %s ignored: %s\n", ecp_json_key_at(settings, i), why);
    }
    ecp_reply_ok(c, r->id);
    a->phase = LOADING;
    a->load_started = ecp_time_ms();
    a->load_step = 0;
    fprintf(stderr, "test-engine: launching %s (game %s)\n", a->content, a->game_id);
}

static void on_stop(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    ecp_reply_ok(c, r->id);
    ecp_send_stopped(c, "frontend", NULL, NULL, NULL);
    a->quit = 1;
}

static void on_pause(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    ecp_reply_ok(c, r->id);
    if (!a->paused) {
        a->paused = 1;
        ecp_send_paused(c, "frontend");
    }
}

static void on_resume(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    ecp_reply_ok(c, r->id);
    if (a->paused) {
        a->paused = 0;
        a->next_frame = ecp_time_ms();
        ecp_send_resumed(c);
    }
}

static void on_save_state(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    int slot = (int)ecp_json_int(ecp_json_get(r->msg, "slot"), -1);
    char path[1200];
    if (!ready(a, c, r)) return;
    if (slot < 0 || slot > 99) {
        ecp_reply_error(c, r->id, "protocol", NULL, "slot must be 0-99");
        return;
    }
    snprintf(path, sizeof path, "%s", a->data_dir);
    make_dir(path);
    snprintf(path, sizeof path, "%s/%s", a->data_dir, a->game_id);
    make_dir(path);
    state_path(a, slot, path, sizeof path);
    FILE *fp = fopen(path, "wb");
    int64_t now = (int64_t)time(NULL);
    if (!fp || fprintf(fp, "ecp-test-state 1 %lld %lld\n", (long long)a->frame, (long long)now) < 0) {
        if (fp) fclose(fp);
        ecp_reply_error(c, r->id, "internal", "The state could not be written.", path);
        return;
    }
    fclose(fp);
    ecp_writer w;
    ecp_msg_result(&w, r->id);
    ecp_w_kint(&w, "slot", slot);
    ecp_w_kint(&w, "time", now);
    write_thumbnail(&w, a, a->frame);
    ecp_send(c, &w);
    fprintf(stderr, "test-engine: saved frame %lld to slot %d\n", (long long)a->frame, slot);
}

static void on_load_state(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    int slot = (int)ecp_json_int(ecp_json_get(r->msg, "slot"), -1);
    int64_t frame, when;
    if (!ready(a, c, r)) return;
    int rc = slot < 0 || slot > 99 ? -1 : read_state(a, slot, &frame, &when);
    if (rc == -1) {
        ecp_reply_error(c, r->id, "slot_empty", "That slot is empty.", NULL);
        return;
    }
    if (rc == -2) {
        ecp_reply_error(c, r->id, "state_incompatible", "That state is from another version.", NULL);
        return;
    }
    a->frame = frame;
    ecp_reply_ok(c, r->id);
    fprintf(stderr, "test-engine: loaded frame %lld from slot %d\n", (long long)frame, slot);
}

static void on_list_states(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    if (!ready(a, c, r)) return;
    ecp_writer w;
    ecp_msg_result(&w, r->id);
    ecp_w_key(&w, "states");
    ecp_w_array_begin(&w);
    for (int slot = 0; slot <= 99; slot++) {
        int64_t frame, when;
        if (read_state(a, slot, &frame, &when)) continue;
        ecp_w_object_begin(&w);
        ecp_w_kint(&w, "slot", slot);
        ecp_w_kint(&w, "time", when);
        write_thumbnail(&w, a, frame);
        ecp_w_object_end(&w);
    }
    ecp_w_array_end(&w);
    ecp_send(c, &w);
}

static void on_delete_state(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    int slot = (int)ecp_json_int(ecp_json_get(r->msg, "slot"), -1);
    char path[1200];
    if (!ready(a, c, r)) return;
    state_path(a, slot, path, sizeof path);
    if (slot < 0 || slot > 99 || remove(path) != 0) {
        ecp_reply_error(c, r->id, "slot_empty", "That slot is empty.", NULL);
        return;
    }
    ecp_reply_ok(c, r->id);
}

static void on_get_schema(void *user, ecp_conn *c, const ecp_request *r) {
    (void)user;
    ecp_writer w;
    ecp_msg_result(&w, r->id);
    write_schema(&w);
    ecp_send(c, &w);
}

static void on_get_settings(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    ecp_writer w;
    ecp_msg_result(&w, r->id);
    ecp_w_key(&w, "values");
    ecp_w_object_begin(&w);
    for (size_t i = 0; i < N_SETTINGS; i++) {
        const setting *s = &k_settings[i];
        ecp_w_key(&w, s->key);
        if (s->type == TOGGLE) ecp_w_bool(&w, a->values[i].num != 0);
        else if (s->type == RANGE) ecp_w_number(&w, a->values[i].num);
        else ecp_w_string(&w, a->values[i].text);
    }
    ecp_w_object_end(&w);
    ecp_send(c, &w);
}

static void on_set_setting(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    const char *key = ecp_json_str(ecp_json_get(r->msg, "key"), "");
    const char *why = apply_setting(a, key, ecp_json_get(r->msg, "value"));
    if (why) {
        ecp_reply_error(c, r->id, setting_index(key) < 0 ? "unsupported" : "protocol", NULL, why);
        return;
    }
    ecp_writer w;
    ecp_msg_result(&w, r->id);
    ecp_w_kstring(&w, "applied", k_settings[setting_index(key)].applies);
    ecp_send(c, &w);
}

static void on_reset(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    if (!ready(a, c, r)) return;
    a->frame = 0;
    ecp_reply_ok(c, r->id);
}

/* set_speed sets the whole speed state: fast_forward false, factor 1, or neither is normal. */
static void on_set_speed(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    double factor = ecp_json_num(ecp_json_get(r->msg, "factor"), 1);
    a->fast_forward = ecp_json_bool(ecp_json_get(r->msg, "fast_forward"), 0);
    a->slow_factor = 1;
    if (a->fast_forward && factor > 1)
        a->values[setting_index("speed.fast_forward_factor")].num = factor > 10 ? 10 : factor;
    else if (!a->fast_forward && factor > 0 && factor < 1)
        a->slow_factor = factor < 0.1 ? 0.1 : factor;
    ecp_reply_ok(c, r->id);
}

static void on_set_rewind(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    a->rewind = ecp_json_bool(ecp_json_get(r->msg, "on"), 0);
    ecp_reply_ok(c, r->id);
}

static void on_screenshot(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    int w, h;
    if (!ready(a, c, r)) return;
    unsigned char *px = render(a, a->frame, (int)num_setting(a, "video.internal_scale"), &w, &h);
    if (!px) {
        ecp_reply_error(c, r->id, "out_of_memory", NULL, NULL);
        return;
    }
    const char *path = ecp_json_str(ecp_json_get(r->msg, "path"), NULL);
    ecp_writer res;
    ecp_msg_result(&res, r->id);
    if (path) {
        size_t len;
        unsigned char *png = ecp_png_encode(px, w, h, w * 4, 0, &len);
        FILE *fp = png ? fopen(path, "wb") : NULL;
        int ok = fp && fwrite(png, 1, len, fp) == len;
        if (fp) fclose(fp);
        free(png);
        if (!ok) {
            free(px);
            ecp_w_free(&res);
            ecp_reply_error(c, r->id, "internal", "The screenshot could not be written.", path);
            return;
        }
        ecp_w_kstring(&res, "path", path);
    } else {
        ecp_w_picture_rgba(&res, "image", px, w, h, w * 4, 0);
    }
    free(px);
    ecp_send(c, &res);
}

static void on_set_volume(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    const ecp_json *p = ecp_json_get(r->msg, "percent");
    if (p) {
        double v = ecp_json_num(p, 100);
        a->values[setting_index("audio.volume")].num = v < 0 ? 0 : v > 100 ? 100 : v;
    }
    a->muted = ecp_json_bool(ecp_json_get(r->msg, "mute"), a->muted);
    ecp_reply_ok(c, r->id);
}

static void on_hud_level(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    int level = (int)ecp_json_int(ecp_json_get(r->msg, "level"), -1);
    if (level < 0 || level > 3) {
        ecp_reply_error(c, r->id, "protocol", NULL, "level must be 0-3");
        return;
    }
    a->hud = level;
    ecp_reply_ok(c, r->id);
}

static void on_notify(void *user, ecp_conn *c, const ecp_request *r) {
    (void)user;
    fprintf(stderr, "test-engine: toast \"%s\" for %.1f s\n", ecp_json_str(ecp_json_get(r->msg, "text"), ""),
            ecp_json_num(ecp_json_get(r->msg, "seconds"), 3));
    ecp_reply_ok(c, r->id);
}

static void on_set_cheats(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    const ecp_json *list = ecp_json_get(r->msg, "cheats");
    a->cheats = 0;
    for (size_t i = 0; i < ecp_json_len(list); i++)
        a->cheats += ecp_json_bool(ecp_json_get(ecp_json_at(list, i), "enabled"), 1);
    fprintf(stderr, "test-engine: %d cheats enabled\n", a->cheats);
    ecp_reply_ok(c, r->id);
}

static void on_list_discs(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    ecp_writer w;
    ecp_msg_result(&w, r->id);
    ecp_w_key(&w, "discs");
    ecp_w_array_begin(&w);
    for (int i = 0; i < 2; i++) {
        char label[16];
        snprintf(label, sizeof label, "Disc %d", i + 1);
        ecp_w_object_begin(&w);
        ecp_w_kint(&w, "index", i);
        ecp_w_kstring(&w, "label", label);
        ecp_w_object_end(&w);
    }
    ecp_w_array_end(&w);
    ecp_w_kint(&w, "current", a->disc);
    ecp_send(c, &w);
}

static void on_set_disc(void *user, ecp_conn *c, const ecp_request *r) {
    app *a = (app *)user;
    int index = (int)ecp_json_int(ecp_json_get(r->msg, "index"), -1);
    if (index < 0 || index > 1) {
        ecp_reply_error(c, r->id, "content_invalid", "There is no such disc.", NULL);
        return;
    }
    a->disc = index;
    ecp_reply_ok(c, r->id);
}

static void on_closed(void *user, ecp_conn *c) {
    (void)c;
    ((app *)user)->quit = 1; /* nothing to save: states are written when they are made */
}

/* ---- main loop -------------------------------------------------------------------------- */

static double frame_interval(const app *a) {
    double fps = strcmp(text_setting(a, "test.region"), "pal") ? 60 : 50;
    double speed = a->fast_forward ? num_setting(a, "speed.fast_forward_factor") : a->slow_factor;
    if (!num_setting(a, "speed.limit")) return 1; /* unthrottled, capped at 1000 fps */
    return 1000.0 / (fps * speed);
}

static void step(app *a, double now) {
    if (a->phase == LOADING) {
        static const double at[] = {0, 100, 200};
        if (a->load_step < 3 && now - a->load_started >= at[a->load_step]) {
            ecp_send_loading(a->c, a->load_step / 2.0, a->load_step ? NULL : "Loading");
            a->load_step++;
        }
        if (a->load_step == 3 && now - a->load_started >= 300) {
            a->phase = RUNNING;
            a->next_frame = a->perf_at = now;
            a->perf_frames = 0;
            ecp_send_first_frame(a->c);
            ecp_send_running(a->c);
        }
        return;
    }
    if (a->phase != RUNNING) return;
    if (!a->paused && now >= a->next_frame) {
        double interval = frame_interval(a);
        a->frame += a->rewind ? (a->frame > 0 ? -1 : 0) : 1;
        a->played_ms += interval;
        a->perf_frames++;
        a->next_frame += interval;
        if (a->next_frame < now - 100) a->next_frame = now;
    }
    if (now - a->perf_at >= 1000) {
        double secs = (now - a->perf_at) / 1000.0, base = strcmp(text_setting(a, "test.region"), "pal") ? 60 : 50;
        ecp_perf p = {a->perf_frames / secs, a->paused ? 0 : base, a->perf_frames / secs / base,
                      a->perf_frames ? secs * 1000 / a->perf_frames : 0, 0.2, -1, 0};
        if (ecp_frontend_has(a->c, "perf")) ecp_send_perf(a->c, &p);
        a->perf_at = now;
        a->perf_frames = 0;
    }
    if (a->menu_after > 0 && !a->menu_sent && !a->paused && a->played_ms >= a->menu_after * 1000 &&
        ecp_frontend_has(a->c, "menu")) {
        int w, h;
        unsigned char *px = render(a, a->frame, 1, &w, &h);
        a->paused = a->menu_sent = 1;
        if (px) ecp_send_menu_request(a->c, px, w, h, w * 4, 0);
        free(px);
        fprintf(stderr, "test-engine: asked for the frontend's menu\n");
    }
}

static int oneshot(const app *a, const char *cmd) {
    ecp_json *req = ecp_oneshot_read_request();
    ecp_writer w = {0};
    int rc = 0;
    if (!strcmp(cmd, "describe")) {
        ecp_json_free(req);
        return ecp_print_descriptor(&k_desc);
    }
    ecp_w_object_begin(&w);
    if (!strcmp(cmd, "schema")) {
        ecp_w_kbool(&w, "ok", 1);
        write_schema(&w);
    } else if (!strcmp(cmd, "status")) {
        ecp_w_kbool(&w, "ok", 1);
        ecp_w_kbool(&w, "ready", 1);
        ecp_w_key(&w, "problems");
        ecp_w_array_begin(&w);
        ecp_w_array_end(&w);
    } else if (!strcmp(cmd, "verify")) {
        const char *content = ecp_json_str(ecp_json_get(req, "content"), "");
        FILE *fp = fopen(content, "rb");
        if (fp) {
            const char *slash = strrchr(content, '/'), *bslash = strrchr(content, '\\');
            fclose(fp);
            ecp_w_kbool(&w, "ok", 1);
            ecp_w_kstring(&w, "title", slash > bslash ? slash + 1 : bslash ? bslash + 1 : content);
            ecp_w_kstring(&w, "serial", "TEST-00000");
            ecp_w_kstring(&w, "region", "world");
        } else {
            rc = 1;
            ecp_w_kbool(&w, "ok", 0);
            ecp_w_error(&w, "content_missing", "The game file was not found.", content);
        }
    } else {
        rc = 1;
        ecp_w_kbool(&w, "ok", 0);
        ecp_w_error(&w, "unsupported", NULL, cmd);
    }
    (void)a;
    ecp_json_free(req);
    ecp_oneshot_print(&w);
    return rc;
}

static int level0(app *a, const char *file, double seconds) {
    FILE *fp = fopen(file, "rb");
    if (!fp) {
        fprintf(stderr, "test-engine: %s not found\n", file);
        return 1;
    }
    fclose(fp);
    double start = ecp_time_ms(), next = start;
    while (ecp_time_ms() - start < seconds * 1000) {
        double now = ecp_time_ms();
        if (now >= next) a->frame++, next += 1000.0 / 60;
#ifdef _WIN32
        Sleep(1);
#else
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
#endif
    }
    fprintf(stderr, "test-engine: played %s for %lld frames\n", file, (long long)a->frame);
    return 0;
}

int main(int argc, char **argv) {
    static app a;
    ecp_config cfg;
    const char *file = NULL;
    double seconds = 3;
    if (ecp_config_parse(&cfg, &argc, argv)) {
        fprintf(stderr, "test-engine: %s\n", cfg.error);
        return 2;
    }
    reset_settings(&a);
    a.slow_factor = 1;
    snprintf(a.data_dir, sizeof a.data_dir, "%s", getenv("ECP_TEST_DATA") ? getenv("ECP_TEST_DATA") : "test-engine-data");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--menu-after") && i + 1 < argc) a.menu_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fail") && i + 1 < argc) snprintf(a.fail_code, sizeof a.fail_code, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) snprintf(a.data_dir, sizeof a.data_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--without") && i + 1 < argc) drop_capability(argv[++i]);
        else file = argv[i];
    }
    if (cfg.describe) return ecp_print_descriptor(&k_desc);
    if (cfg.oneshot[0]) return oneshot(&a, cfg.oneshot);
    if (cfg.transport == ECP_NONE) {
        if (!file) {
            fprintf(stderr, "usage: test-engine --ecp stdio|tcp:127.0.0.1:<port> | <file>\n");
            return 2;
        }
        return level0(&a, file, seconds);
    }

    ecp_handlers h = {0};
    h.launch = on_launch, h.stop = on_stop, h.pause = on_pause, h.resume = on_resume;
    h.save_state = on_save_state, h.load_state = on_load_state;
    h.list_states = on_list_states, h.delete_state = on_delete_state;
    h.get_schema = on_get_schema, h.get_settings = on_get_settings, h.set_setting = on_set_setting;
    h.reset = on_reset, h.set_speed = on_set_speed, h.set_rewind = on_set_rewind;
    h.screenshot = on_screenshot, h.set_volume = on_set_volume, h.hud_level = on_hud_level;
    h.notify = on_notify, h.set_cheats = on_set_cheats;
    h.list_discs = on_list_discs, h.set_disc = on_set_disc;
    h.closed = on_closed;

    char err[256];
    a.c = ecp_open(&cfg, &k_desc, &h, &a, err, sizeof err);
    if (!a.c) {
        fprintf(stderr, "test-engine: %s\n", err);
        return 1;
    }
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    while (!a.quit) {
        double now = ecp_time_ms();
        int wait = 20;
        if (a.phase == RUNNING && !a.paused) {
            double until = a.next_frame - now;
            wait = until <= 0 ? 0 : until < 20 ? (int)until : 20;
        } else if (a.phase == LOADING) {
            wait = 5;
        }
        if (ecp_poll(a.c, wait) < 0) break;
        step(&a, ecp_time_ms());
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    ecp_close(a.c);
    fprintf(stderr, "test-engine: exiting\n");
    return 0;
}
