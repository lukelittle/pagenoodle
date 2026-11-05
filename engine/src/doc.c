/* Document loading: xref tables/streams, object streams, repair, page tree. */
#include "pn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void xref_ensure(pn_doc *doc, int n) {
    if (n <= doc->nxref) return;
    if (n > doc->capxref) {
        int nc = doc->capxref ? doc->capxref : 64;
        while (nc < n) nc *= 2;
        doc->xref = (pn_xref *)realloc(doc->xref, sizeof(pn_xref) * (size_t)nc);
        memset(doc->xref + doc->capxref, 0, sizeof(pn_xref) * (size_t)(nc - doc->capxref));
        doc->capxref = nc;
    }
    doc->nxref = n;
}

int pn_add_obj(pn_doc *doc, pn_obj *o) {
    int num = doc->nxref ? doc->nxref : 1;
    xref_ensure(doc, num + 1);
    doc->xref[num].type = 3;
    doc->xref[num].obj = o;
    return num;
}
void pn_set_obj(pn_doc *doc, int num, pn_obj *o) {
    if (num <= 0) { pn_drop(o); return; }
    xref_ensure(doc, num + 1);
    pn_obj *old = doc->xref[num].obj;
    doc->xref[num].obj = o;
    doc->xref[num].type = 3;
    if (old != o) pn_drop(old);
}
pn_obj *pn_add_ref(pn_doc *doc, pn_obj *o) { return pn_new_ref(pn_add_obj(doc, o), 0); }

pn_doc *pn_doc_new(void) {
    pn_doc *doc = (pn_doc *)calloc(1, sizeof *doc);
    xref_ensure(doc, 1);
    pn_obj *pages = pn_new_dict();
    pn_dict_put(pages, "Type", pn_new_name("Pages"));
    pn_dict_put(pages, "Kids", pn_new_array());
    pn_dict_put(pages, "Count", pn_new_int(0));
    pn_obj *cat = pn_new_dict();
    pn_dict_put(cat, "Type", pn_new_name("Catalog"));
    pn_dict_put(cat, "Pages", pn_add_ref(doc, pages));
    doc->trailer = pn_new_dict();
    pn_dict_put(doc->trailer, "Root", pn_add_ref(doc, cat));
    return doc;
}

void pn_doc_free(pn_doc *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->nxref; i++) pn_drop(doc->xref[i].obj);
    free(doc->xref);
    pn_drop(doc->trailer);
    free(doc->pages);
    free(doc->buf);
    free(doc);
}

/* ---------------- low-level parsing ---------------- */
static const unsigned char *memfind(const unsigned char *h, size_t hl, const char *n) {
    size_t nl = strlen(n);
    if (hl < nl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++) if (h[i] == (unsigned char)n[0] && !memcmp(h + i, n, nl)) return h + i;
    return NULL;
}

static pn_obj *parse_indirect_at(pn_doc *doc, long long ofs, int *onum, int *ogen);

static pn_obj *read_stream_body(pn_doc *doc, pn_lexer *lx, pn_obj *dict) {
    const unsigned char *p = lx->p;
    if (p < lx->end && *p == '\r') p++;
    if (p < lx->end && *p == '\n') p++;
    size_t avail = (size_t)(lx->end - p);
    pn_obj *lo = pn_dict_get(dict, "Length");
    long long len = -1;
    if (lo && lo->type == PN_REF) {
        pn_obj *r = pn_load(doc, lo->u.ref.num);
        len = pn_int(r, -1);
    } else len = pn_int(lo, -1);
    int good = 0;
    if (len >= 0 && (size_t)len <= avail) {
        const unsigned char *q = p + len;
        while (q < lx->end && pn_is_ws(*q)) q++;
        if ((size_t)(lx->end - q) >= 9 && !memcmp(q, "endstream", 9)) good = 1;
    }
    if (!good) {
        const unsigned char *e = memfind(p, avail, "endstream");
        len = e ? (long long)(e - p) : (long long)avail;
        if (e) {
            while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) len--;
        }
    }
    unsigned char *data = (unsigned char *)malloc((size_t)len + 1);
    memcpy(data, p, (size_t)len);
    pn_dict_put(dict, "Length", pn_new_int(len));
    lx->p = p + len;
    return pn_new_stream(dict, data, (size_t)len);
}

