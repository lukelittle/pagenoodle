/* Pagenoodle engine test-suite: merge, split, reorder, redaction verification, compression, forms… */
#define _GNU_SOURCE
#include "noodle.h"
#include "pn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static int g_fail = 0, g_pass = 0;
static const char *g_dir;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static unsigned char *slurp(const char *name, int *len) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing fixture %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n + 1);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *len = (int)n;
    return d;
}
static int open_fx(const char *name) {
    int len;
    unsigned char *d = slurp(name, &len);
    int h = nd_open(d, len);
    free(d);
    if (!h) fprintf(stderr, "open %s failed: %s\n", name, nd_error());
    return h;
}
/* save h and reopen the bytes as a new handle */
static int roundtrip(int h, int flags) {
    int len = nd_save(h, flags);
    unsigned char *copy = malloc((size_t)len);
    memcpy(copy, nd_out_ptr(), (size_t)len);
    int r = nd_open(copy, len);
    free(copy);
    return r;
}
static char *text_of(int h, int page) {
    nd_text_plain(h, page);
    return strdup((const char *)nd_out_ptr());
}
static char *all_text(int h) {
    pn_buf b; pn_buf_init(&b);
    for (int p = 0; p < nd_page_count(h); p++) { nd_text_plain(h, p); pn_buf_put(&b, nd_out_ptr(), (size_t)nd_out_len()); pn_buf_putc(&b, '\n'); }
    return (char *)pn_buf_steal(&b, NULL);
}
/* Search every decoded stream and string in the saved bytes for a needle. */
static int bytes_contain(int h, int flags, const char *needle) {
    int len = nd_save(h, flags);
    unsigned char *copy = malloc((size_t)len);
    memcpy(copy, nd_out_ptr(), (size_t)len);
    int found = memmem(copy, (size_t)len, needle, strlen(needle)) != NULL;
    char err[128];
    pn_doc *d = pn_doc_open(copy, (size_t)len, err, sizeof err);
    for (int i = 1; d && i < d->nxref && !found; i++) {
        pn_obj *o = pn_load(d, i);
        if (pn_is(o, PN_STREAM)) {
            size_t l; int ok;
            unsigned char *s = pn_stream_decode(d, o, &l, &ok);
            if (s && memmem(s, l, needle, strlen(needle))) found = 1;
            free(s);
        }
    }
    pn_doc_free(d);
    free(copy);
    return found;
}

static void test_open_and_text(void) {
    puts("open & text extraction");
    int h = open_fx("text3.pdf");
    CHECK(h > 0, "open text3");
    CHECK(nd_page_count(h) == 3, "3 pages, got %d", nd_page_count(h));
    char *t = text_of(h, 1);
    CHECK(strstr(t, "SECRET-CODE-1234") != NULL, "page 2 text contains the secret: %s", t);
    CHECK(strstr(t, "Jane Doe lives at 42 Linguine Lane.") != NULL, "times font line");
    free(t);
    nd_close(h);
    h = open_fx("unicode.pdf");
    t = text_of(h, 0);
    CHECK(strstr(t, "Crème brûlée") != NULL, "unicode via ToUnicode: %s", t);
    free(t);
    nd_close(h);
}

static void test_repair(void) {
    puts("repair broken xref");
    int h = open_fx("broken_xref.pdf");
    CHECK(h > 0, "opened broken file");
    CHECK(nd_page_count(h) == 3, "pages after repair");
    nd_info(h);
    CHECK(strstr((char *)nd_out_ptr(), "\"repaired\":true") != NULL, "reported repaired");
    nd_close(h);
}

