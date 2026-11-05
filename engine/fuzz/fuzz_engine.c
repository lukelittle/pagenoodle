/* libFuzzer harness: open arbitrary bytes and exercise every read/write path. */
#include "noodle.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 4 << 20) return 0;
    int h = nd_open(data, (int)size);
    if (!h) return 0;
    int n = nd_page_count(h);
    if (n > 50) n = 50;
    nd_info(h);
    nd_forms(h);
    nd_images(h);
    for (int p = 0; p < n; p++) { nd_text(h, p); nd_text_plain(h, p); }
    nd_search(h, "a", 1);
    double r[4] = {0, 0, 300, 300};
    if (n > 0) {
        nd_redact_mode(1);
        nd_redact(h, 0, r, 1, 0);
        nd_verify(h, 0, r, 1);
        nd_ov_begin(h, 0); nd_ov_font("Helvetica"); nd_ov_ops("BT /F1 12 Tf (x) Tj ET"); nd_ov_end();
        nd_stamp(h, "Page {n} of {total}", 4, 10, 24, 0, -1, 1, "Helvetica", 0);
        int hs[3] = {h, h, h}, pg[3] = {0, n - 1, 0}, rot[3] = {90, 0, 270};
        int a = nd_assemble(hs, pg, rot, 3);
        if (a) { nd_save(a, 1 | 2 | 4 | 8 | 16 | 32); nd_close(a); }
    }
    nd_form_flatten(h);
    nd_save(h, 1 | 2 | 4 | 8 | 32);
    int h2 = nd_open(nd_out_ptr(), nd_out_len()); /* re-open our own output */
    if (h2) { nd_page_count(h2); nd_close(h2); }
    nd_save(h, 0);
    nd_close(h);
    return 0;
}