static pn_obj *parse_indirect_at(pn_doc *doc, long long ofs, int *onum, int *ogen) {
    if (ofs < 0 || (size_t)ofs >= doc->len) return NULL;
    pn_lexer lx;
    pn_lex_init(&lx, doc->buf, doc->len);
    lx.p = doc->buf + ofs;
    pn_obj *res = NULL;
    if (pn_lex_next(&lx) != TK_INT) goto out;
    int num = (int)lx.ival;
    if (pn_lex_next(&lx) != TK_INT) goto out;
    int gen = (int)lx.ival;
    if (pn_lex_next(&lx) != TK_KEYWORD || strcmp((char *)lx.sbuf.data, "obj")) goto out;
    if (onum) *onum = num;
    if (ogen) *ogen = gen;
    res = pn_parse_obj(&lx, 1);
    if (!res) { res = pn_null(); goto out; }
    if (res->type == PN_DICT) {
        const unsigned char *save = lx.p;
        if (pn_lex_next(&lx) == TK_KEYWORD && !strcmp((char *)lx.sbuf.data, "stream")) {
            res = read_stream_body(doc, &lx, res);
        } else lx.p = save;
    }
out:
    pn_lex_free(&lx);
    return res;
}

static void load_objstm(pn_doc *doc, int stmnum) {
    pn_obj *stm = pn_load(doc, stmnum);
    if (!pn_is(stm, PN_STREAM)) return;
    size_t len; int ok;
    unsigned char *data = pn_stream_decode(doc, stm, &len, &ok);
    if (!data) return;
    int n = (int)pn_int(pn_dict_get(stm, "N"), 0);
    long long first = pn_int(pn_dict_get(stm, "First"), 0);
    if (n < 0 || n > 1000000) n = 0;
    int *nums = (int *)calloc((size_t)n + 1, sizeof(int));
    long long *ofs = (long long *)calloc((size_t)n + 1, sizeof(long long));
    pn_lexer lx;
    pn_lex_init(&lx, data, len);
    for (int i = 0; i < n; i++) {
        if (pn_lex_next(&lx) != TK_INT) { n = i; break; }
        nums[i] = (int)lx.ival;
        if (pn_lex_next(&lx) != TK_INT) { n = i; break; }
        ofs[i] = lx.ival;
    }
    for (int i = 0; i < n; i++) {
        int num = nums[i];
        if (num <= 0 || num >= doc->nxref) continue;
        pn_xref *x = &doc->xref[num];
        if (x->type != 2 || x->stm != stmnum || x->obj) continue;
        if (first + ofs[i] >= (long long)len) continue;
        lx.p = data + first + ofs[i];
        pn_obj *o = pn_parse_obj(&lx, 1);
        x->obj = o ? o : pn_null();
    }
    pn_lex_free(&lx);
    free(nums); free(ofs); free(data);
}

pn_obj *pn_load(pn_doc *doc, int num) {
    if (num <= 0 || num >= doc->nxref) return NULL;
    pn_xref *x = &doc->xref[num];
    if (x->obj) return x->obj;
    if (x->loading) return NULL;
    x->loading = 1;
    if (x->type == 1) {
        int n = -1;
        pn_obj *o = parse_indirect_at(doc, x->ofs, &n, NULL);
        if (o && n != num) { pn_drop(o); o = NULL; }
        x->obj = o;
    } else if (x->type == 2) {
        load_objstm(doc, x->stm);
    }
    x->loading = 0;
    x = &doc->xref[num];
    return x->obj;
}

pn_obj *pn_resolve(pn_doc *doc, pn_obj *o) {
    for (int depth = 0; o && o->type == PN_REF && depth < 32; depth++) o = pn_load(doc, o->u.ref.num);
    if (o && o->type == PN_REF) return NULL;
    return o;
}
pn_obj *pn_get(pn_doc *doc, pn_obj *d, const char *key) { return pn_resolve(doc, pn_dict_get(pn_resolve(doc, d), key)); }
pn_obj *pn_catalog(pn_doc *doc) { return pn_get(doc, doc->trailer, "Root"); }