static void test_merge(void) {
    puts("merge");
    int a = open_fx("text3.pdf"), b = open_fx("image.pdf"), c = open_fx("form.pdf");
    int hs[5] = {a, a, a, b, c}, pg[5] = {0, 1, 2, 0, 0};
    int m = nd_assemble(hs, pg, NULL, 5);
    CHECK(m > 0, "assemble");
    int r = roundtrip(m, ND_SAVE_GC | ND_SAVE_COMPRESS);
    CHECK(nd_page_count(r) == 5, "merged has 5 pages, got %d", nd_page_count(r));
    char *t = all_text(r);
    CHECK(strstr(t, "Chapter 1") && strstr(t, "Chapter 3") && strstr(t, "Caption under the picture") && strstr(t, "Name:"), "merged text: %s", t);
    free(t);
    nd_forms(r);
    CHECK(strstr((char *)nd_out_ptr(), "\"name\":\"sauce\"") != NULL, "form fields survive merge");
    nd_close(m); nd_close(r); nd_close(a); nd_close(b); nd_close(c);
}

static void test_split_reorder_rotate(void) {
    puts("split / reorder / rotate / duplicate");
    int h = open_fx("many.pdf");
    CHECK(nd_page_count(h) == 300, "300 pages");
    /* split every 100 pages */
    for (int part = 0; part < 3; part++) {
        int hs[100], pg[100];
        for (int i = 0; i < 100; i++) { hs[i] = h; pg[i] = part * 100 + i; }
        int s = nd_assemble(hs, pg, NULL, 100);
        int r = roundtrip(s, ND_SAVE_GC | ND_SAVE_COMPRESS | ND_SAVE_OBJSTM);
        CHECK(nd_page_count(r) == 100, "split part %d has 100 pages", part);
        char *t = text_of(r, 0);
        char want[64];
        snprintf(want, sizeof want, "Page number %d", part * 100 + 1);
        CHECK(strstr(t, want) != NULL, "split part %d starts at %s, got %s", part, want, t);
        free(t);
        nd_close(s); nd_close(r);
    }
    /* reorder + duplicate + rotate */
    int hs[4] = {h, h, h, h}, pg[4] = {9, 0, 4, 4}, rot[4] = {0, 90, 0, 270};
    int s = nd_assemble(hs, pg, rot, 4);
    int r = roundtrip(s, ND_SAVE_GC);
    char *t0 = text_of(r, 0), *t1 = text_of(r, 1), *t3 = text_of(r, 3);
    CHECK(strstr(t0, "Page number 10") != NULL, "reordered first page: %s", t0);
    CHECK(strstr(t1, "Page number 1\n") != NULL || strstr(t1, "Page number 1") != NULL, "second page");
    CHECK(strstr(t3, "Page number 5") != NULL, "duplicate page");
    free(t0); free(t1); free(t3);
    nd_info(r);
    const char *js = (const char *)nd_out_ptr();
    CHECK(strstr(js, "{\"rot\":90") != NULL && strstr(js, "{\"rot\":270") != NULL, "rotation applied: %s", js);
    nd_close(s); nd_close(r); nd_close(h);
}

