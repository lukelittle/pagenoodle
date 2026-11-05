/* Images: create image XObjects (JPEG passthrough or raw pixels), image pages, listing & replacement. */
#include "pn.h"
#include "pn_engine.h"
#include <stdlib.h>
#include <string.h>

int pn_image_components(pn_doc *doc, pn_obj *img, int *is_indexed);

static int jpeg_info(const unsigned char *d, size_t n, int *w, int *h, int *comps) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 0;
    size_t i = 2;
    while (i + 9 < n) {
        if (d[i] != 0xFF) { i++; continue; }
        int m = d[i + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) { i += (m == 0xFF) ? 1 : 2; continue; }
        size_t seglen = ((size_t)d[i + 2] << 8) | d[i + 3];
        if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            *comps = d[i + 9];
            return *w > 0 && *h > 0;
        }
        i += 2 + seglen;
    }
    return 0;
}

static const char *cs_for(int comps) { return comps == 1 ? "DeviceGray" : comps == 4 ? "DeviceCMYK" : "DeviceRGB"; }

int pn_image_new(pn_doc *doc, const unsigned char *data, size_t len, int w, int h, int comps, int is_jpeg, const unsigned char *alpha) {
    pn_obj *d = pn_new_dict();
    pn_dict_put(d, "Type", pn_new_name("XObject"));
    pn_dict_put(d, "Subtype", pn_new_name("Image"));
    pn_obj *s;
    if (is_jpeg) {
        int jw, jh, jc;
        if (!jpeg_info(data, len, &jw, &jh, &jc)) { pn_drop(d); pn_set_error("Not a JPEG I can read"); return 0; }
        w = jw; h = jh; comps = jc;
        unsigned char *copy = (unsigned char *)malloc(len + 1);
        memcpy(copy, data, len);
        s = pn_new_stream(d, copy, len);
        pn_dict_put(s, "Filter", pn_new_name("DCTDecode"));
        pn_dict_put(s, "Length", pn_new_int((long long)len));
        if (comps == 4) { /* Adobe CMYK JPEGs are stored inverted */
            pn_obj *dec = pn_new_array();
            for (int i = 0; i < 4; i++) { pn_arr_push(dec, pn_new_int(1)); pn_arr_push(dec, pn_new_int(0)); }
            pn_dict_put(s, "Decode", dec);
        }
    } else {
        if ((size_t)w * (size_t)h * (size_t)comps > len) { pn_drop(d); pn_set_error("Pixel buffer too small"); return 0; }
        s = pn_new_stream(d, NULL, 0);
        pn_stream_set(s, data, (size_t)w * (size_t)h * (size_t)comps, 1);
    }
    pn_dict_put(s, "Width", pn_new_int(w));
    pn_dict_put(s, "Height", pn_new_int(h));
    pn_dict_put(s, "ColorSpace", pn_new_name(cs_for(comps)));
    pn_dict_put(s, "BitsPerComponent", pn_new_int(8));
    if (alpha) {
        int opaque = 1;
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) if (alpha[i] != 255) { opaque = 0; break; }
        if (!opaque) {
            pn_obj *md = pn_new_dict();
            pn_dict_put(md, "Type", pn_new_name("XObject"));
            pn_dict_put(md, "Subtype", pn_new_name("Image"));
            pn_obj *ms = pn_new_stream(md, NULL, 0);
            pn_stream_set(ms, alpha, (size_t)w * (size_t)h, 1);
            pn_dict_put(ms, "Width", pn_new_int(w));
            pn_dict_put(ms, "Height", pn_new_int(h));
            pn_dict_put(ms, "ColorSpace", pn_new_name("DeviceGray"));
            pn_dict_put(ms, "BitsPerComponent", pn_new_int(8));
            pn_dict_put(s, "SMask", pn_add_ref(doc, ms));
        }
    }
    return pn_add_obj(doc, s);
}