/* ---------------- xref parsing ---------------- */
static void set_entry(pn_doc *doc, int num, int type, long long a, int b, char *seen) {
    if (num <= 0 || num > 10000000) return;
    xref_ensure(doc, num + 1);
    if (seen[0] == 0) { /* seen map not used; rely on "type==0 means unset" */ }
    pn_xref *x = &doc->xref[num];
    if (x->type != 0 || x->gen == -1) return; /* newer section already defined it */
    if (type == 0) { x->gen = -1; return; } /* free (blocks older revisions) */
    x->type = (unsigned char)type;
    if (type == 1) { x->ofs = a; x->gen = b; }
    else { x->stm = (int)a; x->idx = b; }
}

static int read_xref_table(pn_doc *doc, pn_lexer *lx, pn_obj **trailer_out, char *seen) {
    /* lexer is positioned right after "xref" */
    for (;;) {
        const unsigned char *save = lx->p;
        pn_tok t = pn_lex_next(lx);
        if (t == TK_KEYWORD && !strcmp((char *)lx->sbuf.data, "trailer")) break;
        if (t != TK_INT) { lx->p = save; return 0; }
        long long start = lx->ival;
        if (pn_lex_next(lx) != TK_INT) return 0;
        long long count = lx->ival;
        if (count < 0 || count > 10000000) return 0;
        for (long long i = 0; i < count; i++) {
            if (pn_lex_next(lx) != TK_INT) return 0;
            long long ofs = lx->ival;
            if (pn_lex_next(lx) != TK_INT) return 0;
            int gen = (int)lx->ival;
            if (pn_lex_next(lx) != TK_KEYWORD) return 0;
            int inuse = lx->sbuf.data[0] == 'n';
            int num = (int)(start + i);
            if (start == 1 && i == 0 && ofs == 0 && !inuse) { /* off-by-one quirk: "xref 1 N" starting with free entry */ }
            set_entry(doc, num, inuse ? 1 : 0, ofs, gen, seen);
        }
    }
    pn_obj *tr = pn_parse_obj(lx, 1);
    if (!pn_is(tr, PN_DICT)) { pn_drop(tr); return 0; }
    *trailer_out = tr;
    return 1;
}

static int read_xref_stream(pn_doc *doc, pn_obj *stm, char *seen) {
    pn_obj *w = pn_dict_get(stm, "W");
    if (pn_arr_len(w) < 3) return 0;
    int w0 = (int)pn_int(pn_arr_get(w, 0), 0), w1 = (int)pn_int(pn_arr_get(w, 1), 0), w2 = (int)pn_int(pn_arr_get(w, 2), 0);
    if (w0 < 0 || w1 < 0 || w2 < 0 || w0 > 8 || w1 > 8 || w2 > 8) return 0;
    int rec = w0 + w1 + w2;
    if (rec == 0) return 0;
    size_t len; int ok;
    unsigned char *data = pn_stream_decode(doc, stm, &len, &ok);
    if (!data) return 0;
    long long size = pn_int(pn_dict_get(stm, "Size"), 0);
    pn_obj *index = pn_dict_get(stm, "Index");
    pn_obj *defidx = NULL;
    if (!index) { defidx = pn_new_array(); pn_arr_push(defidx, pn_new_int(0)); pn_arr_push(defidx, pn_new_int(size)); index = defidx; }
    size_t pos = 0;
    for (int s = 0; s + 1 < pn_arr_len(index); s += 2) {
        long long start = pn_int(pn_arr_get(index, s), 0), count = pn_int(pn_arr_get(index, s + 1), 0);
        for (long long i = 0; i < count && pos + (size_t)rec <= len; i++) {
            long long f[3] = {w0 ? 0 : 1, 0, 0};
            int ws[3] = {w0, w1, w2};
            for (int k = 0; k < 3; k++) {
                if (!ws[k]) continue;
                long long v = 0;
                for (int j = 0; j < ws[k]; j++) v = (v << 8) | data[pos++];
                f[k] = v;
            }
            int num = (int)(start + i);
            if (f[0] == 1) set_entry(doc, num, 1, f[1], (int)f[2], seen);
            else if (f[0] == 2) set_entry(doc, num, 2, f[1], (int)f[2], seen);
            else if (f[0] == 0) set_entry(doc, num, 0, 0, 0, seen);
        }
    }
    pn_drop(defidx);
    free(data);
    return 1;
}