static void test_redaction(void) {
    puts("true redaction + verification");
    int h = open_fx("text3.pdf");
    CHECK(bytes_contain(h, ND_SAVE_GC, "SECRET-CODE-1234"), "secret present before redaction");
    int n = nd_search(h, "SECRET-CODE-1234", 1);
    CHECK(n == 1, "found secret once, got %d", n);
    double rect[4];
    const char *js = (const char *)nd_out_ptr();
    const char *rp = strstr(js, "\"r\":[[");
    CHECK(rp && sscanf(rp, "\"r\":[[%lf,%lf,%lf,%lf]", &rect[0], &rect[1], &rect[2], &rect[3]) == 4, "parse rect from %s", js);
    nd_redact(h, 1, rect, 1, 0x000000);
    CHECK(strstr((char *)nd_out_ptr(), "\"glyphs\":16") != NULL, "16 glyphs removed: %s", (char *)nd_out_ptr());
    int r = roundtrip(h, ND_SAVE_GC | ND_SAVE_COMPRESS);
    /* 1) re-extract text: gone */
    char *t = all_text(r);
    CHECK(strstr(t, "SECRET") == NULL, "secret not in extracted text: %s", t);
    CHECK(strstr(t, "Jane Doe lives") != NULL, "unrelated text kept");
    CHECK(strstr(t, "The secret code is") != NULL, "text before the box kept");
    free(t);
    /* 2) verify no glyphs remain under the box */
    CHECK(nd_verify(r, 1, rect, 1) == 0, "verify: no glyphs under the redaction box");
    /* 3) not present anywhere in the file, even inside compressed streams */
    CHECK(!bytes_contain(r, ND_SAVE_GC, "SECRET-CODE-1234"), "secret absent from all bytes & streams");
    CHECK(!bytes_contain(r, ND_SAVE_GC, "SECRET"), "partial secret absent too");
    nd_close(r); nd_close(h);

    /* search-driven redaction of every occurrence */
    h = open_fx("text3.pdf");
    nd_search(h, "jane doe", 1);
    char *res = strdup((const char *)nd_out_ptr());
    const char *p = res;
    int hits = 0;
    while ((p = strstr(p, "{\"p\":"))) {
        int page = atoi(p + 5);
        double rr[4];
        const char *q = strstr(p, "[[");
        sscanf(q, "[[%lf,%lf,%lf,%lf]", &rr[0], &rr[1], &rr[2], &rr[3]);
        nd_redact(h, page, rr, 1, 0);
        hits++;
        p = q;
    }
    free(res);
    CHECK(hits == 2, "two Jane Doe hits");
    r = roundtrip(h, ND_SAVE_GC);
    CHECK(nd_search(r, "jane doe", 1) == 0, "no Jane Doe left");
    t = all_text(r);
    CHECK(strstr(t, "lives at 42 Linguine Lane.") != NULL, "rest of line intact: %s", t);
    free(t);
    nd_close(r); nd_close(h);
}

static void test_redact_image(void) {
    puts("image redaction");
    int h = open_fx("image.pdf");
    double rect[4] = {150, 450, 250, 550};
    nd_redact(h, 0, rect, 1, 0);
    CHECK(strstr((char *)nd_out_ptr(), "\"imagesScrubbed\":1") != NULL, "image scrubbed: %s", (char *)nd_out_ptr());
    int r = roundtrip(h, ND_SAVE_GC);
    nd_images(r);
    const char *js = (const char *)nd_out_ptr();
    int count = 0;
    for (const char *q = js; (q = strstr(q, "\"num\"")); q++) count++;
    CHECK(count == 1, "only the scrubbed image remains in file (%d): %s", count, js);
    /* pixel under the box must now be black, outside unchanged */
    int num = atoi(strstr(js, "\"num\":") + 6);
    int comps = nd_image_pixels(r, num);
    CHECK(comps == 3, "pixels decodable");
    const unsigned char *px = nd_out_ptr();
    int w = nd_last_w();
    /* image placed at x 100..500, y 400..600 (200x100 px): box x 150..250 -> cols 25..75, y 450..550 -> rows 25..75 */
    const unsigned char *in = px + (50 * w + 50) * 3, *outp = px + (5 * w + 5) * 3;
    CHECK(in[0] == 0 && in[1] == 0 && in[2] == 0, "pixel inside blanked");
    CHECK(outp[0] == 200 && outp[1] == 30 && outp[2] == 30, "pixel outside untouched (%d,%d,%d)", outp[0], outp[1], outp[2]);
    nd_close(r); nd_close(h);
}

