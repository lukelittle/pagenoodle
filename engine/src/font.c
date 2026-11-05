/* Font handling for text extraction / redaction: widths, encodings, ToUnicode CMaps. */
#include "pn.h"
#include "pn_content.h"
#include "pn_tables.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int glyph_index(const char *name) {
    int lo = 0, hi = pn_glyph_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(pn_glyph_names[mid], name);
        if (!c) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int pn_glyph_to_unicode(const char *name) {
    int gi = glyph_index(name);
    if (gi >= 0 && pn_glyph_unicode[gi]) return (int)pn_glyph_unicode[gi];
    int lo = 0, hi = pn_agl_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(pn_agl_names[mid], name);
        if (!c) return (int)pn_agl_unicode[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    size_t n = strlen(name);
    if (n == 7 && !strncmp(name, "uni", 3)) return (int)strtol(name + 3, NULL, 16);
    if (n >= 5 && n <= 7 && name[0] == 'u' && isxdigit((unsigned char)name[1])) return (int)strtol(name + 1, NULL, 16);
    const char *dot = strchr(name, '.');
    if (dot && dot != name) {
        char tmp[128];
        size_t l = (size_t)(dot - name);
        if (l < sizeof tmp) { memcpy(tmp, name, l); tmp[l] = 0; return pn_glyph_to_unicode(tmp); }
    }
    return 0;
}

static const char *strip_subset(const char *name) {
    const char *plus = strchr(name, '+');
    if (plus && plus - name == 6) return plus + 1;
    return name;
}

int pn_base14_index(const char *basefont) {
    if (!basefont) return -1;
    const char *n = strip_subset(basefont);
    for (int i = 0; i < 14; i++) if (!strcmp(pn_base14_names[i], n)) return i;
    /* common aliases */
    int bold = strstr(n, "Bold") != NULL, ital = strstr(n, "Italic") || strstr(n, "Oblique");
    int style = bold && ital ? 3 : bold ? 1 : ital ? 2 : 0;
    if (!strncmp(n, "Arial", 5) || !strncmp(n, "Helvetica", 9)) return 4 + style;
    if (!strncmp(n, "Times", 5)) return 8 + style;
    if (!strncmp(n, "Courier", 7)) return 0 + style;
    if (!strncmp(n, "Symbol", 6)) return 12;
    if (!strncmp(n, "ZapfDingbats", 12)) return 13;
    return -1;
}

double pn_base14_text_width(int font, const unsigned char *s, int len) {
    if (font < 0 || font >= 14) font = 4;
    const unsigned short *enc = font == 12 ? pn_enc_symbol : font == 13 ? pn_enc_zapf : pn_enc_win;
    double w = 0;
    for (int i = 0; i < len; i++) {
        unsigned short g = enc[s[i]];
        int gw = g != 65535 ? pn_base14_widths[font][g] : -1;
        w += gw >= 0 ? gw : 500;
    }
    return w;
}

/* ---------- CMap parsing ---------- */
static unsigned str_code(pn_obj *s, int *nbytes) {
    unsigned v = 0;
    for (int i = 0; i < s->u.str.len && i < 4; i++) v = (v << 8) | s->u.str.s[i];
    *nbytes = s->u.str.len;
    return v;
}
static int utf16_to_uni(const unsigned char *s, int len, unsigned *out, int max) {
    int n = 0;
    for (int i = 0; i + 1 < len && n < max; i += 2) {
        unsigned u = ((unsigned)s[i] << 8) | s[i + 1];
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < len) {
            unsigned lo = ((unsigned)s[i + 2] << 8) | s[i + 3];
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        out[n++] = u;
    }
    if (len == 1 && max > 0) out[n++] = s[0];
    return n;
}
static void tu_add(pn_font *f, unsigned lo, unsigned hi, int nbytes, const unsigned *uni, int nuni, int *cap) {
    if (f->ntu == *cap) { *cap = *cap ? *cap * 2 : 64; f->tu = (pn_tu_entry *)realloc(f->tu, sizeof(pn_tu_entry) * (size_t)*cap); }
    pn_tu_entry *e = &f->tu[f->ntu++];
    e->lo = lo; e->hi = hi; e->nbytes = nbytes; e->nuni = nuni > 4 ? 4 : nuni;
    for (int i = 0; i < e->nuni; i++) e->uni[i] = uni[i];
}
static int tu_cmp(const void *a, const void *b) {
    const pn_tu_entry *x = (const pn_tu_entry *)a, *y = (const pn_tu_entry *)b;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    return 0;
}

static void parse_cmap(pn_font *f, const unsigned char *data, size_t len, int want_tu) {
    pn_lexer lx;
    pn_lex_init(&lx, data, len);
    int cap = f->ntu, cscap = f->ncs;
    pn_obj *stack[3] = {0};
    int mode = 0; /* 1 codespace, 2 bfchar, 3 bfrange, 4 cidrange (ignored), 5 cidchar */
    pn_obj *vals[3]; int nv = 0;
    for (;;) {
        pn_tok t = pn_lex_next(&lx);
        if (t == TK_EOF) break;
        if (t == TK_KEYWORD) {
            const char *k = (const char *)lx.sbuf.data;
            if (!strcmp(k, "begincodespacerange")) mode = 1;
            else if (!strcmp(k, "beginbfchar")) mode = 2;
            else if (!strcmp(k, "beginbfrange")) mode = 3;
            else if (!strcmp(k, "begincidrange")) mode = 4;
            else if (!strcmp(k, "begincidchar")) mode = 5;
            else if (!strncmp(k, "end", 3)) mode = 0;
            for (int i = 0; i < nv; i++) pn_drop(vals[i]);
            nv = 0;
            continue;
        }
        if (!mode) continue;
        pn_obj *v = pn_parse_obj_tok(&lx, 0);
        if (!v) continue;
        vals[nv++] = v;
        int need = (mode == 1 || mode == 2 || mode == 5) ? 2 : 3;
        if (nv < need) continue;
        if (mode == 1 && pn_is(vals[0], PN_STRING) && pn_is(vals[1], PN_STRING)) {
            int nb;
            unsigned lo = str_code(vals[0], &nb), hi;
            int nb2; hi = str_code(vals[1], &nb2);
            if (f->ncs == cscap) { cscap = cscap ? cscap * 2 : 8; f->cs = (pn_cs_range *)realloc(f->cs, sizeof(pn_cs_range) * (size_t)cscap); }
            f->cs[f->ncs].lo = lo; f->cs[f->ncs].hi = hi; f->cs[f->ncs].nbytes = nb > 0 ? nb : 1; f->ncs++;
        } else if (want_tu && mode == 2 && pn_is(vals[0], PN_STRING)) {
            int nb; unsigned code = str_code(vals[0], &nb);
            unsigned uni[4]; int nu = 0;
            if (pn_is(vals[1], PN_STRING)) nu = utf16_to_uni(vals[1]->u.str.s, vals[1]->u.str.len, uni, 4);
            else if (pn_is(vals[1], PN_NAME)) { uni[0] = (unsigned)pn_glyph_to_unicode(pn_name(vals[1])); nu = uni[0] ? 1 : 0; }
            if (nu) tu_add(f, code, code, nb, uni, nu, &cap);
        } else if (want_tu && mode == 3 && pn_is(vals[0], PN_STRING) && pn_is(vals[1], PN_STRING)) {
            int nb, nb2;
            unsigned lo = str_code(vals[0], &nb), hi = str_code(vals[1], &nb2);
            if (hi >= lo && hi - lo < 0x10000) {
                if (pn_is(vals[2], PN_STRING)) {
                    unsigned uni[4]; int nu = utf16_to_uni(vals[2]->u.str.s, vals[2]->u.str.len, uni, 4);
                    if (nu) tu_add(f, lo, hi, nb, uni, nu, &cap);
                } else if (pn_is(vals[2], PN_ARRAY)) {
                    for (unsigned c = lo; c <= hi && (int)(c - lo) < pn_arr_len(vals[2]); c++) {
                        pn_obj *s = pn_arr_get(vals[2], (int)(c - lo));
                        if (!pn_is(s, PN_STRING)) continue;
                        unsigned uni[4]; int nu = utf16_to_uni(s->u.str.s, s->u.str.len, uni, 4);
                        if (nu) tu_add(f, c, c, nb, uni, nu, &cap);
                    }
                }
            }
        }
        for (int i = 0; i < nv; i++) pn_drop(vals[i]);
        nv = 0;
    }
    for (int i = 0; i < nv; i++) pn_drop(vals[i]);
    (void)stack;
    pn_lex_free(&lx);
    if (f->ntu > 1) qsort(f->tu, (size_t)f->ntu, sizeof(pn_tu_entry), tu_cmp);
}

/* ---------- font loading ---------- */
static void load_simple_encoding(pn_doc *doc, pn_font *f, pn_obj *fd, const char *subtype, int b14) {
    const unsigned short *base = pn_enc_std;
    if (!strcmp(subtype, "TrueType")) base = pn_enc_win;
    if (b14 == 12) base = pn_enc_symbol;
    if (b14 == 13) base = pn_enc_zapf;
    pn_obj *enc = pn_get(doc, fd, "Encoding");
    pn_obj *diffs = NULL;
    const char *bn = NULL;
    if (pn_is(enc, PN_NAME)) bn = pn_name(enc);
    else if (pn_is(enc, PN_DICT)) { bn = pn_name(pn_get(doc, enc, "BaseEncoding")); diffs = pn_get(doc, enc, "Differences"); }
    if (bn && *bn) {
        if (!strcmp(bn, "WinAnsiEncoding")) base = pn_enc_win;
        else if (!strcmp(bn, "MacRomanEncoding")) base = pn_enc_mac;
        else if (!strcmp(bn, "StandardEncoding")) base = pn_enc_std;
    }
    const char *names[256];
    for (int i = 0; i < 256; i++) names[i] = base[i] != 65535 ? pn_glyph_names[base[i]] : NULL;
    int code = 0;
    for (int i = 0; i < pn_arr_len(diffs); i++) {
        pn_obj *v = pn_resolve(doc, pn_arr_get(diffs, i));
        if (pn_is_number(v)) code = (int)pn_int(v, 0);
        else if (pn_is(v, PN_NAME) && code >= 0 && code < 256) names[code++] = pn_name(v);
    }
    for (int i = 0; i < 256; i++) {
        unsigned u = names[i] ? (unsigned)pn_glyph_to_unicode(names[i]) : 0;
        if (!u && !names[i]) u = (i >= 32 && i < 127) ? (unsigned)i : 0;
        f->uni[i] = u;
        /* widths for base-14 fonts by glyph name */
        if (b14 >= 0 && names[i]) {
            int gi = glyph_index(names[i]);
            if (gi >= 0 && pn_base14_widths[b14][gi] >= 0) f->widths[i] = pn_base14_widths[b14][gi];
        }
    }
}

pn_font *pn_font_load(pn_doc *doc, pn_obj *fd, pn_font **cache) {
    for (pn_font *c = *cache; c; c = c->next) if (c->dict == fd) return c;
    pn_font *f = (pn_font *)calloc(1, sizeof *f);
    f->dict = fd; f->next = *cache; *cache = f;
    f->ascent = 0.8; f->descent = -0.2; f->dw = 1000; f->t3scale = 1; f->base14 = -1;
    for (int i = 0; i < 256; i++) f->widths[i] = 500;
    if (!pn_is(fd, PN_DICT)) { for (int i = 0; i < 256; i++) f->uni[i] = (unsigned)i; return f; }
    const char *subtype = pn_name(pn_get(doc, fd, "Subtype"));
    const char *basefont = pn_name(pn_get(doc, fd, "BaseFont"));
    pn_obj *desc = NULL;
    if (!strcmp(subtype, "Type0")) {
        f->composite = 1;
        pn_obj *desc_arr = pn_get(doc, fd, "DescendantFonts");
        pn_obj *cid = pn_resolve(doc, pn_arr_get(desc_arr, 0));
        f->dw = pn_num(pn_get(doc, cid, "DW"), 1000);
        pn_obj *w = pn_get(doc, cid, "W");
        int cap = 0;
        for (int i = 0; i < pn_arr_len(w);) {
            pn_obj *a = pn_resolve(doc, pn_arr_get(w, i));
            pn_obj *b = pn_resolve(doc, pn_arr_get(w, i + 1));
            if (pn_is(b, PN_ARRAY)) {
                unsigned c0 = (unsigned)pn_int(a, 0);
                for (int k = 0; k < pn_arr_len(b); k++) {
                    if (f->ncidw == cap) { cap = cap ? cap * 2 : 64; f->cidw = (pn_cid_w *)realloc(f->cidw, sizeof(pn_cid_w) * (size_t)cap); }
                    f->cidw[f->ncidw].c0 = f->cidw[f->ncidw].c1 = c0 + (unsigned)k;
                    f->cidw[f->ncidw].w = pn_num(pn_resolve(doc, pn_arr_get(b, k)), f->dw);
                    f->ncidw++;
                }
                i += 2;
            } else {
                pn_obj *c = pn_resolve(doc, pn_arr_get(w, i + 2));
                if (!c) break;
                if (f->ncidw == cap) { cap = cap ? cap * 2 : 64; f->cidw = (pn_cid_w *)realloc(f->cidw, sizeof(pn_cid_w) * (size_t)cap); }
                f->cidw[f->ncidw].c0 = (unsigned)pn_int(a, 0);
                f->cidw[f->ncidw].c1 = (unsigned)pn_int(b, 0);
                f->cidw[f->ncidw].w = pn_num(c, f->dw);
                f->ncidw++;
                i += 3;
            }
        }
        desc = pn_get(doc, cid, "FontDescriptor");
        pn_obj *enc = pn_get(doc, fd, "Encoding");
        if (pn_is(enc, PN_STREAM)) {
            size_t l; int ok;
            unsigned char *d = pn_stream_decode(doc, enc, &l, &ok);
            if (d) { parse_cmap(f, d, l, 0); free(d); }
        } else if (pn_is(enc, PN_NAME)) {
            const char *en = pn_name(enc);
            if (strstr(en, "-V")) f->vertical = 1;
        }
    } else {
        if (!strcmp(subtype, "Type3")) {
            f->type3 = 1;
            pn_obj *fm = pn_get(doc, fd, "FontMatrix");
            f->t3scale = pn_num(pn_resolve(doc, pn_arr_get(fm, 0)), 0.001) * 1000.0;
        }
        f->base14 = f->type3 ? -1 : pn_base14_index(basefont);
        if (f->base14 >= 0) {
            f->ascent = pn_base14_ascdesc[f->base14][0] / 1000.0;
            f->descent = pn_base14_ascdesc[f->base14][1] / 1000.0;
        }
        load_simple_encoding(doc, f, fd, subtype, f->base14);
        pn_obj *widths = pn_get(doc, fd, "Widths");
        if (pn_is(widths, PN_ARRAY)) {
            int first = (int)pn_int(pn_get(doc, fd, "FirstChar"), 0);
            desc = pn_get(doc, fd, "FontDescriptor");
            double missing = pn_num(pn_get(doc, desc, "MissingWidth"), 0);
            if (f->base14 < 0) for (int i = 0; i < 256; i++) f->widths[i] = missing;
            for (int i = 0; i < pn_arr_len(widths); i++) {
                int c = first + i;
                if (c >= 0 && c < 256) f->widths[c] = pn_num(pn_resolve(doc, pn_arr_get(widths, i)), missing) * f->t3scale;
            }
        }
        if (!desc) desc = pn_get(doc, fd, "FontDescriptor");
    }
    if (desc) {
        double a = pn_num(pn_get(doc, desc, "Ascent"), 0), d = pn_num(pn_get(doc, desc, "Descent"), 0);
        if (a > 100 && a < 1500) f->ascent = a / 1000.0;
        if (d < 0 && d > -800) f->descent = d / 1000.0;
    }
    pn_obj *tu = pn_get(doc, fd, "ToUnicode");
    if (pn_is(tu, PN_STREAM)) {
        size_t l; int ok;
        unsigned char *d = pn_stream_decode(doc, tu, &l, &ok);
        if (d) { parse_cmap(f, d, l, 1); free(d); }
    }
    return f;
}

void pn_font_cache_free(pn_font *c) {
    while (c) { pn_font *n = c->next; free(c->tu); free(c->cs); free(c->cidw); free(c); c = n; }
}

int pn_font_next_code(pn_font *f, const unsigned char *s, int len, unsigned *code) {
    if (len <= 0) { *code = 0; return 1; }
    if (!f->composite) { *code = s[0]; return 1; }
    if (f->ncs) {
        unsigned v = 0;
        for (int nb = 1; nb <= 4 && nb <= len; nb++) {
            v = (v << 8) | s[nb - 1];
            for (int i = 0; i < f->ncs; i++) if (f->cs[i].nbytes == nb && v >= f->cs[i].lo && v <= f->cs[i].hi) { *code = v; return nb; }
        }
        *code = s[0];
        return 1;
    }
    if (len >= 2) { *code = ((unsigned)s[0] << 8) | s[1]; return 2; }
    *code = s[0];
    return 1;
}

double pn_font_width(pn_font *f, unsigned code) {
    if (!f->composite) return code < 256 ? f->widths[code] : 0;
    for (int i = 0; i < f->ncidw; i++) if (code >= f->cidw[i].c0 && code <= f->cidw[i].c1) return f->cidw[i].w;
    return f->dw;
}

int pn_font_unicode(pn_font *f, unsigned code, int nbytes, unsigned *out, int max) {
    if (f->ntu) {
        int lo = 0, hi = f->ntu - 1, best = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (f->tu[mid].lo <= code) { best = mid; lo = mid + 1; } else hi = mid - 1;
        }
        for (int k = best; k >= 0 && k > best - 8; k--) {
            pn_tu_entry *e = &f->tu[k];
            if (code >= e->lo && code <= e->hi && (e->nbytes == nbytes || !f->composite)) {
                int n = e->nuni < max ? e->nuni : max;
                for (int i = 0; i < n; i++) out[i] = e->uni[i];
                if (n > 0) out[n - 1] += code - e->lo;
                return n;
            }
        }
    }
    if (!f->composite) {
        if (code < 256 && f->uni[code]) { out[0] = f->uni[code]; return 1; }
        return 0;
    }
    return 0;
}
