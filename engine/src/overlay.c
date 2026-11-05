/* Overlays: append new drawing (annotations, signatures, stamps, OCR text) on top of a page. */
#include "pn.h"
#include "pn_engine.h"
#include "pn_content.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    pn_doc *doc;
    pn_obj *page;
    pn_obj *res;
    pn_buf ops;
    char name[64];
    struct { char base[64]; char res[64]; } fonts[16];
    int nfonts;
} ov;

int pn_overlay_begin(pn_doc *doc, int pageidx) {
    pn_obj *page = pn_doc_page(doc, pageidx);
    if (!page) return -1;
    pn_buf_free(&ov.ops);
    ov.doc = doc; ov.page = page; ov.nfonts = 0;
    ov.res = pn_page_private_resources(doc, page);
    pn_buf_init(&ov.ops);
    return 0;
}

const char *pn_overlay_font(const char *basefont) {
    if (!ov.doc) return "";
    for (int i = 0; i < ov.nfonts; i++) if (!strcmp(ov.fonts[i].base, basefont)) return ov.fonts[i].res;
    pn_obj *fonts = pn_res_subdict(ov.doc, ov.res, "Font");
    pn_obj *f = pn_new_dict();
    pn_dict_put(f, "Type", pn_new_name("Font"));
    pn_dict_put(f, "Subtype", pn_new_name("Type1"));
    pn_dict_put(f, "BaseFont", pn_new_name(basefont));
    if (strcmp(basefont, "Symbol") && strcmp(basefont, "ZapfDingbats")) pn_dict_put(f, "Encoding", pn_new_name("WinAnsiEncoding"));
    pn_res_unique_name(ov.doc, fonts, "PnF", ov.name, sizeof ov.name);
    pn_dict_put(fonts, ov.name, pn_add_ref(ov.doc, f));
    if (ov.nfonts < 16) {
        snprintf(ov.fonts[ov.nfonts].base, sizeof ov.fonts[0].base, "%s", basefont);
        snprintf(ov.fonts[ov.nfonts].res, sizeof ov.fonts[0].res, "%s", ov.name);
        ov.nfonts++;
    }
    return ov.name;
}

const char *pn_overlay_gstate(double ca, double CA, const char *blend) {
    if (!ov.doc) return "";
    pn_obj *gs = pn_res_subdict(ov.doc, ov.res, "ExtGState");
    pn_obj *d = pn_new_dict();
    pn_dict_put(d, "Type", pn_new_name("ExtGState"));
    pn_dict_put(d, "ca", pn_new_real(ca));
    pn_dict_put(d, "CA", pn_new_real(CA));
    if (blend && *blend) pn_dict_put(d, "BM", pn_new_name(blend));
    pn_res_unique_name(ov.doc, gs, "PnG", ov.name, sizeof ov.name);
    pn_dict_put(gs, ov.name, pn_add_ref(ov.doc, d));
    return ov.name;
}

const char *pn_overlay_xobject(int objnum) {
    if (!ov.doc) return "";
    pn_obj *xo = pn_res_subdict(ov.doc, ov.res, "XObject");
    pn_res_unique_name(ov.doc, xo, "PnX", ov.name, sizeof ov.name);
    pn_dict_put(xo, ov.name, pn_new_ref(objnum, 0));
    return ov.name;
}

void pn_overlay_ops(const char *ops) { if (ov.doc) { pn_buf_puts(&ov.ops, ops); pn_buf_putc(&ov.ops, '\n'); } }

int pn_overlay_end(void) {
    if (!ov.doc) return -1;
    pn_doc *doc = ov.doc;
    pn_obj *page = ov.page;
    pn_obj *contents = pn_new_array();
    pn_obj *qs = pn_new_stream(NULL, NULL, 0);
    pn_stream_set(qs, (const unsigned char *)"q\n", 2, 0);
    pn_arr_push(contents, pn_add_ref(doc, qs));
    pn_obj *c = pn_dict_get(page, "Contents");
    pn_obj *cr = pn_resolve(doc, c);
    if (pn_is(cr, PN_ARRAY)) {
        for (int i = 0; i < pn_arr_len(cr); i++) pn_arr_push(contents, pn_keep(pn_arr_get(cr, i)));
    } else if (c && c->type == PN_REF) {
        pn_arr_push(contents, pn_keep(c));
    } else if (pn_is(c, PN_STREAM)) {
        pn_arr_push(contents, pn_add_ref(doc, pn_keep(c)));
    }
    pn_buf b; pn_buf_init(&b);
    pn_buf_puts(&b, "\nQ\nq\n");
    pn_buf_put(&b, ov.ops.data, ov.ops.len);
    pn_buf_puts(&b, "\nQ\n");
    pn_obj *ns = pn_new_stream(NULL, NULL, 0);
    pn_stream_set(ns, b.data, b.len, 1);
    pn_arr_push(contents, pn_add_ref(doc, ns));
    pn_dict_put(page, "Contents", contents);
    pn_buf_free(&b);
    pn_buf_free(&ov.ops);
    ov.doc = NULL; ov.page = NULL; ov.res = NULL;
    return 0;
}
