// A SteamVR action manifest, flattened for kl_openvr's IVRInput.
//
// IVRInput::SetActionManifestPath names a SteamVR `actions.json`; next to it
// sits the controller binding it declares for "oculus_touch"
// (`bindings_touch.json`, the binding_url in actions.json's default_bindings).
// This reads both — a small org.json-shaped parser, refusing anything it does
// not understand by name — and writes the S/A/P/H/C line records that
// kl_openvr_input.c parses into its binding table:
//
//   S <set>                              every actions.json action_sets[].name
//   A <action> <type>                    every actions.json actions[]
//   P <action> <hand>                    a /user/hand/{left,right}/pose/raw pose
//   H <action> <hand>                    a /user/hand/{left,right}/output/haptic
//   C <action> <hand> <slot> <kind> <on> <off> <source> <dir> <dz> <max> <overlap> <sticky>
//                                        one per supported bindings[set].sources[]
//                                        input; see flatten_source
//
// Every path is lowercased and checked against /[a-z0-9_/-]{1,254}; a path
// that fails aborts the whole flatten. Anything not turned into a record is
// logged as "pending ..." and counted (the skeleton arrays always are: no
// hand tracking here). Every refusal unwinds to one place and names its
// reason; nothing is invented to paper over a missing file.
//
// Transcribed on hla from the Alyx Jailbreak launcher's OriginalInputManifest
// (runtime/guest/kl_alyx_input.c there, which also installs the same records
// into its jailbreak host); this is the host-free half, generic to any
// OpenVR guest that ships SteamVR input files. Install it as
// kl_openvr_config.flatten_manifest.
#include "kl_openvr.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// The launcher read at most 0x40000 bytes of each JSON file ("JSON too large").
#define INPUT_JSON_MAX 0x40000

// ---- one refusal path ------------------------------------------------------

typedef struct arena_blk { struct arena_blk *next; } arena_blk;

typedef struct {
    jmp_buf     jb;
    char        why[512];
    arena_blk  *mem;
    FILE       *out;
    char       *text;     // the StringBuilder: freed on either exit
    size_t      n, cap;
} ctx;

__attribute__((noreturn, format(printf, 2, 3)))
static void fail(ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->why, sizeof c->why, fmt, ap);
    va_end(ap);
    longjmp(c->jb, 1);
}

static void *arena(ctx *c, size_t n) {
    arena_blk *b = malloc(sizeof *b + n);
    if (!b) fail(c, "out of memory");
    b->next = c->mem;
    c->mem = b;
    return b + 1;
}

static void ctx_free(ctx *c) {
    while (c->mem) { arena_blk *n = c->mem->next; free(c->mem); c->mem = n; }
    free(c->text);
    c->text = NULL;
}

// ---- org.json, the parts the Java touches ----------------------------------
//
// Android's JSONObject is a LinkedHashMap: keys() iterates in FILE order and a
// repeated key replaces the value in its first position. Both matter here —
// the source index in every C record is a running count in that order.

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct jv jv;
struct jv {
    int         t;
    int         b;
    double      num;
    const char *s;        // J_STR: the decoded string; J_NUM: the token as written
    unsigned    n;        // J_ARR / J_OBJ
    jv        **item;
    const char **key;
};

typedef struct { ctx *c; const char *p, *start, *end, *file; } jp;

__attribute__((noreturn))
static void jp_fail(jp *j, const char *what) {
    fail(j->c, "%s: %s at byte %ld", j->file, what, (long)(j->p - j->start));
}

// JSONTokener.nextClean: whitespace plus the three comment forms it skips.
static void jp_ws(jp *j) {
    for (;;) {
        while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
            j->p++;
        if (j->p + 1 < j->end && j->p[0] == '/' && j->p[1] == '*') {
            const char *e = strstr(j->p + 2, "*/");
            if (!e) jp_fail(j, "unterminated comment");
            j->p = e + 2;
        } else if ((j->p + 1 < j->end && j->p[0] == '/' && j->p[1] == '/') ||
                   (j->p < j->end && j->p[0] == '#')) {
            while (j->p < j->end && *j->p != '\n' && *j->p != '\r') j->p++;
        } else {
            return;
        }
    }
}

