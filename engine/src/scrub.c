/* Search-term scrubbing outside page content (annotations, form values, bookmarks, metadata) and a
 * byte-level scan of the whole document used to verify that a redacted term is really gone. */
#include "pn.h"
#include "pn_engine.h"
#include <stdlib.h>
#include <string.h>

static unsigned char lower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }

/* case-insensitive (ASCII) search */
static const unsigned char *find_ci(const unsigned char *h, size_t hl, const unsigned char *n, size_t nl) {
    if (!nl || hl < nl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t k = 0;
        while (k < nl && lower(h[i + k]) == lower(n[k])) k++;
        if (k == nl) return h + i;
    }
    return NULL;
}

/* UTF-16BE encoding of an ASCII/UTF-8 needle (for strings stored as UTF-16 and raw streams) */
static unsigned char *utf16_needle(const char *term, size_t *len) {
    pn_buf b; pn_buf_init(&b);
    const char *p = term;
    while (*p) {
        int u = pn_utf8_decode(&p);
        if (u > 0xFFFF) u = 0xFFFD;
        pn_buf_putc(&b, (u >> 8) & 255);
        pn_buf_putc(&b, u & 255);
    }
    return pn_buf_steal(&b, len);
}

static int bytes_contain(const unsigned char *d, size_t n, const char *term, const unsigned char *t16, size_t t16len) {
    size_t tl = strlen(term);
    if (find_ci(d, n, (const unsigned char *)term, tl)) return 1;
    if (t16len && n >= t16len) {
        for (size_t i = 0; i + t16len <= n; i++) {
            size_t k = 0;
            while (k < t16len && lower(d[i + k]) == lower(t16[k])) k++;
            if (k == t16len) return 1;
        }
    }
    return 0;
}

static int string_contains(pn_obj *s, const char *term) {
    pn_buf u; pn_buf_init(&u);
    pn_pdfstring_to_utf8(s->u.str.s, s->u.str.len, &u);
    int hit = u.data && find_ci(u.data, u.len, (const unsigned char *)term, strlen(term)) != NULL;
    pn_buf_free(&u);
    return hit;
}

/* Replace every occurrence of term (case-insensitive) in a string object with "[REDACTED]". */
static void scrub_string(pn_obj *s, const char *term) {
    pn_buf u; pn_buf_init(&u);
    pn_pdfstring_to_utf8(s->u.str.s, s->u.str.len, &u);
    pn_buf out; pn_buf_init(&out);
    size_t tl = strlen(term), i = 0;
    while (u.data && i < u.len) {
        const unsigned char *m = find_ci(u.data + i, u.len - i, (const unsigned char *)term, tl);
        if (!m) { pn_buf_put(&out, u.data + i, u.len - i); break; }
        pn_buf_put(&out, u.data + i, (size_t)(m - (u.data + i)));
        pn_buf_puts(&out, "[REDACTED]");
        i = (size_t)(m - u.data) + tl;
    }
    pn_buf enc; pn_buf_init(&enc);
    pn_utf8_to_pdfstring(out.data ? (const char *)out.data : "", &enc);
    free(s->u.str.s);
    s->u.str.len = (int)enc.len;
    s->u.str.s = pn_buf_steal(&enc, NULL);
    s->u.str.hex = 0;
    pn_buf_free(&u);
    pn_buf_free(&out);
}

/* Walk the direct structure of an object, scrubbing strings. Returns number of strings changed. */
static int scrub_direct(pn_obj *o, const char *term, int depth) {
    if (!o || depth > PN_MAX_NESTING + 4) return 0;
    int n = 0;
    switch (o->type) {
    case PN_STRING: if (string_contains(o, term)) { scrub_string(o, term); n++; } break;
    case PN_ARRAY: for (int i = 0; i < o->u.arr.len; i++) n += scrub_direct(o->u.arr.items[i], term, depth + 1); break;
    case PN_DICT: for (int i = 0; i < o->u.dict.len; i++) n += scrub_direct(o->u.dict.vals[i], term, depth + 1); break;
    case PN_STREAM: n += scrub_direct(o->u.stm.dict, term, depth + 1); break;
    default: break;
    }
    return n;
}

static int count_direct(pn_obj *o, const char *term, int depth) {
    if (!o || depth > PN_MAX_NESTING + 4) return 0;
    int n = 0;
    switch (o->type) {
    case PN_STRING: n += string_contains(o, term); break;
    case PN_ARRAY: for (int i = 0; i < o->u.arr.len; i++) n += count_direct(o->u.arr.items[i], term, depth + 1); break;
    case PN_DICT: for (int i = 0; i < o->u.dict.len; i++) n += count_direct(o->u.dict.vals[i], term, depth + 1); break;
    case PN_STREAM: n += count_direct(o->u.stm.dict, term, depth + 1); break;
    default: break;
    }
    return n;
}

