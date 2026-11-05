/* Content interpreter for text extraction, search, and true redaction (content rewriting). */
#include "pn.h"
#include "pn_content.h"
#include "pn_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    pn_mat ctm;
    double tc, tw, th, tl, tfs, trise;
    int tr;
    pn_font *font;
} gstate;

typedef struct {
    unsigned uni[4];
    int nuni;
    pn_rect box;
    double size;
} glyph;

typedef struct walker {
    pn_doc *doc;
    pn_font *fonts;
    int redact;
    /* extraction */
    glyph *g; int ng, capg;
    /* redaction */
    const pn_rect *rects; int nrects;
    pn_redact_stats *stats;
    pn_buf removed_text;
    int depth;
} walker;

static void add_glyph(walker *w, const unsigned *uni, int nuni, pn_rect box, double size) {
    if (w->ng == w->capg) { w->capg = w->capg ? w->capg * 2 : 1024; w->g = (glyph *)realloc(w->g, sizeof(glyph) * (size_t)w->capg); }
    glyph *g = &w->g[w->ng++];
    g->nuni = nuni > 4 ? 4 : nuni;
    for (int i = 0; i < g->nuni; i++) g->uni[i] = uni[i];
    g->box = box; g->size = size;
}

static void put_utf8(pn_buf *b, unsigned u) {
    if (u < 0x80) pn_buf_putc(b, (int)u);
    else if (u < 0x800) { pn_buf_putc(b, 0xC0 | (u >> 6)); pn_buf_putc(b, 0x80 | (u & 63)); }
    else if (u < 0x10000) { pn_buf_putc(b, 0xE0 | (u >> 12)); pn_buf_putc(b, 0x80 | ((u >> 6) & 63)); pn_buf_putc(b, 0x80 | (u & 63)); }
    else { pn_buf_putc(b, 0xF0 | (u >> 18)); pn_buf_putc(b, 0x80 | ((u >> 12) & 63)); pn_buf_putc(b, 0x80 | ((u >> 6) & 63)); pn_buf_putc(b, 0x80 | (u & 63)); }
}

/* A glyph counts as covered if its centre lies in the rect or >=10% of its area overlaps. */
int pn_glyph_hit(pn_rect g, pn_rect r) {
    if (!pn_rect_intersects(g, r)) return 0;
    double cx = (g.x0 + g.x1) / 2, cy = (g.y0 + g.y1) / 2;
    if (cx >= r.x0 && cx <= r.x1 && cy >= r.y0 && cy <= r.y1) return 1;
    double ix = (g.x1 < r.x1 ? g.x1 : r.x1) - (g.x0 > r.x0 ? g.x0 : r.x0);
    double iy = (g.y1 < r.y1 ? g.y1 : r.y1) - (g.y0 > r.y0 ? g.y0 : r.y0);
    double ga = (g.x1 - g.x0) * (g.y1 - g.y0);
    return ga <= 0 || ix * iy >= 0.1 * ga;
}

static int hits(walker *w, pn_rect r) {
    if (r.x1 - r.x0 <= 1e-6 && r.y1 - r.y0 <= 1e-6) return 0;
    for (int i = 0; i < w->nrects; i++) if (pn_rect_intersects(r, w->rects[i])) return 1;
    return 0;
}
static int glyph_hits(walker *w, pn_rect r) {
    if (r.x1 - r.x0 <= 1e-6 && r.y1 - r.y0 <= 1e-6) return 0;
    for (int i = 0; i < w->nrects; i++) if (pn_glyph_hit(r, w->rects[i])) return 1;
    return 0;
}

