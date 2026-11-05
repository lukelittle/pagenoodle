/* Public, handle-based API exported to JavaScript (and used by the native CLI/tests). */
#include "pn.h"
#include "pn_engine.h"
#include "noodle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, js_progress, (int done, int total), {
    if (Module.onProgress) Module.onProgress(done, total);
});
void pn_progress(int done, int total) { js_progress(done, total); }
#else
void pn_progress(int done, int total) { (void)done; (void)total; }
#endif

#define MAXDOCS 1024
static pn_doc *g_docs[MAXDOCS];
static pn_buf g_out;
static int g_w, g_h;

static pn_doc *D(int h) { return (h > 0 && h < MAXDOCS) ? g_docs[h] : NULL; }
static int reg(pn_doc *d) {
    if (!d) return 0;
    for (int i = 1; i < MAXDOCS; i++) if (!g_docs[i]) { g_docs[i] = d; return i; }
    pn_doc_free(d);
    pn_set_error("Too many open documents");
    return 0;
}
static void out_reset(void) { g_out.len = 0; if (g_out.data) g_out.data[0] = 0; }

NOODLE_API const char *nd_error(void) { return pn_get_error(); }
NOODLE_API unsigned char *nd_out_ptr(void) { return g_out.data; }
NOODLE_API int nd_out_len(void) { return (int)g_out.len; }
NOODLE_API void nd_out_free(void) { pn_buf_free(&g_out); }
NOODLE_API int nd_last_w(void) { return g_w; }
NOODLE_API int nd_last_h(void) { return g_h; }
NOODLE_API void *nd_malloc(int n) { return malloc((size_t)(n > 0 ? n : 1)); }
NOODLE_API void nd_free(void *p) { free(p); }

NOODLE_API int nd_open(const unsigned char *data, int len) {
    char err[256] = {0};
    pn_doc *d = pn_doc_open(data, (size_t)len, err, sizeof err);
    if (!d) { pn_set_error("%s", err); return 0; }
    if (d->encrypted) {
        pn_doc_free(d);
        pn_set_error("This PDF is password-protected or encrypted. Pagenoodle can't edit encrypted PDFs yet.");
        return 0;
    }
    return reg(d);
}
NOODLE_API int nd_new(void) { return reg(pn_doc_new()); }
NOODLE_API void nd_close(int h) { if (D(h)) { pn_doc_free(g_docs[h]); g_docs[h] = NULL; } }
NOODLE_API int nd_page_count(int h) { return D(h) ? pn_doc_page_count(D(h)) : -1; }

static void info_str(pn_doc *d, pn_obj *info, const char *key, int *first) {
    pn_obj *v = pn_get(d, info, key);
    if (!pn_is(v, PN_STRING)) return;
    pn_buf t; pn_buf_init(&t);
    pn_pdfstring_to_utf8(v->u.str.s, v->u.str.len, &t);
    if (!*first) pn_buf_putc(&g_out, ',');
    *first = 0;
    pn_json_str(&g_out, key, -1);
    pn_buf_putc(&g_out, ':');
    pn_json_str(&g_out, t.data ? (const char *)t.data : "", (int)t.len);
    pn_buf_free(&t);
}

NOODLE_API int nd_info(int h) {
    pn_doc *d = D(h);
    if (!d) return -1;
    out_reset();
    int np = pn_doc_page_count(d);
    pn_obj *cat = pn_catalog(d);
    pn_obj *info = pn_get(d, d->trailer, "Info");
    pn_obj *af = pn_get(d, cat, "AcroForm");
    pn_buf_printf(&g_out, "{\"pages\":%d,\"repaired\":%s,\"hasXmp\":%s,\"hasForm\":%s,\"info\":{", np,
                  d->repaired ? "true" : "false", pn_get(d, cat, "Metadata") ? "true" : "false",
                  pn_arr_len(pn_get(d, af, "Fields")) > 0 ? "true" : "false");
    int first = 1;
    const char *keys[] = {"Title", "Author", "Subject", "Keywords", "Creator", "Producer", "CreationDate", "ModDate"};
    for (int i = 0; i < 8; i++) info_str(d, info, keys[i], &first);
    pn_buf_puts(&g_out, "},\"pageInfo\":[");
    for (int i = 0; i < np; i++) {
        pn_obj *pg = pn_doc_page(d, i);
        double vw, vh;
        pn_mat m;
        pn_page_visual_frame(d, pg, &vw, &vh, &m);
        if (i) pn_buf_putc(&g_out, ',');
        pn_buf_printf(&g_out, "{\"rot\":%d,\"vw\":%.3f,\"vh\":%.3f,\"m\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]}",
                      pn_page_rotation(d, pg), vw, vh, m.a, m.b, m.c, m.d, m.e, m.f);
    }
    pn_buf_puts(&g_out, "]}");
    return (int)g_out.len;
}

