/* Cross-document object copying and page assembly (merge / split / reorder / rotate / duplicate). */
#include "pn.h"
#include "pn_engine.h"
#include <stdlib.h>
#include <string.h>

struct pn_copymap {
    pn_doc *src, *dst;
    int *map;      /* src num -> dst num */
    char *allowed; /* page objects that may be copied */
    int n;
    int *queue; int qlen, qcap; /* src objects allocated but not yet copied (iterative, no deep recursion) */
    int draining;
};

pn_copymap *pn_copymap_new(pn_doc *src, pn_doc *dst) {
    pn_copymap *m = (pn_copymap *)calloc(1, sizeof *m);
    m->src = src; m->dst = dst; m->n = src->nxref + 1;
    m->map = (int *)calloc((size_t)m->n, sizeof(int));
    m->allowed = (char *)calloc((size_t)m->n, 1);
    return m;
}
void pn_copymap_free(pn_copymap *m) { if (!m) return; free(m->map); free(m->allowed); free(m->queue); free(m); }
void pn_copymap_allow_page(pn_copymap *m, int num) { if (num > 0 && num < m->n) m->allowed[num] = 1; }
int pn_copymap_lookup(pn_copymap *m, int num) { return (num > 0 && num < m->n) ? m->map[num] : 0; }

static pn_obj *copy_direct(pn_copymap *m, pn_obj *o, int is_page);

static pn_obj *copy_into(pn_copymap *m, pn_obj *o);

/* Copy an object; referenced objects are allocated immediately and copied from a work queue,
 * so long reference chains cannot exhaust the stack. */
pn_obj *pn_copy_into(pn_copymap *m, pn_obj *o) {
    pn_obj *r = copy_into(m, o);
    if (!m->draining) {
        m->draining = 1;
        while (m->qlen > 0) {
            int num = m->queue[--m->qlen];
            pn_obj *src = pn_load(m->src, num);
            int is_page = pn_is(src, PN_DICT) && pn_is_name(pn_dict_get(src, "Type"), "Page");
            pn_set_obj(m->dst, m->map[num], src ? copy_direct(m, src, is_page) : pn_null());
        }
        m->draining = 0;
    }
    return r;
}

static pn_obj *copy_into(pn_copymap *m, pn_obj *o) {
    if (!o) return pn_null();
    if (o->type == PN_REF) {
        int num = o->u.ref.num;
        if (num <= 0 || num >= m->n) return pn_null();
        if (m->map[num]) return pn_new_ref(m->map[num], 0);
        pn_obj *src = pn_load(m->src, num);
        if (!src) return pn_null();
        int is_page = pn_is(src, PN_DICT) && pn_is_name(pn_dict_get(src, "Type"), "Page");
        if (is_page && !m->allowed[num]) return pn_null();
        if (pn_is(src, PN_DICT) && pn_is_name(pn_dict_get(src, "Type"), "Pages")) return pn_null();
        int dn = pn_add_obj(m->dst, pn_null());
        m->map[num] = dn;
        if (m->qlen == m->qcap) { m->qcap = m->qcap ? m->qcap * 2 : 64; m->queue = (int *)realloc(m->queue, sizeof(int) * (size_t)m->qcap); }
        m->queue[m->qlen++] = num;
        return pn_new_ref(dn, 0);
    }
    return copy_direct(m, o, 0);
}

static pn_obj *copy_direct_impl(pn_copymap *m, pn_obj *o, int is_page);
static int g_cdepth = 0;
static pn_obj *copy_direct(pn_copymap *m, pn_obj *o, int is_page) {
    if (g_cdepth > 256) return pn_null(); /* defence in depth against direct-object cycles */
    g_cdepth++;
    pn_obj *r = copy_direct_impl(m, o, is_page);
    g_cdepth--;
    return r;
}

