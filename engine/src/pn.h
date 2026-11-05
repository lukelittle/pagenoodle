/*
 * Pagenoodle engine — internal header.
 * A small, dependency-light PDF object model, parser, writer and page toolkit in C99.
 */
#ifndef PN_H
#define PN_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Growable byte buffer                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned char *data;
    size_t len, cap;
} pn_buf;

void pn_buf_init(pn_buf *b);
void pn_buf_free(pn_buf *b);
void pn_buf_reserve(pn_buf *b, size_t extra);
void pn_buf_put(pn_buf *b, const void *data, size_t n);
void pn_buf_putc(pn_buf *b, int c);
void pn_buf_puts(pn_buf *b, const char *s);
void pn_buf_printf(pn_buf *b, const char *fmt, ...);
void pn_buf_put_real(pn_buf *b, double v);
unsigned char *pn_buf_steal(pn_buf *b, size_t *len);

/* ------------------------------------------------------------------ */
/* Objects                                                             */
/* ------------------------------------------------------------------ */
typedef enum {
    PN_NULL = 0, PN_BOOL, PN_INT, PN_REAL, PN_STRING, PN_NAME,
    PN_ARRAY, PN_DICT, PN_REF, PN_STREAM
} pn_type;

typedef struct pn_obj pn_obj;
struct pn_obj {
    pn_type type;
    int refs;
    union {
        int b;
        long long i;
        double r;
        struct { unsigned char *s; int len; int hex; } str; /* STRING, NAME (NUL terminated) */
        struct { pn_obj **items; int len, cap; } arr;
        struct { char **keys; pn_obj **vals; int len, cap; } dict;
        struct { int num, gen; } ref;
        struct { pn_obj *dict; unsigned char *data; size_t len; } stm; /* data encoded per dict /Filter */
    } u;
};

pn_obj *pn_null(void);
pn_obj *pn_new_bool(int b);
pn_obj *pn_new_int(long long i);
pn_obj *pn_new_real(double r);
pn_obj *pn_new_string(const void *s, int len);
pn_obj *pn_new_name(const char *s);
pn_obj *pn_new_array(void);
pn_obj *pn_new_dict(void);
pn_obj *pn_new_ref(int num, int gen);
pn_obj *pn_new_stream(pn_obj *dict, unsigned char *data, size_t len); /* takes ownership of both */

pn_obj *pn_keep(pn_obj *o);
void pn_drop(pn_obj *o);

int pn_is(const pn_obj *o, pn_type t);
int pn_is_name(const pn_obj *o, const char *name);
int pn_is_number(const pn_obj *o);
double pn_num(const pn_obj *o, double def);
long long pn_int(const pn_obj *o, long long def);
const char *pn_name(const pn_obj *o); /* "" if not a name */

int pn_arr_len(const pn_obj *a);
pn_obj *pn_arr_get(const pn_obj *a, int i);
void pn_arr_push(pn_obj *a, pn_obj *v);     /* takes ownership */
void pn_arr_insert(pn_obj *a, int i, pn_obj *v);
void pn_arr_remove(pn_obj *a, int i);
void pn_arr_set(pn_obj *a, int i, pn_obj *v);

pn_obj *pn_dict_get(const pn_obj *d, const char *key); /* works for streams too */
void pn_dict_put(pn_obj *d, const char *key, pn_obj *v); /* takes ownership; NULL removes */
void pn_dict_del(pn_obj *d, const char *key);
int pn_dict_len(const pn_obj *d);
const char *pn_dict_key(const pn_obj *d, int i);
pn_obj *pn_dict_val(const pn_obj *d, int i);

pn_obj *pn_shallow_copy(pn_obj *o);
pn_obj *pn_deep_copy_direct(pn_obj *o); /* copies direct structure; refs stay refs */

/* ------------------------------------------------------------------ */
/* Lexer / parser                                                      */
/* ------------------------------------------------------------------ */
#define PN_MAX_NESTING 64
#define PN_MAX_STREAM ((size_t)256 << 20) /* decompressed bytes per stream */
#define PN_MAX_IMAGE_DIM 65536
#define PN_MAX_CIDW 65536