static int read_xref_chain(pn_doc *doc, long long startofs, long long hdrofs) {
    char seen[1] = {0};
    long long visited[64];
    int nvisited = 0;
    long long ofs = startofs;
    int sections = 0;
    while (ofs > 0 && nvisited < 64) {
        for (int i = 0; i < nvisited; i++) if (visited[i] == ofs) return sections > 0;
        visited[nvisited++] = ofs;
        long long tryofs[2] = {ofs, ofs + hdrofs};
        pn_obj *trailer = NULL;
        int okk = 0;
        for (int attempt = 0; attempt < 2 && !okk; attempt++) {
            long long o = tryofs[attempt];
            if (attempt == 1 && hdrofs == 0) break;
            if (o < 0 || (size_t)o >= doc->len) continue;
            pn_lexer lx;
            pn_lex_init(&lx, doc->buf, doc->len);
            lx.p = doc->buf + o;
            pn_tok t = pn_lex_next(&lx);
            if (t == TK_KEYWORD && !strcmp((char *)lx.sbuf.data, "xref")) {
                if (read_xref_table(doc, &lx, &trailer, seen)) {
                    okk = 1;
                    /* hybrid file: /XRefStm */
                    long long xs = pn_int(pn_dict_get(trailer, "XRefStm"), -1);
                    if (xs > 0) {
                        pn_obj *xso = parse_indirect_at(doc, xs, NULL, NULL);
                        if (pn_is(xso, PN_STREAM)) read_xref_stream(doc, xso, seen);
                        pn_drop(xso);
                    }
                }
            } else if (t == TK_INT) {
                pn_obj *xso = parse_indirect_at(doc, o, NULL, NULL);
                if (pn_is(xso, PN_STREAM) && pn_is_name(pn_dict_get(xso, "Type"), "XRef")) {
                    if (read_xref_stream(doc, xso, seen)) {
                        okk = 1;
                        trailer = pn_shallow_copy(xso->u.stm.dict);
                    }
                }
                pn_drop(xso);
            }
            pn_lex_free(&lx);
        }
        if (!okk) return sections > 0 ? 1 : 0;
        sections++;
        if (!doc->trailer) {
            doc->trailer = pn_new_dict();
            const char *keys[] = {"Root", "Info", "ID", "Encrypt"};
            for (int k = 0; k < 4; k++) { pn_obj *v = pn_dict_get(trailer, keys[k]); if (v) pn_dict_put(doc->trailer, keys[k], pn_keep(v)); }
        } else {
            const char *keys[] = {"Root", "Info", "ID", "Encrypt"};
            for (int k = 0; k < 4; k++) if (!pn_dict_get(doc->trailer, keys[k]) && pn_dict_get(trailer, keys[k])) pn_dict_put(doc->trailer, keys[k], pn_keep(pn_dict_get(trailer, keys[k])));
        }
        ofs = pn_int(pn_dict_get(trailer, "Prev"), 0);
        pn_drop(trailer);
    }
    return sections > 0;
}

static void reset_xref(pn_doc *doc) {
    for (int i = 0; i < doc->nxref; i++) pn_drop(doc->xref[i].obj);
    if (doc->xref) memset(doc->xref, 0, sizeof(pn_xref) * (size_t)doc->capxref);
    doc->nxref = 0;
    pn_drop(doc->trailer);
    doc->trailer = NULL;
}