/* Show a sequence of TJ items. Returns 1 if any glyph was removed (redact mode); newtj receives the rewritten array. */
static int show_text(walker *w, gstate *g, pn_mat *tm, pn_obj **items, int nitems, pn_obj *newtj) {
    int removed_any = 0;
    pn_font *f = g->font;
    pn_font dummy;
    if (!f) {
        memset(&dummy, 0, sizeof dummy);
        dummy.ascent = 0.8; dummy.descent = -0.2; dummy.dw = 1000; dummy.base14 = -1;
        for (int i = 0; i < 256; i++) { dummy.widths[i] = 500; dummy.uni[i] = (unsigned)i; }
        f = &dummy;
    }
    double scale = g->tfs * g->th;
    pn_buf seg; pn_buf_init(&seg);
    int seg_hex = 0;
    double pending = 0;
    int have_pending = 0;
#define FLUSH_NUM() do { if (have_pending && newtj) { if (fabs(pending) > 1e-6) pn_arr_push(newtj, pn_new_real(pending)); } pending = 0; have_pending = 0; } while (0)
#define FLUSH_SEG() do { if (seg.len && newtj) { FLUSH_NUM(); pn_obj *s_ = pn_new_string(seg.data, (int)seg.len); s_->u.str.hex = seg_hex; pn_arr_push(newtj, s_); } seg.len = 0; } while (0)
    for (int it = 0; it < nitems; it++) {
        pn_obj *o = items[it];
        if (pn_is_number(o)) {
            double n = pn_num(o, 0);
            double tx = -n / 1000.0 * scale;
            tm->e += tx * tm->a; tm->f += tx * tm->b;
            FLUSH_SEG();
            pending += n; have_pending = 1;
            continue;
        }
        if (!pn_is(o, PN_STRING)) continue;
        const unsigned char *s = o->u.str.s;
        int len = o->u.str.len;
        seg_hex = o->u.str.hex || f->composite;
        int pos = 0;
        while (pos < len) {
            unsigned code;
            int nb = pn_font_next_code(f, s + pos, len - pos, &code);
            double w0 = pn_font_width(f, code) / 1000.0;
            pn_mat fm = {g->tfs * g->th, 0, 0, g->tfs, 0, g->trise};
            pn_mat trm = pn_mat_mul(pn_mat_mul(fm, *tm), g->ctm);
            pn_rect gr = {0, f->descent, w0 > 0 ? w0 : 0.3, f->ascent};
            pn_rect box = pn_rect_transform(gr, trm);
            double adv = (w0 * g->tfs + g->tc + ((nb == 1 && code == 32) ? g->tw : 0)) * g->th;
            if (w->redact) {
                int rm = g->tfs != 0 && glyph_hits(w, box);
                if (rm) {
                    removed_any = 1;
                    w->stats->glyphs++;
                    unsigned uni[4];
                    int nu = pn_font_unicode(f, code, nb, uni, 4);
                    for (int k = 0; k < nu; k++) if (w->removed_text.len < 4000) put_utf8(&w->removed_text, uni[k]);
                    FLUSH_SEG();
                    if (fabs(scale) > 1e-9) { pending += -adv / scale * 1000.0; have_pending = 1; }
                } else {
                    if (have_pending) FLUSH_NUM();
                    pn_buf_put(&seg, s + pos, (size_t)nb);
                }
            } else if (g->tfs != 0) {
                unsigned uni[4];
                int nu = pn_font_unicode(f, code, nb, uni, 4);
                if (nu == 0) { uni[0] = 0xFFFD; nu = 1; }
                double size = fabs(g->tfs) * sqrt(fabs(g->ctm.a * g->ctm.d - g->ctm.b * g->ctm.c)) * sqrt(fabs(tm->a * tm->d - tm->b * tm->c));
                add_glyph(w, uni, nu, box, size);
            }
            tm->e += adv * tm->a; tm->f += adv * tm->b;
            pos += nb;
        }
    }
    FLUSH_SEG();
    FLUSH_NUM();
    pn_buf_free(&seg);
    return removed_any;
#undef FLUSH_SEG
#undef FLUSH_NUM
}

static pn_obj *lookup_res(pn_doc *doc, pn_obj *res, const char *cat, const char *name) {
    return pn_get(doc, pn_get(doc, res, cat), name);
}

static pn_obj *private_xobjects(walker *w, pn_obj *out_res) {
    (void)w;
    pn_obj *x = pn_dict_get(out_res, "XObject");
    if (pn_is(x, PN_DICT)) return x; /* already a direct dict owned by out_res (private copy) */
    return pn_res_subdict(w->doc, out_res, "XObject");
}

