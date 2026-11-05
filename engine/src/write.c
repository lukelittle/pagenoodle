/* Serializer: full rewrite with garbage collection, renumbering, optional object streams,
 * stream compression, de-duplication and metadata stripping. Never writes incremental updates,
 * so removed content (e.g. redacted text) cannot survive in an old revision. */
#include "pn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pn_doc *doc;
    int *newnum;   /* old -> new object number (0 = not written) */
    int flags;
} wctx;

static void write_name(pn_buf *b, const char *s) {
    pn_buf_putc(b, '/');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x21 || *p > 0x7E || *p == '#' || pn_is_delim(*p)) pn_buf_printf(b, "#%02X", *p);
        else pn_buf_putc(b, *p);
    }
}

static void write_string(pn_buf *b, const unsigned char *s, int len, int hex) {
    int binary = 0;
    for (int i = 0; i < len; i++) if (s[i] < 0x20 && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') binary++;
    if (hex || binary * 4 > len) {
        static const char hx[] = "0123456789ABCDEF";
        pn_buf_putc(b, '<');
        for (int i = 0; i < len; i++) { pn_buf_putc(b, hx[s[i] >> 4]); pn_buf_putc(b, hx[s[i] & 15]); }
        pn_buf_putc(b, '>');
        return;
    }
    pn_buf_putc(b, '(');
    for (int i = 0; i < len; i++) {
        unsigned c = s[i];
        if (c == '(' || c == ')' || c == '\\') { pn_buf_putc(b, '\\'); pn_buf_putc(b, (int)c); }
        else if (c == '\n') pn_buf_puts(b, "\\n");
        else if (c == '\r') pn_buf_puts(b, "\\r");
        else if (c < 0x20 || c == 0x7F) pn_buf_printf(b, "\\%03o", c);
        else pn_buf_putc(b, (int)c);
    }
    pn_buf_putc(b, ')');
}

static void write_obj_ctx(wctx *w, pn_buf *b, pn_obj *o) {
    if (!o) { pn_buf_puts(b, "null"); return; }
    int i;
    switch (o->type) {
    case PN_NULL: pn_buf_puts(b, "null"); break;
    case PN_BOOL: pn_buf_puts(b, o->u.b ? "true" : "false"); break;
    case PN_INT: pn_buf_printf(b, "%lld", o->u.i); break;
    case PN_REAL: pn_buf_put_real(b, o->u.r); break;
    case PN_STRING: write_string(b, o->u.str.s, o->u.str.len, o->u.str.hex); break;
    case PN_NAME: write_name(b, (const char *)o->u.str.s); break;
    case PN_ARRAY:
        pn_buf_putc(b, '[');
        for (i = 0; i < o->u.arr.len; i++) { if (i) pn_buf_putc(b, ' '); write_obj_ctx(w, b, o->u.arr.items[i]); }
        pn_buf_putc(b, ']');
        break;
    case PN_DICT:
        pn_buf_puts(b, "<<");
        for (i = 0; i < o->u.dict.len; i++) {
            pn_obj *v = o->u.dict.vals[i];
            if (w && v && v->type == PN_REF) {
                int n = v->u.ref.num;
                if (n <= 0 || n >= w->doc->nxref || !w->newnum[n]) continue; /* dangling: drop key */
            }
            write_name(b, o->u.dict.keys[i]);
            if (v && (v->type == PN_INT || v->type == PN_REAL || v->type == PN_BOOL || v->type == PN_NULL || v->type == PN_REF)) pn_buf_putc(b, ' ');
            write_obj_ctx(w, b, v);
        }
        pn_buf_puts(b, ">>");
        break;
    case PN_REF:
        if (w) {
            int n = o->u.ref.num;
            if (n <= 0 || n >= w->doc->nxref || !w->newnum[n]) pn_buf_puts(b, "null");
            else pn_buf_printf(b, "%d 0 R", w->newnum[n]);
        } else pn_buf_printf(b, "%d %d R", o->u.ref.num, o->u.ref.gen);
        break;
    case PN_STREAM: write_obj_ctx(w, b, o->u.stm.dict); break;
    }
}

void pn_write_obj(pn_buf *b, pn_obj *o) { write_obj_ctx(NULL, b, o); }

/* ---------- reachability ---------- */
static void mark(pn_doc *doc, pn_obj *root, char *reach, int **order, int *norder, int *cap) {
    /* iterative DFS over direct structures, following refs */
    int scap = 256, sp = 0;
    pn_obj **stack = (pn_obj **)malloc(sizeof(pn_obj *) * (size_t)scap);
    stack[sp++] = root;
    while (sp > 0) {
        pn_obj *o = stack[--sp];
        if (!o) continue;
        if (o->type == PN_REF) {
            int n = o->u.ref.num;
            if (n <= 0 || n >= doc->nxref || reach[n]) continue;
            pn_obj *t = pn_load(doc, n);
            if (!t) continue;
            reach[n] = 1;
            if (*norder == *cap) { *cap = *cap ? *cap * 2 : 256; *order = (int *)realloc(*order, sizeof(int) * (size_t)*cap); }
            (*order)[(*norder)++] = n;
            o = t;
        }
        int cnt = 0;
        if (o->type == PN_ARRAY) cnt = o->u.arr.len;
        else if (o->type == PN_DICT) cnt = o->u.dict.len;
        else if (o->type == PN_STREAM) { o = o->u.stm.dict; cnt = o->u.dict.len; }
        else continue;
        if (sp + cnt >= scap) { while (sp + cnt >= scap) scap *= 2; stack = (pn_obj **)realloc(stack, sizeof(pn_obj *) * (size_t)scap); }
        /* push in reverse so traversal order is natural */
        for (int i = cnt - 1; i >= 0; i--) stack[sp++] = o->type == PN_ARRAY ? o->u.arr.items[i] : o->u.dict.vals[i];
    }
    free(stack);
}

static void strip_metadata(pn_doc *doc) {
    pn_dict_del(doc->trailer, "Info");
    pn_obj *cat = pn_catalog(doc);
    pn_dict_del(cat, "Metadata");
    pn_dict_del(cat, "PieceInfo");
    pn_dict_del(cat, "SpiderInfo");
    pn_dict_del(cat, "Perms");
    for (int i = 1; i < doc->nxref; i++) {
        pn_obj *o = pn_load(doc, i);
        if (!o || (o->type != PN_DICT && o->type != PN_STREAM)) continue;
        pn_dict_del(o, "PieceInfo");
        pn_dict_del(o, "LastModified");
        if (pn_is_name(pn_dict_get(o, "Type"), "Page")) { pn_dict_del(o, "Metadata"); pn_dict_del(o, "Thumb"); }
        if (o->type == PN_STREAM && pn_is_name(pn_dict_get(o, "Type"), "XObject")) pn_dict_del(o, "Metadata");
    }
}

static unsigned long long fnv(const unsigned char *d, size_t n, unsigned long long h) {
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

static int renum(wctx *w, pn_obj *ref) {
    if (!ref || ref->type != PN_REF) return 0;
    int n = ref->u.ref.num;
    return (n > 0 && n < w->doc->nxref) ? w->newnum[n] : 0;
}

unsigned char *pn_doc_save(pn_doc *doc, int flags, size_t *outlen) {
    if (flags & PN_SAVE_STRIPMETA) strip_metadata(doc);
    /* refresh page count in page tree if needed */
    char *reach = (char *)calloc((size_t)doc->nxref + 1, 1);
    int *order = NULL, norder = 0, cap = 0;
    pn_obj *root = pn_dict_get(doc->trailer, "Root");
    pn_obj *info = pn_dict_get(doc->trailer, "Info");
    mark(doc, root, reach, &order, &norder, &cap);
    if (info) mark(doc, info, reach, &order, &norder, &cap);
    if (!(flags & PN_SAVE_GC)) {
        for (int i = 1; i < doc->nxref; i++) {
            if (reach[i] || (doc->xref[i].type == 0)) continue;
            pn_obj *o = pn_load(doc, i);
            if (!o) continue;
            if (pn_is(o, PN_STREAM) && (pn_is_name(pn_dict_get(o, "Type"), "XRef") || pn_is_name(pn_dict_get(o, "Type"), "ObjStm"))) continue;
            pn_obj tmpref; memset(&tmpref, 0, sizeof tmpref);
            tmpref.type = PN_REF; tmpref.refs = -1; tmpref.u.ref.num = i;
            mark(doc, &tmpref, reach, &order, &norder, &cap);
        }
    }
    wctx w;
    w.doc = doc; w.flags = flags;
    w.newnum = (int *)calloc((size_t)doc->nxref + 1, sizeof(int));
    /* de-duplicate streams with identical dict+data */
    int *canon = (int *)calloc((size_t)doc->nxref + 1, sizeof(int));
    if (flags & PN_SAVE_DEDUP) {
        int hcap = 1; while (hcap < norder * 2 + 2) hcap *= 2;
        int *htab = (int *)calloc((size_t)hcap, sizeof(int));
        unsigned long long *hval = (unsigned long long *)calloc((size_t)doc->nxref + 1, sizeof(unsigned long long));
        pn_buf tmp; pn_buf_init(&tmp);
        for (int k = 0; k < norder; k++) {
            int n = order[k];
            pn_obj *o = pn_load(doc, n);
            if (!pn_is(o, PN_STREAM)) continue;
            if (pn_is_name(pn_dict_get(o, "Type"), "Page")) continue;
            tmp.len = 0;
            pn_write_obj(&tmp, o->u.stm.dict);
            unsigned long long h = fnv(tmp.data, tmp.len, 1469598103934665603ULL);
            h = fnv(o->u.stm.data, o->u.stm.len, h);
            hval[n] = h;
            unsigned slot = (unsigned)(h & (unsigned long long)(hcap - 1));
            for (;;) {
                int m = htab[slot];
                if (!m) { htab[slot] = n; break; }
                if (hval[m] == h) {
                    pn_obj *p = pn_load(doc, m);
                    pn_buf t2; pn_buf_init(&t2);
                    pn_write_obj(&t2, p->u.stm.dict);
                    int same = t2.len == tmp.len && !memcmp(t2.data, tmp.data, tmp.len) && p->u.stm.len == o->u.stm.len && !memcmp(p->u.stm.data, o->u.stm.data, o->u.stm.len);
                    pn_buf_free(&t2);
                    if (same) { canon[n] = m; break; }
                }
                slot = (slot + 1) & (unsigned)(hcap - 1);
            }
        }
        pn_buf_free(&tmp);
        free(htab); free(hval);
    }
    int next = 1;
    for (int k = 0; k < norder; k++) { int n = order[k]; if (!canon[n]) w.newnum[n] = next++; }
    for (int k = 0; k < norder; k++) { int n = order[k]; if (canon[n]) w.newnum[n] = w.newnum[canon[n]]; }
    int total = next; /* objects 1..total-1 */

    pn_buf out; pn_buf_init(&out);
    pn_buf_puts(&out, "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n");
    long long *offs = (long long *)calloc((size_t)total + 2, sizeof(long long));
    int *instm = (int *)calloc((size_t)total + 2, sizeof(int));
    int *inidx = (int *)calloc((size_t)total + 2, sizeof(int));
    int use_objstm = (flags & PN_SAVE_OBJSTM) != 0;
    int extra = total; /* numbers for object streams & xref stream */
    pn_buf body; pn_buf_init(&body);

    /* object streams batch */
    pn_buf osh, osb; pn_buf_init(&osh); pn_buf_init(&osb);
    int os_count = 0;
    int written_cnt = 0;
#define FLUSH_OBJSTM() do { if (os_count) { \
        int sn = extra++; \
        offs = (long long *)realloc(offs, sizeof(long long) * (size_t)(extra + 2)); \
        instm = (int *)realloc(instm, sizeof(int) * (size_t)(extra + 2)); \
        inidx = (int *)realloc(inidx, sizeof(int) * (size_t)(extra + 2)); \
        instm[sn] = 0; \
        pn_buf all; pn_buf_init(&all); pn_buf_put(&all, osh.data, osh.len); size_t first = all.len; pn_buf_put(&all, osb.data, osb.len); \
        size_t cl; unsigned char *c = pn_deflate(all.data, all.len, &cl, 9); \
        offs[sn] = (long long)out.len; \
        pn_buf_printf(&out, "%d 0 obj\n<</Type/ObjStm/N %d/First %zu/Filter/FlateDecode/Length %zu>>\nstream\n", sn, os_count, first, cl); \
        pn_buf_put(&out, c, cl); pn_buf_puts(&out, "\nendstream\nendobj\n"); \
        free(c); pn_buf_free(&all); \
        for (int q = 0; q < total; q++) if (instm[q] == -1) instm[q] = sn; \
        osh.len = 0; osb.len = 0; os_count = 0; } } while (0)

    for (int k = 0; k < norder; k++) {
        int n = order[k];
        if (canon[n]) continue;
        int nn = w.newnum[n];
        pn_obj *o = pn_load(doc, n);
        if (!o) o = pn_null();
        if ((++written_cnt & 255) == 0) pn_progress(written_cnt, norder);
        if (o->type != PN_STREAM) {
            if (use_objstm) {
                pn_buf_printf(&osh, "%d %zu ", nn, osb.len);
                write_obj_ctx(&w, &osb, o);
                pn_buf_putc(&osb, '\n');
                instm[nn] = -1; inidx[nn] = os_count++;
                if (os_count >= 200) FLUSH_OBJSTM();
                continue;
            }
            offs[nn] = (long long)out.len;
            pn_buf_printf(&out, "%d 0 obj\n", nn);
            write_obj_ctx(&w, &out, o);
            pn_buf_puts(&out, "\nendobj\n");
            continue;
        }
        /* stream */
        const unsigned char *data = o->u.stm.data;
        size_t dlen = o->u.stm.len;
        unsigned char *tmpdata = NULL;
        pn_obj *dict = pn_shallow_copy(o->u.stm.dict);
        pn_obj *filter = pn_resolve(doc, pn_dict_get(dict, "Filter"));
        if ((flags & PN_SAVE_COMPRESS) && !filter && dlen > 64) {
            size_t cl;
            unsigned char *c = pn_deflate(data, dlen, &cl, 9);
            if (c && cl + 20 < dlen) { tmpdata = c; data = c; dlen = cl; pn_dict_put(dict, "Filter", pn_new_name("FlateDecode")); }
            else free(c);
        } else if ((flags & PN_SAVE_RECOMPRESS) && pn_is_name(filter, "FlateDecode") && !pn_dict_get(dict, "DecodeParms") && !pn_dict_get(dict, "DP")) {
            size_t rl;
            unsigned char *raw = pn_inflate(data, dlen, &rl);
            if (raw) {
                size_t cl;
                unsigned char *c = pn_deflate(raw, rl, &cl, 10);
                if (c && cl < dlen) { tmpdata = c; data = c; dlen = cl; } else free(c);
                free(raw);
            }
        }
        pn_dict_put(dict, "Length", pn_new_int((long long)dlen));
        offs[nn] = (long long)out.len;
        pn_buf_printf(&out, "%d 0 obj\n", nn);
        write_obj_ctx(&w, &out, dict);
        pn_buf_puts(&out, "\nstream\n");
        pn_buf_put(&out, data, dlen);
        pn_buf_puts(&out, "\nendstream\nendobj\n");
        pn_drop(dict);
        free(tmpdata);
    }
    FLUSH_OBJSTM();

    /* trailer pieces */
    unsigned long long h = fnv(out.data, out.len, 1469598103934665603ULL);
    unsigned char idbytes[16];
    for (int i = 0; i < 8; i++) { idbytes[i] = (unsigned char)(h >> (8 * i)); idbytes[8 + i] = (unsigned char)((h * 31 + 7) >> (8 * i)); }
    pn_obj *idarr = pn_new_array();
    pn_obj *oldid = pn_resolve(doc, pn_dict_get(doc->trailer, "ID"));
    pn_obj *first = pn_arr_get(oldid, 0);
    if (pn_is(first, PN_STRING) && !(flags & PN_SAVE_STRIPMETA)) pn_arr_push(idarr, pn_shallow_copy(first));
    else { pn_obj *s = pn_new_string(idbytes, 16); s->u.str.hex = 1; pn_arr_push(idarr, s); }
    { pn_obj *s = pn_new_string(idbytes, 16); s->u.str.hex = 1; pn_arr_push(idarr, s); }

    if (use_objstm) {
        int xn = extra++;
        offs = (long long *)realloc(offs, sizeof(long long) * (size_t)(extra + 2));
        instm = (int *)realloc(instm, sizeof(int) * (size_t)(extra + 2));
        inidx = (int *)realloc(inidx, sizeof(int) * (size_t)(extra + 2));
        instm[xn] = 0;
        offs[xn] = (long long)out.len;
        pn_buf x; pn_buf_init(&x);
        for (int i = 0; i < extra; i++) {
            unsigned char rec[7];
            if (i == 0) { rec[0] = 0; memset(rec + 1, 0, 4); rec[5] = 0xFF; rec[6] = 0xFF; }
            else if (i < total && instm[i] > 0) {
                rec[0] = 2; unsigned v = (unsigned)instm[i];
                rec[1] = (unsigned char)(v >> 24); rec[2] = (unsigned char)(v >> 16); rec[3] = (unsigned char)(v >> 8); rec[4] = (unsigned char)v;
                rec[5] = (unsigned char)(inidx[i] >> 8); rec[6] = (unsigned char)inidx[i];
            } else {
                rec[0] = 1; unsigned long long v = (unsigned long long)offs[i];
                rec[1] = (unsigned char)(v >> 24); rec[2] = (unsigned char)(v >> 16); rec[3] = (unsigned char)(v >> 8); rec[4] = (unsigned char)v;
                rec[5] = 0; rec[6] = 0;
            }
            pn_buf_put(&x, rec, 7);
        }
        size_t cl;
        unsigned char *c = pn_deflate(x.data, x.len, &cl, 9);
        pn_buf_printf(&out, "%d 0 obj\n<</Type/XRef/Size %d/W[1 4 2]/Filter/FlateDecode/Length %zu", xn, extra, cl);
        pn_buf_printf(&out, "/Root %d 0 R", renum(&w, root));
        if (renum(&w, info)) pn_buf_printf(&out, "/Info %d 0 R", renum(&w, info));
        pn_buf_puts(&out, "/ID");
        pn_write_obj(&out, idarr);
        pn_buf_puts(&out, ">>\nstream\n");
        pn_buf_put(&out, c, cl);
        pn_buf_puts(&out, "\nendstream\nendobj\n");
        pn_buf_printf(&out, "startxref\n%lld\n%%%%EOF\n", offs[xn]);
        free(c);
        pn_buf_free(&x);
    } else {
        long long xref_ofs = (long long)out.len;
        pn_buf_printf(&out, "xref\n0 %d\n0000000000 65535 f\r\n", total);
        for (int i = 1; i < total; i++) pn_buf_printf(&out, "%010lld 00000 n\r\n", offs[i]);
        pn_buf_printf(&out, "trailer\n<</Size %d", total);
        pn_buf_printf(&out, "/Root %d 0 R", renum(&w, root));
        if (renum(&w, info)) pn_buf_printf(&out, "/Info %d 0 R", renum(&w, info));
        pn_buf_puts(&out, "/ID");
        pn_write_obj(&out, idarr);
        pn_buf_printf(&out, ">>\nstartxref\n%lld\n%%%%EOF\n", xref_ofs);
    }
    pn_drop(idarr);
    pn_buf_free(&osh); pn_buf_free(&osb); pn_buf_free(&body);
    free(offs); free(instm); free(inidx); free(canon); free(w.newnum); free(order); free(reach);
    return pn_buf_steal(&out, outlen);
}
