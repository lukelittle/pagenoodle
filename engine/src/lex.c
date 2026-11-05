/* Tokenizer and object parser (shared by file parsing and content streams). */
#include "pn.h"

static pn_obj *parse_obj_tok_inner(pn_lexer *lx, int allow_refs);
#include <stdlib.h>
#include <string.h>
#include <math.h>

int pn_is_ws(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == 0; }
int pn_is_delim(int c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }

void pn_lex_init(pn_lexer *lx, const unsigned char *data, size_t len) {
    lx->p = lx->start = data; lx->end = data + len;
    lx->tok = TK_EOF; lx->ival = 0; lx->rval = 0; lx->hex = 0; lx->depth = 0;
    pn_buf_init(&lx->sbuf);
}
void pn_lex_free(pn_lexer *lx) { pn_buf_free(&lx->sbuf); }

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void skip_ws(pn_lexer *lx) {
    while (lx->p < lx->end) {
        int c = *lx->p;
        if (pn_is_ws(c)) lx->p++;
        else if (c == '%') { while (lx->p < lx->end && *lx->p != '\n' && *lx->p != '\r') lx->p++; }
        else break;
    }
}

pn_tok pn_lex_next(pn_lexer *lx) {
    skip_ws(lx);
    lx->sbuf.len = 0;
    if (lx->sbuf.data) lx->sbuf.data[0] = 0;
    if (lx->p >= lx->end) return lx->tok = TK_EOF;
    int c = *lx->p;
    if (c == '[') { lx->p++; return lx->tok = TK_ARR_OPEN; }
    if (c == ']') { lx->p++; return lx->tok = TK_ARR_CLOSE; }
    if (c == '{') { lx->p++; return lx->tok = TK_BRACE_OPEN; }
    if (c == '}') { lx->p++; return lx->tok = TK_BRACE_CLOSE; }
    if (c == '<') {
        if (lx->p + 1 < lx->end && lx->p[1] == '<') { lx->p += 2; return lx->tok = TK_DICT_OPEN; }
        lx->p++;
        int hi = -1;
        while (lx->p < lx->end && *lx->p != '>') {
            int v = hexval(*lx->p++);
            if (v < 0) continue;
            if (hi < 0) hi = v; else { pn_buf_putc(&lx->sbuf, hi * 16 + v); hi = -1; }
        }
        if (hi >= 0) pn_buf_putc(&lx->sbuf, hi * 16);
        if (lx->p < lx->end) lx->p++;
        lx->hex = 1;
        return lx->tok = TK_STRING;
    }
    if (c == '>') {
        if (lx->p + 1 < lx->end && lx->p[1] == '>') { lx->p += 2; return lx->tok = TK_DICT_CLOSE; }
        lx->p++;
        return lx->tok = TK_ERR;
    }
    if (c == '(') {
        int depth = 1;
        lx->p++;
        lx->hex = 0;
        while (lx->p < lx->end) {
            c = *lx->p++;
            if (c == '(') { depth++; pn_buf_putc(&lx->sbuf, c); }
            else if (c == ')') { if (--depth == 0) break; pn_buf_putc(&lx->sbuf, c); }
            else if (c == '\\') {
                if (lx->p >= lx->end) break;
                c = *lx->p++;
                switch (c) {
                case 'n': pn_buf_putc(&lx->sbuf, '\n'); break;
                case 'r': pn_buf_putc(&lx->sbuf, '\r'); break;
                case 't': pn_buf_putc(&lx->sbuf, '\t'); break;
                case 'b': pn_buf_putc(&lx->sbuf, '\b'); break;
                case 'f': pn_buf_putc(&lx->sbuf, '\f'); break;
                case '\r': if (lx->p < lx->end && *lx->p == '\n') lx->p++; break;
                case '\n': break;
                default:
                    if (c >= '0' && c <= '7') {
                        int v = c - '0';
                        for (int k = 0; k < 2 && lx->p < lx->end && *lx->p >= '0' && *lx->p <= '7'; k++) v = v * 8 + (*lx->p++ - '0');
                        pn_buf_putc(&lx->sbuf, v & 255);
                    } else pn_buf_putc(&lx->sbuf, c);
                }
            } else if (c == '\r') {
                /* normalize EOL to \n */
                if (lx->p < lx->end && *lx->p == '\n') lx->p++;
                pn_buf_putc(&lx->sbuf, '\n');
            } else pn_buf_putc(&lx->sbuf, c);
        }
        return lx->tok = TK_STRING;
    }
    if (c == '/') {
        lx->p++;
        while (lx->p < lx->end && !pn_is_ws(*lx->p) && !pn_is_delim(*lx->p)) {
            c = *lx->p++;
            if (c == '#' && lx->p + 1 < lx->end && hexval(lx->p[0]) >= 0 && hexval(lx->p[1]) >= 0) {
                c = hexval(lx->p[0]) * 16 + hexval(lx->p[1]);
                lx->p += 2;
            }
            pn_buf_putc(&lx->sbuf, c);
        }
        if (!lx->sbuf.data) pn_buf_reserve(&lx->sbuf, 1), lx->sbuf.data[0] = 0;
        return lx->tok = TK_NAME;
    }
    /* number or keyword */
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
        const unsigned char *s = lx->p;
        int neg = 0, seen_dot = 0, digits = 0;
        long long iv = 0;
        double frac = 0, scale = 1;
        if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
        while (s < lx->end && *s == '-') s++; /* tolerate "--5" */
        while (s < lx->end) {
            c = *s;
            if (c >= '0' && c <= '9') {
                digits++;
                if (seen_dot) { scale /= 10; frac += (c - '0') * scale; }
                else if (iv < 100000000000000000LL) iv = iv * 10 + (c - '0');
            } else if (c == '.' && !seen_dot) seen_dot = 1;
            else break;
            s++;
        }
        if (digits || seen_dot) {
            lx->p = s;
            /* consume junk glued to number e.g. "12abc" as part (treat as number anyway) */
            if (seen_dot) { lx->rval = (neg ? -1 : 1) * ((double)iv + frac); return lx->tok = TK_REAL; }
            lx->ival = neg ? -iv : iv;
            return lx->tok = TK_INT;
        }
    }
    while (lx->p < lx->end && !pn_is_ws(*lx->p) && !pn_is_delim(*lx->p)) pn_buf_putc(&lx->sbuf, *lx->p++);
    if (lx->sbuf.len == 0) { lx->p++; return lx->tok = TK_ERR; }
    return lx->tok = TK_KEYWORD;
}

