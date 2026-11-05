/* Page numbers, headers and footers. */
#include "pn.h"
#include "pn_engine.h"
#include "pn_content.h"
#include "pn_tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Visual (as displayed, after /Rotate) frame of the page and the matrix mapping visual coords to user space. */
void pn_page_visual_frame(pn_doc *doc, pn_obj *page, double *vw, double *vh, pn_mat *m) {
    double b[4];
    pn_page_box(doc, page, "CropBox", b);
    double w = b[2] - b[0], h = b[3] - b[1];
    int r = pn_page_rotation(doc, page);
    pn_mat t = {1, 0, 0, 1, b[0], b[1]};
    *vw = w; *vh = h;
    if (r == 90) { t.a = 0; t.b = 1; t.c = -1; t.d = 0; t.e = b[2]; t.f = b[1]; *vw = h; *vh = w; }
    else if (r == 180) { t.a = -1; t.b = 0; t.c = 0; t.d = -1; t.e = b[2]; t.f = b[3]; }
    else if (r == 270) { t.a = 0; t.b = -1; t.c = 1; t.d = 0; t.e = b[0]; t.f = b[3]; *vw = h; *vh = w; }
    *m = t;
}

static void expand(const char *tmpl, int n, int total, pn_buf *out) {
    for (const char *p = tmpl; *p;) {
        if (!strncmp(p, "{n}", 3)) { pn_buf_printf(out, "%d", n); p += 3; }
        else if (!strncmp(p, "{total}", 7)) { pn_buf_printf(out, "%d", total); p += 7; }
        else {
            int u = pn_utf8_decode(&p);
            int c = pn_unicode_to_winansi(u);
            pn_buf_putc(out, c < 0 ? '?' : c);
        }
    }
}

int pn_stamp_text(pn_doc *doc, const char *tmpl, int position, double size, double margin,
                  int from, int to, int start_number, const char *font, int rgb) {
    int np = pn_doc_page_count(doc);
    if (from < 0) from = 0;
    if (to < 0 || to >= np) to = np - 1;
    if (size <= 0) size = 10;
    if (margin < 0) margin = 24;
    int b14 = pn_base14_index(font && *font ? font : "Helvetica");
    if (b14 < 0) b14 = 4;
    int total = start_number + (to - from);
    for (int i = from; i <= to; i++) {
        pn_obj *page = pn_doc_page(doc, i);
        if (!page) continue;
        double vw, vh;
        pn_mat m;
        pn_page_visual_frame(doc, page, &vw, &vh, &m);
        pn_buf txt; pn_buf_init(&txt);
        expand(tmpl, start_number + (i - from), total, &txt);
        double tw = pn_base14_text_width(b14, txt.data, (int)txt.len) / 1000.0 * size;
        int col = position % 3, top = position < 3;
        double x = col == 0 ? margin : col == 1 ? (vw - tw) / 2 : vw - margin - tw;
        double y = top ? vh - margin - size * 0.75 : margin;
        if (pn_overlay_begin(doc, i) < 0) { pn_buf_free(&txt); continue; }
        const char *fn = pn_overlay_font(pn_base14_names[b14]);
        pn_buf ops; pn_buf_init(&ops);
        pn_buf_puts(&ops, "q ");
        pn_buf_put_real(&ops, m.a); pn_buf_putc(&ops, ' '); pn_buf_put_real(&ops, m.b); pn_buf_putc(&ops, ' ');
        pn_buf_put_real(&ops, m.c); pn_buf_putc(&ops, ' '); pn_buf_put_real(&ops, m.d); pn_buf_putc(&ops, ' ');
        pn_buf_put_real(&ops, m.e); pn_buf_putc(&ops, ' '); pn_buf_put_real(&ops, m.f);
        pn_buf_printf(&ops, " cm BT /%s ", fn);
        pn_buf_put_real(&ops, size);
        pn_buf_printf(&ops, " Tf %.3f %.3f %.3f rg ", ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
        pn_buf_put_real(&ops, x); pn_buf_putc(&ops, ' '); pn_buf_put_real(&ops, y);
        pn_buf_puts(&ops, " Td <");
        for (size_t k = 0; k < txt.len; k++) pn_buf_printf(&ops, "%02X", txt.data[k]);
        pn_buf_puts(&ops, "> Tj ET Q");
        pn_overlay_ops((const char *)ops.data);
        pn_overlay_end();
        pn_buf_free(&ops);
        pn_buf_free(&txt);
    }
    return 0;
}