static void put_utf8(char **o, unsigned cp) {
    char *q = *o;
    if (cp < 0x80) *q++ = (char)cp;
    else if (cp < 0x800) { *q++ = (char)(0xc0 | cp >> 6); *q++ = (char)(0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) {
        *q++ = (char)(0xe0 | cp >> 12); *q++ = (char)(0x80 | ((cp >> 6) & 0x3f));
        *q++ = (char)(0x80 | (cp & 0x3f));
    } else {
        *q++ = (char)(0xf0 | cp >> 18); *q++ = (char)(0x80 | ((cp >> 12) & 0x3f));
        *q++ = (char)(0x80 | ((cp >> 6) & 0x3f)); *q++ = (char)(0x80 | (cp & 0x3f));
    }
    *o = q;
}

static int hex4(const char *p, unsigned *v) {
    *v = 0;
    for (int i = 0; i < 4; i++) {
        int ch = p[i], d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else return 0;
        *v = *v << 4 | (unsigned)d;
    }
    return 1;
}

static const char *jp_string(jp *j) {
    char q = *j->p++;                 // '"' (or the tokener's lenient '\'')
    const char *s = j->p;
    size_t cap = 0;
    while (s < j->end && *s != q) { if (*s == '\\') s++; s++; cap++; }
    if (s >= j->end) jp_fail(j, "unterminated string");
    char *out = arena(j->c, cap * 4 + 1), *o = out;
    while (*j->p != q) {
        char ch = *j->p++;
        if (ch != '\\') { *o++ = ch; continue; }
        ch = *j->p++;
        switch (ch) {
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (j->end - j->p < 4 || !hex4(j->p, &cp)) jp_fail(j, "bad \\u escape");
            j->p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && j->end - j->p >= 6 && j->p[0] == '\\' &&
                j->p[1] == 'u' && hex4(j->p + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                j->p += 6;
            }
            put_utf8(&o, cp);
            break;
        }
        default: *o++ = ch; break;    // \" \\ \/ and the tokener's "any other"
        }
    }
    j->p++;
    *o = 0;
    return out;
}

static jv *jp_value(jp *j, int depth);

static jv *jv_new(jp *j, int t) {
    jv *v = arena(j->c, sizeof *v);
    memset(v, 0, sizeof *v);
    v->t = t;
    return v;
}

static void jv_push(jp *j, jv *v, const char *key, jv *item) {
    if (key) {
        for (unsigned i = 0; i < v->n; i++)
            if (strcmp(v->key[i], key) == 0) { v->item[i] = item; return; }
    }
    if ((v->n & (v->n - 1)) == 0) {             // grow at 0, 1, 2, 4, 8, ...
        unsigned cap = v->n ? v->n * 2 : 1;
        jv **ni = arena(j->c, cap * sizeof *ni);
        if (v->n) memcpy(ni, v->item, v->n * sizeof *ni);
        v->item = ni;
        if (key || v->t == J_OBJ) {
            const char **nk = arena(j->c, cap * sizeof *nk);
            if (v->n) memcpy(nk, v->key, v->n * sizeof *nk);
            v->key = nk;
        }
    }
    if (v->t == J_OBJ) v->key[v->n] = key;
    v->item[v->n++] = item;
}

