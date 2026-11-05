/* Stream filters: Flate (via miniz), predictors, ASCIIHex, ASCII85, LZW, RunLength. */
#include "pn.h"
#include <stdlib.h>
#include <string.h>
#include "miniz.h"

static unsigned char *inflate_impl(const unsigned char *in, size_t inlen, size_t *outlen, int zlib_hdr, int *ok) {
    tinfl_decompressor inf;
    tinfl_init(&inf);
    size_t cap = inlen * 4 + 1024, len = 0, inpos = 0;
    unsigned char *out = (unsigned char *)malloc(cap + 1); /* +1: callers NUL-terminate */
    int flags = (zlib_hdr ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0) | TINFL_FLAG_HAS_MORE_INPUT;
    *ok = 0;
    for (;;) {
        size_t in_bytes = inlen - inpos, out_bytes = cap - len;
        int fl = flags;
        if (inpos >= inlen) fl &= ~TINFL_FLAG_HAS_MORE_INPUT;
        tinfl_status st = tinfl_decompress(&inf, in + inpos, &in_bytes, out, out + len, &out_bytes,
                                           fl | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        inpos += in_bytes; len += out_bytes;
        if (st == TINFL_STATUS_DONE) { *ok = 1; break; }
        if (st == TINFL_STATUS_HAS_MORE_OUTPUT) {
            if (cap >= PN_MAX_STREAM) break; /* decompression bomb: keep what we have */
            cap *= 2; out = (unsigned char *)realloc(out, cap + 1); continue;
        }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT) {
            if (inpos >= inlen) { *ok = len > 0; break; } /* truncated: keep what we have */
            continue;
        }
        break; /* error: keep partial output */
    }
    *outlen = len;
    return out;
}

unsigned char *pn_inflate(const unsigned char *in, size_t inlen, size_t *outlen) {
    int ok;
    int hdr = inlen >= 2 && (in[0] & 0x0F) == 8 && ((in[0] << 8) | in[1]) % 31 == 0;
    unsigned char *out = inflate_impl(in, inlen, outlen, hdr, &ok);
    if (!ok && *outlen == 0) {
        free(out);
        out = inflate_impl(in, inlen, outlen, !hdr, &ok);
    }
    return out;
}

unsigned char *pn_deflate(const unsigned char *in, size_t inlen, size_t *outlen, int level) {
    mz_ulong bound = mz_compressBound((mz_ulong)inlen);
    unsigned char *out = (unsigned char *)malloc(bound + 1);
    mz_ulong ol = bound;
    if (mz_compress2(out, &ol, in, (mz_ulong)inlen, level) != MZ_OK) { free(out); *outlen = 0; return NULL; }
    *outlen = ol;
    return out;
}

static unsigned char *apply_predictor(pn_obj *parms, unsigned char *data, size_t len, size_t *outlen) {
    int pred = (int)pn_int(pn_dict_get(parms, "Predictor"), 1);
    if (pred < 2) { *outlen = len; return data; }
    int colors = (int)pn_int(pn_dict_get(parms, "Colors"), 1);
    int bpc = (int)pn_int(pn_dict_get(parms, "BitsPerComponent"), 8);
    int columns = (int)pn_int(pn_dict_get(parms, "Columns"), 1);
    if (colors < 1 || colors > 32) colors = 1;
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) bpc = 8;
    if (columns < 1 || columns > (1 << 24)) columns = 1;
    int bpp = (colors * bpc + 7) / 8;
    size_t rowlen = ((size_t)colors * bpc * columns + 7) / 8;
    if (pred == 2) { /* TIFF, 8-bit only */
        if (bpc == 8) for (size_t r = 0; r + rowlen <= len; r += rowlen)
            for (size_t i = (size_t)bpp; i < rowlen; i++) data[r + i] = (unsigned char)(data[r + i] + data[r + i - bpp]);
        *outlen = len; return data;
    }
    /* PNG predictors: each row prefixed by a filter byte */
    size_t nrows = len / (rowlen + 1);
    unsigned char *out = (unsigned char *)calloc(nrows * rowlen + 1, 1);
    unsigned char *prev = (unsigned char *)calloc(rowlen + 1, 1);
    for (size_t r = 0; r < nrows; r++) {
        const unsigned char *src = data + r * (rowlen + 1);
        unsigned char *dst = out + r * rowlen;
        int ft = src[0];
        src++;
        for (size_t i = 0; i < rowlen; i++) {
            int a = i >= (size_t)bpp ? dst[i - bpp] : 0;
            int b = prev[i];
            int c = i >= (size_t)bpp ? prev[i - bpp] : 0;
            int x = src[i], v;
            switch (ft) {
            case 1: v = x + a; break;
            case 2: v = x + b; break;
            case 3: v = x + ((a + b) >> 1); break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                v = x + pr; break;
            }
            default: v = x;
            }
            dst[i] = (unsigned char)v;
        }
        memcpy(prev, dst, rowlen);
    }
    free(prev);
    free(data);
    *outlen = nrows * rowlen;
    return out;
}

static unsigned char *ahx_decode(const unsigned char *in, size_t n, size_t *ol) {
    pn_buf b; pn_buf_init(&b);
    int hi = -1;
    for (size_t i = 0; i < n; i++) {
        int c = in[i], v;
        if (c == '>') break;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;
        if (hi < 0) hi = v; else { pn_buf_putc(&b, hi * 16 + v); hi = -1; }
    }
    if (hi >= 0) pn_buf_putc(&b, hi * 16);
    return pn_buf_steal(&b, ol);
}

