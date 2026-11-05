/* True redaction: image pixel scrubbing, annotation/field removal, page-level orchestration. */
#include "pn.h"
#include "pn_engine.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

int pn_image_components(pn_doc *doc, pn_obj *img, int *is_indexed) {
    *is_indexed = 0;
    if (pn_is(pn_get(doc, img, "ImageMask"), PN_BOOL) && pn_get(doc, img, "ImageMask")->u.b) return 1;
    pn_obj *cs = pn_get(doc, img, "ColorSpace");
    if (!cs) cs = pn_get(doc, img, "CS");
    if (pn_is(cs, PN_NAME)) {
        const char *n = pn_name(cs);
        if (!strcmp(n, "DeviceRGB") || !strcmp(n, "RGB") || !strcmp(n, "CalRGB")) return 3;
        if (!strcmp(n, "DeviceCMYK") || !strcmp(n, "CMYK")) return 4;
        if (!strcmp(n, "DeviceGray") || !strcmp(n, "G") || !strcmp(n, "CalGray")) return 1;
        return 0;
    }
    if (pn_is(cs, PN_ARRAY)) {
        const char *fam = pn_name(pn_resolve(doc, pn_arr_get(cs, 0)));
        if (!strcmp(fam, "ICCBased")) return (int)pn_int(pn_get(doc, pn_resolve(doc, pn_arr_get(cs, 1)), "N"), 0);
        if (!strcmp(fam, "Indexed") || !strcmp(fam, "I")) { *is_indexed = 1; return 1; }
        if (!strcmp(fam, "Separation")) return 1;
        if (!strcmp(fam, "DeviceN")) return pn_arr_len(pn_resolve(doc, pn_arr_get(cs, 1)));
        if (!strcmp(fam, "CalRGB") || !strcmp(fam, "Lab")) return 3;
        if (!strcmp(fam, "CalGray")) return 1;
    }
    return 0;
}

static void set_sample(unsigned char *row, int x, int comp, int ncomp, int bpc, unsigned v) {
    long bit = ((long)x * ncomp + comp) * bpc;
    if (bpc == 8) { row[bit / 8] = (unsigned char)v; return; }
    if (bpc == 16) { row[bit / 8] = (unsigned char)(v >> 8); row[bit / 8 + 1] = (unsigned char)v; return; }
    int shift = 8 - bpc - (int)(bit % 8);
    unsigned mask = ((1u << bpc) - 1) << shift;
    row[bit / 8] = (unsigned char)((row[bit / 8] & ~mask) | ((v << shift) & mask));
}

/* returns 0 untouched, 1 replaced (newnum), 2 drop entirely */
int pn_redact_image(pn_doc *doc, pn_obj *img, pn_mat ctm, const pn_rect *rects, int n, int *newnum) {
    if (!pn_is(img, PN_STREAM)) return 2;
    int w = (int)pn_int(pn_get(doc, img, "Width"), 0), h = (int)pn_int(pn_get(doc, img, "Height"), 0);
    int is_mask = pn_is(pn_get(doc, img, "ImageMask"), PN_BOOL) && pn_get(doc, img, "ImageMask")->u.b;
    int bpc = is_mask ? 1 : (int)pn_int(pn_get(doc, img, "BitsPerComponent"), 8);
    int indexed;
    int comps = pn_image_components(doc, img, &indexed);
    if (w <= 0 || h <= 0 || comps <= 0 || (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16)) return 2;
    /* fully covered? */
    pn_rect unit = {0, 0, 1, 1};
    pn_rect bb = pn_rect_transform(unit, ctm);
    for (int i = 0; i < n; i++) if (pn_rect_contains(rects[i], bb)) return 2;
    size_t len; int ok;
    unsigned char *data = pn_stream_decode(doc, img, &len, &ok);
    size_t rowbytes = ((size_t)w * (size_t)comps * (size_t)bpc + 7) / 8;
    if (!data || !ok || len < rowbytes * (size_t)h) { free(data); return 2; }
    pn_mat inv = pn_mat_invert(ctm);
    unsigned blank = 0;
    if (is_mask) {
        pn_obj *dec = pn_get(doc, img, "Decode");
        int inverted = pn_arr_len(dec) >= 1 && pn_num(pn_arr_get(dec, 0), 0) == 1;
        blank = inverted ? 0 : 1; /* sample value that does NOT paint */
    }
    int touched = 0;
    for (int i = 0; i < n; i++) {
        pn_rect ur = pn_rect_transform(rects[i], inv); /* in image unit space */
        if (ur.x1 <= 0 || ur.x0 >= 1 || ur.y1 <= 0 || ur.y0 >= 1) continue;
        int c0 = (int)floor(ur.x0 * w), c1 = (int)ceil(ur.x1 * w);
        int r0 = (int)floor((1 - ur.y1) * h), r1 = (int)ceil((1 - ur.y0) * h);
        if (c0 < 0) c0 = 0;
        if (r0 < 0) r0 = 0;
        if (c1 > w) c1 = w;
        if (r1 > h) r1 = h;
        for (int r = r0; r < r1; r++) {
            unsigned char *row = data + (size_t)r * rowbytes;
            for (int x = c0; x < c1; x++) {
                for (int c = 0; c < comps; c++) {
                    unsigned v = blank;
                    if (!is_mask && comps == 4 && c == 3) v = (1u << bpc) - 1; /* CMYK black */
                    set_sample(row, x, c, comps, bpc, v);
                }
                touched = 1;
            }
        }
    }
    if (!touched) { free(data); return 0; }
    pn_obj *dict = pn_shallow_copy(img->u.stm.dict);
    pn_obj *ns = pn_new_stream(dict, NULL, 0);
    pn_stream_set(ns, data, rowbytes * (size_t)h, 1);
    pn_dict_del(ns, "Metadata");
    free(data);
    *newnum = pn_add_obj(doc, ns);
    return 1;
}