typedef enum {
    TK_EOF, TK_ERR, TK_INT, TK_REAL, TK_STRING, TK_NAME, TK_KEYWORD,
    TK_ARR_OPEN, TK_ARR_CLOSE, TK_DICT_OPEN, TK_DICT_CLOSE, TK_BRACE_OPEN, TK_BRACE_CLOSE
} pn_tok;

typedef struct {
    const unsigned char *p, *end, *start;
    pn_tok tok;
    long long ival;
    double rval;
    pn_buf sbuf; /* string / name / keyword text */
    int hex;
    int depth;   /* nesting depth guard against stack exhaustion */
} pn_lexer;

void pn_lex_init(pn_lexer *lx, const unsigned char *data, size_t len);
void pn_lex_free(pn_lexer *lx);
pn_tok pn_lex_next(pn_lexer *lx);
int pn_is_ws(int c);
int pn_is_delim(int c);
/* Parse one object; refs ("1 0 R") are recognized when allow_refs. Returns NULL on error/EOF. */
pn_obj *pn_parse_obj(pn_lexer *lx, int allow_refs);
pn_obj *pn_parse_obj_tok(pn_lexer *lx, int allow_refs); /* uses current token */

/* ------------------------------------------------------------------ */
/* Document                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    pn_obj *obj;       /* loaded object (or NULL) */
    long long ofs;     /* file offset for type 1 */
    int gen;
    int stm, idx;      /* for type 2 (in object stream) */
    unsigned char type; /* 0 free, 1 in file, 2 in objstm, 3 in memory */
    unsigned char loading;
} pn_xref;

typedef struct pn_doc {
    unsigned char *buf; size_t len; /* original file (owned) */
    pn_xref *xref; int nxref, capxref;
    pn_obj *trailer;
    int *pages; int npages; /* object numbers of page dicts, in order */
    int pages_dirty;
    int encrypted;
    int repaired;
    unsigned char *stm_loaded; /* object streams already decoded (each is decoded at most once) */
    pn_obj **retired; int nretired, capretired; /* replaced objects, freed only with the document */
    int stm_loaded_n;
} pn_doc;

pn_doc *pn_doc_new(void);                 /* empty document with catalog + page tree */
pn_doc *pn_doc_open(const unsigned char *data, size_t len, char *err, int errlen); /* copies data */
void pn_doc_free(pn_doc *doc);

pn_obj *pn_resolve(pn_doc *doc, pn_obj *o);        /* follow refs (borrowed) */
pn_obj *pn_load(pn_doc *doc, int num);              /* borrowed */
pn_obj *pn_get(pn_doc *doc, pn_obj *d, const char *key); /* dict get + resolve (borrowed) */
int pn_add_obj(pn_doc *doc, pn_obj *o);             /* takes ownership, returns obj num */
void pn_set_obj(pn_doc *doc, int num, pn_obj *o);   /* takes ownership; old object is retired, not freed */
void pn_retire(pn_doc *doc, pn_obj *o);             /* keep o alive until the document is freed (takes a reference) */
pn_obj *pn_add_ref(pn_doc *doc, pn_obj *o);         /* add and return new ref (owned) */
pn_obj *pn_catalog(pn_doc *doc);
int pn_is_annotation(pn_doc *doc, pn_obj *o); /* a real annotation dict (not a page, catalog, …) */

int pn_doc_page_count(pn_doc *doc);
pn_obj *pn_doc_page(pn_doc *doc, int idx);          /* page dict (borrowed) */
int pn_doc_page_num(pn_doc *doc, int idx);
pn_obj *pn_page_inherited(pn_doc *doc, pn_obj *page, const char *key); /* borrowed, resolved */
pn_obj *pn_page_inherited_raw(pn_doc *doc, pn_obj *page, const char *key); /* borrowed, unresolved */
pn_obj *pn_page_resources(pn_doc *doc, pn_obj *page); /* merged like PDF.js; owned */
void pn_page_box(pn_doc *doc, pn_obj *page, const char *which, double box[4]);
int pn_page_rotation(pn_doc *doc, pn_obj *page);
void pn_rebuild_page_tree(pn_doc *doc, const int *page_nums, int n); /* flat /Pages */