static unsigned char *a85_decode(const unsigned char *in, size_t n, size_t *ol) {
    pn_buf b; pn_buf_init(&b);
    unsigned long long acc = 0;
    int cnt = 0;
    size_t i = 0;
    if (n >= 2 && in[0] == '<' && in[1] == '~') i = 2;
    for (; i < n; i++) {
        int c = in[i];
        if (c == '~') break;
        if (pn_is_ws(c)) continue;
        if (c == 'z' && cnt == 0) { pn_buf_put(&b, "\0\0\0\0", 4); continue; }
        if (c < '!' || c > 'u') continue;
        acc = acc * 85 + (unsigned)(c - '!');
        if (++cnt == 5) {
            for (int k = 3; k >= 0; k--) pn_buf_putc(&b, (int)((acc >> (8 * k)) & 255));
            acc = 0; cnt = 0;
        }
    }
    if (cnt > 1) {
        for (int k = cnt; k < 5; k++) acc = acc * 85 + 84;
        for (int k = 0; k < cnt - 1; k++) pn_buf_putc(&b, (int)((acc >> (8 * (3 - k))) & 255));
    }
    return pn_buf_steal(&b, ol);
}

static unsigned char *rl_decode(const unsigned char *in, size_t n, size_t *ol) {
    pn_buf b; pn_buf_init(&b);
    size_t i = 0;
    while (i < n && b.len < PN_MAX_STREAM) {
        int len = in[i++];
        if (len == 128) break;
        if (len < 128) { size_t k = (size_t)len + 1; if (i + k > n) k = n - i; pn_buf_put(&b, in + i, k); i += k; }
        else if (i < n) { int c = in[i++]; for (int k = 0; k < 257 - len; k++) pn_buf_putc(&b, c); }
    }
    return pn_buf_steal(&b, ol);
}

static unsigned char *lzw_decode(const unsigned char *in, size_t n, size_t *ol, int early) {
    pn_buf b; pn_buf_init(&b);
    enum { MAXC = 4096 };
    int prefix[MAXC]; unsigned char suffix[MAXC]; int length[MAXC];
    for (int i = 0; i < 256; i++) { prefix[i] = -1; suffix[i] = (unsigned char)i; length[i] = 1; }
    int next = 258, codelen = 9, prev = -1;
    unsigned long bitbuf = 0; int bits = 0;
    size_t pos = 0;
    unsigned char stack[MAXC + 1];
    for (;;) {
        while (bits < codelen && pos < n) { bitbuf = (bitbuf << 8) | in[pos++]; bits += 8; }
        if (bits < codelen) break;
        int code = (int)((bitbuf >> (bits - codelen)) & ((1u << codelen) - 1));
        bits -= codelen;
        if (b.len > PN_MAX_STREAM) break; /* decompression bomb */
        if (code == 256) { next = 258; codelen = 9; prev = -1; continue; }
        if (code == 257) break;
        int cur = code, first;
        if (code < next && (code < 256 || code >= 258)) {
            /* emit string for code */
        } else if (code == next && prev >= 0) {
            cur = prev;
        } else break;
        int sp = 0, c = cur;
        while (c >= 0 && sp < MAXC) { stack[sp++] = suffix[c]; c = prefix[c]; }
        first = stack[sp - 1];
        for (int k = sp - 1; k >= 0; k--) pn_buf_putc(&b, stack[k]);
        if (code == next && prev >= 0) pn_buf_putc(&b, first);
        if (prev >= 0 && next < MAXC) {
            prefix[next] = prev; suffix[next] = (unsigned char)first; length[next] = length[prev] + 1;
            next++;
        }
        prev = code;
        if (next + early >= (1 << codelen) && codelen < 12) codelen++;
    }
    return pn_buf_steal(&b, ol);
}

unsigned char *pn_decode_filter(const char *name, pn_obj *parms, const unsigned char *in, size_t inlen, size_t *outlen, int *ok) {
    *ok = 1;
    unsigned char *out;
    if (!strcmp(name, "FlateDecode") || !strcmp(name, "Fl")) {
        out = pn_inflate(in, inlen, outlen);
        return apply_predictor(parms, out, *outlen, outlen);
    }
    if (!strcmp(name, "LZWDecode") || !strcmp(name, "LZW")) {
        int early = (int)pn_int(pn_dict_get(parms, "EarlyChange"), 1);
        out = lzw_decode(in, inlen, outlen, early);
        return apply_predictor(parms, out, *outlen, outlen);
    }
    if (!strcmp(name, "ASCIIHexDecode") || !strcmp(name, "AHx")) return ahx_decode(in, inlen, outlen);
    if (!strcmp(name, "ASCII85Decode") || !strcmp(name, "A85")) return a85_decode(in, inlen, outlen);
    if (!strcmp(name, "RunLengthDecode") || !strcmp(name, "RL")) return rl_decode(in, inlen, outlen);
    /* image codecs (DCT, JPX, CCITT, JBIG2) and Crypt are not decoded */
    *ok = 0;
    out = (unsigned char *)malloc(inlen + 1);
    memcpy(out, in, inlen);
    *outlen = inlen;
    return out;
}