static int rect_from_array(pn_doc *doc, pn_obj *a, pn_rect *r) {
    if (pn_arr_len(a) < 4) return 0;
    double v[4];
    for (int i = 0; i < 4; i++) v[i] = pn_num(pn_resolve(doc, pn_arr_get(a, i)), 0);
    r->x0 = v[0] < v[2] ? v[0] : v[2]; r->x1 = v[0] < v[2] ? v[2] : v[0];
    r->y0 = v[1] < v[3] ? v[1] : v[3]; r->y1 = v[1] < v[3] ? v[3] : v[1];
    return 1;
}

static void scrub_field(pn_doc *doc, pn_obj *widget) {
    /* clear the value on the widget's field (widget itself if merged) and its parents' values */
    pn_obj *node = widget;
    for (int d = 0; node && d < 16; d++) {
        pn_dict_del(node, "V");
        pn_dict_del(node, "DV");
        pn_dict_del(node, "RV");
        if (pn_dict_get(node, "T")) break;
        node = pn_get(doc, node, "Parent");
    }
    pn_dict_del(widget, "AP");
    pn_dict_del(widget, "AS");
}

int pn_redact_page(pn_doc *doc, int pageidx, const pn_rect *rects, int n, int fill_rgb, pn_redact_stats *st, pn_buf *removed) {
    pn_obj *page = pn_doc_page(doc, pageidx);
    if (!page) return -1;
    if (pn_redact_page_content(doc, pageidx, rects, n, st, removed) < 0) return -1;
    /* annotations */
    pn_obj *annots = pn_get(doc, page, "Annots");
    if (pn_is(annots, PN_ARRAY)) {
        pn_obj *na = pn_new_array();
        for (int i = 0; i < pn_arr_len(annots); i++) {
            pn_obj *ar = pn_arr_get(annots, i);
            pn_obj *a = pn_resolve(doc, ar);
            pn_rect r;
            int hit = 0;
            if (a && rect_from_array(doc, pn_get(doc, a, "Rect"), &r))
                for (int k = 0; k < n; k++) if (pn_rect_intersects(r, rects[k])) hit = 1;
            if (hit) {
                st->annots++;
                if (pn_is_name(pn_get(doc, a, "Subtype"), "Widget")) scrub_field(doc, a);
                pn_obj *popup = pn_get(doc, a, "Popup");
                if (popup) pn_dict_del(popup, "Contents");
                pn_dict_del(a, "Contents");
                continue;
            }
            pn_arr_push(na, pn_keep(ar));
        }
        pn_dict_put(page, "Annots", na);
    }
    /* draw fill boxes on top */
    if (fill_rgb >= 0) {
        pn_buf ops; pn_buf_init(&ops);
        pn_buf_printf(&ops, "q %.3f %.3f %.3f rg\n", ((fill_rgb >> 16) & 255) / 255.0, ((fill_rgb >> 8) & 255) / 255.0, (fill_rgb & 255) / 255.0);
        for (int k = 0; k < n; k++) {
            pn_buf_put_real(&ops, rects[k].x0); pn_buf_putc(&ops, ' ');
            pn_buf_put_real(&ops, rects[k].y0); pn_buf_putc(&ops, ' ');
            pn_buf_put_real(&ops, rects[k].x1 - rects[k].x0); pn_buf_putc(&ops, ' ');
            pn_buf_put_real(&ops, rects[k].y1 - rects[k].y0); pn_buf_puts(&ops, " re f\n");
        }
        pn_buf_puts(&ops, "Q\n");
        pn_obj *c = pn_get(doc, page, "Contents");
        size_t len;
        unsigned char *old = pn_page_content(doc, page, &len);
        (void)c;
        pn_buf all; pn_buf_init(&all);
        pn_buf_put(&all, old, len);
        pn_buf_put(&all, ops.data, ops.len);
        free(old);
        pn_page_set_content(doc, page, all.data, all.len);
        pn_buf_free(&all);
        pn_buf_free(&ops);
    }
    return 0;
}
