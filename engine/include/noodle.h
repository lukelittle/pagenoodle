/*
 * Pagenoodle engine public API (exported to WebAssembly as nd_*).
 * All document access is through integer handles. Functions that produce data
 * write into a single output buffer read with nd_out_ptr()/nd_out_len().
 */
#ifndef NOODLE_H
#define NOODLE_H

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define NOODLE_API EMSCRIPTEN_KEEPALIVE
#else
#define NOODLE_API
#endif

/* save flags */
#define ND_SAVE_GC 1
#define ND_SAVE_COMPRESS 2
#define ND_SAVE_OBJSTM 4
#define ND_SAVE_DEDUP 8
#define ND_SAVE_STRIPMETA 16
#define ND_SAVE_RECOMPRESS 32

NOODLE_API const char *nd_error(void);
NOODLE_API unsigned char *nd_out_ptr(void);
NOODLE_API int nd_out_len(void);
NOODLE_API void nd_out_free(void);
NOODLE_API int nd_last_w(void);
NOODLE_API int nd_last_h(void);
NOODLE_API void *nd_malloc(int n);
NOODLE_API void nd_free(void *p);

NOODLE_API int nd_open(const unsigned char *data, int len);
NOODLE_API int nd_new(void);
NOODLE_API void nd_close(int h);
NOODLE_API int nd_page_count(int h);
NOODLE_API int nd_info(int h);
NOODLE_API int nd_save(int h, int flags);
NOODLE_API int nd_assemble(const int *handles, const int *pages, const int *rots, int n);

NOODLE_API int nd_ov_begin(int h, int page);
NOODLE_API const char *nd_ov_font(const char *base);
NOODLE_API const char *nd_ov_gstate(double ca, double CA, const char *bm);
NOODLE_API const char *nd_ov_xobject(int num);
NOODLE_API void nd_ov_ops(const char *ops);
NOODLE_API int nd_ov_end(void);

NOODLE_API int nd_image_new(int h, const unsigned char *data, int len, int w, int ih, int comps, int is_jpeg, const unsigned char *alpha);
NOODLE_API int nd_add_image_page(int h, int img, double pw, double ph, double margin);
NOODLE_API int nd_images(int h);
NOODLE_API int nd_image_pixels(int h, int num);
NOODLE_API int nd_image_replace(int h, int num, const unsigned char *data, int len, int w, int ih, int comps, int is_jpeg);

NOODLE_API int nd_text(int h, int page);
NOODLE_API int nd_text_plain(int h, int page);
NOODLE_API int nd_search(int h, const char *needle, int ci);
NOODLE_API int nd_redact(int h, int page, const double *rects, int n, int fill_rgb);
NOODLE_API void nd_redact_mode(int drop_undecodable);
NOODLE_API int nd_stream_raw(int h, int num);
NOODLE_API int nd_scrub_term(int h, const char *term);
NOODLE_API int nd_scan_term(int h, const char *term);
NOODLE_API int nd_verify(int h, int page, const double *rects, int n);

NOODLE_API int nd_forms(int h);
NOODLE_API int nd_form_set(int h, int id, const char *value);
NOODLE_API int nd_form_flatten(int h);

NOODLE_API int nd_stamp(int h, const char *tmpl, int pos, double size, double margin, int from, int to, int start, const char *font, int rgb);
NOODLE_API const char *nd_version(void);

#endif
