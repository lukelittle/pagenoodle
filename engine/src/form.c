/* Interactive forms (AcroForm): enumerate fields, set values with generated appearances, flatten. */
#include "pn.h"
#include "pn_engine.h"
#include "pn_content.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FF_READONLY (1 << 0)
#define FF_MULTILINE (1 << 12)
#define FF_RADIO (1 << 15)
#define FF_PUSH (1 << 16)
#define FF_COMBO (1 << 17)

static pn_obj *inh(pn_doc *doc, pn_obj *f, const char *key) {
    for (int d = 0; f && d < 32; d++) {
        pn_obj *v = pn_get(doc, f, key);
        if (v) return v;
        f = pn_get(doc, f, "Parent");
    }
    return NULL;
}

/* widget object number -> page index */
static int *widget_pages(pn_doc *doc) {
    int *map = (int *)malloc(sizeof(int) * (size_t)(doc->nxref + 1));
    for (int i = 0; i <= doc->nxref; i++) map[i] = -1;
    int np = pn_doc_page_count(doc);
    for (int p = 0; p < np; p++) {
        pn_obj *annots = pn_get(doc, pn_doc_page(doc, p), "Annots");
        for (int i = 0; i < pn_arr_len(annots); i++) {
            pn_obj *r = pn_arr_get(annots, i);
            if (r && r->type == PN_REF && r->u.ref.num < doc->nxref) map[r->u.ref.num] = p;
        }
    }
    return map;
}

static void put_pdfstr_json(pn_buf *out, pn_obj *s) {
    if (pn_is(s, PN_STRING)) {
        pn_buf t; pn_buf_init(&t);
        pn_pdfstring_to_utf8(s->u.str.s, s->u.str.len, &t);
        pn_json_str(out, t.data ? (const char *)t.data : "", (int)t.len);
        pn_buf_free(&t);
    } else if (pn_is(s, PN_NAME)) pn_json_str(out, pn_name(s), -1);
    else pn_buf_puts(out, "\"\"");
}

static void on_states(pn_doc *doc, pn_obj *widget, pn_buf *out) {
    pn_obj *n = pn_get(doc, pn_get(doc, widget, "AP"), "N");
    pn_obj *d = pn_get(doc, pn_get(doc, widget, "AP"), "D");
    pn_obj *src = pn_is(n, PN_DICT) ? n : d;
    for (int i = 0; i < pn_dict_len(src); i++) {
        const char *k = pn_dict_key(src, i);
        if (strcmp(k, "Off")) { pn_json_str(out, k, -1); return; }
    }
    pn_buf_puts(out, "\"Yes\"");
}

