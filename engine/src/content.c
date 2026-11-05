/* Content stream tokenizer: splits a content stream into operators with operands. */
#include "pn.h"
#include "pn_content.h"
#include <stdlib.h>
#include <string.h>

static void push_op(pn_content *c, const char *name, pn_obj **args, int nargs) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->ops = (pn_op *)realloc(c->ops, sizeof(pn_op) * (size_t)c->cap);
    }
    pn_op *op = &c->ops[c->n++];
    memset(op, 0, sizeof *op);
    strncpy(op->name, name, sizeof op->name - 1);
    op->nargs = nargs;
    if (nargs) {
        op->args = (pn_obj **)malloc(sizeof(pn_obj *) * (size_t)nargs);
        memcpy(op->args, args, sizeof(pn_obj *) * (size_t)nargs);
    }
}

static int is_known_op(const char *k) {
    static const char *ops[] = {"b", "B", "b*", "B*", "BDC", "BI", "BMC", "BT", "BX", "c", "cm", "CS", "cs", "d", "d0", "d1", "Do", "DP", "EI", "EMC", "ET", "EX", "f", "F", "f*", "G", "g", "gs", "h", "i", "ID", "j", "J", "K", "k", "l", "m", "M", "MP", "n", "q", "Q", "re", "RG", "rg", "ri", "s", "S", "SC", "sc", "SCN", "scn", "sh", "T*", "Tc", "Td", "TD", "Tf", "Tj", "TJ", "TL", "Tm", "Tr", "Ts", "Tw", "Tz", "v", "w", "W", "W*", "y", "'", "\"", NULL};
    for (int i = 0; ops[i]; i++) if (!strcmp(ops[i], k)) return 1;
    return 0;
}

/* Approximation of PDF.js's findDefaultInlineStreamEnd: the first "EI" followed by space/LF/CR
 * whose next 15 bytes are printable and whose next tokens begin with a known operator. Returns a
 * pointer to the 'E' (or end of data). */
static const unsigned char *find_pdfjs_ei(const unsigned char *s, const unsigned char *end) {
    for (const unsigned char *q = s; q + 2 <= end; q++) {
        if (q[0] != 'E' || q[1] != 'I') continue;
        if (q + 2 == end) return q;
        if (q[2] != ' ' && q[2] != '\n' && q[2] != '\r') continue;
        const unsigned char *f = q + 3;
        size_t n = (size_t)(end - f) < 15 ? (size_t)(end - f) : 15;
        if (n == 0) return q;
        int printable = 1;
        for (size_t i = 0; i < n; i++) {
            unsigned c = f[i];
            if (c == 0 && i + 1 < n && f[i + 1] != 0) continue;
            if (c != '\n' && c != '\r' && (c < 0x20 || c > 0x7f)) { printable = 0; break; }
        }
        if (!printable) continue;
        pn_lexer lx;
        pn_lex_init(&lx, f, (size_t)(end - f) < 75 ? (size_t)(end - f) : 75);
        int ok = 0;
        for (int k = 0; k < 16; k++) {
            pn_tok t = pn_lex_next(&lx);
            if (t == TK_EOF || t == TK_ERR) break;
            if (t == TK_KEYWORD) {
                const char *kw = (const char *)lx.sbuf.data;
                if (!strcmp(kw, "true") || !strcmp(kw, "false") || !strcmp(kw, "null")) continue; /* operands */
                ok = is_known_op(kw);
                break;
            }
            if (t == TK_ARR_OPEN || t == TK_DICT_OPEN) { pn_obj *v = pn_parse_obj_tok(&lx, 0); pn_drop(v); }
        }
        pn_lex_free(&lx);
        if (ok) return q;
    }
    return end;
}