int pn_add_image_page(pn_doc *doc, int imgnum, int w, int h, double pw, double ph, double margin) {
    pn_obj *img = pn_load(doc, imgnum);
    if (!img) return -1;
    if (w <= 0) w = (int)pn_int(pn_get(doc, img, "Width"), 1);
    if (h <= 0) h = (int)pn_int(pn_get(doc, img, "Height"), 1);
    if (pw <= 0 || ph <= 0) { pw = w * 0.75; ph = h * 0.75; margin = 0; } /* 96 dpi */
    double aw = pw - 2 * margin, ah = ph - 2 * margin;
    double sc = aw / w < ah / h ? aw / w : ah / h;
    double dw = w * sc, dh = h * sc;
    double x = (pw - dw) / 2, y = (ph - dh) / 2;
    pn_obj *page = pn_new_dict();
    pn_dict_put(page, "Type", pn_new_name("Page"));
    pn_obj *mb = pn_new_array();
    pn_arr_push(mb, pn_new_int(0)); pn_arr_push(mb, pn_new_int(0));
    pn_arr_push(mb, pn_new_real(pw)); pn_arr_push(mb, pn_new_real(ph));
    pn_dict_put(page, "MediaBox", mb);
    pn_obj *res = pn_new_dict(), *xo = pn_new_dict();
    pn_dict_put(xo, "Im1", pn_new_ref(imgnum, 0));
    pn_dict_put(res, "XObject", xo);
    pn_dict_put(page, "Resources", res);
    pn_buf ops; pn_buf_init(&ops);
    pn_buf_puts(&ops, "q ");
    pn_buf_put_real(&ops, dw); pn_buf_puts(&ops, " 0 0 ");
    pn_buf_put_real(&ops, dh); pn_buf_putc(&ops, ' ');
    pn_buf_put_real(&ops, x); pn_buf_putc(&ops, ' ');
    pn_buf_put_real(&ops, y); pn_buf_puts(&ops, " cm /Im1 Do Q\n");
    int pnum = pn_add_obj(doc, page);
    pn_page_set_content(doc, page, ops.data, ops.len);
    pn_buf_free(&ops);
    int n = pn_doc_page_count(doc);
    int *nums = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) nums[i] = pn_doc_page_num(doc, i);
    nums[n] = pnum;
    pn_rebuild_page_tree(doc, nums, n + 1);
    free(nums);
    return n;
}

void pn_images_json(pn_doc *doc, pn_buf *out) {
    pn_buf_putc(out, '[');
    int first = 1;
    for (int i = 1; i < doc->nxref; i++) {
        pn_obj *o = pn_load(doc, i);
        if (!pn_is(o, PN_STREAM) || !pn_is_name(pn_dict_get(o, "Subtype"), "Image")) continue;
        pn_obj *f = pn_resolve(doc, pn_dict_get(o, "Filter"));
        if (pn_is(f, PN_ARRAY)) f = pn_resolve(doc, pn_arr_get(f, pn_arr_len(f) - 1));
        int idx;
        int comps = pn_image_components(doc, o, &idx);
        pn_obj *im = pn_get(doc, o, "ImageMask");
        if (!first) pn_buf_putc(out, ',');
        first = 0;
        pn_buf_printf(out, "{\"num\":%d,\"w\":%lld,\"h\":%lld,\"bpc\":%lld,\"comps\":%d,\"indexed\":%s,\"mask\":%s,\"smask\":%s,\"filter\":",
                      i, pn_int(pn_get(doc, o, "Width"), 0), pn_int(pn_get(doc, o, "Height"), 0),
                      pn_int(pn_get(doc, o, "BitsPerComponent"), 0), comps, idx ? "true" : "false",
                      (pn_is(im, PN_BOOL) && im->u.b) ? "true" : "false",
                      pn_dict_get(o, "SMask") ? "true" : "false");
        pn_json_str(out, f ? pn_name(f) : "", -1);
        pn_buf_printf(out, ",\"len\":%zu,\"decode\":%s}", o->u.stm.len, pn_dict_get(o, "Decode") ? "true" : "false");
    }
    pn_buf_putc(out, ']');
}