static jv *jp_value(jp *j, int depth) {
    if (depth > 64) jp_fail(j, "nesting too deep");
    jp_ws(j);
    if (j->p >= j->end) jp_fail(j, "unexpected end of input");
    char ch = *j->p;
    if (ch == '"' || ch == '\'') {
        jv *v = jv_new(j, J_STR);
        v->s = jp_string(j);
        return v;
    }
    if (ch == '{') {
        jv *v = jv_new(j, J_OBJ);
        j->p++;
        jp_ws(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return v; }
        for (;;) {
            jp_ws(j);
            if (j->p >= j->end || (*j->p != '"' && *j->p != '\'')) jp_fail(j, "expected a name");
            const char *k = jp_string(j);
            jp_ws(j);
            if (j->p >= j->end || (*j->p != ':' && *j->p != '=')) jp_fail(j, "expected ':'");
            j->p++;
            if (j->p < j->end && *j->p == '>') j->p++;          // the tokener's "=>"
            jv_push(j, v, k, jp_value(j, depth + 1));
            jp_ws(j);
            if (j->p < j->end && *j->p == '}') { j->p++; return v; }
            if (j->p >= j->end || (*j->p != ',' && *j->p != ';')) jp_fail(j, "expected ',' or '}'");
            j->p++;
        }
    }
    if (ch == '[') {
        jv *v = jv_new(j, J_ARR);
        j->p++;
        jp_ws(j);
        if (j->p < j->end && *j->p == ']') { j->p++; return v; }
        for (;;) {
            jv_push(j, v, NULL, jp_value(j, depth + 1));
            jp_ws(j);
            if (j->p < j->end && *j->p == ']') { j->p++; return v; }
            if (j->p >= j->end || (*j->p != ',' && *j->p != ';')) jp_fail(j, "expected ',' or ']'");
            j->p++;
        }
    }
    // A literal: true/false/null or a number (the tokener reads the token up to
    // a delimiter and then decides; an unquoted word is a string to it, and a
    // string where an object was required fails later as a type mismatch).
    const char *s = j->p;
    while (j->p < j->end && !strchr(" \t\n\r,:]}/\\\"[{;=#", *j->p)) j->p++;
    size_t n = (size_t)(j->p - s);
    if (!n) jp_fail(j, "expected a value");
    char *tok = arena(j->c, n + 1);
    memcpy(tok, s, n);
    tok[n] = 0;
    if (!strcasecmp(tok, "null")) return jv_new(j, J_NULL);
    if (!strcasecmp(tok, "true") || !strcasecmp(tok, "false")) {
        jv *v = jv_new(j, J_BOOL);
        v->b = tok[0] == 't' || tok[0] == 'T';
        return v;
    }
    char *e;
    double d = strtod(tok, &e);
    jv *v;
    if (*e == 0 && (isdigit((unsigned char)tok[0]) || tok[0] == '-' || tok[0] == '.')) {
        v = jv_new(j, J_NUM);
        v->num = d;
    } else {
        v = jv_new(j, J_STR);
    }
    v->s = tok;
    return v;
}

static jv *json_read(ctx *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fail(c, "%s (%s)", path, strerror(errno));   // FileNotFoundException's text
    size_t cap = 8192, n = 0;
    char *buf = malloc(cap + 1);
    for (;;) {
        if (!buf) { fclose(f); fail(c, "out of memory"); }
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (n > INPUT_JSON_MAX) { free(buf); fclose(f); fail(c, "%s: JSON too large", path); }
        if (r == 0) break;
        if (n == cap) { cap *= 2; buf = realloc(buf, cap + 1); }
    }
    fclose(f);
    buf[n] = 0;
    char *text = arena(c, n + 1);
    memcpy(text, buf, n + 1);
    free(buf);
    if (strlen(text) != n) fail(c, "%s: a NUL byte in the JSON", path);
    jp j = { c, text, text, text + n, path };
    jv *v = jp_value(&j, 0);
    jp_ws(&j);
    if (v->t != J_OBJ) fail(c, "%s: not a JSON object", path);
    // new JSONObject(String) reads one value and ignores what follows it.
    return v;
}