static void walk(walker *w, const unsigned char *data, size_t len, pn_obj *res, pn_mat basectm, pn_buf *out, pn_obj *out_res, int toplevel);

/* Drop XObject resource entries that the (rewritten) content no longer uses, so replaced
 * originals are not kept alive in the saved file. */
static void prune_xobjects(pn_doc *doc, pn_obj *res, const unsigned char *data, size_t len) {
    pn_obj *xo = pn_dict_get(res, "XObject");
    if (!xo) return;
    if (xo->type == PN_REF) xo = pn_res_subdict(doc, res, "XObject");
    if (!pn_is(xo, PN_DICT)) return;
    pn_content *c = pn_content_parse(data, len);
    for (int i = pn_dict_len(xo) - 1; i >= 0; i--) {
        const char *k = pn_dict_key(xo, i);
        int used = 0;
        for (int j = 0; j < c->n && !used; j++)
            if (!strcmp(c->ops[j].name, "Do") && c->ops[j].nargs && !strcmp(pn_name(c->ops[j].args[0]), k)) used = 1;
        if (!used) pn_dict_del(xo, k);
    }
    pn_content_free(c);
}

static int redact_form(walker *w, pn_obj *form, pn_obj *res, pn_mat ctm, pn_obj *out_res, char *newname) {
    pn_mat fmat = pn_mat_from_array(w->doc, pn_get(w->doc, form, "Matrix"));
    pn_mat fctm = pn_mat_mul(fmat, ctm);
    pn_obj *fres = pn_get(w->doc, form, "Resources");
    if (!fres) fres = res;
    size_t dl; int ok;
    unsigned char *d = pn_stream_decode(w->doc, form, &dl, &ok);
    if (!d) return 0;
    pn_obj *nres = pn_is(fres, PN_DICT) ? pn_shallow_copy(fres) : pn_new_dict();
    pn_buf ob; pn_buf_init(&ob);
    int before = w->stats->glyphs + w->stats->images_removed + w->stats->images_scrubbed;
    walk(w, d, dl, fres, fctm, &ob, nres, 0);
    free(d);
    int changed = (w->stats->glyphs + w->stats->images_removed + w->stats->images_scrubbed) != before;
    if (!changed) { pn_buf_free(&ob); pn_drop(nres); return 0; }
    prune_xobjects(w->doc, nres, ob.data, ob.len);
    pn_obj *nf = pn_new_stream(pn_shallow_copy(form->u.stm.dict), NULL, 0);
    pn_stream_set(nf, ob.data, ob.len, 1);
    pn_dict_put(nf, "Resources", nres);
    pn_buf_free(&ob);
    int num = pn_add_obj(w->doc, nf);
    pn_obj *xo = private_xobjects(w, out_res);
    pn_res_unique_name(w->doc, xo, "PnR", newname, 32);
    pn_dict_put(xo, newname, pn_new_ref(num, 0));
    w->stats->forms++;
    return 1;
}