/* Decode an image to 8-bit gray or RGB pixels (for re-encoding in the UI). */
int pn_image_pixels(pn_doc *doc, int num, pn_buf *out, int *pw, int *ph, int *pcomps) {
    pn_obj *img = pn_load(doc, num);
    if (!pn_is(img, PN_STREAM)) return 0;
    int w = (int)pn_int(pn_get(doc, img, "Width"), 0), h = (int)pn_int(pn_get(doc, img, "Height"), 0);
    int bpc = (int)pn_int(pn_get(doc, img, "BitsPerComponent"), 8);
    int indexed;
    int comps = pn_image_components(doc, img, &indexed);
    if (w <= 0 || h <= 0 || bpc != 8 || comps <= 0 || pn_dict_get(img, "Decode")) return 0;
    size_t len; int ok;
    unsigned char *d = pn_stream_decode(doc, img, &len, &ok);
    if (!d || !ok || len < (size_t)w * h * comps) { free(d); return 0; }
    size_t npx = (size_t)w * h;
    if (indexed) {
        pn_obj *cs = pn_get(doc, img, "ColorSpace");
        pn_obj *base = pn_resolve(doc, pn_arr_get(cs, 1));
        int bc = 3;
        if (pn_is_name(base, "DeviceGray")) bc = 1;
        else if (pn_is_name(base, "DeviceRGB")) bc = 3;
        else if (pn_is(base, PN_ARRAY) && pn_is_name(pn_resolve(doc, pn_arr_get(base, 0)), "ICCBased")) bc = (int)pn_int(pn_get(doc, pn_resolve(doc, pn_arr_get(base, 1)), "N"), 3);
        else { free(d); return 0; }
        if (bc != 1 && bc != 3) { free(d); return 0; }
        pn_obj *lk = pn_resolve(doc, pn_arr_get(cs, 3));
        const unsigned char *lut = NULL; size_t lutlen = 0; unsigned char *lutd = NULL;
        if (pn_is(lk, PN_STRING)) { lut = lk->u.str.s; lutlen = (size_t)lk->u.str.len; }
        else if (pn_is(lk, PN_STREAM)) { int ok2; lutd = pn_stream_decode(doc, lk, &lutlen, &ok2); lut = lutd; }
        if (!lut) { free(d); return 0; }
        pn_buf_reserve(out, npx * (size_t)bc);
        for (size_t i = 0; i < npx; i++) {
            size_t k = (size_t)d[i] * (size_t)bc;
            for (int c = 0; c < bc; c++) out->data[out->len++] = k + (size_t)c < lutlen ? lut[k + (size_t)c] : 0;
        }
        free(lutd);
        comps = bc;
    } else if (comps == 4) {
        pn_buf_reserve(out, npx * 3);
        for (size_t i = 0; i < npx; i++) {
            int c = d[i * 4], m = d[i * 4 + 1], y = d[i * 4 + 2], k = d[i * 4 + 3];
            out->data[out->len++] = (unsigned char)((255 - c) * (255 - k) / 255);
            out->data[out->len++] = (unsigned char)((255 - m) * (255 - k) / 255);
            out->data[out->len++] = (unsigned char)((255 - y) * (255 - k) / 255);
        }
        comps = 3;
    } else if (comps == 1 || comps == 3) {
        pn_buf_put(out, d, npx * (size_t)comps);
    } else { free(d); return 0; }
    free(d);
    *pw = w; *ph = h; *pcomps = comps;
    return 1;
}

int pn_image_replace(pn_doc *doc, int num, const unsigned char *data, size_t len, int w, int h, int comps, int is_jpeg) {
    pn_obj *old = pn_load(doc, num);
    if (!pn_is(old, PN_STREAM)) return -1;
    int tmp = pn_image_new(doc, data, len, w, h, comps, is_jpeg, NULL);
    if (!tmp) return -1;
    pn_obj *nw = pn_keep(pn_load(doc, tmp));
    pn_set_obj(doc, tmp, pn_null());
    doc->xref[tmp].type = 0;
    const char *keep[] = {"SMask", "Mask", "Interpolate", "Intent", "OC", "StructParent"};
    for (int i = 0; i < 6; i++) { pn_obj *v = pn_dict_get(old, keep[i]); if (v) pn_dict_put(nw, keep[i], pn_keep(v)); }
    pn_set_obj(doc, num, nw);
    return 0;
}