static void emit_field(pn_doc *doc, pn_obj *ref, const char *prefix, int *wp, pn_buf *out, int *first, int depth) {
    if (depth > 16) return;
    pn_obj *f = pn_resolve(doc, ref);
    if (!pn_is(f, PN_DICT)) return;
    char name[512];
    pn_obj *t = pn_get(doc, f, "T");
    pn_buf tn; pn_buf_init(&tn);
    if (pn_is(t, PN_STRING)) pn_pdfstring_to_utf8(t->u.str.s, t->u.str.len, &tn);
    if (prefix[0] && tn.len) snprintf(name, sizeof name, "%s.%s", prefix, tn.data);
    else snprintf(name, sizeof name, "%s", tn.len ? (const char *)tn.data : prefix);
    pn_buf_free(&tn);
    pn_obj *kids = pn_get(doc, f, "Kids");
    /* do kids contain fields (with /T) or only widgets? */
    int kid_fields = 0;
    for (int i = 0; i < pn_arr_len(kids); i++) if (pn_get(doc, pn_resolve(doc, pn_arr_get(kids, i)), "T")) kid_fields = 1;
    if (kid_fields) {
        for (int i = 0; i < pn_arr_len(kids); i++) emit_field(doc, pn_arr_get(kids, i), name, wp, out, first, depth + 1);
        return;
    }
    if (!ref || ref->type != PN_REF) return;
    const char *ft = pn_name(inh(doc, f, "FT"));
    long long ff = pn_int(inh(doc, f, "Ff"), 0);
    const char *type = "text";
    if (!strcmp(ft, "Btn")) type = (ff & FF_PUSH) ? "button" : (ff & FF_RADIO) ? "radio" : "checkbox";
    else if (!strcmp(ft, "Ch")) type = (ff & FF_COMBO) ? "combo" : "list";
    else if (!strcmp(ft, "Sig")) type = "signature";
    else if (strcmp(ft, "Tx")) return;
    if (!*first) pn_buf_putc(out, ',');
    *first = 0;
    pn_buf_printf(out, "{\"id\":%d,\"name\":", ref->u.ref.num);
    pn_json_str(out, name, -1);
    pn_buf_printf(out, ",\"type\":\"%s\",\"readonly\":%s,\"multiline\":%s,\"maxlen\":%lld,\"value\":", type,
                  (ff & FF_READONLY) ? "true" : "false", (ff & FF_MULTILINE) ? "true" : "false", pn_int(inh(doc, f, "MaxLen"), 0));
    put_pdfstr_json(out, inh(doc, f, "V"));
    pn_buf_puts(out, ",\"options\":[");
    pn_obj *opt = inh(doc, f, "Opt");
    for (int i = 0; i < pn_arr_len(opt); i++) {
        if (i) pn_buf_putc(out, ',');
        pn_obj *o = pn_resolve(doc, pn_arr_get(opt, i));
        if (pn_is(o, PN_ARRAY)) o = pn_resolve(doc, pn_arr_get(o, 0));
        put_pdfstr_json(out, o);
    }
    pn_buf_puts(out, "],\"widgets\":[");
    int nw = 0;
    pn_obj *wl[256]; pn_obj *wr[256];
    if (pn_arr_len(kids)) for (int i = 0; i < pn_arr_len(kids) && nw < 256; i++) { wr[nw] = pn_arr_get(kids, i); wl[nw++] = pn_resolve(doc, pn_arr_get(kids, i)); }
    else { wr[0] = ref; wl[0] = f; nw = 1; }
    for (int i = 0; i < nw; i++) {
        if (i) pn_buf_putc(out, ',');
        int num = (wr[i] && wr[i]->type == PN_REF) ? wr[i]->u.ref.num : 0;
        int page = num > 0 && num < doc->nxref ? wp[num] : -1;
        pn_obj *rect = pn_get(doc, wl[i], "Rect");
        pn_buf_printf(out, "{\"id\":%d,\"page\":%d,\"rect\":[", num, page);
        for (int k = 0; k < 4; k++) { if (k) pn_buf_putc(out, ','); pn_buf_put_real(out, pn_num(pn_resolve(doc, pn_arr_get(rect, k)), 0)); }
        pn_buf_puts(out, "],\"on\":");
        on_states(doc, wl[i], out);
        pn_buf_puts(out, ",\"state\":");
        put_pdfstr_json(out, pn_get(doc, wl[i], "AS"));
        pn_buf_putc(out, '}');
    }
    pn_buf_puts(out, "]}");
}

void pn_form_json(pn_doc *doc, pn_buf *out) {
    pn_obj *af = pn_get(doc, pn_catalog(doc), "AcroForm");
    pn_obj *fields = pn_get(doc, af, "Fields");
    int *wp = widget_pages(doc);
    int first = 1;
    pn_buf_putc(out, '[');
    for (int i = 0; i < pn_arr_len(fields); i++) emit_field(doc, pn_arr_get(fields, i), "", wp, out, &first, 0);
    pn_buf_putc(out, ']');
    free(wp);
}

/* ---------- appearance generation for text/choice ---------- */
static void winansi(const char *utf8, pn_buf *out) {
    const char *p = utf8;
    while (*p) {
        int u = pn_utf8_decode(&p);
        int c = pn_unicode_to_winansi(u);
        pn_buf_putc(out, c < 0 ? '?' : c);
    }
}