NOODLE_API int nd_save(int h, int flags) {
    pn_doc *d = D(h);
    if (!d) return -1;
    size_t len;
    unsigned char *data = pn_doc_save(d, flags, &len);
    pn_buf_free(&g_out);
    g_out.data = data; g_out.len = len; g_out.cap = len + 1;
    return (int)len;
}

NOODLE_API int nd_assemble(const int *handles, const int *pages, const int *rots, int n) {
    pn_doc **docs = (pn_doc **)malloc(sizeof(pn_doc *) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        docs[i] = D(handles[i]);
        if (!docs[i]) { free(docs); pn_set_error("Unknown document"); return 0; }
        if (pages[i] < 0 || pages[i] >= pn_doc_page_count(docs[i])) { free(docs); pn_set_error("Page out of range"); return 0; }
    }
    pn_doc *out = pn_assemble(docs, pages, rots, n);
    free(docs);
    return reg(out);
}

/* overlays */
NOODLE_API int nd_ov_begin(int h, int page) { return D(h) ? pn_overlay_begin(D(h), page) : -1; }
NOODLE_API const char *nd_ov_font(const char *base) { return pn_overlay_font(base); }
NOODLE_API const char *nd_ov_gstate(double ca, double CA, const char *bm) { return pn_overlay_gstate(ca, CA, bm); }
NOODLE_API const char *nd_ov_xobject(int num) { return pn_overlay_xobject(num); }
NOODLE_API void nd_ov_ops(const char *ops) { pn_overlay_ops(ops); }
NOODLE_API int nd_ov_end(void) { return pn_overlay_end(); }

/* images */
NOODLE_API int nd_image_new(int h, const unsigned char *data, int len, int w, int ih, int comps, int is_jpeg, const unsigned char *alpha) {
    return D(h) ? pn_image_new(D(h), data, (size_t)len, w, ih, comps, is_jpeg, alpha) : 0;
}
NOODLE_API int nd_add_image_page(int h, int img, double pw, double ph, double margin) {
    return D(h) ? pn_add_image_page(D(h), img, 0, 0, pw, ph, margin) : -1;
}
NOODLE_API int nd_images(int h) { if (!D(h)) return -1; out_reset(); pn_images_json(D(h), &g_out); return (int)g_out.len; }
NOODLE_API int nd_image_pixels(int h, int num) {
    if (!D(h)) return 0;
    out_reset();
    int comps = 0;
    if (!pn_image_pixels(D(h), num, &g_out, &g_w, &g_h, &comps)) return 0;
    return comps;
}
NOODLE_API int nd_image_replace(int h, int num, const unsigned char *data, int len, int w, int ih, int comps, int is_jpeg) {
    return D(h) ? pn_image_replace(D(h), num, data, (size_t)len, w, ih, comps, is_jpeg) : -1;
}

/* text */
NOODLE_API int nd_text(int h, int page) { if (!D(h)) return -1; out_reset(); pn_text_json(D(h), page, &g_out); return (int)g_out.len; }
NOODLE_API int nd_text_plain(int h, int page) { if (!D(h)) return -1; out_reset(); pn_text_plain(D(h), page, &g_out); return (int)g_out.len; }
NOODLE_API int nd_search(int h, const char *needle, int ci) {
    pn_doc *d = D(h);
    if (!d) return -1;
    out_reset();
    pn_buf_putc(&g_out, '[');
    int np = pn_doc_page_count(d), total = 0;
    for (int p = 0; p < np; p++) {
        total += pn_search_page(d, p, needle, ci, &g_out, total == 0);
        pn_progress(p + 1, np);
    }
    pn_buf_putc(&g_out, ']');
    return total;
}