static pn_obj *copy_direct_impl(pn_copymap *m, pn_obj *o, int is_page) {
    int i;
    switch (o->type) {
    case PN_ARRAY: {
        pn_obj *a = pn_new_array();
        for (i = 0; i < o->u.arr.len; i++) pn_arr_push(a, copy_into(m, o->u.arr.items[i]));
        return a;
    }
    case PN_DICT: {
        pn_obj *d = pn_new_dict();
        for (i = 0; i < o->u.dict.len; i++) {
            const char *k = o->u.dict.keys[i];
            if (is_page && (!strcmp(k, "Parent") || !strcmp(k, "B") || !strcmp(k, "Thumb"))) continue;
            pn_obj *v = copy_into(m, o->u.dict.vals[i]);
            if (v->type == PN_NULL) { pn_drop(v); continue; }
            pn_dict_put(d, k, v);
        }
        return d;
    }
    case PN_STREAM: {
        unsigned char *data = (unsigned char *)malloc(o->u.stm.len + 1);
        memcpy(data, o->u.stm.data, o->u.stm.len);
        pn_obj *dict = copy_direct(m, o->u.stm.dict, 0);
        return pn_new_stream(dict, data, o->u.stm.len);
    }
    default: return pn_shallow_copy(o);
    }
}

/* ------------------------------------------------------------------ */
/* Assemble: build a new document from (doc, page, rotate) specs.      */
/* ------------------------------------------------------------------ */
pn_doc *pn_assemble(pn_doc **docs, const int *pages, const int *rot, int n) {
    pn_doc *dst = pn_doc_new();
    /* unique source docs */
    pn_doc **srcs = (pn_doc **)calloc((size_t)n + 1, sizeof(pn_doc *));
    pn_copymap **maps = (pn_copymap **)calloc((size_t)n + 1, sizeof(pn_copymap *));
    int nsrc = 0;
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int j = 0; j < nsrc; j++) if (srcs[j] == docs[i]) found = 1;
        if (!found) { srcs[nsrc] = docs[i]; maps[nsrc] = pn_copymap_new(docs[i], dst); nsrc++; }
    }
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < nsrc; j++) if (srcs[j] == docs[i]) pn_copymap_allow_page(maps[j], pn_doc_page_num(docs[i], pages[i]));
    }
    int *nums = (int *)calloc((size_t)n + 1, sizeof(int));
    int count = 0;
    for (int i = 0; i < n; i++) {
        pn_doc *src = docs[i];
        pn_copymap *m = NULL;
        for (int j = 0; j < nsrc; j++) if (srcs[j] == src) m = maps[j];
        int snum = pn_doc_page_num(src, pages[i]);
        pn_obj *spage = pn_load(src, snum);
        if (!snum || !pn_is(spage, PN_DICT)) continue;
        int already = pn_copymap_lookup(m, snum);
        int dnum;
        if (!already) {
            pn_obj *ref = pn_new_ref(snum, 0);
            pn_obj *r = pn_copy_into(m, ref);
            pn_drop(ref);
            dnum = r->type == PN_REF ? r->u.ref.num : 0;
            pn_drop(r);
        } else {
            /* duplicate page: fresh page dict sharing resources */
            pn_obj *orig = pn_load(dst, already);
            pn_obj *dup = pn_shallow_copy(orig);
            pn_obj *annots = pn_dict_get(dup, "Annots");
            if (annots) pn_dict_put(dup, "Annots", pn_shallow_copy(annots));
            dnum = pn_add_obj(dst, dup);
        }
        if (!dnum) continue;
        pn_obj *dpage = pn_load(dst, dnum);
        /* inherited attributes from source tree */
        const char *inh[] = {"Resources", "MediaBox", "CropBox"};
        for (int k = 0; k < 3; k++) {
            if (pn_dict_get(dpage, inh[k])) continue;
            pn_obj *node = pn_get(src, spage, "Parent");
            pn_obj *raw = NULL;
            for (int d = 0; node && d < 64 && !raw; d++) { raw = pn_dict_get(node, inh[k]); node = pn_get(src, node, "Parent"); }
            if (raw) pn_dict_put(dpage, inh[k], pn_copy_into(m, raw));
        }
        if (!pn_dict_get(dpage, "MediaBox")) {
            pn_obj *mb = pn_new_array();
            pn_arr_push(mb, pn_new_int(0)); pn_arr_push(mb, pn_new_int(0)); pn_arr_push(mb, pn_new_int(612)); pn_arr_push(mb, pn_new_int(792));
            pn_dict_put(dpage, "MediaBox", mb);
        }
        int r = pn_page_rotation(src, spage) + (rot ? rot[i] : 0);
        r %= 360; if (r < 0) r += 360;
        r = (r / 90) * 90;
        if (r) pn_dict_put(dpage, "Rotate", pn_new_int(r)); else pn_dict_del(dpage, "Rotate");
        pn_dict_del(dpage, "Parent");
        nums[count++] = dnum;
        if ((i & 31) == 0) pn_progress(i, n);
    }
    pn_rebuild_page_tree(dst, nums, count);
    /* carry interactive forms over */
    pn_obj *dcat = pn_catalog(dst);
    pn_obj *dform = NULL;
    for (int j = 0; j < nsrc; j++) {
        pn_obj *sform = pn_get(srcs[j], pn_catalog(srcs[j]), "AcroForm");
        pn_obj *sfields = pn_get(srcs[j], sform, "Fields");
        if (!pn_is(sfields, PN_ARRAY) || pn_arr_len(sfields) == 0) continue;
        if (!dform) {
            dform = pn_new_dict();
            pn_dict_put(dform, "Fields", pn_new_array());
            pn_obj *v;
            if ((v = pn_dict_get(sform, "DR"))) pn_dict_put(dform, "DR", pn_copy_into(maps[j], v));
            if ((v = pn_dict_get(sform, "DA"))) pn_dict_put(dform, "DA", pn_copy_into(maps[j], v));
            if ((v = pn_dict_get(sform, "NeedAppearances"))) pn_dict_put(dform, "NeedAppearances", pn_copy_into(maps[j], v));
        }
        pn_obj *dfields = pn_dict_get(dform, "Fields");
        for (int k = 0; k < pn_arr_len(sfields); k++) {
            pn_obj *f = pn_arr_get(sfields, k);
            /* only keep fields that have a widget on a copied page */
            pn_obj *c = pn_copy_into(maps[j], f);
            if (c->type == PN_REF) {
                int dup = 0;
                for (int q = 0; q < pn_arr_len(dfields); q++) { pn_obj *e = pn_arr_get(dfields, q); if (e->type == PN_REF && e->u.ref.num == c->u.ref.num) dup = 1; }
                if (!dup) { pn_arr_push(dfields, c); continue; }
            }
            pn_drop(c);
        }
    }
    if (dform) pn_dict_put(dcat, "AcroForm", pn_add_ref(dst, dform));
    /* keep the first source's document metadata (stripping is an explicit, separate choice) */
    if (nsrc > 0) {
        pn_obj *info = pn_dict_get(srcs[0]->trailer, "Info");
        if (info) {
            pn_obj *c = pn_copy_into(maps[0], info);
            if (c->type == PN_REF) pn_dict_put(dst->trailer, "Info", c); else pn_drop(c);
        }
        pn_obj *scat = pn_catalog(srcs[0]);
        const char *keep[] = {"Metadata", "Lang", "ViewerPreferences"};
        for (int k = 0; k < 3; k++) { pn_obj *v = pn_dict_get(scat, keep[k]); if (v) pn_dict_put(dcat, keep[k], pn_copy_into(maps[0], v)); }
    }
    for (int j = 0; j < nsrc; j++) pn_copymap_free(maps[j]);
    free(maps); free(srcs); free(nums);
    pn_progress(n, n);
    return dst;
}