static void test_metadata_and_compress(void) {
    puts("strip metadata & compress");
    int h = open_fx("text3.pdf");
    CHECK(bytes_contain(h, ND_SAVE_GC, "Agent Spaghetti"), "author present before");
    int r = roundtrip(h, ND_SAVE_GC | ND_SAVE_STRIPMETA);
    CHECK(!bytes_contain(r, ND_SAVE_GC, "Agent Spaghetti"), "author stripped");
    CHECK(!bytes_contain(r, ND_SAVE_GC, "ReportLab"), "producer stripped");
    nd_close(r); nd_close(h);

    h = open_fx("many.pdf");
    int before = nd_save(h, ND_SAVE_GC);
    int after = nd_save(h, ND_SAVE_GC | ND_SAVE_COMPRESS | ND_SAVE_OBJSTM | ND_SAVE_DEDUP | ND_SAVE_RECOMPRESS);
    CHECK(after < before * 0.75, "compressed %d -> %d", before, after);
    r = roundtrip(h, ND_SAVE_GC | ND_SAVE_COMPRESS | ND_SAVE_OBJSTM | ND_SAVE_DEDUP | ND_SAVE_RECOMPRESS);
    CHECK(nd_page_count(r) == 300, "object-stream output re-opens with all pages");
    char *t = text_of(r, 299);
    CHECK(strstr(t, "Page number 300") != NULL, "last page text");
    free(t);
    nd_close(r); nd_close(h);
}

static void test_forms(void) {
    puts("forms");
    int h = open_fx("form.pdf");
    nd_forms(h);
    char *js = strdup((const char *)nd_out_ptr());
    CHECK(strstr(js, "\"type\":\"checkbox\"") && strstr(js, "\"type\":\"radio\"") && strstr(js, "\"type\":\"combo\""), "field types: %s", js);
    int name_id = atoi(strstr(js, "\"id\":") + 5);
    int likes_id = atoi(strstr(strstr(js, "\"likes\"") - 20, "\"id\":") + 5);
    free(js);
    CHECK(nd_form_set(h, name_id, "Señor Noodle") == 0, "set text");
    CHECK(nd_form_set(h, likes_id, "Yes") == 0, "set checkbox");
    int r = roundtrip(h, ND_SAVE_GC);
    nd_forms(r);
    js = (char *)nd_out_ptr();
    CHECK(strstr(js, "\"value\":\"Señor Noodle\"") != NULL, "text value persisted: %s", js);
    CHECK(strstr(js, "\"value\":\"Yes\"") != NULL, "checkbox value persisted");
    CHECK(nd_form_flatten(r) >= 1, "flatten");
    int f = roundtrip(r, ND_SAVE_GC);
    nd_forms(f);
    CHECK(!strcmp((char *)nd_out_ptr(), "[]"), "no fields after flatten");
    char *t = text_of(f, 0);
    CHECK(strstr(t, "Señor Noodle") != NULL, "flattened value is page content");
    free(t);
    nd_close(f); nd_close(r); nd_close(h);
}

static void test_stamp_overlay_images(void) {
    puts("page numbers, overlays, images→PDF");
    int h = open_fx("text3.pdf");
    nd_stamp(h, "Page {n} of {total}", 4, 10, 24, 0, -1, 1, "Helvetica", 0);
    int r = roundtrip(h, ND_SAVE_GC);
    char *t = text_of(r, 2);
    CHECK(strstr(t, "Page 3 of 3") != NULL, "page number stamped: %s", t);
    free(t);
    nd_close(r);
    /* overlay with font + gstate */
    nd_ov_begin(h, 0);
    char fn[64];
    snprintf(fn, sizeof fn, "%s", nd_ov_font("Helvetica-Bold"));
    char ops[256];
    snprintf(ops, sizeof ops, "BT /%s 18 Tf 100 100 Td (Hello noodle) Tj ET", fn);
    nd_ov_ops(ops);
    nd_ov_end();
    r = roundtrip(h, ND_SAVE_GC);
    t = text_of(r, 0);
    CHECK(strstr(t, "Hello noodle") != NULL, "overlay text: %s", t);
    free(t);
    nd_close(r); nd_close(h);
    /* images → pdf */
    int d = nd_new();
    unsigned char px[4 * 4 * 3];
    memset(px, 128, sizeof px);
    unsigned char alpha[16];
    memset(alpha, 255, sizeof alpha);
    alpha[0] = 0;
    int img = nd_image_new(d, px, (int)sizeof px, 4, 4, 3, 0, alpha);
    CHECK(img > 0, "image object");
    nd_add_image_page(d, img, 595, 842, 36);
    nd_add_image_page(d, img, 0, 0, 0);
    r = roundtrip(d, ND_SAVE_GC | ND_SAVE_COMPRESS);
    CHECK(nd_page_count(r) == 2, "two image pages");
    nd_images(r);
    CHECK(strstr((char *)nd_out_ptr(), "\"smask\":true") != NULL, "alpha kept as SMask");
    nd_close(r); nd_close(d);
}