/* Scan the whole file for "N G obj" and rebuild the xref. */
static int repair(pn_doc *doc) {
    reset_xref(doc);
    doc->repaired = 1;
    const unsigned char *b = doc->buf;
    size_t n = doc->len;
    pn_obj *root = NULL, *info = NULL;
    for (size_t i = 0; i + 4 < n; i++) {
        if (b[i] == 'o' && b[i + 1] == 'b' && b[i + 2] == 'j' && (i + 3 >= n || pn_is_ws(b[i + 3]) || pn_is_delim(b[i + 3]))) {
            /* walk back over "N G " */
            size_t j = i;
            while (j > 0 && pn_is_ws(b[j - 1])) j--;
            size_t ge = j;
            while (j > 0 && b[j - 1] >= '0' && b[j - 1] <= '9') j--;
            if (j == ge) continue;
            size_t gs = j;
            while (j > 0 && pn_is_ws(b[j - 1])) j--;
            size_t ne = j;
            while (j > 0 && b[j - 1] >= '0' && b[j - 1] <= '9') j--;
            if (j == ne) continue;
            if (j > 0 && !pn_is_ws(b[j - 1]) && !pn_is_delim(b[j - 1])) continue;
            int num = atoi((const char *)b + j);
            if (num <= 0 || num > 10000000) continue;
            xref_ensure(doc, num + 1);
            pn_xref *x = &doc->xref[num];
            pn_drop(x->obj); x->obj = NULL;
            x->type = 1; x->ofs = (long long)j; x->gen = atoi((const char *)b + gs);
        } else if (b[i] == 't' && !memcmp(b + i, "trailer", 7)) {
            pn_lexer lx; pn_lex_init(&lx, b, n); lx.p = b + i + 7;
            pn_obj *t = pn_parse_obj(&lx, 1);
            pn_lex_free(&lx);
            if (pn_is(t, PN_DICT)) {
                if (pn_dict_get(t, "Root")) { pn_drop(root); root = pn_keep(pn_dict_get(t, "Root")); }
                if (pn_dict_get(t, "Info")) { pn_drop(info); info = pn_keep(pn_dict_get(t, "Info")); }
            }
            pn_drop(t);
        }
    }
    /* object streams: register their members */
    for (int num = 1; num < doc->nxref; num++) {
        if (doc->xref[num].type != 1) continue;
        pn_obj *o = pn_load(doc, num);
        if (!pn_is(o, PN_STREAM)) continue;
        pn_obj *type = pn_dict_get(o, "Type");
        if (pn_is_name(type, "ObjStm")) {
            size_t len; int ok;
            unsigned char *data = pn_stream_decode(doc, o, &len, &ok);
            int cnt = (int)pn_int(pn_dict_get(o, "N"), 0);
            pn_lexer lx; pn_lex_init(&lx, data, len);
            for (int k = 0; k < cnt; k++) {
                if (pn_lex_next(&lx) != TK_INT) break;
                int on = (int)lx.ival;
                if (pn_lex_next(&lx) != TK_INT) break;
                if (on <= 0 || on > 10000000) continue;
                xref_ensure(doc, on + 1);
                if (doc->xref[on].type == 0) { doc->xref[on].type = 2; doc->xref[on].stm = num; doc->xref[on].idx = k; }
            }
            pn_lex_free(&lx);
            free(data);
        } else if (pn_is_name(type, "XRef") && !root && pn_dict_get(o, "Root")) {
            root = pn_keep(pn_dict_get(o, "Root"));
            if (pn_dict_get(o, "Info")) { pn_drop(info); info = pn_keep(pn_dict_get(o, "Info")); }
        }
    }
    if (!root || !pn_is(pn_resolve(doc, root), PN_DICT)) {
        pn_drop(root); root = NULL;
        for (int num = 1; num < doc->nxref && !root; num++) {
            pn_obj *o = pn_load(doc, num);
            if (pn_is(o, PN_DICT) && pn_is_name(pn_dict_get(o, "Type"), "Catalog")) root = pn_new_ref(num, 0);
        }
    }
    if (!root) { pn_drop(info); return 0; }
    doc->trailer = pn_new_dict();
    pn_dict_put(doc->trailer, "Root", root);
    if (info) pn_dict_put(doc->trailer, "Info", info);
    return 1;
}

/* ---------------- pages ---------------- */
static void collect_pages(pn_doc *doc, pn_obj *node_ref, int depth, int *cap) {
    if (depth > 64 || !node_ref) return;
    pn_obj *node = pn_resolve(doc, node_ref);
    if (!pn_is(node, PN_DICT)) return;
    pn_obj *kids = pn_get(doc, node, "Kids");
    pn_obj *type = pn_get(doc, node, "Type");
    if (pn_is(kids, PN_ARRAY) && !pn_is_name(type, "Page")) {
        for (int i = 0; i < pn_arr_len(kids); i++) collect_pages(doc, pn_arr_get(kids, i), depth + 1, cap);
        return;
    }
    if (node_ref->type != PN_REF) return;
    int num = node_ref->u.ref.num;
    for (int i = 0; i < doc->npages; i++) if (doc->pages[i] == num) return; /* cycle / duplicate */
    if (doc->npages == *cap) { *cap = *cap ? *cap * 2 : 64; doc->pages = (int *)realloc(doc->pages, sizeof(int) * (size_t)*cap); }
    doc->pages[doc->npages++] = num;
}

