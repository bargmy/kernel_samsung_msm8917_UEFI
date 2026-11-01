/* Regression suite for defects identified in J250F independent audit.
 * Uses same fake-MMIO simulator as test_core.c; not hardware validation.
 */
#define main inherited_tests_main
#include "test_core.c"
#undef main

static uint32_t locked_r1(void *c, j250f_region r, uint32_t a) {
    uint32_t v=read32(c,r,a);
    emu *e=c;
    if(r==J250F_REGION_HC && a==0x10 && e->cmd_count[6])
        v|=1u<<25; /* R1_CARD_IS_LOCKED */
    return v;
}
static void test_reject_locked(void) {
    emu e;j250f_emmc h; j250f_ops o=ops;
    setup(&e);o.read32=locked_r1;
    ASSERT(j250f_init_host(&h,&o,&e)==J250F_OK);
    ASSERT(j250f_init_card(&h)==J250F_E_PROTOCOL);
    ASSERT(!h.card_ready);
    puts("PASS: CMD6 R1_CARD_IS_LOCKED is rejected");
}

static uint32_t one_time_r1b_timeout(void *c, j250f_region r, uint32_t a) {
    emu *e=c;
    uint32_t v=read32(c,r,a);
    if(r==J250F_REGION_HC && a==0x30 && e->cmd_count[6] && !e->op) {
        /* Use emu.op as a one-time injection marker; when CMD6 is issued,
         * command_received() sets op=0. The first poll sees timeout+summary.
         * Later reads are normal after a write-one-to-clear operation. */
        e->op=60;
        return v|0x00108000u;
    }
    return v;
}
static void test_ignore_only_r1b_data_timeout(void) {
    emu e;j250f_emmc h; j250f_ops o=ops;
    setup(&e);o.read32=one_time_r1b_timeout;
    ASSERT(j250f_init_host(&h,&o,&e)==J250F_OK);
    ASSERT(j250f_init_card(&h)==J250F_OK);
    ASSERT(h.card_ready);
    puts("PASS: Qualcomm R1b-only spurious DAT_TIMEOUT quirk handled");
}
static uint32_t r1b_crc_error(void *c, j250f_region r, uint32_t a) {
    emu *e=c;
    uint32_t v=read32(c,r,a);
    if(r==J250F_REGION_HC && a==0x30 && e->cmd_count[6])
        return v | (1u<<17); /* data CRC error remains fatal */
    return v;
}
static void test_quirk_not_mask_crc(void) {
    emu e;j250f_emmc h; j250f_ops o=ops;
    setup(&e);o.read32=r1b_crc_error;
    ASSERT(j250f_init_host(&h,&o,&e)==J250F_OK);
    ASSERT(j250f_init_card(&h)==J250F_E_IO);
    puts("PASS: R1b quirk does not mask unrelated controller CRC errors");
}
int main(void) {
    inherited_tests_main();
    test_reject_locked();
    test_ignore_only_r1b_data_timeout();
    test_quirk_not_mask_crc();
    puts("All J250F Project Mu transport regressions passed.");
    return 0;
}