static void test_garbage(void) {
    puts("robustness");
    unsigned char junk[64] = "not a pdf at all";
    CHECK(nd_open(junk, 16) == 0, "rejects non-PDF");
    int len;
    unsigned char *d = slurp("text3.pdf", &len);
    /* truncate in the middle: should still open (repair) or fail gracefully */
    int h = nd_open(d, len / 2);
    if (h) nd_close(h);
    CHECK(1, "truncated file handled");
    /* random corruption must not crash */
    srand(7);
    for (int k = 0; k < 200; k++) {
        unsigned char *c = malloc((size_t)len);
        memcpy(c, d, (size_t)len);
        for (int i = 0; i < 20; i++) c[rand() % len] = (unsigned char)rand();
        int hh = nd_open(c, len);
        if (hh) {
            for (int p = 0; p < nd_page_count(hh); p++) nd_text_plain(hh, p);
            nd_save(hh, ND_SAVE_GC | ND_SAVE_COMPRESS);
            nd_close(hh);
        }
        free(c);
    }
    CHECK(1, "fuzzed 200 corrupt files without crashing");
    free(d);
}

static void test_adversarial(void) {
    puts("adversarial files from the security review");
    /* text 14 Form XObjects deep: redaction must still remove it (fail closed) */
    int h = open_fx("nested.pdf");
    double r[4] = {60, 490, 400, 530};
    nd_redact(h, 0, r, 1, 0);
    CHECK(!bytes_contain(h, ND_SAVE_GC | ND_SAVE_COMPRESS, "TOPSECRET"), "deeply nested text removed");
    int o = roundtrip(h, ND_SAVE_GC);
    char *t = text_of(o, 0);
    CHECK(strstr(t, "Public header") != NULL, "content outside the box kept: %s", t);
    free(t);
    nd_close(o); nd_close(h);
    /* geometry must match PDF.js: invalid /Rotate is ignored, CropBox is clipped to MediaBox */
    h = open_fx("rot135.pdf");
    nd_info(h);
    CHECK(strstr((char *)nd_out_ptr(), "{\"rot\":0,\"vw\":612.000,\"vh\":792.000") != NULL, "/Rotate 135 treated as 0: %s", (char *)nd_out_ptr());
    nd_close(h);
    h = open_fx("crop.pdf");
    nd_info(h);
    CHECK(strstr((char *)nd_out_ptr(), "\"vw\":612.000,\"vh\":792.000") != NULL, "CropBox clipped to MediaBox: %s", (char *)nd_out_ptr());
    nd_close(h);
    /* a term that also lives in a form value, its appearance, a note, a bookmark and Info */
    h = open_fx("elsewhere.pdf");
    CHECK(nd_scan_term(h, "John Smith") > 0, "term found before");
    nd_search(h, "John Smith", 1);
    const char *rp = strstr((const char *)nd_out_ptr(), "[[");
    double rr[4] = {0};
    if (rp) sscanf(rp, "[[%lf,%lf,%lf,%lf]", &rr[0], &rr[1], &rr[2], &rr[3]);
    nd_redact(h, 0, rr, 1, 0);
    CHECK(nd_scrub_term(h, "John Smith") > 0, "scrubbed outside page content");
    o = roundtrip(h, ND_SAVE_GC | ND_SAVE_COMPRESS | ND_SAVE_STRIPMETA);
    CHECK(nd_scan_term(o, "John Smith") == 0, "term gone everywhere: %s", (char *)nd_out_ptr());
    CHECK(!bytes_contain(o, ND_SAVE_GC, "John Smith"), "not in any byte or stream");
    t = text_of(o, 0);
    CHECK(strstr(t, "Emergency contact:") != NULL, "other text kept");
    free(t);
    nd_close(o); nd_close(h);
}