static void load_pages(pn_doc *doc) {
    free(doc->pages); doc->pages = NULL; doc->npages = 0;
    int cap = 0;
    pn_obj *cat = pn_catalog(doc);
    collect_pages(doc, pn_dict_get(cat, "Pages"), 0, &cap);
    if (doc->npages == 0) {
        /* broken tree: fall back to all /Type /Page objects in file order */
        for (int num = 1; num < doc->nxref; num++) {
            pn_obj *o = pn_load(doc, num);
            if (pn_is(o, PN_DICT) && pn_is_name(pn_dict_get(o, "Type"), "Page")) {
                if (doc->npages == cap) { cap = cap ? cap * 2 : 64; doc->pages = (int *)realloc(doc->pages, sizeof(int) * (size_t)cap); }
                doc->pages[doc->npages++] = num;
            }
        }
    }
    doc->pages_dirty = 0;
}

int pn_doc_page_count(pn_doc *doc) { if (doc->pages_dirty) load_pages(doc); return doc->npages; }
int pn_doc_page_num(pn_doc *doc, int idx) { if (doc->pages_dirty) load_pages(doc); return (idx >= 0 && idx < doc->npages) ? doc->pages[idx] : 0; }
pn_obj *pn_doc_page(pn_doc *doc, int idx) { int n = pn_doc_page_num(doc, idx); return n ? pn_load(doc, n) : NULL; }

pn_obj *pn_page_inherited(pn_doc *doc, pn_obj *page, const char *key) {
    pn_obj *node = page;
    for (int depth = 0; node && depth < 64; depth++) {
        pn_obj *v = pn_get(doc, node, key);
        if (v) return v;
        node = pn_get(doc, node, "Parent");
    }
    return NULL;
}

void pn_page_box(pn_doc *doc, pn_obj *page, const char *which, double box[4]) {
    pn_obj *b = pn_page_inherited(doc, page, which);
    if (!pn_is(b, PN_ARRAY) && strcmp(which, "MediaBox")) b = pn_page_inherited(doc, page, "MediaBox");
    if (pn_arr_len(b) >= 4) {
        for (int i = 0; i < 4; i++) box[i] = pn_num(pn_resolve(doc, pn_arr_get(b, i)), 0);
        if (box[0] > box[2]) { double t = box[0]; box[0] = box[2]; box[2] = t; }
        if (box[1] > box[3]) { double t = box[1]; box[1] = box[3]; box[3] = t; }
        if (box[2] - box[0] < 1 || box[3] - box[1] < 1) goto deflt;
        return;
    }
deflt:
    box[0] = 0; box[1] = 0; box[2] = 612; box[3] = 792;
}

int pn_page_rotation(pn_doc *doc, pn_obj *page) {
    int r = (int)pn_int(pn_page_inherited(doc, page, "Rotate"), 0);
    r %= 360;
    if (r < 0) r += 360;
    return (r / 90) * 90;
}

