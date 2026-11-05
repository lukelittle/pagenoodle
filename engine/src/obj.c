/* Object model, buffers, small utilities. */
#include "pn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

/* ---------------- buffers ---------------- */
void pn_buf_init(pn_buf *b) { b->data = NULL; b->len = b->cap = 0; }
void pn_buf_free(pn_buf *b) { free(b->data); pn_buf_init(b); }
void pn_buf_reserve(pn_buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + extra + 1) nc *= 2;
    unsigned char *nd = (unsigned char *)realloc(b->data, nc);
    if (!nd) abort(); /* out of memory: stop cleanly rather than write through NULL */
    b->data = nd;
    b->cap = nc;
}
void pn_buf_put(pn_buf *b, const void *d, size_t n) {
    pn_buf_reserve(b, n);
    if (n) memcpy(b->data + b->len, d, n);
    b->len += n;
    b->data[b->len] = 0;
}
void pn_buf_putc(pn_buf *b, int c) { pn_buf_reserve(b, 1); b->data[b->len++] = (unsigned char)c; b->data[b->len] = 0; }
void pn_buf_puts(pn_buf *b, const char *s) { pn_buf_put(b, s, strlen(s)); }
void pn_buf_printf(pn_buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { pn_buf_put(b, tmp, (size_t)n); return; }
    pn_buf_reserve(b, (size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf((char *)b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}
void pn_buf_put_real(pn_buf *b, double v) {
    if (!isfinite(v)) v = 0;
    if (fabs(v - (double)(long long)v) < 1e-9 && fabs(v) < 1e15) { pn_buf_printf(b, "%lld", (long long)v); return; }
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.5f", v);
    /* trim trailing zeros */
    char *e = tmp + strlen(tmp) - 1;
    while (e > tmp && *e == '0') *e-- = 0;
    if (*e == '.') *e = 0;
    if (!strcmp(tmp, "-0")) strcpy(tmp, "0");
    pn_buf_puts(b, tmp);
}
unsigned char *pn_buf_steal(pn_buf *b, size_t *len) {
    unsigned char *d = b->data;
    if (len) *len = b->len;
    if (!d) d = (unsigned char *)calloc(1, 1);
    pn_buf_init(b);
    return d;
}

char *pn_strdup(const char *s) { size_t n = strlen(s); char *d = (char *)malloc(n + 1); memcpy(d, s, n + 1); return d; }

static char g_err[512];
void pn_set_error(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(g_err, sizeof g_err, fmt, ap); va_end(ap); }
const char *pn_get_error(void) { return g_err; }

/* ---------------- objects ---------------- */
static pn_obj g_null = { PN_NULL, -1, {0} };
static pn_obj *alloc_obj(pn_type t) { pn_obj *o = (pn_obj *)calloc(1, sizeof *o); o->type = t; o->refs = 1; return o; }
pn_obj *pn_null(void) { return &g_null; }
pn_obj *pn_new_bool(int b) { pn_obj *o = alloc_obj(PN_BOOL); o->u.b = !!b; return o; }
pn_obj *pn_new_int(long long i) { pn_obj *o = alloc_obj(PN_INT); o->u.i = i; return o; }
pn_obj *pn_new_real(double r) { pn_obj *o = alloc_obj(PN_REAL); o->u.r = r; return o; }
pn_obj *pn_new_string(const void *s, int len) {
    pn_obj *o = alloc_obj(PN_STRING);
    o->u.str.s = (unsigned char *)malloc((size_t)len + 1);
    if (len) memcpy(o->u.str.s, s, (size_t)len);
    o->u.str.s[len] = 0; o->u.str.len = len;
    return o;
}
pn_obj *pn_new_name(const char *s) { pn_obj *o = pn_new_string(s, (int)strlen(s)); o->type = PN_NAME; return o; }
pn_obj *pn_new_array(void) { return alloc_obj(PN_ARRAY); }
pn_obj *pn_new_dict(void) { return alloc_obj(PN_DICT); }
pn_obj *pn_new_ref(int num, int gen) { pn_obj *o = alloc_obj(PN_REF); o->u.ref.num = num; o->u.ref.gen = gen; return o; }
pn_obj *pn_new_stream(pn_obj *dict, unsigned char *data, size_t len) {
    pn_obj *o = alloc_obj(PN_STREAM);
    o->u.stm.dict = dict ? dict : pn_new_dict();
    o->u.stm.data = data; o->u.stm.len = len;
    return o;
}
pn_obj *pn_keep(pn_obj *o) { if (o && o->refs >= 0) o->refs++; return o; }
void pn_drop(pn_obj *o) {
    if (!o || o->refs < 0) return;
    if (--o->refs > 0) return;
    int i;
    switch (o->type) {
    case PN_STRING: case PN_NAME: free(o->u.str.s); break;
    case PN_ARRAY: for (i = 0; i < o->u.arr.len; i++) pn_drop(o->u.arr.items[i]); free(o->u.arr.items); break;
    case PN_DICT:
        for (i = 0; i < o->u.dict.len; i++) { free(o->u.dict.keys[i]); pn_drop(o->u.dict.vals[i]); }
        free(o->u.dict.keys); free(o->u.dict.vals); break;
    case PN_STREAM: pn_drop(o->u.stm.dict); free(o->u.stm.data); break;
    default: break;
    }
    free(o);
}

int pn_is(const pn_obj *o, pn_type t) { return o && o->type == t; }
int pn_is_name(const pn_obj *o, const char *n) { return o && o->type == PN_NAME && !strcmp((const char *)o->u.str.s, n); }
int pn_is_number(const pn_obj *o) { return o && (o->type == PN_INT || o->type == PN_REAL); }
double pn_num(const pn_obj *o, double def) {
    if (!o) return def;
    if (o->type == PN_INT) return (double)o->u.i;
    if (o->type == PN_REAL) return o->u.r;
    return def;
}
long long pn_int(const pn_obj *o, long long def) {
    if (!o) return def;
    if (o->type == PN_INT) return o->u.i;
    if (o->type == PN_REAL) return (long long)o->u.r;
    return def;
}
const char *pn_name(const pn_obj *o) { return (o && o->type == PN_NAME) ? (const char *)o->u.str.s : ""; }

int pn_arr_len(const pn_obj *a) { return (a && a->type == PN_ARRAY) ? a->u.arr.len : 0; }
pn_obj *pn_arr_get(const pn_obj *a, int i) { return (a && a->type == PN_ARRAY && i >= 0 && i < a->u.arr.len) ? a->u.arr.items[i] : NULL; }
void pn_arr_insert(pn_obj *a, int i, pn_obj *v) {
    if (!a || a->type != PN_ARRAY) { pn_drop(v); return; }
    if (a->u.arr.len == a->u.arr.cap) {
        a->u.arr.cap = a->u.arr.cap ? a->u.arr.cap * 2 : 8;
        a->u.arr.items = (pn_obj **)realloc(a->u.arr.items, sizeof(pn_obj *) * (size_t)a->u.arr.cap);
    }
    if (i < 0 || i > a->u.arr.len) i = a->u.arr.len;
    memmove(a->u.arr.items + i + 1, a->u.arr.items + i, sizeof(pn_obj *) * (size_t)(a->u.arr.len - i));
    a->u.arr.items[i] = v ? v : pn_null();
    a->u.arr.len++;
}
void pn_arr_push(pn_obj *a, pn_obj *v) { pn_arr_insert(a, -1, v); }
void pn_arr_remove(pn_obj *a, int i) {
    if (!a || a->type != PN_ARRAY || i < 0 || i >= a->u.arr.len) return;
    pn_drop(a->u.arr.items[i]);
    memmove(a->u.arr.items + i, a->u.arr.items + i + 1, sizeof(pn_obj *) * (size_t)(a->u.arr.len - i - 1));
    a->u.arr.len--;
}
void pn_arr_set(pn_obj *a, int i, pn_obj *v) {
    if (!a || a->type != PN_ARRAY || i < 0 || i >= a->u.arr.len) { pn_drop(v); return; }
    pn_drop(a->u.arr.items[i]);
    a->u.arr.items[i] = v ? v : pn_null();
}

static pn_obj *as_dict(const pn_obj *d) {
    if (!d) return NULL;
    if (d->type == PN_STREAM) return d->u.stm.dict;
    if (d->type == PN_DICT) return (pn_obj *)d;
    return NULL;
}
pn_obj *pn_dict_get(const pn_obj *d0, const char *key) {
    pn_obj *d = as_dict(d0);
    if (!d) return NULL;
    for (int i = 0; i < d->u.dict.len; i++)
        if (!strcmp(d->u.dict.keys[i], key)) return d->u.dict.vals[i];
    return NULL;
}
void pn_dict_put(pn_obj *d0, const char *key, pn_obj *v) {
    pn_obj *d = as_dict(d0);
    if (!d) { pn_drop(v); return; }
    for (int i = 0; i < d->u.dict.len; i++) {
        if (!strcmp(d->u.dict.keys[i], key)) {
            if (!v) { pn_dict_del(d, key); return; }
            pn_drop(d->u.dict.vals[i]);
            d->u.dict.vals[i] = v;
            return;
        }
    }
    if (!v) return;
    if (d->u.dict.len == d->u.dict.cap) {
        d->u.dict.cap = d->u.dict.cap ? d->u.dict.cap * 2 : 8;
        d->u.dict.keys = (char **)realloc(d->u.dict.keys, sizeof(char *) * (size_t)d->u.dict.cap);
        d->u.dict.vals = (pn_obj **)realloc(d->u.dict.vals, sizeof(pn_obj *) * (size_t)d->u.dict.cap);
    }
    d->u.dict.keys[d->u.dict.len] = pn_strdup(key);
    d->u.dict.vals[d->u.dict.len] = v;
    d->u.dict.len++;
}
void pn_dict_del(pn_obj *d0, const char *key) {
    pn_obj *d = as_dict(d0);
    if (!d) return;
    for (int i = 0; i < d->u.dict.len; i++) {
        if (!strcmp(d->u.dict.keys[i], key)) {
            free(d->u.dict.keys[i]); pn_drop(d->u.dict.vals[i]);
            memmove(d->u.dict.keys + i, d->u.dict.keys + i + 1, sizeof(char *) * (size_t)(d->u.dict.len - i - 1));
            memmove(d->u.dict.vals + i, d->u.dict.vals + i + 1, sizeof(pn_obj *) * (size_t)(d->u.dict.len - i - 1));
            d->u.dict.len--;
            return;
        }
    }
}
int pn_dict_len(const pn_obj *d) { d = as_dict(d); return d ? d->u.dict.len : 0; }
const char *pn_dict_key(const pn_obj *d, int i) { d = as_dict(d); return d ? d->u.dict.keys[i] : NULL; }
pn_obj *pn_dict_val(const pn_obj *d, int i) { d = as_dict(d); return d ? d->u.dict.vals[i] : NULL; }

pn_obj *pn_shallow_copy(pn_obj *o) {
    if (!o) return NULL;
    int i;
    switch (o->type) {
    case PN_ARRAY: { pn_obj *a = pn_new_array(); for (i = 0; i < o->u.arr.len; i++) pn_arr_push(a, pn_keep(o->u.arr.items[i])); return a; }
    case PN_DICT: { pn_obj *d = pn_new_dict(); for (i = 0; i < o->u.dict.len; i++) pn_dict_put(d, o->u.dict.keys[i], pn_keep(o->u.dict.vals[i])); return d; }
    case PN_STREAM: {
        unsigned char *data = (unsigned char *)malloc(o->u.stm.len + 1);
        memcpy(data, o->u.stm.data, o->u.stm.len);
        return pn_new_stream(pn_shallow_copy(o->u.stm.dict), data, o->u.stm.len);
    }
    case PN_STRING: { pn_obj *s = pn_new_string(o->u.str.s, o->u.str.len); s->u.str.hex = o->u.str.hex; return s; }
    case PN_NAME: return pn_new_name((const char *)o->u.str.s);
    default: return pn_keep(o);
    }
}
pn_obj *pn_deep_copy_direct(pn_obj *o) {
    if (!o) return NULL;
    int i;
    switch (o->type) {
    case PN_ARRAY: { pn_obj *a = pn_new_array(); for (i = 0; i < o->u.arr.len; i++) pn_arr_push(a, pn_deep_copy_direct(o->u.arr.items[i])); return a; }
    case PN_DICT: { pn_obj *d = pn_new_dict(); for (i = 0; i < o->u.dict.len; i++) pn_dict_put(d, o->u.dict.keys[i], pn_deep_copy_direct(o->u.dict.vals[i])); return d; }
    case PN_STREAM: {
        unsigned char *data = (unsigned char *)malloc(o->u.stm.len + 1);
        memcpy(data, o->u.stm.data, o->u.stm.len);
        return pn_new_stream(pn_deep_copy_direct(o->u.stm.dict), data, o->u.stm.len);
    }
    default: return pn_shallow_copy(o);
    }
}

/* ---------------- JSON ---------------- */
void pn_json_str(pn_buf *b, const char *s, int len) {
    if (len < 0) len = (int)strlen(s);
    pn_buf_putc(b, '"');
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { pn_buf_putc(b, '\\'); pn_buf_putc(b, c); }
        else if (c == '\n') pn_buf_puts(b, "\\n");
        else if (c == '\r') pn_buf_puts(b, "\\r");
        else if (c == '\t') pn_buf_puts(b, "\\t");
        else if (c < 0x20) pn_buf_printf(b, "\\u%04x", c);
        else pn_buf_putc(b, c);
    }
    pn_buf_putc(b, '"');
}

/* ---------------- text encodings ---------------- */
static void put_utf8(pn_buf *b, unsigned u) {
    if (u < 0x80) pn_buf_putc(b, (int)u);
    else if (u < 0x800) { pn_buf_putc(b, 0xC0 | (u >> 6)); pn_buf_putc(b, 0x80 | (u & 63)); }
    else if (u < 0x10000) { pn_buf_putc(b, 0xE0 | (u >> 12)); pn_buf_putc(b, 0x80 | ((u >> 6) & 63)); pn_buf_putc(b, 0x80 | (u & 63)); }
    else { pn_buf_putc(b, 0xF0 | (u >> 18)); pn_buf_putc(b, 0x80 | ((u >> 12) & 63)); pn_buf_putc(b, 0x80 | ((u >> 6) & 63)); pn_buf_putc(b, 0x80 | (u & 63)); }
}
int pn_utf8_decode(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    int c = *p, n = 0, u;
    if (c < 0x80) { u = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { u = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { u = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { u = c & 0x07; n = 4; }
    else { *pp += 1; return 0xFFFD; }
    for (int i = 1; i < n; i++) { if ((p[i] & 0xC0) != 0x80) { *pp += i; return 0xFFFD; } u = (u << 6) | (p[i] & 0x3F); }
    *pp += n;
    return u;
}
/* WinAnsi 0x80-0x9F */
static const unsigned short win_high[32] = {
    0x20AC,0,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0,0x017D,0,
    0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0,0x017E,0x0178 };
int pn_unicode_to_winansi(int u) {
    if (u < 0x80) return u;
    if (u >= 0xA0 && u <= 0xFF) return u;
    for (int i = 0; i < 32; i++) if (win_high[i] && win_high[i] == u) return 0x80 + i;
    return -1;
}
int pn_utf8_to_pdfstring(const char *utf8, pn_buf *out) {
    /* ASCII → literal bytes; otherwise UTF-16BE with BOM */
    const char *p = utf8;
    int ascii = 1;
    for (const unsigned char *q = (const unsigned char *)utf8; *q; q++) if (*q >= 0x80) { ascii = 0; break; }
    if (ascii) { pn_buf_puts(out, utf8); return 0; }
    pn_buf_putc(out, 0xFE); pn_buf_putc(out, 0xFF);
    while (*p) {
        int u = pn_utf8_decode(&p);
        if (u >= 0x10000) {
            u -= 0x10000;
            int hi = 0xD800 + (u >> 10), lo = 0xDC00 + (u & 0x3FF);
            pn_buf_putc(out, hi >> 8); pn_buf_putc(out, hi & 255); pn_buf_putc(out, lo >> 8); pn_buf_putc(out, lo & 255);
        } else { pn_buf_putc(out, u >> 8); pn_buf_putc(out, u & 255); }
    }
    return 1;
}
#include "pn_tables.h"
int pn_pdfstring_to_utf8(const unsigned char *s, int len, pn_buf *out) {
    if (len >= 2 && s[0] == 0xFE && s[1] == 0xFF) {
        for (int i = 2; i + 1 < len; i += 2) {
            unsigned u = ((unsigned)s[i] << 8) | s[i + 1];
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < len) {
                unsigned lo = ((unsigned)s[i + 2] << 8) | s[i + 3];
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2;
            }
            put_utf8(out, u);
        }
        return 1;
    }
    if (len >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { pn_buf_put(out, s + 3, (size_t)len - 3); return 1; }
    for (int i = 0; i < len; i++) {
        unsigned short g = pn_enc_pdfdoc[s[i]];
        unsigned u = (g != 65535) ? pn_glyph_unicode[g] : s[i];
        if (!u) u = s[i];
        put_utf8(out, u);
    }
    return 0;
}

/* ---------------- matrices & rects ---------------- */
pn_mat pn_mat_identity(void) { pn_mat m = {1, 0, 0, 1, 0, 0}; return m; }
pn_mat pn_mat_mul(pn_mat m1, pn_mat m2) {
    pn_mat r;
    r.a = m1.a * m2.a + m1.b * m2.c;
    r.b = m1.a * m2.b + m1.b * m2.d;
    r.c = m1.c * m2.a + m1.d * m2.c;
    r.d = m1.c * m2.b + m1.d * m2.d;
    r.e = m1.e * m2.a + m1.f * m2.c + m2.e;
    r.f = m1.e * m2.b + m1.f * m2.d + m2.f;
    return r;
}
pn_mat pn_mat_invert(pn_mat m) {
    double det = m.a * m.d - m.b * m.c;
    pn_mat r = {1, 0, 0, 1, 0, 0};
    if (fabs(det) < 1e-12) return r;
    r.a = m.d / det; r.b = -m.b / det; r.c = -m.c / det; r.d = m.a / det;
    r.e = -(m.e * r.a + m.f * r.c);
    r.f = -(m.e * r.b + m.f * r.d);
    return r;
}
void pn_mat_apply(pn_mat m, double x, double y, double *ox, double *oy) {
    *ox = x * m.a + y * m.c + m.e;
    *oy = x * m.b + y * m.d + m.f;
}
pn_rect pn_rect_transform(pn_rect r, pn_mat m) {
    double xs[4], ys[4];
    pn_mat_apply(m, r.x0, r.y0, &xs[0], &ys[0]);
    pn_mat_apply(m, r.x1, r.y0, &xs[1], &ys[1]);
    pn_mat_apply(m, r.x0, r.y1, &xs[2], &ys[2]);
    pn_mat_apply(m, r.x1, r.y1, &xs[3], &ys[3]);
    pn_rect o = {xs[0], ys[0], xs[0], ys[0]};
    for (int i = 1; i < 4; i++) {
        if (xs[i] < o.x0) o.x0 = xs[i];
        if (xs[i] > o.x1) o.x1 = xs[i];
        if (ys[i] < o.y0) o.y0 = ys[i];
        if (ys[i] > o.y1) o.y1 = ys[i];
    }
    return o;
}
int pn_rect_intersects(pn_rect a, pn_rect b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
int pn_rect_contains(pn_rect o, pn_rect i) { return i.x0 >= o.x0 && i.x1 <= o.x1 && i.y0 >= o.y0 && i.y1 <= o.y1; }
