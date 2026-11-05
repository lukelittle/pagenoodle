/* True redaction: image pixel scrubbing, annotation/field removal, page-level orchestration. */
#include "pn.h"
#include "pn_engine.h"
int pn_redact_pattern(pn_doc *doc, pn_obj *pat);
int pn_redact_type3(pn_doc *doc, pn_obj *font);
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

static void set_sample(unsigned char *row, uint64_t x, int comp, int ncomp, int bpc, unsigned v) {
    uint64_t bit = (x * (uint64_t)ncomp + (uint64_t)comp) * (uint64_t)bpc;
    if (bpc == 8) { row[bit / 8] = (unsigned char)v; return; }
    if (bpc == 16) { row[bit / 8] = (unsigned char)(v >> 8); row[bit / 8 + 1] = (unsigned char)v; return; }
    int shift = 8 - bpc - (int)(bit % 8);
    unsigned mask = ((1u << bpc) - 1) << shift;
    row[bit / 8] = (unsigned char)((row[bit / 8] & ~mask) | ((v << shift) & mask));
}

/* Images that cannot be decoded in C (e.g. JPEG). In mode 0 they are reported so the UI can
 * convert them to raw pixels and retry; in mode 1 they are dropped entirely (always safe). */
int pn_redact_drop_undecodable = 1;
pn_obj *pn_redact_need_raster[256];
int pn_redact_need_raster_n = 0;

/* Blank the samples of `img` under the rects. Returns a new stream, or NULL with *status:
 * 0 = nothing under the rects, 2 = cannot be scrubbed safely (drop it), 3 = deferred (JPEG). */
static pn_obj *scrub_pixels(pn_doc *doc, pn_obj *img, pn_mat ctm, const pn_rect *rects, int n, int is_smask, int *status) {
    *status = 2;
    if (!pn_is(img, PN_STREAM)) return NULL;
    long long w = pn_int(pn_get(doc, img, "Width"), 0), h = pn_int(pn_get(doc, img, "Height"), 0);
    int is_mask = !is_smask && pn_is(pn_get(doc, img, "ImageMask"), PN_BOOL) && pn_get(doc, img, "ImageMask")->u.b;
    long long bpcl = is_mask ? 1 : pn_int(pn_get(doc, img, "BitsPerComponent"), 8);
    int indexed;
    int comps = is_smask ? 1 : pn_image_components(doc, img, &indexed);
    if (w <= 0 || h <= 0 || w > PN_MAX_IMAGE_DIM || h > PN_MAX_IMAGE_DIM || comps <= 0 || comps > 32) return NULL;
    if (bpcl != 1 && bpcl != 2 && bpcl != 4 && bpcl != 8 && bpcl != 16) return NULL;
    int bpc = (int)bpcl;
    uint64_t rowbytes = ((uint64_t)w * (uint64_t)comps * (uint64_t)bpc + 7) / 8;
    uint64_t need = rowbytes * (uint64_t)h;
    if (need > PN_MAX_STREAM) return NULL;
    size_t len; int ok;
    unsigned char *data = pn_stream_decode(doc, img, &len, &ok);
    if (data && !ok && !is_smask && !pn_redact_drop_undecodable) {
        pn_obj *f = pn_resolve(doc, pn_dict_get(img, "Filter"));
        if (pn_is(f, PN_ARRAY)) f = pn_resolve(doc, pn_arr_get(f, pn_arr_len(f) - 1));
        if (pn_is_name(f, "DCTDecode") || pn_is_name(f, "DCT")) {
            int dup = 0;
            for (int i = 0; i < pn_redact_need_raster_n; i++) if (pn_redact_need_raster[i] == img) dup = 1;
            if (!dup && pn_redact_need_raster_n < 256) pn_redact_need_raster[pn_redact_need_raster_n++] = img;
            free(data);
            *status = 3;
            return NULL;
        }
    }
    if (!data || !ok || (uint64_t)len < need) { free(data); return NULL; }
    pn_mat inv = pn_mat_invert(ctm);
    unsigned blank = 0; /* soft masks: 0 = fully transparent */
    if (is_mask) {
        pn_obj *dec = pn_get(doc, img, "Decode");
        int inverted = pn_arr_len(dec) >= 1 && pn_num(pn_arr_get(dec, 0), 0) == 1;
        blank = inverted ? 0 : 1; /* sample value that does NOT paint */
    }
    int touched = 0;
    for (int i = 0; i < n; i++) {
        pn_rect ur = pn_rect_transform(rects[i], inv); /* in image unit space */
        if (ur.x1 <= 0 || ur.x0 >= 1 || ur.y1 <= 0 || ur.y0 >= 1) continue;
        double fc0 = floor(ur.x0 * (double)w), fc1 = ceil(ur.x1 * (double)w);
        double fr0 = floor((1 - ur.y1) * (double)h), fr1 = ceil((1 - ur.y0) * (double)h);
        long long c0 = fc0 < 0 ? 0 : (long long)fc0, c1 = fc1 > (double)w ? w : (long long)fc1;
        long long r0 = fr0 < 0 ? 0 : (long long)fr0, r1 = fr1 > (double)h ? h : (long long)fr1;
        for (long long r = r0; r < r1; r++) {
            unsigned char *row = data + (uint64_t)r * rowbytes;
            for (long long x = c0; x < c1; x++) {
                for (int c = 0; c < comps; c++) {
                    unsigned v = blank;
                    if (!is_mask && !is_smask && comps == 4 && c == 3) v = (1u << bpc) - 1; /* CMYK black */
                    set_sample(row, (uint64_t)x, c, comps, bpc, v);
                }
                touched = 1;
            }
        }
    }
    if (!touched) { free(data); *status = 0; return NULL; }
    pn_obj *ns = pn_new_stream(pn_shallow_copy(img->u.stm.dict), NULL, 0);
    pn_stream_set(ns, data, (size_t)need, 1);
    free(data);
    *status = 1;
    return ns;
}