static void walk(walker *w, const unsigned char *data, size_t len, pn_obj *res, pn_mat basectm, pn_buf *out, pn_obj *out_res, int toplevel) {
    if (w->depth > 12) { if (out) pn_buf_put(out, data, len); return; }
    w->depth++;
    pn_content *c = pn_content_parse(data, len);
    gstate stack[64];
    int sp = 0, qdepth = 0;
    gstate g;
    memset(&g, 0, sizeof g);
    g.ctm = basectm; g.th = 1;
    pn_mat tm = pn_mat_identity(), tlm = pn_mat_identity();
    for (int i = 0; i < c->n; i++) {
        pn_op *op = &c->ops[i];
        const char *nm = op->name;
        pn_obj **a = op->args;
        int na = op->nargs;
        int written = 0;
#define NUM(k) (na > (k) ? pn_num(a[k], 0) : 0)
        if (!strcmp(nm, "q")) { if (sp < 64) stack[sp++] = g; qdepth++; }
        else if (!strcmp(nm, "Q")) {
            if (sp > 0) g = stack[--sp];
            if (qdepth > 0) qdepth--;
            else if (out && toplevel) written = 1; /* drop unbalanced Q */
        }
        else if (!strcmp(nm, "cm") && na >= 6) {
            pn_mat m = {NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), NUM(5)};
            g.ctm = pn_mat_mul(m, g.ctm);
        }
        else if (!strcmp(nm, "BT")) { tm = tlm = pn_mat_identity(); }
        else if (!strcmp(nm, "Tf") && na >= 2) {
            g.tfs = NUM(1);
            pn_obj *fd = lookup_res(w->doc, res, "Font", pn_name(a[0]));
            g.font = fd ? pn_font_load(w->doc, fd, &w->fonts) : NULL;
        }
        else if (!strcmp(nm, "Tc")) g.tc = NUM(0);
        else if (!strcmp(nm, "Tw")) g.tw = NUM(0);
        else if (!strcmp(nm, "Tz")) g.th = NUM(0) / 100.0;
        else if (!strcmp(nm, "TL")) g.tl = NUM(0);
        else if (!strcmp(nm, "Ts")) g.trise = NUM(0);
        else if (!strcmp(nm, "Tr")) g.tr = (int)NUM(0);
        else if (!strcmp(nm, "Td") && na >= 2) {
            pn_mat t = {1, 0, 0, 1, NUM(0), NUM(1)};
            tlm = pn_mat_mul(t, tlm); tm = tlm;
        }
        else if (!strcmp(nm, "TD") && na >= 2) {
            g.tl = -NUM(1);
            pn_mat t = {1, 0, 0, 1, NUM(0), NUM(1)};
            tlm = pn_mat_mul(t, tlm); tm = tlm;
        }
        else if (!strcmp(nm, "Tm") && na >= 6) {
            pn_mat m = {NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), NUM(5)};
            tlm = tm = m;
        }
        else if (!strcmp(nm, "T*")) {
            pn_mat t = {1, 0, 0, 1, 0, -g.tl};
            tlm = pn_mat_mul(t, tlm); tm = tlm;
        }
        else if (!strcmp(nm, "Tj") || !strcmp(nm, "TJ") || !strcmp(nm, "'") || !strcmp(nm, "\"")) {
            pn_obj *arg = na ? a[na - 1] : NULL;
            if (!strcmp(nm, "\"") && na >= 3) { g.tw = NUM(0); g.tc = NUM(1); }
            if (nm[0] == '\'' || nm[0] == '"') {
                pn_mat t = {1, 0, 0, 1, 0, -g.tl};
                tlm = pn_mat_mul(t, tlm); tm = tlm;
            }
            pn_obj **items = &arg;
            int nitems = arg ? 1 : 0;
            if (pn_is(arg, PN_ARRAY)) { items = arg->u.arr.items; nitems = arg->u.arr.len; }
            pn_obj *newtj = w->redact ? pn_new_array() : NULL;
            int rm = show_text(w, &g, &tm, items, nitems, newtj);
            if (rm && out) {
                if (!strcmp(nm, "\"")) {
                    pn_buf_put_real(out, NUM(0)); pn_buf_puts(out, " Tw ");
                    pn_buf_put_real(out, NUM(1)); pn_buf_puts(out, " Tc\n");
                }
                if (nm[0] == '\'' || nm[0] == '"') pn_buf_puts(out, "T*\n");
                pn_write_obj(out, newtj);
                pn_buf_puts(out, " TJ\n");
                written = 1;
            }
            pn_drop(newtj);
        }
        else if (!strcmp(nm, "Do") && na >= 1) {
            pn_obj *xo = lookup_res(w->doc, res, "XObject", pn_name(a[0]));
            const char *st = pn_name(pn_get(w->doc, xo, "Subtype"));
            if (!strcmp(st, "Image")) {
                if (w->redact) {
                    pn_rect unit = {0, 0, 1, 1};
                    pn_rect bb = pn_rect_transform(unit, g.ctm);
                    if (hits(w, bb)) {
                        int newnum = 0;
                        int r = pn_redact_image(w->doc, xo, g.ctm, w->rects, w->nrects, &newnum);
                        if (r == 2) { w->stats->images_removed++; written = 1; }
                        else if (r == 1 && out_res) {
                            char newname[32];
                            pn_obj *x = private_xobjects(w, out_res);
                            pn_res_unique_name(w->doc, x, "PnI", newname, 32);
                            pn_dict_put(x, newname, pn_new_ref(newnum, 0));
                            pn_obj *nmo = pn_new_name(newname);
                            pn_write_obj(out, nmo);
                            pn_drop(nmo);
                            pn_buf_puts(out, " Do\n");
                            w->stats->images_scrubbed++;
                            written = 1;
                        }
                    }
                }
            } else if (!strcmp(st, "Form") && pn_is(xo, PN_STREAM)) {
                pn_mat fmat = pn_mat_from_array(w->doc, pn_get(w->doc, xo, "Matrix"));
                pn_mat fctm = pn_mat_mul(fmat, g.ctm);
                if (w->redact) {
                    pn_obj *bbox = pn_get(w->doc, xo, "BBox");
                    pn_rect fb = {-1e6, -1e6, 1e6, 1e6};
                    if (pn_arr_len(bbox) >= 4) {
                        fb.x0 = pn_num(pn_resolve(w->doc, pn_arr_get(bbox, 0)), 0); fb.y0 = pn_num(pn_resolve(w->doc, pn_arr_get(bbox, 1)), 0);
                        fb.x1 = pn_num(pn_resolve(w->doc, pn_arr_get(bbox, 2)), 0); fb.y1 = pn_num(pn_resolve(w->doc, pn_arr_get(bbox, 3)), 0);
                        if (fb.x0 > fb.x1) { double t = fb.x0; fb.x0 = fb.x1; fb.x1 = t; }
                        if (fb.y0 > fb.y1) { double t = fb.y0; fb.y0 = fb.y1; fb.y1 = t; }
                    }
                    if (hits(w, pn_rect_transform(fb, fctm)) && out_res) {
                        char newname[32];
                        if (redact_form(w, xo, res, g.ctm, out_res, newname)) {
                            pn_obj *nmo = pn_new_name(newname);
                            pn_write_obj(out, nmo);
                            pn_drop(nmo);
                            pn_buf_puts(out, " Do\n");
                            written = 1;
                        }
                    }
                } else {
                    size_t dl; int ok;
                    unsigned char *d = pn_stream_decode(w->doc, xo, &dl, &ok);
                    pn_obj *fres = pn_get(w->doc, xo, "Resources");
                    if (d) { walk(w, d, dl, fres ? fres : res, fctm, NULL, NULL, 0); free(d); }
                }
            }
        }
        else if (!strcmp(nm, "BI") && w->redact) {
            pn_rect unit = {0, 0, 1, 1};
            if (hits(w, pn_rect_transform(unit, g.ctm))) { w->stats->images_removed++; written = 1; }
        }