static pn_rect *to_rects(const double *r, int n) {
    pn_rect *rects = (pn_rect *)malloc(sizeof(pn_rect) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        double x0 = r[i * 4], y0 = r[i * 4 + 1], x1 = r[i * 4 + 2], y1 = r[i * 4 + 3];
        rects[i].x0 = x0 < x1 ? x0 : x1; rects[i].x1 = x0 < x1 ? x1 : x0;
        rects[i].y0 = y0 < y1 ? y0 : y1; rects[i].y1 = y0 < y1 ? y1 : y0;
    }
    return rects;
}

extern int pn_redact_drop_undecodable;
extern pn_obj *pn_redact_need_raster[];
extern int pn_redact_need_raster_n;

NOODLE_API void nd_redact_mode(int drop_undecodable) { pn_redact_drop_undecodable = drop_undecodable; }

NOODLE_API int nd_stream_raw(int h, int num) {
    pn_doc *d = D(h);
    if (!d) return -1;
    pn_obj *o = pn_load(d, num);
    if (!pn_is(o, PN_STREAM)) return -1;
    out_reset();
    pn_buf_put(&g_out, o->u.stm.data, o->u.stm.len);
    return (int)g_out.len;
}

NOODLE_API int nd_redact(int h, int page, const double *r, int n, int fill_rgb) {
    pn_doc *d = D(h);
    if (!d) return -1;
    pn_redact_need_raster_n = 0;
    pn_rect *rects = to_rects(r, n);
    pn_redact_stats st;
    memset(&st, 0, sizeof st);
    pn_buf removed; pn_buf_init(&removed);
    int rc = pn_redact_page(d, page, rects, n, fill_rgb, &st, &removed);
    free(rects);
    out_reset();
    pn_buf_printf(&g_out, "{\"ok\":%s,\"glyphs\":%d,\"imagesRemoved\":%d,\"imagesScrubbed\":%d,\"forms\":%d,\"annots\":%d,\"removedText\":",
                  rc == 0 ? "true" : "false", st.glyphs, st.images_removed, st.images_scrubbed, st.forms, st.annots);
    pn_json_str(&g_out, removed.data ? (const char *)removed.data : "", (int)removed.len);
    pn_buf_puts(&g_out, ",\"needRaster\":[");
    int first = 1;
    for (int k = 0; k < pn_redact_need_raster_n; k++)
        for (int i = 1; i < d->nxref; i++)
            if (d->xref[i].obj == pn_redact_need_raster[k]) { pn_buf_printf(&g_out, first ? "%d" : ",%d", i); first = 0; break; }
    pn_buf_puts(&g_out, "]}");
    pn_buf_free(&removed);
    return rc;
}

NOODLE_API int nd_verify(int h, int page, const double *r, int n) {
    pn_doc *d = D(h);
    if (!d) return -1;
    pn_rect *rects = to_rects(r, n);
    pn_buf leaked; pn_buf_init(&leaked);
    int cnt = pn_count_glyphs_in(d, page, rects, n, &leaked);
    free(rects);
    out_reset();
    pn_buf_printf(&g_out, "{\"remaining\":%d,\"leaked\":", cnt);
    pn_json_str(&g_out, leaked.data ? (const char *)leaked.data : "", (int)leaked.len);
    pn_buf_putc(&g_out, '}');
    pn_buf_free(&leaked);
    return cnt;
}

/* forms */
NOODLE_API int nd_forms(int h) { if (!D(h)) return -1; out_reset(); pn_form_json(D(h), &g_out); return (int)g_out.len; }
NOODLE_API int nd_form_set(int h, int id, const char *value) { return D(h) ? pn_form_set(D(h), id, value) : -1; }
NOODLE_API int nd_form_flatten(int h) { return D(h) ? pn_form_flatten(D(h)) : -1; }

/* stamps */
NOODLE_API int nd_stamp(int h, const char *tmpl, int pos, double size, double margin, int from, int to, int start, const char *font, int rgb) {
    return D(h) ? pn_stamp_text(D(h), tmpl, pos, size, margin, from, to, start, font, rgb) : -1;
}

NOODLE_API const char *nd_version(void) { return "pagenoodle-engine 1.0.0"; }