void pn_rebuild_page_tree(pn_doc *doc, const int *nums, int n) {
    pn_obj *cat = pn_catalog(doc);
    /* materialize inherited attributes */
    const char *inh[] = {"Resources", "MediaBox", "CropBox", "Rotate"};
    for (int i = 0; i < n; i++) {
        pn_obj *pg = pn_load(doc, nums[i]);
        if (!pn_is(pg, PN_DICT)) continue;
        for (int k = 0; k < 4; k++) {
            if (pn_dict_get(pg, inh[k])) continue;
            pn_obj *v = pn_page_inherited(doc, pg, inh[k]);
            if (v) {
                /* keep the original (possibly indirect) value if found on a parent */
                pn_obj *node = pn_get(doc, pg, "Parent");
                pn_obj *raw = NULL;
                for (int d = 0; node && d < 64 && !raw; d++) { raw = pn_dict_get(node, inh[k]); node = pn_get(doc, node, "Parent"); }
                pn_dict_put(pg, inh[k], raw ? pn_deep_copy_direct(raw) : pn_deep_copy_direct(v));
            }
        }
    }
    pn_obj *pages = pn_new_dict();
    pn_dict_put(pages, "Type", pn_new_name("Pages"));
    pn_obj *kids = pn_new_array();
    for (int i = 0; i < n; i++) pn_arr_push(kids, pn_new_ref(nums[i], 0));
    pn_dict_put(pages, "Kids", kids);
    pn_dict_put(pages, "Count", pn_new_int(n));
    int pnum = pn_add_obj(doc, pages);
    for (int i = 0; i < n; i++) {
        pn_obj *pg = pn_load(doc, nums[i]);
        if (pn_is(pg, PN_DICT)) pn_dict_put(pg, "Parent", pn_new_ref(pnum, 0));
    }
    pn_dict_put(cat, "Pages", pn_new_ref(pnum, 0));
    free(doc->pages);
    doc->pages = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1));
    memcpy(doc->pages, nums, sizeof(int) * (size_t)n);
    doc->npages = n;
    doc->pages_dirty = 0;
}

/* ---------------- open ---------------- */
pn_doc *pn_doc_open(const unsigned char *data, size_t len, char *err, int errlen) {
    pn_doc *doc = (pn_doc *)calloc(1, sizeof *doc);
    doc->buf = (unsigned char *)malloc(len + 1);
    memcpy(doc->buf, data, len);
    doc->buf[len] = 0;
    doc->len = len;
    const unsigned char *hdr = memfind(data, len < 1024 ? len : 1024, "%PDF-");
    if (!hdr) {
        snprintf(err, (size_t)errlen, "This doesn't look like a PDF (no %%PDF header).");
        pn_doc_free(doc);
        return NULL;
    }
    long long hdrofs = hdr - data;
    /* find startxref */
    long long startxref = -1;
    size_t tail = len < 4096 ? len : 4096;
    for (size_t i = len - tail; i + 9 <= len; i++) {
        if (!memcmp(data + i, "startxref", 9)) {
            pn_lexer lx; pn_lex_init(&lx, data, len); lx.p = data + i + 9;
            if (pn_lex_next(&lx) == TK_INT) startxref = lx.ival;
            pn_lex_free(&lx);
        }
    }
    int ok = 0;
    if (startxref > 0) ok = read_xref_chain(doc, startxref, hdrofs);
    if (ok) {
        pn_obj *cat = pn_catalog(doc);
        if (!pn_is(cat, PN_DICT)) ok = 0;
    }
    if (ok) {
        load_pages(doc);
        if (doc->npages == 0) ok = 0;
        else {
            /* sanity: first page must load */
            if (!pn_is(pn_doc_page(doc, 0), PN_DICT)) ok = 0;
        }
    }
    if (!ok) {
        if (!repair(doc)) {
            snprintf(err, (size_t)errlen, "This PDF is too broken for me to read, sorry!");
            pn_doc_free(doc);
            return NULL;
        }
        load_pages(doc);
    }
    if (pn_dict_get(doc->trailer, "Encrypt")) doc->encrypted = 1;
    if (doc->npages == 0) {
        snprintf(err, (size_t)errlen, "I couldn't find any pages in this PDF.");
        pn_doc_free(doc);
        return NULL;
    }
    return doc;
}