#undef NUM
        if (out && !written) pn_op_write(out, op);
    }
    if (out && toplevel) while (qdepth-- > 0) pn_buf_puts(out, "Q\n");
    pn_content_free(c);
    w->depth--;
}

pn_mat pn_mat_from_array(pn_doc *doc, pn_obj *arr) {
    pn_mat m = pn_mat_identity();
    if (pn_arr_len(arr) >= 6) {
        m.a = pn_num(pn_resolve(doc, pn_arr_get(arr, 0)), 1); m.b = pn_num(pn_resolve(doc, pn_arr_get(arr, 1)), 0);
        m.c = pn_num(pn_resolve(doc, pn_arr_get(arr, 2)), 0); m.d = pn_num(pn_resolve(doc, pn_arr_get(arr, 3)), 1);
        m.e = pn_num(pn_resolve(doc, pn_arr_get(arr, 4)), 0); m.f = pn_num(pn_resolve(doc, pn_arr_get(arr, 5)), 0);
    }
    return m;
}

/* ------------------------------------------------------------------ */
/* Text extraction                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    pn_buf text;      /* UTF-8 page text with spaces/newlines */
    int *glyph_of;    /* byte offset -> glyph index (-1 for inserted separators) */
    int cap;
} pagetext;

static void pt_put(pagetext *pt, unsigned u, int gi) {
    size_t before = pt->text.len;
    put_utf8(&pt->text, u);
    while ((int)pt->text.len + 1 > pt->cap) { pt->cap = pt->cap ? pt->cap * 2 : 4096; pt->glyph_of = (int *)realloc(pt->glyph_of, sizeof(int) * (size_t)pt->cap); }
    for (size_t k = before; k < pt->text.len; k++) pt->glyph_of[k] = gi;
}