static void put_hex(pn_buf *b, const unsigned char *s, size_t n) {
    static const char hx[] = "0123456789ABCDEF";
    pn_buf_putc(b, '<');
    for (size_t i = 0; i < n; i++) { pn_buf_putc(b, hx[s[i] >> 4]); pn_buf_putc(b, hx[s[i] & 15]); }
    pn_buf_putc(b, '>');
}

static void text_appearance(pn_doc *doc, pn_obj *field, pn_obj *widget, const char *value) {
    pn_obj *rect = pn_get(doc, widget, "Rect");
    double r[4];
    for (int k = 0; k < 4; k++) r[k] = pn_num(pn_resolve(doc, pn_arr_get(rect, k)), 0);
    double w = r[2] > r[0] ? r[2] - r[0] : r[0] - r[2], h = r[3] > r[1] ? r[3] - r[1] : r[1] - r[3];
    if (w <= 0 || h <= 0) return;
    double size = 0;
    double col[3] = {0, 0, 0};
    pn_obj *da = inh(doc, field, "DA");
    if (!da) da = pn_get(doc, pn_get(doc, pn_catalog(doc), "AcroForm"), "DA");
    if (pn_is(da, PN_STRING)) {
        pn_content *c = pn_content_parse(da->u.str.s, (size_t)da->u.str.len);
        for (int i = 0; i < c->n; i++) {
            if (!strcmp(c->ops[i].name, "Tf") && c->ops[i].nargs >= 2) size = pn_num(c->ops[i].args[1], 0);
            if (!strcmp(c->ops[i].name, "rg") && c->ops[i].nargs >= 3) for (int k = 0; k < 3; k++) col[k] = pn_num(c->ops[i].args[k], 0);
            if (!strcmp(c->ops[i].name, "g") && c->ops[i].nargs >= 1) col[0] = col[1] = col[2] = pn_num(c->ops[i].args[0], 0);
        }
        pn_content_free(c);
    }
    long long ff = pn_int(inh(doc, field, "Ff"), 0);
    int multiline = (ff & FF_MULTILINE) != 0;
    pn_buf enc; pn_buf_init(&enc);
    winansi(value, &enc);
    if (size <= 0) {
        size = multiline ? 10 : h * 0.65;
        if (size > 12) size = 12;
        double tw = pn_base14_text_width(4, enc.data, (int)enc.len) / 1000.0;
        if (!multiline && tw * size > w - 4 && tw > 0) size = (w - 4) / tw;
        if (size < 4) size = 4;
    }
    pn_buf ops; pn_buf_init(&ops);
    pn_buf_puts(&ops, "/Tx BMC\nq\n1 1 ");
    pn_buf_put_real(&ops, w - 2); pn_buf_putc(&ops, ' ');
    pn_buf_put_real(&ops, h - 2); pn_buf_puts(&ops, " re W n\nBT\n/Helv ");
    pn_buf_put_real(&ops, size);
    pn_buf_printf(&ops, " Tf %.3f %.3f %.3f rg\n", col[0], col[1], col[2]);
    if (!multiline) {
        double y = (h - size * 0.78) / 2;
        pn_buf_puts(&ops, "2 "); pn_buf_put_real(&ops, y); pn_buf_puts(&ops, " Td ");
        put_hex(&ops, enc.data, enc.len);
        pn_buf_puts(&ops, " Tj\n");
    } else {
        double lead = size * 1.15;
        pn_buf_puts(&ops, "2 "); pn_buf_put_real(&ops, h - 2 - size); pn_buf_puts(&ops, " Td ");
        pn_buf_put_real(&ops, lead); pn_buf_puts(&ops, " TL\n");
        /* greedy word wrap */
        size_t i = 0;
        while (i < enc.len) {
            size_t lineend = i, lastspace = 0;
            int has_space = 0;
            double lw = 0;
            while (lineend < enc.len && enc.data[lineend] != '\n') {
                double cw = pn_base14_text_width(4, enc.data + lineend, 1) / 1000.0 * size;
                if (lw + cw > w - 4 && lineend > i) break;
                if (enc.data[lineend] == ' ') { lastspace = lineend; has_space = 1; }
                lw += cw;
                lineend++;
            }
            size_t cut = lineend;
            if (lineend < enc.len && enc.data[lineend] != '\n' && has_space) cut = lastspace;
            put_hex(&ops, enc.data + i, cut - i);
            pn_buf_puts(&ops, " Tj T*\n");
            i = cut;
            if (i < enc.len && (enc.data[i] == ' ' || enc.data[i] == '\n')) i++;
        }
    }
    pn_buf_puts(&ops, "ET\nQ\nEMC\n");
    pn_obj *ap = pn_new_stream(NULL, NULL, 0);
    pn_stream_set(ap, ops.data, ops.len, 1);
    pn_dict_put(ap, "Type", pn_new_name("XObject"));
    pn_dict_put(ap, "Subtype", pn_new_name("Form"));
    pn_obj *bb = pn_new_array();
    pn_arr_push(bb, pn_new_int(0)); pn_arr_push(bb, pn_new_int(0)); pn_arr_push(bb, pn_new_real(w)); pn_arr_push(bb, pn_new_real(h));
    pn_dict_put(ap, "BBox", bb);
    pn_obj *res = pn_new_dict(), *fonts = pn_new_dict(), *helv = pn_new_dict();
    pn_dict_put(helv, "Type", pn_new_name("Font"));
    pn_dict_put(helv, "Subtype", pn_new_name("Type1"));
    pn_dict_put(helv, "BaseFont", pn_new_name("Helvetica"));
    pn_dict_put(helv, "Encoding", pn_new_name("WinAnsiEncoding"));
    pn_dict_put(fonts, "Helv", pn_add_ref(doc, helv));
    pn_dict_put(res, "Font", fonts);
    pn_dict_put(ap, "Resources", res);
    pn_obj *apd = pn_new_dict();
    pn_dict_put(apd, "N", pn_add_ref(doc, ap));
    pn_dict_put(widget, "AP", apd);
    pn_buf_free(&ops);
    pn_buf_free(&enc);
}