// JSONObject.opt / get
static jv *j_opt(jv *o, const char *k) {
    if (!o || o->t != J_OBJ) return NULL;
    for (unsigned i = 0; i < o->n; i++) if (strcmp(o->key[i], k) == 0) return o->item[i];
    return NULL;
}
static jv *j_get(ctx *c, jv *o, const char *k) {
    jv *v = j_opt(o, k);
    if (!v) fail(c, "No value for %s", k);
    return v;
}
// JSON.toString(Object): String.valueOf of whatever is there. An object or an
// array stringifies to JSON text, which starts with '{' or '[' — never a path
// and never equal to any name compared below, so its exact text is not needed.
static const char *j_str(jv *v) {
    switch (v->t) {
    case J_NULL: return "null";
    case J_BOOL: return v->b ? "true" : "false";
    case J_NUM:
    case J_STR:  return v->s;
    case J_ARR:  return "[...]";
    default:     return "{...}";
    }
}
static const char *j_get_string(ctx *c, jv *o, const char *k) { return j_str(j_get(c, o, k)); }
static const char *j_opt_string(jv *o, const char *k, const char *def) {
    jv *v = j_opt(o, k);
    return v ? j_str(v) : def;
}
static jv *j_get_array(ctx *c, jv *o, const char *k) {
    jv *v = j_get(c, o, k);
    if (v->t != J_ARR) fail(c, "Value at %s is not a JSONArray", k);
    return v;
}
static jv *j_get_object(ctx *c, jv *o, const char *k) {
    jv *v = j_get(c, o, k);
    if (v->t != J_OBJ) fail(c, "Value at %s is not a JSONObject", k);
    return v;
}
static jv *j_opt_array(jv *o, const char *k) {
    jv *v = j_opt(o, k);
    return v && v->t == J_ARR ? v : NULL;
}
static jv *j_opt_object(jv *o, const char *k) {
    jv *v = j_opt(o, k);
    return v && v->t == J_OBJ ? v : NULL;
}
static jv *j_at_object(ctx *c, jv *a, unsigned i) {   // JSONArray.getJSONObject
    if (a->item[i]->t != J_OBJ) fail(c, "Value at %u is not a JSONObject", i);
    return a->item[i];
}
// JSON.toDouble: a number, or a string Double.valueOf accepts.
static double j_opt_double(jv *o, const char *k, double def) {
    jv *v = j_opt(o, k);
    if (!v) return def;
    if (v->t == J_NUM) return v->num;
    if (v->t != J_STR) return def;
    const char *s = v->s;
    while (*s && (unsigned char)*s <= ' ') s++;
    if (!*s) return def;
    char *e;
    double d = strtod(s, &e);
    if (e == s) return def;
    if (*e == 'd' || *e == 'D' || *e == 'f' || *e == 'F') e++;
    while (*e && (unsigned char)*e <= ' ') e++;
    return *e ? def : d;
}
// JSON.toBoolean: a boolean, or the string "true"/"false" in any case.
static int j_opt_bool(jv *o, const char *k, int def) {
    jv *v = j_opt(o, k);
    if (!v) return def;
    if (v->t == J_BOOL) return v->b;
    if (v->t == J_STR) {
        if (!strcasecmp(v->s, "true")) return 1;
        if (!strcasecmp(v->s, "false")) return 0;
    }
    return def;
}

// ---- the Java's two string helpers ----------------------------------------

// lower(): toLowerCase(Locale.ROOT). Only ASCII can survive path()'s regex, and
// the only comparisons made on unchecked text are with ASCII literals.
static const char *lower(ctx *c, const char *s) {
    size_t n = strlen(s);
    char *o = arena(c, n + 1);
    for (size_t i = 0; i <= n; i++) o[i] = (char)tolower((unsigned char)s[i]);
    return o;
}

// path(): lower(s).matches("/[a-z0-9_/-]{1,254}") or IllegalArgumentException("input path").
static const char *path(ctx *c, const char *s) {
    const char *l = lower(c, s);
    size_t n = strlen(l);
    int ok = l[0] == '/' && n >= 2 && n <= 255;
    for (size_t i = 1; ok && i < n; i++)
        ok = (l[i] >= 'a' && l[i] <= 'z') || (l[i] >= '0' && l[i] <= '9') ||
             l[i] == '_' || l[i] == '/' || l[i] == '-';
    if (!ok) fail(c, "input path (\"%s\")", s);
    return l;
}

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static int eq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