static int is_font_or_image_stream(pn_doc *doc, pn_obj *o) {
    if (pn_dict_get(o, "Length1") || pn_dict_get(o, "Length2")) return 1; /* embedded font program */
    const char *st = pn_name(pn_get(doc, o, "Subtype"));
    return !strcmp(st, "Image") || !strcmp(st, "Type1C") || !strcmp(st, "CIDFontType0C") || !strcmp(st, "OpenType");
}

static int stream_contains(pn_doc *doc, pn_obj *o, const char *term, const unsigned char *t16, size_t t16len) {
    size_t len; int ok;
    unsigned char *d = pn_stream_decode(doc, o, &len, &ok);
    int hit = d && bytes_contain(d, len, term, t16, t16len);
    free(d);
    return hit;
}

/* Remove a term from everything that is not page content: annotation text and appearances, form
 * values, bookmarks, document info. Returns the number of places changed. */
int pn_scrub_term(pn_doc *doc, const char *term) {
    if (!term || !*term) return 0;
    size_t t16len;
    unsigned char *t16 = utf16_needle(term, &t16len);
    int changed = 0;
    /* annotations: drop markup annotations that mention the term; widgets lose their appearance */
    int np = pn_doc_page_count(doc);
    for (int p = 0; p < np; p++) {
        pn_obj *page = pn_doc_page(doc, p);
        pn_obj *annots = pn_get(doc, page, "Annots");
        if (!pn_is(annots, PN_ARRAY)) continue;
        pn_obj *keep = pn_new_array();
        int dropped = 0;
        for (int i = 0; i < pn_arr_len(annots); i++) {
            pn_obj *ar = pn_arr_get(annots, i);
            pn_obj *a = pn_resolve(doc, ar);
            int hit = a && count_direct(a, term, 0) > 0;
            pn_obj *ap = pn_get(doc, a, "AP");
            for (int k = 0; !hit && k < pn_dict_len(ap); k++) {
                pn_obj *v = pn_resolve(doc, pn_dict_val(ap, k));
                if (pn_is(v, PN_STREAM)) hit = stream_contains(doc, v, term, t16, t16len);
                else for (int j = 0; !hit && j < pn_dict_len(v); j++) {
                    pn_obj *s = pn_resolve(doc, pn_dict_val(v, j));
                    if (pn_is(s, PN_STREAM)) hit = stream_contains(doc, s, term, t16, t16len);
                }
            }
            /* a widget whose field (parent) value mentions the term */
            pn_obj *parent = pn_get(doc, a, "Parent");
            for (int d = 0; parent && d < 16 && !hit; d++) { hit = count_direct(pn_dict_get(parent, "V"), term, 0) > 0; parent = pn_get(doc, parent, "Parent"); }
            if (!hit) { pn_arr_push(keep, pn_keep(ar)); continue; }
            changed++;
            if (pn_is_name(pn_get(doc, a, "Subtype"), "Widget")) {
                pn_dict_del(a, "AP");
                pn_dict_del(a, "AS");
                pn_arr_push(keep, pn_keep(ar));
            } else dropped++;
        }
        if (dropped) pn_dict_put(page, "Annots", keep); else pn_drop(keep);
    }
    /* strings anywhere (field values, names, bookmarks, Info, …) */
    for (int i = 1; i < doc->nxref; i++) {
        pn_obj *o = pn_load(doc, i);
        if (o) changed += scrub_direct(o, term, 0);
    }
    changed += scrub_direct(doc->trailer, term, 0);
    pn_obj *af = pn_get(doc, pn_catalog(doc), "AcroForm");
    if (changed && pn_is(af, PN_DICT)) pn_dict_put(af, "NeedAppearances", pn_new_bool(1));
    free(t16);
    return changed;
}

/* Count where a term still occurs: decoded page text (via search), any string object, any decoded
 * stream other than font programs and images. Writes a JSON report to out. */
int pn_scan_term(pn_doc *doc, const char *term, pn_buf *out) {
    size_t t16len;
    unsigned char *t16 = utf16_needle(term, &t16len);
    int text = 0, strings = 0, streams = 0;
    pn_buf tmp; pn_buf_init(&tmp);
    int np = pn_doc_page_count(doc);
    for (int p = 0; p < np; p++) text += pn_search_page(doc, p, term, 1, &tmp, 1);
    pn_buf_free(&tmp);
    for (int i = 1; i < doc->nxref; i++) {
        pn_obj *o = pn_load(doc, i);
        if (!o) continue;
        strings += count_direct(o, term, 0);
        if (pn_is(o, PN_STREAM) && !is_font_or_image_stream(doc, o) &&
            !pn_is_name(pn_dict_get(o, "Type"), "XRef") && !pn_is_name(pn_dict_get(o, "Type"), "ObjStm"))
            streams += stream_contains(doc, o, term, t16, t16len);
    }
    strings += count_direct(doc->trailer, term, 0);
    free(t16);
    pn_buf_printf(out, "{\"text\":%d,\"strings\":%d,\"streams\":%d}", text, strings, streams);
    return text + strings + streams;
}
