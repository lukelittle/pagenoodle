/* Engine-level operations shared between modules and the API layer. */
#ifndef PN_ENGINE_H
#define PN_ENGINE_H
#include "pn.h"

typedef struct {
    int glyphs;          /* glyphs removed from content streams */
    int images_removed;  /* images dropped entirely */
    int images_scrubbed; /* images whose pixels were blanked */
    int forms;           /* form XObjects rewritten */
    int annots;          /* annotations / form widgets removed */
} pn_redact_stats;

/* pages.c */
pn_doc *pn_assemble(pn_doc **docs, const int *pages, const int *rot, int n);

/* text.c */
void pn_text_json(pn_doc *doc, int page, pn_buf *out);
void pn_text_plain(pn_doc *doc, int page, pn_buf *out);
int pn_search_page(pn_doc *doc, int page, const char *needle, int ci, pn_buf *out, int first);
int pn_redact_page_content(pn_doc *doc, int page, const pn_rect *rects, int n, pn_redact_stats *st, pn_buf *removed);
int pn_glyph_hit(pn_rect glyph, pn_rect r);
int pn_count_glyphs_in(pn_doc *doc, int page, const pn_rect *rects, int n, pn_buf *leaked);

/* redact.c */
int pn_redact_image(pn_doc *doc, pn_obj *img, pn_mat ctm, const pn_rect *rects, int n, int *newnum);
int pn_redact_page(pn_doc *doc, int page, const pn_rect *rects, int n, int fill_rgb, pn_redact_stats *st, pn_buf *removed);

/* scrub.c */
int pn_scrub_term(pn_doc *doc, const char *term);
int pn_scan_term(pn_doc *doc, const char *term, pn_buf *out);

/* overlay.c */
int pn_overlay_begin(pn_doc *doc, int page);
const char *pn_overlay_font(const char *basefont);
const char *pn_overlay_gstate(double fill_alpha, double stroke_alpha, const char *blend);
const char *pn_overlay_xobject(int objnum);
void pn_overlay_ops(const char *ops);
int pn_overlay_end(void);

/* image.c */
int pn_image_new(pn_doc *doc, const unsigned char *data, size_t len, int w, int h, int comps, int is_jpeg,
                 const unsigned char *alpha);
int pn_add_image_page(pn_doc *doc, int imgnum, int w, int h, double pw, double ph, double margin);
void pn_images_json(pn_doc *doc, pn_buf *out);
int pn_image_pixels(pn_doc *doc, int num, pn_buf *out, int *w, int *h, int *comps);
int pn_image_replace(pn_doc *doc, int num, const unsigned char *data, size_t len, int w, int h, int comps, int is_jpeg);

/* form.c */
void pn_form_json(pn_doc *doc, pn_buf *out);
int pn_form_set(pn_doc *doc, int fieldnum, const char *value);
int pn_form_flatten(pn_doc *doc);

/* stamp.c */
int pn_stamp_text(pn_doc *doc, const char *tmpl, int position, double size, double margin,
                  int from, int to, int start_number, const char *font, int rgb);
void pn_page_visual_frame(pn_doc *doc, pn_obj *page, double *vw, double *vh, pn_mat *to_user);

#endif