/* Inputs that once crashed the fuzzer must stay harmless. */
static void test_fuzz_regressions(void) {
    puts("fuzzer regressions");
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/../fuzz-regressions", g_dir);
    DIR *dd = opendir(dir);
    char names[64][128];
    int nn = 0;
    for (struct dirent *e; dd && (e = readdir(dd)) && nn < 64;)
        if (e->d_name[0] != '.') snprintf(names[nn++], sizeof names[0], "%s", e->d_name);
    if (dd) closedir(dd);
    CHECK(nn >= 5, "fuzz regression corpus present (%d files)", nn);
    for (int i = 0; i < nn; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { CHECK(0, "missing %s", path); continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *d = malloc((size_t)n + 1);
        if (fread(d, 1, (size_t)n, f) != (size_t)n) n = 0;
        fclose(f);
        int h = nd_open(d, (int)n);
        if (h) {
            int np = nd_page_count(h);
            for (int p = 0; p < np && p < 50; p++) nd_text(h, p);
            nd_search(h, "a", 1);
            double r[4] = {0, 0, 300, 300};
            nd_redact(h, 0, r, 1, 0);
            nd_ov_begin(h, 0); nd_ov_font("Helvetica"); nd_ov_ops("q Q"); nd_ov_end();
            nd_stamp(h, "{n}", 4, 10, 24, 0, -1, 1, "Helvetica", 0);
            int hs[2] = {h, h}, pg[2] = {0, np - 1};
            int a = nd_assemble(hs, pg, NULL, 2);
            if (a) { nd_save(a, 1 | 2 | 4 | 8 | 16 | 32); nd_close(a); }
            nd_save(h, 1 | 2 | 4 | 8 | 32);
            nd_close(h);
        }
        free(d);
        CHECK(1, "%s handled", names[i]);
    }
}

/* Proof-of-concept files from the security review (tests/security). Each must open and process
 * without crashing or hanging, and redaction must leave no trace of its secret. */
static unsigned char *slurp_path(const char *path, int *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n + 1);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) n = 0;
    fclose(f);
    *len = (int)n;
    return d;
}

