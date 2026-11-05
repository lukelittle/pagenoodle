/* pnoodle — tiny native CLI over the engine API, used for debugging and tests. */
#include "noodle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, int *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n + 1);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read error\n"); exit(2); }
    fclose(f);
    *len = (int)n;
    return d;
}
static int open_file(const char *path) {
    int len;
    unsigned char *d = slurp(path, &len);
    int h = nd_open(d, len);
    free(d);
    if (!h) { fprintf(stderr, "%s: %s\n", path, nd_error()); exit(1); }
    return h;
}
static void write_out(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fwrite(nd_out_ptr(), 1, (size_t)nd_out_len(), f);
    fclose(f);
}
static void print_out(void) { fwrite(nd_out_ptr(), 1, (size_t)nd_out_len(), stdout); putchar('\n'); }

static void usage(void) {
    fprintf(stderr,
            "usage: pnoodle <command> ...\n"
            "  info in.pdf\n"
            "  text in.pdf [page]\n"
            "  words in.pdf page\n"
            "  search in.pdf needle\n"
            "  merge out.pdf a.pdf b.pdf ...\n"
            "  pages out.pdf in.pdf 3,1,2,2      (0-based page list; suffix r=rotate 90)\n"
            "  compress out.pdf in.pdf\n"
            "  strip out.pdf in.pdf\n"
            "  redact out.pdf in.pdf page x0 y0 x1 y1\n"
            "  redact-text out.pdf in.pdf needle\n"
            "  forms in.pdf\n"
            "  fill out.pdf in.pdf id value\n"
            "  number out.pdf in.pdf template\n"
            "  images in.pdf\n");
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 3) usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "info")) { int h = open_file(argv[2]); nd_info(h); print_out(); return 0; }
    if (!strcmp(cmd, "text")) {
        int h = open_file(argv[2]);
        int n = nd_page_count(h);
        for (int p = 0; p < n; p++) {
            if (argc > 3 && atoi(argv[3]) != p) continue;
            nd_text_plain(h, p);
            printf("--- page %d ---\n", p + 1);
            print_out();
        }
        return 0;
    }
    if (!strcmp(cmd, "words") && argc > 3) { int h = open_file(argv[2]); nd_text(h, atoi(argv[3])); print_out(); return 0; }
    if (!strcmp(cmd, "search") && argc > 3) { int h = open_file(argv[2]); int n = nd_search(h, argv[3], 1); print_out(); fprintf(stderr, "%d matches\n", n); return 0; }
    if (!strcmp(cmd, "images")) { int h = open_file(argv[2]); nd_images(h); print_out(); return 0; }
    if (!strcmp(cmd, "forms")) { int h = open_file(argv[2]); nd_forms(h); print_out(); return 0; }
    if (argc < 4) usage();
    const char *out = argv[2];
    if (!strcmp(cmd, "merge")) {
        int hs[64], pages[4096], handles[4096], n = 0;
        for (int i = 3; i < argc && i - 3 < 64; i++) {
            hs[i - 3] = open_file(argv[i]);
            int pc = nd_page_count(hs[i - 3]);
            for (int p = 0; p < pc && n < 4096; p++) { handles[n] = hs[i - 3]; pages[n] = p; n++; }
        }
        int r = nd_assemble(handles, pages, NULL, n);
        if (!r) { fprintf(stderr, "%s\n", nd_error()); return 1; }
        nd_save(r, ND_SAVE_GC | ND_SAVE_COMPRESS);
        write_out(out);
        return 0;
    }
    int h = open_file(argv[3]);
    if (!strcmp(cmd, "pages") && argc > 4) {
        int handles[4096], pages[4096], rots[4096], n = 0;
        char *s = strdup(argv[4]);
        for (char *tok = strtok(s, ","); tok && n < 4096; tok = strtok(NULL, ",")) {
            handles[n] = h; pages[n] = atoi(tok); rots[n] = strchr(tok, 'r') ? 90 : 0; n++;
        }
        int r = nd_assemble(handles, pages, rots, n);
        if (!r) { fprintf(stderr, "%s\n", nd_error()); return 1; }
        nd_save(r, ND_SAVE_GC | ND_SAVE_COMPRESS);
        write_out(out);
        return 0;
    }
    if (!strcmp(cmd, "compress")) { nd_save(h, ND_SAVE_GC | ND_SAVE_COMPRESS | ND_SAVE_OBJSTM | ND_SAVE_DEDUP | ND_SAVE_RECOMPRESS); write_out(out); return 0; }
    if (!strcmp(cmd, "strip")) { nd_save(h, ND_SAVE_GC | ND_SAVE_STRIPMETA); write_out(out); return 0; }
    if (!strcmp(cmd, "redact") && argc > 8) {
        double r[4] = {atof(argv[5]), atof(argv[6]), atof(argv[7]), atof(argv[8])};
        nd_redact(h, atoi(argv[4]), r, 1, 0);
        print_out();
        nd_save(h, ND_SAVE_GC | ND_SAVE_COMPRESS);
        write_out(out);
        return 0;
    }
    if (!strcmp(cmd, "redact-text") && argc > 4) {
        int n = nd_search(h, argv[4], 1);
        fprintf(stderr, "%d matches\n", n);
        /* naive JSON scan: {"p":N,"r":[[a,b,c,d],...]} */
        char *js = strdup((const char *)nd_out_ptr());
        char *p = js;
        while ((p = strstr(p, "{\"p\":"))) {
            int page = atoi(p + 5);
            char *rs = strstr(p, "\"r\":[");
            char *end = strstr(p, "]}");
            double rects[4 * 64];
            int nr = 0;
            for (char *q = rs + 5; q && q < end && nr < 64; ) {
                q = strchr(q, '[');
                if (!q || q > end) break;
                sscanf(q, "[%lf,%lf,%lf,%lf]", &rects[nr * 4], &rects[nr * 4 + 1], &rects[nr * 4 + 2], &rects[nr * 4 + 3]);
                nr++;
                q++;
            }
            nd_redact(h, page, rects, nr, 0);
            print_out();
            p = end;
        }
        free(js);
        nd_save(h, ND_SAVE_GC | ND_SAVE_COMPRESS);
        write_out(out);
        return 0;
    }
    if (!strcmp(cmd, "fill") && argc > 5) {
        if (nd_form_set(h, atoi(argv[4]), argv[5])) { fprintf(stderr, "%s\n", nd_error()); return 1; }
        nd_save(h, ND_SAVE_GC);
        write_out(out);
        return 0;
    }
    if (!strcmp(cmd, "number") && argc > 4) {
        nd_stamp(h, argv[4], 4, 10, 24, 0, -1, 1, "Helvetica", 0x333333);
        nd_save(h, ND_SAVE_GC | ND_SAVE_COMPRESS);
        write_out(out);
        return 0;
    }
    usage();
    return 2;
}