/* ---------------- streams ---------------- */
unsigned char *pn_stream_decode(pn_doc *doc, pn_obj *stm, size_t *outlen, int *ok) {
    *ok = 1;
    if (!pn_is(stm, PN_STREAM)) { *outlen = 0; *ok = 0; return NULL; }
    pn_obj *filter = pn_resolve(doc, pn_dict_get(stm, "Filter"));
    pn_obj *parms = pn_resolve(doc, pn_dict_get(stm, "DecodeParms"));
    if (!parms) parms = pn_resolve(doc, pn_dict_get(stm, "DP"));
    unsigned char *cur = (unsigned char *)malloc(stm->u.stm.len + 1);
    memcpy(cur, stm->u.stm.data, stm->u.stm.len);
    size_t curlen = stm->u.stm.len;
    int nf = filter ? (filter->type == PN_ARRAY ? pn_arr_len(filter) : 1) : 0;
    for (int i = 0; i < nf; i++) {
        pn_obj *f = filter->type == PN_ARRAY ? pn_resolve(doc, pn_arr_get(filter, i)) : filter;
        pn_obj *p = parms ? (parms->type == PN_ARRAY ? pn_resolve(doc, pn_arr_get(parms, i)) : parms) : NULL;
        if (!pn_is(f, PN_NAME)) continue;
        size_t nl; int fok;
        unsigned char *next = pn_decode_filter(pn_name(f), pn_is(p, PN_DICT) ? p : NULL, cur, curlen, &nl, &fok);
        free(cur);
        cur = next; curlen = nl;
        if (!fok) { *ok = 0; break; }
    }
    cur[curlen] = 0;
    *outlen = curlen;
    return cur;
}

void pn_stream_set(pn_obj *stm, const unsigned char *data, size_t len, int compress) {
    if (!pn_is(stm, PN_STREAM)) return;
    free(stm->u.stm.data);
    pn_dict_del(stm, "Filter");
    pn_dict_del(stm, "DecodeParms");
    pn_dict_del(stm, "DP");
    if (compress && len > 32) {
        size_t cl;
        unsigned char *c = pn_deflate(data, len, &cl, 6);
        if (c && cl < len) {
            stm->u.stm.data = c; stm->u.stm.len = cl;
            pn_dict_put(stm, "Filter", pn_new_name("FlateDecode"));
            pn_dict_put(stm, "Length", pn_new_int((long long)cl));
            return;
        }
        free(c);
    }
    stm->u.stm.data = (unsigned char *)malloc(len + 1);
    memcpy(stm->u.stm.data, data, len);
    stm->u.stm.len = len;
    pn_dict_put(stm, "Length", pn_new_int((long long)len));
}

unsigned char *pn_page_content(pn_doc *doc, pn_obj *page, size_t *outlen) {
    pn_buf b; pn_buf_init(&b);
    pn_obj *c = pn_get(doc, page, "Contents");
    int n = c ? (c->type == PN_ARRAY ? pn_arr_len(c) : 1) : 0;
    for (int i = 0; i < n; i++) {
        pn_obj *s = c->type == PN_ARRAY ? pn_resolve(doc, pn_arr_get(c, i)) : c;
        if (!pn_is(s, PN_STREAM)) continue;
        size_t l; int ok;
        unsigned char *d = pn_stream_decode(doc, s, &l, &ok);
        if (d) { pn_buf_put(&b, d, l); free(d); }
        pn_buf_putc(&b, '\n');
    }
    return pn_buf_steal(&b, outlen);
}

void pn_page_set_content(pn_doc *doc, pn_obj *page, const unsigned char *data, size_t len) {
    pn_obj *s = pn_new_stream(NULL, NULL, 0);
    pn_stream_set(s, data, len, 1);
    pn_dict_put(page, "Contents", pn_add_ref(doc, s));
}

pn_obj *pn_page_private_resources(pn_doc *doc, pn_obj *page) {
    pn_obj *res = pn_page_inherited(doc, page, "Resources");
    pn_obj *copy = pn_is(res, PN_DICT) ? pn_shallow_copy(res) : pn_new_dict();
    pn_dict_put(page, "Resources", copy);
    return copy;
}

pn_obj *pn_res_subdict(pn_doc *doc, pn_obj *res, const char *cat) {
    pn_obj *sub = pn_get(doc, res, cat);
    pn_obj *copy = pn_is(sub, PN_DICT) ? pn_shallow_copy(sub) : pn_new_dict();
    pn_dict_put(res, cat, copy);
    return copy;
}

void pn_res_unique_name(pn_doc *doc, pn_obj *sub, const char *prefix, char *out, int outlen) {
    (void)doc;
    for (int i = 1; i < 100000; i++) {
        snprintf(out, (size_t)outlen, "%s%d", prefix, i);
        if (!pn_dict_get(sub, out)) return;
    }
}