// ---- the output: a StringBuilder (ctx.text) --------------------------------

typedef ctx sb;

static void sb_add(sb *b, const char *s) {
    size_t n = strlen(s);
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (b->n + n + 1 > cap) cap *= 2;
        char *p = realloc(b->text, cap);
        if (!p) fail(b, "out of memory");
        b->text = p;
        b->cap = cap;
    }
    memcpy(b->text + b->n, s, n + 1);
    b->n += n;
}

static void sb_int(sb *b, long v) { char t[32]; snprintf(t, sizeof t, "%ld", v); sb_add(b, t); }

// StringBuilder.append(double) == Double.toString: the shortest digits that
// read back as the same double; plain between 1e-3 and 1e7 with at least one
// fraction digit ("1.0", "0.75"), else "d.dddE<exp>" ("1.0E-4"). The host
// reads each with istream >> float, so any exact spelling would do — this one
// is the Java's so a record here can be diffed against a Quest log.
static void sb_double(sb *b, double v) {
    if (isnan(v)) { sb_add(b, "NaN"); return; }
    if (isinf(v)) { sb_add(b, v < 0 ? "-Infinity" : "Infinity"); return; }
    if (v == 0) { sb_add(b, signbit(v) ? "-0.0" : "0.0"); return; }
    char e[40];
    for (int p = 1; p <= 17; p++) {
        snprintf(e, sizeof e, "%.*e", p - 1, v);
        if (strtod(e, NULL) == v) break;
    }
    // e = [-]d[.ddd]e[+-]XX
    char digits[24];
    size_t nd = 0;
    const char *q = e;
    int neg = *q == '-';
    if (neg) q++;
    for (; *q && *q != 'e'; q++) if (*q != '.') digits[nd++] = *q;
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = 0;
    int x = atoi(q + 1);
    char t[64], *o = t;
    if (neg) *o++ = '-';
    double a = fabs(v);
    if (a >= 1e-3 && a < 1e7) {
        if (x < 0) {
            *o++ = '0'; *o++ = '.';
            for (int i = -1; i > x; i--) *o++ = '0';
            for (size_t i = 0; i < nd; i++) *o++ = digits[i];
        } else {
            for (int i = 0; i <= x; i++) *o++ = (size_t)i < nd ? digits[i] : '0';
            *o++ = '.';
            if ((size_t)x + 1 < nd) for (size_t i = (size_t)x + 1; i < nd; i++) *o++ = digits[i];
            else *o++ = '0';
        }
        *o = 0;
    } else {
        *o++ = digits[0];
        *o++ = '.';
        if (nd > 1) for (size_t i = 1; i < nd; i++) *o++ = digits[i];
        else *o++ = '0';
        snprintf(o, sizeof t - (size_t)(o - t), "E%d", x);
    }
    sb_add(b, t);
}

// ---- the transcription -----------------------------------------------------

typedef struct { int sets, actions, bindings, movement, pending; } counts;

static void pending(ctx *c, const char *fmt, ...) {
    if (!c->out) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(c->out, "  [openvr-input] ");
    vfprintf(c->out, fmt, ap);
    fprintf(c->out, "\n");
    va_end(ap);
}

// The action -> type map (a HashMap; lookups only, order irrelevant).
typedef struct { unsigned n; const char **name, **type; } amap;
static const char *amap_get(amap *m, const char *k) {
    for (unsigned i = 0; i < m->n; i++) if (strcmp(m->name[i], k) == 0) return m->type[i];
    return NULL;
}