int pn_form_set(pn_doc *doc, int fieldnum, const char *value) {
    pn_obj *f = pn_load(doc, fieldnum);
    if (!pn_is(f, PN_DICT)) { pn_set_error("No such field"); return -1; }
    const char *ft = pn_name(inh(doc, f, "FT"));
    long long ff = pn_int(inh(doc, f, "Ff"), 0);
    pn_obj *kids = pn_get(doc, f, "Kids");
    int nw = pn_arr_len(kids);
    pn_obj *af = pn_get(doc, pn_catalog(doc), "AcroForm");
    if (!strcmp(ft, "Btn")) {
        if (ff & FF_PUSH) return 0;
        const char *v = (value && *value) ? value : "Off";
        pn_dict_put(f, "V", pn_new_name(v));
        for (int i = 0; i < (nw ? nw : 1); i++) {
            pn_obj *w = nw ? pn_resolve(doc, pn_arr_get(kids, i)) : f;
            pn_obj *n = pn_get(doc, pn_get(doc, w, "AP"), "N");
            int has = pn_is(n, PN_DICT) && pn_dict_get(n, v);
            pn_dict_put(w, "AS", pn_new_name(has ? v : "Off"));
        }
        return 0;
    }
    if (!strcmp(ft, "Tx") || !strcmp(ft, "Ch")) {
        pn_buf s; pn_buf_init(&s);
        pn_utf8_to_pdfstring(value ? value : "", &s);
        pn_dict_put(f, "V", pn_new_string(s.data ? s.data : (unsigned char *)"", (int)s.len));
        pn_buf_free(&s);
        for (int i = 0; i < (nw ? nw : 1); i++) {
            pn_obj *w = nw ? pn_resolve(doc, pn_arr_get(kids, i)) : f;
            if (pn_is(w, PN_DICT)) text_appearance(doc, f, w, value ? value : "");
        }
        if (pn_is(af, PN_DICT)) pn_dict_put(af, "NeedAppearances", pn_new_bool(1));
        return 0;
    }
    pn_set_error("Unsupported field type");
    return -1;
}