static walker *extract_page(pn_doc *doc, int pageidx) {
    walker *w = (walker *)calloc(1, sizeof *w);
    w->doc = doc;
    pn_obj *page = pn_doc_page(doc, pageidx);
    if (!page) return w;
    size_t len;
    unsigned char *data = pn_page_content(doc, page, &len);
    walk(w, data, len, pn_page_inherited(doc, page, "Resources"), pn_mat_identity(), NULL, NULL, 1);
    free(data);
    return w;
}

static void free_walker(walker *w) {
    pn_font_cache_free(w->fonts);
    free(w->g);
    pn_buf_free(&w->removed_text);
    free(w);
}

/* Build page text, deciding spaces and line breaks from glyph geometry. */
static void build_text(walker *w, pagetext *pt) {
    glyph *prev = NULL;
    for (int i = 0; i < w->ng; i++) {
        glyph *g = &w->g[i];
        if (g->nuni == 1 && (g->uni[0] == 0 || g->uni[0] == 0xFFFD) && 0) continue;
        if (prev) {
            double h = g->box.y1 - g->box.y0, ph = prev->box.y1 - prev->box.y0;
            double cy = (g->box.y0 + g->box.y1) / 2, pcy = (prev->box.y0 + prev->box.y1) / 2;
            double mh = h > ph ? h : ph;
            if (mh <= 0) mh = g->size > 0 ? g->size : 10;
            int newline = fabs(cy - pcy) > mh * 0.5 || g->box.x0 < prev->box.x0 - mh * 2;
            if (newline) {
                if (pt->text.len && pt->text.data[pt->text.len - 1] != '\n') pt_put(pt, '\n', -1);
            } else {
                double gap = g->box.x0 - prev->box.x1;
                int prev_space = pt->text.len && (pt->text.data[pt->text.len - 1] == ' ');
                if (gap > mh * 0.12 && !prev_space && !(g->nuni == 1 && g->uni[0] == ' ')) pt_put(pt, ' ', -1);
            }
        }
        for (int k = 0; k < g->nuni; k++) {
            unsigned u = g->uni[k];
            if (u == ' ' || u == 0xA0) {
                if (pt->text.len && (pt->text.data[pt->text.len - 1] == ' ' || pt->text.data[pt->text.len - 1] == '\n')) continue;
                u = ' ';
            }
            if (u < 32 && u != '\n') continue;
            pt_put(pt, u, i);
        }
        prev = g;
    }
}