// One bindings[set].sources[] entry. `index` is the running source count over
// every set in file order — supported or not — which the C record carries.
static void flatten_source(ctx *c, counts *k, amap *types, const char *set,
                           jv *src, int index) {
    const char *spath = path(c, j_get_string(c, src, "path"));
    const char *mode = j_get_string(c, src, "mode");
    int hand = starts(spath, "/user/hand/left/input/") ? 0
             : starts(spath, "/user/hand/right/input/") ? 1 : -1;
    const char *leaf = strrchr(spath, '/') + 1;
    // The control: 0 trigger, 1 grip, 2 joystick, 3 the lower face button
    // (x left / a right), 4 the upper (y left / b right); -1 anything else
    // (a/b on the left, x/y on the right, thumbrest, system, ...).
    int comp = eq(leaf, "trigger") ? 0 : eq(leaf, "grip") ? 1 : eq(leaf, "joystick") ? 2
             : ((hand == 0 && eq(leaf, "x")) || (hand == 1 && eq(leaf, "a"))) ? 3
             : ((hand == 0 && eq(leaf, "y")) || (hand == 1 && eq(leaf, "b"))) ? 4 : -1;
    jv *params = j_opt_object(src, "parameters");

    // Which (control, mode) pairs the host models at all.
    int ok = hand >= 0 && comp >= 0 &&
             (eq(mode, "button") || eq(mode, "trigger") || eq(mode, "joystick") ||
              (comp == 2 && eq(mode, "dpad")));
    // ...and which parameters it models for them: an unknown one disables the
    // source rather than being silently dropped.
    if (params) {
        for (unsigned i = 0; i < params->n; i++) {
            const char *p = params->key[i];
            int known = eq(p, "click_activate_threshold") || eq(p, "click_deactivate_threshold") ||
                        eq(p, "haptic_amplitude");
            if (comp == 2 && eq(mode, "joystick"))
                known |= eq(p, "deadzone_pct") || eq(p, "maxzone_pct") || eq(p, "sticky_click");
            if (comp == 2 && eq(mode, "dpad"))
                known |= eq(p, "deadzone_pct") || eq(p, "overlap_pct") || eq(p, "sticky") ||
                         eq(p, "sub_mode");
            if (comp == 2 && eq(mode, "button"))
                known |= eq(p, "force_input");
            if (!known) ok = 0;
        }
    }
    int dpad = comp == 2 && eq(mode, "dpad");
    int force_position = comp == 2 && eq(mode, "button") && params &&
                         eq(j_opt_string(params, "force_input", ""), "position");
    if (dpad && params && !eq(j_opt_string(params, "sub_mode", "touch"), "touch")) ok = 0;
    if (params && j_opt(params, "force_input") && !force_position) ok = 0;

    double deadzone = params ? j_opt_double(params, "deadzone_pct", 0.0) / 100.0 : 0.0;
    double maxzone  = params ? j_opt_double(params, "maxzone_pct", 100.0) / 100.0 : 1.0;
    double overlap  = params ? j_opt_double(params, "overlap_pct", 0.0) / 100.0 : 0.0;
    int sticky      = params && j_opt_bool(params, "sticky", 0);
    double on  = params ? j_opt_double(params, "click_activate_threshold",
                                       force_position ? 0.25 : 0.75) : 0.75;
    double off = params ? j_opt_double(params, "click_deactivate_threshold",
                                       force_position ? 0.2 : 0.72) : 0.72;

    const char *setdir = lower(c, set);
    size_t sdl = strlen(setdir);
    sb *b = c;
    jv *inputs = j_get_object(c, src, "inputs");
    for (unsigned i = 0; i < inputs->n; i++) {
        const char *in = inputs->key[i];
        if (inputs->item[i]->t != J_OBJ) fail(c, "Value at %s is not a JSONObject", in);
        const char *action = path(c, j_get_string(c, inputs->item[i], "output"));
        const char *type = amap_get(types, action);   // NULL: not declared
        // kind: 0 digital, 1 analog (vector1), 2 stick (vector2), 3 dpad
        // direction, 4 digital read off a force_input=position stick; -1 none.
        int slot = comp, kind = -1, dir = 0;
        if (eq(in, "click")) {
            if (comp == 2 && !force_position) slot = 5;          // the stick's click
            if (eq(type, "boolean")) kind = force_position ? 4 : 0;
            else if (eq(type, "vector1") && comp >= 3) kind = 1;
        } else if (eq(in, "pull") && comp <= 1 && eq(type, "vector1")) {
            kind = 1;
        } else if (eq(in, "position") && comp == 2 && eq(type, "vector2")) {
            kind = 2;
        } else if (dpad && eq(type, "boolean")) {
            dir = eq(in, "east") ? 0 : eq(in, "north") ? 1 : eq(in, "west") ? 2
                : eq(in, "south") ? 3 : -1;
            if (dir >= 0) kind = 3;
        }
        // sticky_click turns the WHOLE source off from this input on (the
        // Java clears the source's flag, not a per-input one).
        if (eq(in, "click") && params && j_opt_bool(params, "sticky_click", 0)) ok = 0;

        if (ok && kind >= 0 && strncmp(action, setdir, sdl) == 0 && action[sdl] == '/') {
            sb_add(b, "C "); sb_add(b, action);
            sb_add(b, " "); sb_int(b, hand);
            sb_add(b, " "); sb_int(b, slot);
            sb_add(b, " "); sb_int(b, kind);
            sb_add(b, " "); sb_double(b, on);
            sb_add(b, " "); sb_double(b, off);
            sb_add(b, " "); sb_int(b, index);
            sb_add(b, " "); sb_int(b, dir);
            sb_add(b, " "); sb_double(b, deadzone);
            sb_add(b, " "); sb_double(b, maxzone);
            sb_add(b, " "); sb_double(b, overlap);
            sb_add(b, " "); sb_int(b, sticky);
            sb_add(b, "\n");
            k->bindings++;
            if (eq(setdir, "/actions/move")) k->movement++;
        } else {
            k->pending++;
            pending(c, "pending binding %s source=%s mode=%s slot=%s", action, spath, mode, in);
        }
    }
}