/* PDF.js uses format-specific end finders for these filters (the data itself says where it ends). */
static const unsigned char *find_filtered_end(pn_obj *dict, const unsigned char *s, const unsigned char *end) {
    pn_obj *f = pn_dict_get(dict, "F");
    if (!f) f = pn_dict_get(dict, "Filter");
    if (pn_is(f, PN_ARRAY)) f = pn_arr_get(f, 0);
    const char *n = pn_name(f);
    const unsigned char *q = NULL;
    if (!strcmp(n, "A85") || !strcmp(n, "ASCII85Decode")) {
        for (const unsigned char *t = s; t + 1 < end; t++) if (t[0] == '~' && t[1] == '>') { q = t + 2; break; }
    } else if (!strcmp(n, "AHx") || !strcmp(n, "ASCIIHexDecode")) {
        for (const unsigned char *t = s; t < end; t++) if (*t == '>') { q = t + 1; break; }
    } else if (!strcmp(n, "DCT") || !strcmp(n, "DCTDecode")) {
        for (const unsigned char *t = s; t + 1 < end; t++) if (t[0] == 0xFF && t[1] == 0xD9) { q = t + 2; break; }
    } else return NULL;
    if (!q) return end;
    while (q < end && pn_is_ws(*q)) q++;
    return q; /* should now point at "EI" */
}

static pn_obj *ii(pn_obj *d, const char *a, const char *b) { pn_obj *v = pn_dict_get(d, a); return v ? v : pn_dict_get(d, b); }

/* Byte length of an inline image's data, or -1 when it cannot be computed (filtered, or a colour
 * space given by resource name). */
static long long inline_image_size(pn_obj *d) {
    pn_obj *l = pn_dict_get(d, "L");
    if (pn_is(l, PN_INT) && l->u.i >= 0) return l->u.i;
    if (ii(d, "F", "Filter")) return -1;
    long long w = pn_int(ii(d, "W", "Width"), -1), h = pn_int(ii(d, "H", "Height"), -1);
    pn_obj *im = ii(d, "IM", "ImageMask");
    int mask = pn_is(im, PN_BOOL) && im->u.b;
    long long bpc = mask ? 1 : pn_int(ii(d, "BPC", "BitsPerComponent"), -1);
    int comps = 1;
    if (!mask) {
        pn_obj *cs = ii(d, "CS", "ColorSpace");
        const char *n = pn_name(cs);
        if (!strcmp(n, "G") || !strcmp(n, "DeviceGray")) comps = 1;
        else if (!strcmp(n, "RGB") || !strcmp(n, "DeviceRGB")) comps = 3;
        else if (!strcmp(n, "CMYK") || !strcmp(n, "DeviceCMYK")) comps = 4;
        else if (pn_is(cs, PN_ARRAY) && (pn_is_name(pn_arr_get(cs, 0), "I") || pn_is_name(pn_arr_get(cs, 0), "Indexed"))) comps = 1;
        else return -1;
    }
    if (w <= 0 || h <= 0 || w > 65536 || h > 65536 || (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16)) return -1;
    return ((w * comps * bpc + 7) / 8) * h;
}