static void test_security_pocs(void) {
    puts("security review proof-of-concept files");
    struct { const char *file; double r[4]; const char *secret; } cases[] = {
        {"actualtext.pdf", {0, 0, 1000, 1000}, "123-45-6789"},
        {"annot_popup.pdf", {0, 0, 1000, 1000}, "SECRET-APPEARANCE"},
        {"annot_popup.pdf", {0, 0, 1000, 1000}, "SECRET-RICHTEXT"},
        {"annot_popup.pdf", {0, 0, 1000, 1000}, "SECRET-AUTHOR"},
        {"deepform13.pdf", {90, 690, 400, 730}, "SECRET-SSN-123"},
        {"deepform30.pdf", {0, 0, 1000, 1000}, "SECRET-SSN-123"},
        {"inline_ei.pdf", {0, 0, 1000, 1000}, "PIXELDATA-SECRET"},
        {"pattern.pdf", {0, 0, 1000, 1000}, "PATTERN-SECRET"},
        {"qstack.pdf", {90, 190, 300, 230}, "TOPSECRET"},
        {"smask.pdf", {0, 0, 1000, 1000}, "SECRETALPHAMASK"},
        {"struct_stm.pdf", {0, 0, 1000, 1000}, "FORM-SECRET-42"},
        {"tf0.pdf", {0, 0, 1000, 1000}, "ZEROSIZE-SECRET"},
        {"vertical.pdf", {0, 0, 1000, 1000}, "ABCDEFGH"},
        {"inline_wh.pdf", {90, 690, 400, 730}, "INLINE-HIDDEN"},
        {"resmerge.pdf", {90, 690, 400, 730}, "MERGED-RES-SECRET"},
        {"props.pdf", {90, 690, 400, 730}, "PROP-SSN"},
        {"props.pdf", {90, 690, 400, 730}, "STRUCT-SSN"},
        {"props.pdf", {90, 690, 400, 730}, "ALT-SSN"},
        {"pattern_quote.pdf", {0, 0, 1000, 1000}, "PATTERN-SECRET"},
        {"attach-zip.pdf", {0, 0, 1000, 1000}, "EmbeddedFile"},
    };
    char path[1200];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(path, sizeof path, "%s/../security/%s", g_dir, cases[i].file);
        int len;
        unsigned char *d = slurp_path(path, &len);
        if (!d) { CHECK(0, "missing %s", path); continue; }
        int h = nd_open(d, len);
        free(d);
        if (!h) { CHECK(0, "open %s: %s", cases[i].file, nd_error()); continue; }
        nd_redact_mode(1);
        nd_redact(h, 0, cases[i].r, 1, 0);
        CHECK(!bytes_contain(h, ND_SAVE_GC | ND_SAVE_COMPRESS, cases[i].secret), "%s: \"%s\" removed", cases[i].file, cases[i].secret);
        nd_close(h);
    }
    /* memory-safety / hang cases: every API must just work */
    const char *crashers[] = {"annots_negref.pdf", "bigtr.pdf", "fields_dag.pdf", "img_rowbytes_overflow.pdf", "inflate_exact.pdf",
                              "objstm_neg.pdf", "objstm_quad.pdf", "pagedag.pdf", "predictor.pdf", "sharedform.pdf", "sharedres.pdf",
                              "uaf_popup_page.pdf", "uaf_popup_self.pdf", "uaf_annot_page.pdf", "uaf_image_is_page.pdf", "uaf_selfform.pdf",
                              "formfan.pdf", "parentchain.pdf"};
    for (size_t i = 0; i < sizeof crashers / sizeof crashers[0]; i++) {
        snprintf(path, sizeof path, "%s/../security/%s", g_dir, crashers[i]);
        int len;
        unsigned char *d = slurp_path(path, &len);
        if (!d) { CHECK(0, "missing %s", path); continue; }
        int h = nd_open(d, len);
        free(d);
        if (h) {
            nd_info(h); nd_forms(h); nd_images(h);
            for (int p = 0; p < nd_page_count(h) && p < 20; p++) nd_text(h, p);
            double r[4] = {90, 690, 400, 730};
            nd_redact(h, 0, r, 1, 0);
            nd_search(h, "SECRET", 1);
            nd_save(h, 1 | 2 | 4 | 8 | 16 | 32);
            nd_close(h);
        }
        CHECK(1, "%s handled", crashers[i]);
    }
}

static void test_geometry_like_pdfjs(void) {
    puts("page geometry follows PDF.js");
    char path[1200];
    snprintf(path, sizeof path, "%s/../security/parentchain.pdf", g_dir);
    int len;
    unsigned char *d = slurp_path(path, &len);
    int h = d ? nd_open(d, len) : 0;
    free(d);
    CHECK(h > 0, "open parentchain");
    nd_info(h);
    CHECK(strstr((char *)nd_out_ptr(), "{\"rot\":90,\"vw\":492.000,\"vh\":612.000") != NULL, "70-level /Parent chain inherits rotate+crop: %s", (char *)nd_out_ptr());
    nd_close(h);
}

int main(int argc, char **argv) {
    g_dir = argc > 1 ? argv[1] : "tests/fixtures";
    test_open_and_text();
    test_repair();
    test_merge();
    test_split_reorder_rotate();
    test_redaction();
    test_redact_image();
    test_metadata_and_compress();
    test_forms();
    test_stamp_overlay_images();
    test_garbage();
    test_adversarial();
    test_fuzz_regressions();
    test_security_pocs();
    test_geometry_like_pdfjs();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