// The body of install(), from the canonical cfg directory on; the text is c->text.
static void flatten(ctx *c, const char *cfg, counts *k) {
    char file[PATH_MAX];
    snprintf(file, sizeof file, "%s/actions.json", cfg);
    jv *manifest = json_read(c, file);

    jv *defaults = j_get_array(c, manifest, "default_bindings");
    int declared = 0;
    for (unsigned i = 0; i < defaults->n; i++) {
        jv *d = j_at_object(c, defaults, i);
        if (eq(j_get_string(c, d, "controller_type"), "oculus_touch") &&
            eq(j_get_string(c, d, "binding_url"), "bindings_touch.json"))
            declared = 1;
    }
    if (!declared) fail(c, "original Touch binding not declared (%s)", file);

    snprintf(file, sizeof file, "%s/bindings_touch.json", cfg);
    jv *touch = json_read(c, file);
    if (!eq(j_get_string(c, touch, "controller_type"), "oculus_touch"))
        fail(c, "controller type (%s says \"%s\")", file, j_get_string(c, touch, "controller_type"));

    sb *b = c;
    jv *sets = j_get_array(c, manifest, "action_sets");
    for (unsigned i = 0; i < sets->n; i++) {
        sb_add(b, "S ");
        sb_add(b, path(c, j_get_string(c, j_at_object(c, sets, i), "name")));
        sb_add(b, "\n");
    }
    jv *actions = j_get_array(c, manifest, "actions");
    amap types = { 0, arena(c, (actions->n + 1) * sizeof(char *)),
                      arena(c, (actions->n + 1) * sizeof(char *)) };
    for (unsigned i = 0; i < actions->n; i++) {
        jv *a = j_at_object(c, actions, i);
        const char *name = path(c, j_get_string(c, a, "name"));
        const char *type = j_get_string(c, a, "type");
        if (amap_get(&types, name)) fail(c, "duplicate action (%s)", name);
        types.name[types.n] = name;
        types.type[types.n++] = type;
        sb_add(b, "A "); sb_add(b, name); sb_add(b, " "); sb_add(b, type); sb_add(b, "\n");
    }
    k->sets = (int)sets->n;
    k->actions = (int)actions->n;

    jv *bindings = j_get_object(c, touch, "bindings");
    int index = 0;
    for (unsigned s = 0; s < bindings->n; s++) {
        const char *set = bindings->key[s];
        jv *bs = bindings->item[s];
        if (bs->t != J_OBJ) fail(c, "Value at %s is not a JSONObject", set);
        const char *setdir = lower(c, set);
        size_t sdl = strlen(setdir);

        jv *poses = j_opt_array(bs, "poses");
        for (unsigned i = 0; poses && i < poses->n; i++) {
            jv *p = j_at_object(c, poses, i);
            const char *src = path(c, j_get_string(c, p, "path"));
            const char *out = path(c, j_get_string(c, p, "output"));
            int hand = eq(src, "/user/hand/left/pose/raw") ? 0
                     : eq(src, "/user/hand/right/pose/raw") ? 1 : -1;
            if (hand >= 0 && eq(amap_get(&types, out), "pose") &&
                strncmp(out, setdir, sdl) == 0 && out[sdl] == '/') {
                sb_add(b, "P "); sb_add(b, out); sb_add(b, " "); sb_int(b, hand); sb_add(b, "\n");
                k->bindings++;
            } else {
                k->pending++;
                pending(c, "pending pose %s source=%s", out, src);
            }
        }
        jv *haptics = j_opt_array(bs, "haptics");
        for (unsigned i = 0; haptics && i < haptics->n; i++) {
            jv *h = j_at_object(c, haptics, i);
            const char *dst = path(c, j_get_string(c, h, "path"));
            const char *out = path(c, j_get_string(c, h, "output"));
            int hand = eq(dst, "/user/hand/left/output/haptic") ? 0
                     : eq(dst, "/user/hand/right/output/haptic") ? 1 : -1;
            if (hand >= 0 && eq(amap_get(&types, out), "vibration") &&
                strncmp(out, setdir, sdl) == 0 && out[sdl] == '/') {
                sb_add(b, "H "); sb_add(b, out); sb_add(b, " "); sb_int(b, hand); sb_add(b, "\n");
                k->bindings++;
            } else {
                k->pending++;
                pending(c, "pending haptic %s source=%s", out, dst);
            }
        }
        jv *skel = j_opt_array(bs, "skeleton");
        if (skel) {
            k->pending += (int)skel->n;
            pending(c, "pending %s skeleton=%u", set, skel->n);
        }
        jv *sources = j_opt_array(bs, "sources");
        for (unsigned i = 0; sources && i < sources->n; i++)
            flatten_source(c, k, &types, set, j_at_object(c, sources, i), index++);
    }
    sb_add(b, "");
}

