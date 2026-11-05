#ifndef PN_CONTENT_H
#define PN_CONTENT_H
#include "pn.h"

typedef struct {
    char name[8];
    pn_obj **args;
    int nargs;
    unsigned char *inl; /* inline image data for BI */
    size_t inl_len;
} pn_op;

typedef struct {
    pn_op *ops;
    int n, cap;
} pn_content;

pn_content *pn_content_parse(const unsigned char *data, size_t len);
void pn_content_free(pn_content *c);
void pn_op_write(pn_buf *b, const pn_op *op);

/* ---------- fonts ---------- */
typedef struct { unsigned lo, hi; int nbytes; int nuni; unsigned uni[4]; } pn_tu_entry;
typedef struct { unsigned lo, hi; int nbytes; } pn_cs_range;
typedef struct { unsigned c0, c1; double w; } pn_cid_w;

typedef struct pn_font {
    pn_obj *dict;       /* identity key (borrowed) */
    int composite;      /* Type0 */
    int type3;
    int vertical;
    double widths[256]; /* simple fonts, text-space units * 1000 */
    unsigned uni[256];  /* simple fonts: code -> unicode */
    pn_cid_w *cidw; int ncidw; double dw;
    pn_tu_entry *tu; int ntu;
    pn_cs_range *cs; int ncs;
    double ascent, descent; /* /1000 */
    double t3scale;
    int base14;         /* index or -1 */
    struct pn_font *next;
} pn_font;

pn_font *pn_font_load(pn_doc *doc, pn_obj *fontdict, pn_font **cache);
void pn_font_cache_free(pn_font *cache);
/* Decode next code from string; returns bytes consumed (>=1). */
int pn_font_next_code(pn_font *f, const unsigned char *s, int len, unsigned *code);
double pn_font_width(pn_font *f, unsigned code);
int pn_font_unicode(pn_font *f, unsigned code, int nbytes, unsigned *out, int max);
int pn_base14_index(const char *basefont);
double pn_base14_text_width(int font, const unsigned char *winansi, int len); /* in 1/1000 em */
int pn_glyph_to_unicode(const char *name);

#endif