static int kw_is(pn_lexer *lx, const char *k) { return lx->tok == TK_KEYWORD && !strcmp((const char *)lx->sbuf.data, k); }

static pn_obj *parse_obj_tok_inner(pn_lexer *lx, int allow_refs) {
    switch (lx->tok) {
    case TK_INT: {
        long long v = lx->ival;
        if (allow_refs && v > 0 && v <= 0x7fffffff) {
            const unsigned char *save = lx->p;
            pn_tok t2 = pn_lex_next(lx);
            if (t2 == TK_INT && lx->ival >= 0 && lx->ival <= 65535) {
                long long gen = lx->ival;
                pn_lex_next(lx);
                if (kw_is(lx, "R")) return pn_new_ref((int)v, (int)gen);
            }
            lx->p = save;
            lx->tok = TK_INT; lx->ival = v;
        }
        return pn_new_int(v);
    }
    case TK_REAL: return pn_new_real(lx->rval);
    case TK_STRING: { pn_obj *s = pn_new_string(lx->sbuf.data ? lx->sbuf.data : (unsigned char *)"", (int)lx->sbuf.len); s->u.str.hex = lx->hex; return s; }
    case TK_NAME: return pn_new_name(lx->sbuf.data ? (const char *)lx->sbuf.data : "");
    case TK_ARR_OPEN: {
        pn_obj *a = pn_new_array();
        for (;;) {
            pn_tok t = pn_lex_next(lx);
            if (t == TK_ARR_CLOSE || t == TK_EOF) break;
            if (t == TK_ERR) continue;
            if (t == TK_KEYWORD && (kw_is(lx, "endobj") || kw_is(lx, "stream"))) break;
            pn_obj *v = pn_parse_obj_tok(lx, allow_refs);
            if (v) pn_arr_push(a, v);
        }
        return a;
    }
    case TK_DICT_OPEN: {
        pn_obj *d = pn_new_dict();
        for (;;) {
            pn_tok t = pn_lex_next(lx);
            if (t == TK_DICT_CLOSE || t == TK_EOF) break;
            if (t == TK_KEYWORD && (kw_is(lx, "endobj") || kw_is(lx, "stream"))) break;
            if (t != TK_NAME) continue;
            char *key = pn_strdup((const char *)lx->sbuf.data);
            t = pn_lex_next(lx);
            if (t == TK_DICT_CLOSE) { free(key); break; }
            pn_obj *v = pn_parse_obj_tok(lx, allow_refs);
            if (v && !(v->type == PN_NULL)) pn_dict_put(d, key, v); else pn_drop(v);
            free(key);
        }
        return d;
    }
    case TK_BRACE_OPEN: { /* PostScript calculator functions: skip balanced braces, keep as keyword */
        int depth = 1;
        while (depth > 0) {
            pn_tok t = pn_lex_next(lx);
            if (t == TK_EOF) break;
            if (t == TK_BRACE_OPEN) depth++;
            else if (t == TK_BRACE_CLOSE) depth--;
        }
        return pn_null();
    }
    case TK_KEYWORD:
        if (kw_is(lx, "true")) return pn_new_bool(1);
        if (kw_is(lx, "false")) return pn_new_bool(0);
        if (kw_is(lx, "null")) return pn_null();
        return NULL;
    default: return NULL;
    }
}

/* Nesting is capped so crafted input cannot exhaust the (small, WASM) stack; every other recursion
 * over object structure (copy, write, free) is bounded by this too. */
pn_obj *pn_parse_obj_tok(pn_lexer *lx, int allow_refs) {
    if (lx->tok == TK_ARR_OPEN || lx->tok == TK_DICT_OPEN) {
        if (lx->depth >= PN_MAX_NESTING) {
            int lvl = 1;
            while (lvl > 0) {
                pn_tok t = pn_lex_next(lx);
                if (t == TK_EOF) break;
                if (t == TK_ARR_OPEN || t == TK_DICT_OPEN) lvl++;
                else if (t == TK_ARR_CLOSE || t == TK_DICT_CLOSE) lvl--;
            }
            return pn_null();
        }
        lx->depth++;
        pn_obj *r = parse_obj_tok_inner(lx, allow_refs);
        lx->depth--;
        return r;
    }
    return parse_obj_tok_inner(lx, allow_refs);
}

pn_obj *pn_parse_obj(pn_lexer *lx, int allow_refs) {
    pn_lex_next(lx);
    return pn_parse_obj_tok(lx, allow_refs);
}
