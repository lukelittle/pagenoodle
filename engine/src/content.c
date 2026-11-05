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
                const unsigned char *s = p, *e = NULL;
                for (const unsigned char *q = p; q + 1 < lx.end; q++) {
                    if (q[0] == 'E' && q[1] == 'I' && q > s && pn_is_ws(q[-1]) && (q + 2 >= lx.end || pn_is_ws(q[2]) || pn_is_delim(q[2]))) { e = q; break; }
                }
                if (!e) e = lx.end;
                size_t dl = (size_t)(e - s);
                if (dl > 0 && pn_is_ws(s[dl - 1])) dl--;
                for (int i = 0; i < nargs; i++) pn_drop(args[i]);
                nargs = 0;
                pn_obj *a1[1] = {dict};
                push_op(c, "BI", a1, 1);
                pn_op *op = &c->ops[c->n - 1];
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