pn_content *pn_content_parse(const unsigned char *data, size_t len) {
    pn_content *c = (pn_content *)calloc(1, sizeof *c);
    pn_lexer lx;
    pn_lex_init(&lx, data, len);
    pn_obj *args[64];
    int nargs = 0;
    for (;;) {
        pn_tok t = pn_lex_next(&lx);
        if (t == TK_EOF) break;
        if (t == TK_ERR || t == TK_ARR_CLOSE || t == TK_DICT_CLOSE || t == TK_BRACE_CLOSE) continue;
        if (t == TK_KEYWORD) {
            const char *kw = (const char *)lx.sbuf.data;
            if (!strcmp(kw, "true") || !strcmp(kw, "false") || !strcmp(kw, "null")) {
                pn_obj *v = pn_parse_obj_tok(&lx, 0);
                if (nargs < 64) args[nargs++] = v; else pn_drop(v);
                continue;
            }
            if (!strcmp(kw, "BI")) {
                /* inline image: key/value pairs until ID */
                pn_obj *dict = pn_new_dict();
                for (;;) {
                    pn_tok t2 = pn_lex_next(&lx);
                    if (t2 == TK_EOF) break;
                    if (t2 == TK_KEYWORD && !strcmp((const char *)lx.sbuf.data, "ID")) break;
                    if (t2 != TK_NAME) continue;
                    char key[64];
                    strncpy(key, (const char *)lx.sbuf.data, sizeof key - 1); key[63] = 0;
                    pn_obj *v = pn_parse_obj(&lx, 0);
                    if (v) pn_dict_put(dict, key, v);
                }
                const unsigned char *p = lx.p;
                if (p < lx.end && pn_is_ws(*p)) p++;
                const unsigned char *s = p;
                /* Where does the image data end?
                 *  O: our answer (exact size for unfiltered images, else the first plausible EI)
                 *  P: what PDF.js would pick (first EI + space/LF/CR whose following bytes are
                 *     printable and start with a known operator)
                 * If they disagree the image is ambiguous: we take the LATER end, so that both
                 * readings of the bytes in between are treated as image data, and redaction drops
                 * the whole thing (fail closed). */
                const unsigned char *ff = find_filtered_end(dict, s, lx.end);
                const unsigned char *eo = NULL, *ep = ff ? ff : find_pdfjs_ei(s, lx.end);
                long long explicit_len = inline_image_size(dict);
                if (explicit_len >= 0 && explicit_len <= (long long)(lx.end - s)) {
                    const unsigned char *q = s + explicit_len;
                    while (q < lx.end && pn_is_ws(*q)) q++;
                    if (q + 1 < lx.end && q[0] == 'E' && q[1] == 'I' && (q + 2 >= lx.end || pn_is_ws(q[2]))) eo = q;
                }
                if (!eo) eo = ep;
                const unsigned char *e = eo > ep ? eo : ep;
                int ambiguous = eo != ep;
                /* any other EI + whitespace inside (unfiltered) data could also be an end for some reader */
                for (const unsigned char *q = s; !ff && !ambiguous && q + 2 < e; q++)
                    if (q[0] == 'E' && q[1] == 'I' && (q[2] == ' ' || q[2] == '\n' || q[2] == '\r')) ambiguous = 1;
                size_t dl = (size_t)(e - s);
                if (!ambiguous && explicit_len >= 0 && (size_t)explicit_len <= dl) dl = (size_t)explicit_len;
                else while (dl > 0 && pn_is_ws(s[dl - 1])) dl--;
                for (int i = 0; i < nargs; i++) pn_drop(args[i]);
                nargs = 0;
                pn_obj *a1[1] = {dict};
                push_op(c, "BI", a1, 1);
                pn_op *op = &c->ops[c->n - 1];
                op->ambiguous = ambiguous;
                op->inl = (unsigned char *)malloc(dl + 1);
                memcpy(op->inl, s, dl);
                op->inl_len = dl;
                lx.p = e + 2 <= lx.end ? e + 2 : lx.end;
                continue;
            }
            push_op(c, kw, args, nargs);
            nargs = 0;
            continue;
        }
        pn_obj *v = pn_parse_obj_tok(&lx, 0);
        if (!v) continue;
        if (nargs < 64) args[nargs++] = v; else pn_drop(v);
    }
    for (int i = 0; i < nargs; i++) pn_drop(args[i]);
    pn_lex_free(&lx);
    return c;
}

void pn_content_free(pn_content *c) {
    if (!c) return;
    for (int i = 0; i < c->n; i++) {
        for (int k = 0; k < c->ops[i].nargs; k++) pn_drop(c->ops[i].args[k]);
        free(c->ops[i].args);
        free(c->ops[i].inl);
    }
    free(c->ops);
    free(c);
}

void pn_op_write(pn_buf *b, const pn_op *op) {
    if (!strcmp(op->name, "BI")) {
        pn_obj *d = op->nargs ? op->args[0] : NULL;
        pn_buf_puts(b, "BI");
        for (int i = 0; i < pn_dict_len(d); i++) {
            pn_buf_putc(b, ' ');
            pn_obj *nm = pn_new_name(pn_dict_key(d, i));
            pn_write_obj(b, nm);
            pn_drop(nm);
            pn_buf_putc(b, ' ');
            pn_write_obj(b, pn_dict_val(d, i));
        }
        pn_buf_puts(b, " ID ");
        pn_buf_put(b, op->inl, op->inl_len);
        pn_buf_puts(b, "\nEI\n");
        return;
    }
    for (int i = 0; i < op->nargs; i++) { pn_write_obj(b, op->args[i]); pn_buf_putc(b, ' '); }
    pn_buf_puts(b, op->name);
    pn_buf_putc(b, '\n');
}