/* Streams */
unsigned char *pn_stream_decode(pn_doc *doc, pn_obj *stm, size_t *len, int *ok); /* malloc'd */
void pn_stream_set(pn_obj *stm, const unsigned char *data, size_t len, int compress); /* replaces data, clears filters */
unsigned char *pn_page_content(pn_doc *doc, pn_obj *page, size_t *len); /* concatenated decoded */
void pn_page_set_content(pn_doc *doc, pn_obj *page, const unsigned char *data, size_t len);
pn_obj *pn_page_private_resources(pn_doc *doc, pn_obj *page); /* ensure page-local resources dict */
pn_obj *pn_res_subdict(pn_doc *doc, pn_obj *res, const char *cat); /* resolves/creates private subdict */
void pn_res_unique_name(pn_doc *doc, pn_obj *subdict, const char *prefix, char *out, int outlen);

/* Filters */
unsigned char *pn_inflate(const unsigned char *in, size_t inlen, size_t *outlen);
unsigned char *pn_deflate(const unsigned char *in, size_t inlen, size_t *outlen, int level);
unsigned char *pn_decode_filter(const char *name, pn_obj *parms, const unsigned char *in, size_t inlen, size_t *outlen, int *ok);

/* Writer */
#define PN_SAVE_GC        1   /* drop unreachable objects, renumber */
#define PN_SAVE_COMPRESS  2   /* flate-compress uncompressed streams */
#define PN_SAVE_OBJSTM    4   /* pack objects in object streams + xref stream */
#define PN_SAVE_DEDUP     8   /* merge identical streams/objects */
#define PN_SAVE_STRIPMETA 16  /* remove /Info, XMP, PieceInfo, thumbnails */
#define PN_SAVE_RECOMPRESS 32 /* re-deflate existing flate streams at max level */
unsigned char *pn_doc_save(pn_doc *doc, int flags, size_t *outlen);
void pn_write_obj(pn_buf *b, pn_obj *o);

/* Copy objects between docs */
typedef struct pn_copymap pn_copymap;
pn_copymap *pn_copymap_new(pn_doc *src, pn_doc *dst);
void pn_copymap_free(pn_copymap *m);
void pn_copymap_allow_page(pn_copymap *m, int src_num);
pn_obj *pn_copy_into(pn_copymap *m, pn_obj *o); /* returns owned obj valid in dst */
int pn_copymap_lookup(pn_copymap *m, int src_num);

/* JSON helper */
void pn_json_str(pn_buf *b, const char *s, int len);

/* Misc */
char *pn_strdup(const char *s);
void pn_set_error(const char *fmt, ...);
const char *pn_get_error(void);
void pn_progress(int done, int total);
int pn_utf8_to_pdfstring(const char *utf8, pn_buf *out); /* PDFDoc if possible, else UTF-16BE w/BOM */
int pn_pdfstring_to_utf8(const unsigned char *s, int len, pn_buf *out);
int pn_utf8_decode(const char **p);
int pn_unicode_to_winansi(int u); /* -1 if unmappable */

/* Matrices */
typedef struct { double a, b, c, d, e, f; } pn_mat;
pn_mat pn_mat_mul(pn_mat m1, pn_mat m2); /* m1 then m2 */
pn_mat pn_mat_identity(void);
pn_mat pn_mat_invert(pn_mat m);
void pn_mat_apply(pn_mat m, double x, double y, double *ox, double *oy);
pn_mat pn_mat_from_array(pn_doc *doc, pn_obj *arr);

typedef struct { double x0, y0, x1, y1; } pn_rect;
pn_rect pn_rect_transform(pn_rect r, pn_mat m);
int pn_rect_intersects(pn_rect a, pn_rect b);
int pn_rect_contains(pn_rect outer, pn_rect inner);

#endif