/* Flatten all widget appearances into page content and remove the form. */
int pn_form_flatten(pn_doc *doc) {
    int np = pn_doc_page_count(doc);
    int count = 0;
    for (int p = 0; p < np; p++) {
        pn_obj *page = pn_doc_page(doc, p);
        pn_obj *annots = pn_get(doc, page, "Annots");
        if (!pn_arr_len(annots)) continue;
        pn_obj *keep = pn_new_array();
        int started = 0;
        for (int i = 0; i < pn_arr_len(annots); i++) {
            pn_obj *ar = pn_arr_get(annots, i);
            pn_obj *a = pn_resolve(doc, ar);
            if (!pn_is_name(pn_get(doc, a, "Subtype"), "Widget")) { pn_arr_push(keep, pn_keep(ar)); continue; }
            long long flags = pn_int(pn_get(doc, a, "F"), 0);
            pn_obj *n = pn_get(doc, pn_get(doc, a, "AP"), "N");
            if (pn_is(n, PN_DICT)) n = pn_get(doc, n, pn_name(pn_get(doc, a, "AS")));
            if (!pn_is(n, PN_STREAM) || (flags & 2)) continue; /* hidden or no appearance */
            int apnum = 0;
            pn_obj *apref = pn_dict_get(pn_get(doc, a, "AP"), "N");
            if (apref && apref->type == PN_REF) apnum = apref->u.ref.num;
            else {
                pn_obj *nd = pn_get(doc, pn_get(doc, a, "AP"), "N");
                if (pn_is(nd, PN_DICT)) { pn_obj *sr = pn_dict_get(nd, pn_name(pn_get(doc, a, "AS"))); if (sr && sr->type == PN_REF) apnum = sr->u.ref.num; }
            }
            if (!apnum) apnum = pn_add_obj(doc, pn_keep(n));
            pn_obj *rect = pn_get(doc, a, "Rect");
            double r[4];
            for (int k = 0; k < 4; k++) r[k] = pn_num(pn_resolve(doc, pn_arr_get(rect, k)), 0);
            pn_obj *bbox = pn_get(doc, n, "BBox");
            pn_rect bb = {0, 0, 1, 1};
            if (pn_arr_len(bbox) >= 4) {
                bb.x0 = pn_num(pn_resolve(doc, pn_arr_get(bbox, 0)), 0); bb.y0 = pn_num(pn_resolve(doc, pn_arr_get(bbox, 1)), 0);
                bb.x1 = pn_num(pn_resolve(doc, pn_arr_get(bbox, 2)), 1); bb.y1 = pn_num(pn_resolve(doc, pn_arr_get(bbox, 3)), 1);
            }
            pn_mat fm = pn_mat_from_array(doc, pn_get(doc, n, "Matrix"));
            pn_rect tb = pn_rect_transform(bb, fm);
            double rx0 = r[0] < r[2] ? r[0] : r[2], ry0 = r[1] < r[3] ? r[1] : r[3];
            double rw = r[2] > r[0] ? r[2] - r[0] : r[0] - r[2], rh = r[3] > r[1] ? r[3] - r[1] : r[1] - r[3];
            double tw = tb.x1 - tb.x0, th = tb.y1 - tb.y0;
            if (tw <= 0 || th <= 0) continue;
            double sx = rw / tw, sy = rh / th;
            if (!started) { pn_overlay_begin(doc, p); started = 1; }
            const char *nm = pn_overlay_xobject(apnum);
            char ops[256];
            snprintf(ops, sizeof ops, "q %.4f 0 0 %.4f %.4f %.4f cm /%s Do Q", sx, sy, rx0 - tb.x0 * sx, ry0 - tb.y0 * sy, nm);
            pn_overlay_ops(ops);
            count++;
        }
        if (started) pn_overlay_end();
        page = pn_doc_page(doc, p);
        pn_dict_put(page, "Annots", keep);
    }
    pn_dict_del(pn_catalog(doc), "AcroForm");
    return count;
}
