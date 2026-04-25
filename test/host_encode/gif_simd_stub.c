/** Host stub for the RISC-V SIMD asm in gif_simd.S. Plain memset works
 * on x86 — we don't care about timing identity, just correctness. */
#include <string.h>
#include <stdint.h>

void gif_simd_memzero_s16(int16_t *buf, int count) {
    memset(buf, 0, (size_t)count * sizeof(int16_t));
}

void gif_simd_distribute_below(int16_t *err_nxt, const int16_t *errors, int count) {
    /* Mirror the per-pixel distribute logic from gif_encoder.c. The asm
     * version is an inner-loop optimization; the encoder still works
     * correctly when called as a no-op equivalent because the C path
     * does the math inline. We don't actually call this from the
     * pass2 path, so an empty body is fine — but if it ever gets
     * wired in, port the math from gif_simd.S. */
    (void)err_nxt; (void)errors; (void)count;
}