void pn_text_json(pn_doc *doc, int pageidx, pn_buf *out) {
    walker *w = extract_page(doc, pageidx);
    pagetext pt; memset(&pt, 0, sizeof pt); pn_buf_init(&pt.text);
    build_text(w, &pt);
    pn_buf_puts(out, "{\"text\":");
    pn_json_str(out, pt.text.data ? (const char *)pt.text.data : "", (int)pt.text.len);
    /* words with boxes */
    pn_buf_puts(out, ",\"words\":[");
    int first = 1;
    size_t i = 0;
    int line = 0;
    while (i < pt.text.len) {
        unsigned char ch = pt.text.data[i];
        if (ch == ' ') { i++; continue; }
        if (ch == '\n') { line++; i++; continue; }
        size_t s = i;
        pn_rect bb = {1e9, 1e9, -1e9, -1e9};
        while (i < pt.text.len && pt.text.data[i] != ' ' && pt.text.data[i] != '\n') {
            int gi = pt.glyph_of[i];
            if (gi >= 0) {
                pn_rect r = w->g[gi].box;
                if (r.x0 < bb.x0) bb.x0 = r.x0;
                if (r.y0 < bb.y0) bb.y0 = r.y0;
                if (r.x1 > bb.x1) bb.x1 = r.x1;
                if (r.y1 > bb.y1) bb.y1 = r.y1;
            }
            i++;
        }
        if (!first) pn_buf_putc(out, ',');
        first = 0;
        pn_buf_puts(out, "{\"t\":");
        pn_json_str(out, (const char *)pt.text.data + s, (int)(i - s));
        pn_buf_printf(out, ",\"l\":%d,\"b\":[", line);
        pn_buf_put_real(out, bb.x0); pn_buf_putc(out, ',');
        pn_buf_put_real(out, bb.y0); pn_buf_putc(out, ',');
        pn_buf_put_real(out, bb.x1); pn_buf_putc(out, ',');
        pn_buf_put_real(out, bb.y1); pn_buf_puts(out, "]}");
    }
    pn_buf_puts(out, "]}");
    pn_buf_free(&pt.text);
    free(pt.glyph_of);
    free_walker(w);
}

void pn_text_plain(pn_doc *doc, int pageidx, pn_buf *out) {
    walker *w = extract_page(doc, pageidx);
    pagetext pt; memset(&pt, 0, sizeof pt); pn_buf_init(&pt.text);
    build_text(w, &pt);
    pn_buf_put(out, pt.text.data, pt.text.len);
    pn_buf_free(&pt.text);
    free(pt.glyph_of);
    free_walker(w);
}

static unsigned fold(unsigned u) {
    if (u >= 'A' && u <= 'Z') return u + 32;
    if (u >= 0xC0 && u <= 0xDE && u != 0xD7) return u + 32;
    if (u == 0x2019 || u == 0x2018) return '\'';
    if (u == 0x201C || u == 0x201D) return '"';
    if (u == 0xA0 || u == '\n' || u == '\t') return ' ';
    return u;
}

int pn_search_page(pn_doc *doc, int pageidx, const char *needle, int ci, pn_buf *out, int first) {
    /* decode needle to codepoints */
    unsigned nd[512]; int nn = 0;
    const char *p = needle;
    while (*p && nn < 512) {
        unsigned u = (unsigned)pn_utf8_decode(&p);
        if (ci) u = fold(u);
        if (u == ' ' && nn > 0 && nd[nn - 1] == ' ') continue;
        nd[nn++] = u;
    }
    while (nn > 0 && nd[nn - 1] == ' ') nn--;
    if (nn == 0) return 0;
    walker *w = extract_page(doc, pageidx);
    pagetext pt; memset(&pt, 0, sizeof pt); pn_buf_init(&pt.text);
    build_text(w, &pt);
    if (!pt.text.len) { pn_buf_free(&pt.text); free(pt.glyph_of); free_walker(w); return 0; }
    /* decode page text to codepoints with glyph mapping */
    int cap = (int)pt.text.len + 1, n = 0;
    unsigned *cp = (unsigned *)malloc(sizeof(unsigned) * (size_t)cap);
    int *cg = (int *)malloc(sizeof(int) * (size_t)cap);
    const char *q = (const char *)pt.text.data, *end = q + pt.text.len;
    while (q && q < end) {
        int gi = pt.glyph_of[q - (const char *)pt.text.data];
        unsigned u = (unsigned)pn_utf8_decode(&q);
        cp[n] = ci ? fold(u) : (u == '\n' ? ' ' : u);
        cg[n] = gi;
        n++;
    }
    int found = 0;
    for (int s = 0; s + nn <= n; s++) {
        int k;
        for (k = 0; k < nn; k++) if (cp[s + k] != nd[k]) break;
        if (k < nn) continue;
        /* collect boxes, splitting by line */
        if (!first || found) pn_buf_putc(out, ',');
        pn_buf_printf(out, "{\"p\":%d,\"r\":[", pageidx);
        pn_rect bb = {1e9, 1e9, -1e9, -1e9};
        int nr = 0;
        double lastcy = 1e18;
        for (k = 0; k < nn; k++) {
            int gi = cg[s + k];
            if (gi < 0) continue;
            pn_rect r = w->g[gi].box;
            double cy = (r.y0 + r.y1) / 2, h = r.y1 - r.y0;
            if (lastcy < 1e17 && fabs(cy - lastcy) > h * 0.5 && bb.x1 > bb.x0) {
                if (nr++) pn_buf_putc(out, ',');
                pn_buf_printf(out, "[%.2f,%.2f,%.2f,%.2f]", bb.x0, bb.y0, bb.x1, bb.y1);
                bb.x0 = bb.y0 = 1e9; bb.x1 = bb.y1 = -1e9;
            }
            lastcy = cy;
            if (r.x0 < bb.x0) bb.x0 = r.x0;
            if (r.y0 < bb.y0) bb.y0 = r.y0;
            if (r.x1 > bb.x1) bb.x1 = r.x1;
            if (r.y1 > bb.y1) bb.y1 = r.y1;
        }
        if (bb.x1 > bb.x0) { if (nr++) pn_buf_putc(out, ','); pn_buf_printf(out, "[%.2f,%.2f,%.2f,%.2f]", bb.x0, bb.y0, bb.x1, bb.y1); }
        pn_buf_puts(out, "]}");
        found++;
        s += nn - 1;
    }
    free(cp); free(cg);
    pn_buf_free(&pt.text);
    free(pt.glyph_of);
    free_walker(w);
    return found;
}