// ---- the entry point -------------------------------------------------------

int kl_openvr_manifest_flatten(const char *manifest_path, char **text_out, FILE *out) {
    static ctx c;
    memset(&c, 0, sizeof c);
    c.out = out;
    if (text_out) *text_out = NULL;
    if (setjmp(c.jb)) {
        if (out) {
            fprintf(out, "  [openvr-input] input unavailable; no fabricated handles: %s\n", c.why);
            fflush(out);
        }
        ctx_free(&c);
        return 1;
    }
    if (!manifest_path || !text_out) fail(&c, "no manifest path");
    char real[PATH_MAX], cfg[PATH_MAX];
    if (!realpath(manifest_path, real)) fail(&c, "%s: %s", manifest_path, strerror(errno));
    snprintf(cfg, sizeof cfg, "%s", real);
    char *slash = strrchr(cfg, '/');
    if (!slash) fail(&c, "%s: no directory", real);
    *slash = 0;

    counts k = { 0, 0, 0, 0, 0 };
    flatten(&c, cfg, &k);
    if (out) {
        fprintf(out, "  [openvr-input] manifest flattened \"%s\" sets=%d actions=%d bindings=%d "
                     "movement=%d pending=%d (%zu bytes)\n",
                real, k.sets, k.actions, k.bindings, k.movement, k.pending, c.n);
        fflush(out);
    }
    *text_out = strdup(c.text ? c.text : "");
    ctx_free(&c);
    return *text_out ? 0 : 1;
}
