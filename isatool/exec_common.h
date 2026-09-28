/* Helpers shared by every generated executor (<arch>/exec_gen.h): arithmetic
 * with no undefined C behaviour, memory access with the alignment rule, and
 * instruction fetch. Needs sim_c's read8/16/32, write8/16/32, check_align16/32,
 * Core and g_gen_bad. */
#ifndef ISATOOL_EXEC_COMMON_H
#define ISATOOL_EXEC_COMMON_H
static uint32_t sx32(uint32_t v, int n) { uint32_t m = 1u << (n - 1); v &= (n == 32) ? 0xffffffffu : ((1u << n) - 1); return (v ^ m) - m; }
static uint32_t shl32(uint32_t a, uint32_t b) { return b >= 32 ? 0 : a << b; }
static uint32_t shr32(uint32_t a, uint32_t b) { return b >= 32 ? 0 : a >> b; }
static uint32_t sar32(uint32_t a, uint32_t b) { return b >= 32 ? ((int32_t)a < 0 ? 0xffffffffu : 0) : (uint32_t)((int32_t)a >> b); }
static uint32_t divu32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static uint32_t modu32(uint32_t a, uint32_t b) { return b ? a % b : 0; }
static uint32_t divs32(uint32_t a, uint32_t b) { if (!b) return 0; if (a == 0x80000000u && b == 0xffffffffu) return a; return (uint32_t)((int32_t)a / (int32_t)b); }
static uint32_t mods32(uint32_t a, uint32_t b) { if (!b) return 0; if (a == 0x80000000u && b == 0xffffffffu) return 0; return (uint32_t)((int32_t)a % (int32_t)b); }
static uint32_t gen_rd8 (uint32_t a, uint16_t pc) { (void)pc; return read8(a); }
static uint32_t gen_rd16(uint32_t a, uint16_t pc) { check_align16(a, pc); return read16(a); }
static uint32_t gen_rd32(uint32_t a, uint16_t pc) { check_align32(a, pc); return read32(a); }
static void gen_wr8 (uint32_t a, uint32_t v, uint16_t pc) { (void)pc; write8(a, (uint8_t)v); }
static void gen_wr16(uint32_t a, uint32_t v, uint16_t pc) { check_align16(a, pc); write16(a, (uint16_t)v); }
static void gen_wr32(uint32_t a, uint32_t v, uint16_t pc) { check_align32(a, pc); write32(a, v); }
static void gen_putchar(uint32_t v) { fputc((int)(v & 0xff), stderr); fflush(stderr); }
static uint32_t gen_fetch(uint16_t a, int n) { uint32_t w = 0; for (int i = 0; i < n; i++) w = (w << 8) | read8((uint16_t)(a + i)); return w; }
static inline void gen_unknown(Core *cc, uint32_t w, uint16_t oldpc) {
    fprintf(stderr, "%s: unknown instruction 0x%x at pc=%04x\n", g_arch_name, (unsigned)w, oldpc);
    g_gen_bad = 1; cc->H = 1;
}
static inline void gen_unimplemented(Core *cc, const char *name, uint16_t oldpc) {
    fprintf(stderr, "%s: instruction %s at pc=%04x has no semantics yet\n", g_arch_name, name, oldpc);
    g_gen_bad = 1; cc->H = 1;
}
#endif