/* ------------------------------------------------------------------ */
/* Redaction: rewrite page content removing glyphs/images in rects     */
/* ------------------------------------------------------------------ */
int pn_redact_page_content(pn_doc *doc, int pageidx, const pn_rect *rects, int nrects, pn_redact_stats *stats, pn_buf *removed_text) {
    pn_obj *page = pn_doc_page(doc, pageidx);
    if (!page) return -1;
    walker *w = (walker *)calloc(1, sizeof *w);
    w->doc = doc; w->redact = 1; w->rects = rects; w->nrects = nrects; w->stats = stats;
    size_t len;
    unsigned char *data = pn_page_content(doc, page, &len);
    pn_obj *res = pn_page_inherited(doc, page, "Resources");
    pn_obj *nres = pn_page_private_resources(doc, page); /* private shallow copy now on page */
    pn_buf out; pn_buf_init(&out);
    pn_buf_puts(&out, "q\n");
    walk(w, data, len, res ? nres : nres, pn_mat_identity(), &out, nres, 1);
    pn_buf_puts(&out, "Q\n");
    free(data);
    if (stats->images_removed || stats->images_scrubbed || stats->forms) prune_xobjects(doc, nres, out.data, out.len);
    pn_page_set_content(doc, page, out.data, out.len);
    pn_buf_free(&out);
    if (removed_text) pn_buf_put(removed_text, w->removed_text.data, w->removed_text.len);
    free_walker(w);
    return 0;
}

/* Count glyphs whose boxes still intersect the given rects (0 = verified clean). */
int pn_count_glyphs_in(pn_doc *doc, int pageidx, const pn_rect *rects, int nrects, pn_buf *leaked) {
    walker *w = extract_page(doc, pageidx);
    int n = 0;
    for (int i = 0; i < w->ng; i++) {
        pn_rect b = w->g[i].box;
        for (int k = 0; k < nrects; k++) {
            /* shrink a little so glyphs merely touching an edge do not count */
            pn_rect r = rects[k];
            if (pn_glyph_hit(b, r)) {
                n++;
                if (leaked && leaked->len < 2000) for (int u = 0; u < w->g[i].nuni; u++) put_utf8(leaked, w->g[i].uni[u]);
                break;
            }
        }
    }
    free_walker(w);
    return n;
}