/* returns 0 untouched, 1 replaced (newnum), 2 drop entirely */
int pn_redact_image(pn_doc *doc, pn_obj *img, pn_mat ctm, const pn_rect *rects, int n, int *newnum) {
    if (!pn_is(img, PN_STREAM)) return 2;
    pn_rect unit = {0, 0, 1, 1};
    pn_rect bb = pn_rect_transform(unit, ctm);
    for (int i = 0; i < n; i++) if (pn_rect_contains(rects[i], bb)) return 2;
    int st;
    pn_obj *ns = scrub_pixels(doc, img, ctm, rects, n, 0, &st);
    if (!ns) return st == 3 || st == 0 ? 0 : 2;
    /* side data that could still show what was under the box */
    const char *drop[] = {"Alternates", "OPI", "Measure", "PtData", "Metadata", "Mask"};
    for (int i = 0; i < 6; i++) {
        pn_obj *m = pn_dict_get(ns, drop[i]);
        if (!strcmp(drop[i], "Mask") && pn_is(m, PN_ARRAY)) continue; /* colour-key mask holds no content */
        pn_dict_del(ns, drop[i]);
    }
    /* the soft mask is an image of the same area: scrub it too, or drop it */
    pn_obj *sm = pn_get(doc, ns, "SMask");
    if (sm) {
        int sst;
        pn_obj *nsm = scrub_pixels(doc, sm, ctm, rects, n, 1, &sst);
        if (nsm) pn_dict_put(ns, "SMask", pn_add_ref(doc, nsm));
        else if (sst != 0) pn_dict_del(ns, "SMask");
    }
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

/* Things that can carry a full unredacted copy of the document (attachments, often the original
 * file) or run code are removed from the whole document whenever anything is redacted. */
static void redact_doc_cleanup(pn_doc *doc, pn_redact_stats *st) {
    pn_obj *cat = pn_catalog(doc);
    pn_obj *names = pn_get(doc, cat, "Names");
    if (pn_is(names, PN_DICT)) {
        if (pn_dict_get(names, "EmbeddedFiles")) st->annots++;
        pn_dict_del(names, "EmbeddedFiles");
        pn_dict_del(names, "JavaScript");
    }
    const char *catkeys[] = {"OpenAction", "AA", "AF", "Collection"};
    for (int i = 0; i < 4; i++) pn_dict_del(cat, catkeys[i]);
    int np = pn_doc_page_count(doc);
    for (int p = 0; p < np; p++) {
        pn_obj *page = pn_doc_page(doc, p);
        pn_dict_del(page, "AA");
        pn_dict_del(page, "AF");
        pn_obj *annots = pn_get(doc, page, "Annots");
        if (!pn_is(annots, PN_ARRAY)) continue;
        pn_obj *keep = pn_new_array();
        for (int i = 0; i < pn_arr_len(annots); i++) {
            pn_obj *ar = pn_arr_get(annots, i);
            pn_obj *a = pn_resolve(doc, ar);
            const char *sub = pn_name(pn_get(doc, a, "Subtype"));
            if (a != page && pn_is_annotation(doc, a) && (!strcmp(sub, "FileAttachment") || !strcmp(sub, "Sound") || !strcmp(sub, "Movie") || !strcmp(sub, "RichMedia") || !strcmp(sub, "Screen") || !strcmp(sub, "3D"))) {
                st->annots++;
                if (ar->type == PN_REF && ar->u.ref.num > 0 && ar->u.ref.num < doc->nxref) pn_set_obj(doc, ar->u.ref.num, pn_null());
                continue;
            }
            if (pn_is(a, PN_DICT)) { pn_dict_del(a, "AA"); pn_dict_del(a, "AF"); }
            pn_arr_push(keep, pn_keep(ar));
        }
        pn_dict_put(page, "Annots", keep);
    }
    /* attachments and file-launching actions anywhere in the document */
    for (int i = 1; i < doc->nxref; i++) {
        pn_obj *o = doc->xref[i].obj ? doc->xref[i].obj : pn_load(doc, i);
        if (!o || (o->type != PN_DICT && o->type != PN_STREAM)) continue;
        if (pn_is_name(pn_dict_get(o, "Type"), "EmbeddedFile")) { pn_set_obj(doc, i, pn_null()); st->annots++; continue; }
        pn_dict_del(o, "AF");
        if (pn_dict_get(o, "EF")) { pn_dict_del(o, "EF"); pn_dict_del(o, "RF"); } /* file specification payloads */
        pn_obj *act = pn_get(doc, o, "A");
        const char *s = pn_name(pn_get(doc, act, "S"));
        if (!strcmp(s, "Launch") || !strcmp(s, "GoToE") || !strcmp(s, "GoToR") || !strcmp(s, "ImportData") || !strcmp(s, "SubmitForm") ||
            !strcmp(s, "JavaScript") || !strcmp(s, "RichMediaExecute") || !strcmp(s, "Rendition"))
            pn_dict_del(o, "A");
    }
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
            if (hit && pn_is_annotation(doc, a)) {
                st->annots++;
                int is_widget = pn_is_name(pn_get(doc, a, "Subtype"), "Widget");
                if (is_widget) scrub_field(doc, a);
                /* The annotation may still be reachable (popup /Parent, structure tree, form
                 * fields), so blank its content-bearing keys and then null the object itself. */
                const char *keys[] = {"Contents", "RC", "T", "Subj", "AP", "AS", "RichMediaContent", "DS", "TU", "V", "DV"};
                pn_obj *popup_ref = pn_dict_get(a, "Popup");
                pn_obj *popup = pn_get(doc, a, "Popup");
                int popup_ok = popup && popup != a && popup != page && pn_is_annotation(doc, popup);
                for (int k = 0; k < 11; k++) { pn_dict_del(a, keys[k]); if (popup_ok) pn_dict_del(popup, keys[k]); }
                if (popup_ok && popup_ref->type == PN_REF && popup_ref->u.ref.num > 0 && popup_ref->u.ref.num < doc->nxref)
                    pn_set_obj(doc, popup_ref->u.ref.num, pn_null());
                if (!is_widget && ar->type == PN_REF && ar->u.ref.num > 0 && ar->u.ref.num < doc->nxref)
                    pn_set_obj(doc, ar->u.ref.num, pn_null());
                continue;
            }
            pn_arr_push(na, pn_keep(ar));
        }
        pn_dict_put(page, "Annots", na);
    }
    redact_doc_cleanup(doc, st);
    /* page-level side data that can carry copies of the page content */
    pn_dict_del(page, "PieceInfo");
    pn_dict_del(page, "Thumb");
    pn_dict_del(page, "Metadata");
    pn_obj *af = pn_get(doc, pn_catalog(doc), "AcroForm");
    if (pn_is(af, PN_DICT)) pn_dict_del(af, "XFA"); /* XFA duplicates all form data */
    /* tiling patterns can draw text; patterns on a redacted page that do are emptied (fail closed) */
    {
        pn_obj *mres = pn_page_resources(doc, page);
        pn_retire(doc, mres);
        pn_drop(mres);
        pn_obj *pd = pn_get(doc, mres, "Pattern");
        for (int i = 0; i < pn_dict_len(pd); i++)
            if (pn_redact_pattern(doc, pn_resolve(doc, pn_dict_val(pd, i)))) st->forms++;
    }
    /* structure elements of this page: alternate/replacement text may repeat redacted content */
    {
        int pnum = pn_doc_page_num(doc, pageidx);
        for (int i = 1; i < doc->nxref; i++) {
            pn_obj *o = doc->xref[i].obj ? doc->xref[i].obj : pn_load(doc, i);
            if (!pn_is(o, PN_DICT) || !pn_dict_get(o, "S")) continue;
            pn_obj *pg = NULL, *node = o; /* /Pg may be inherited from an ancestor element */
            for (int d = 0; node && d < 64 && !pg; d++) { pg = pn_dict_get(node, "Pg"); node = pn_get(doc, node, "P"); }
            int on_page = pg && pg->type == PN_REF && pg->u.ref.num == pnum;
            /* ...or given only on marked-content / object reference kids */
            pn_obj *k = pn_get(doc, o, "K");
            int nk = pn_is(k, PN_ARRAY) ? pn_arr_len(k) : (k ? 1 : 0);
            for (int j = 0; j < nk && !on_page; j++) {
                pn_obj *kid = pn_is(k, PN_ARRAY) ? pn_resolve(doc, pn_arr_get(k, j)) : k;
                pn_obj *kpg = pn_is(kid, PN_DICT) ? pn_dict_get(kid, "Pg") : NULL;
                if (kpg && kpg->type == PN_REF && kpg->u.ref.num == pnum) on_page = 1;
            }
            int has_alt = pn_dict_get(o, "ActualText") || pn_dict_get(o, "Alt") || pn_dict_get(o, "E");
            if (on_page || (!pg && has_alt)) { /* page unknown: fail closed */
                pn_dict_del(o, "ActualText");
                pn_dict_del(o, "Alt");
                pn_dict_del(o, "E");
            }
        }
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
