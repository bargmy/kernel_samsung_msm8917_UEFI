/* SPDX-License-Identifier: GPL-2.0-only */
#include "j250f_board.h"
#include "j250f_emmc.h"

/* From original Qualcomm clock-gcc-8952.c and msm-clocks-hwio-8952.h:
 * SDCC1_APPS_CMD_RCGR = 0x42004, CBCR = 0x42018, AHB CBCR = 0x4201C;
 * GPLL0_ENA_VOTE = 0x45000 and GPLL0_MODE = 0x21000 on MSM8917.
 * RCG M,~(N-M),~N; cfg 2*divider-1 | (source << 8) | (2<<12).
 * Must not assume that a GPLL0 vote is equivalent to required VDD votes. */
enum { GCC_CMD=0x42004, GCC_CFG=0x42008, GCC_M=0x4200C,
       GCC_N=0x42010, GCC_D=0x42014, GCC_APPS=0x42018,
       GCC_AHB=0x4201C, GCC_PLL_VOTE=0x45000, GCC_PLL_MODE=0x21000,
       TLMM_SDC1_CTL=0x10A000 };
static int valid(j250f_board *b) {
    return b && b->reg_read32 && b->reg_write32 && b->barrier && b->delay_us;
}
static uint32_t rd(j250f_board *b,uint32_t base,uint32_t offset) {
    return b->reg_read32(b->ctx,base+offset);
}
static void wr(j250f_board *b,uint32_t base,uint32_t offset,uint32_t val) {
    b->reg_write32(b->ctx,base+offset,val);
}
static int wait_clear(j250f_board *b,uint32_t offset,uint32_t mask) {
    unsigned i;
    for(i=0;i<500;i++) {
        if(!(rd(b,J250F_GCC_PHYS,offset)&mask))return 0;
        b->delay_us(b->ctx,1);
    }
    return -1;
}
int j250f_gcc_sdcc1_clock(j250f_board *b,uint32_t hz) {
    uint32_t cfg,m=1,n,d;
    uint32_t v;
    if (!valid(b))return -1;
    if (hz!=0 && hz!=144000u && hz!=400000u && hz!=20000000u && hz!=25000000u)
        return -1;
    if (!hz) {
        v=rd(b,J250F_GCC_PHYS,GCC_APPS);
        wr(b,J250F_GCC_PHYS,GCC_APPS,v&~1u);
        b->barrier(b->ctx);
        return 0;
    }
    if (hz>400000u) {
        if(!b->gpll0_power_ready || b->gpll0_power_ready(b->ctx))return -1;
        v=rd(b,J250F_GCC_PHYS,GCC_PLL_VOTE);
        wr(b,J250F_GCC_PHYS,GCC_PLL_VOTE,v|1u);
        b->barrier(b->ctx);
        /* On 8917, Linux uses GPLL0_MODE bit 30 (8937 family). */
        for(unsigned i=0;i<500;i++) {
            if (rd(b,J250F_GCC_PHYS,GCC_PLL_MODE)&(1u<<30)) break;
            if (i==499)return -1;
            b->delay_us(b->ctx,1);
        }
    }
    if (hz==144000u) {
        /* Samsung ftbl_gcc_sdcc1_apps_clk: F(144000,xo,16,3,25). */
        cfg=31u;m=3u;n=~(25u-3u);d=~25u;
    } else if (hz==400000u) {
        cfg=23u; /* XO 19.2MHz / 12 * 1/4 = 400kHz */
        n=~(4u-1u);d=~4u;
    } else if (hz==20000000u) {
        cfg=19u|(1u<<8); /* GPLL0 800MHz / 10 * 1/4 */
        n=~(4u-1u);d=~4u;
    } else {
        cfg=31u|(1u<<8); /* GPLL0 800MHz / 16 * 1/2 */
        n=~(2u-1u);d=~2u;
    }
    /* When changing rate, disable just the apps branch. Keep AHB alive. */
    v=rd(b,J250F_GCC_PHYS,GCC_APPS);
    wr(b,J250F_GCC_PHYS,GCC_APPS,v&~1u);
    wr(b,J250F_GCC_PHYS,GCC_M,m);
    wr(b,J250F_GCC_PHYS,GCC_N,n);
    wr(b,J250F_GCC_PHYS,GCC_D,d);
    v=rd(b,J250F_GCC_PHYS,GCC_CFG);
    v&=~((31u<<0)|(7u<<8)|(3u<<12));
    v|=cfg|(2u<<12);
    wr(b,J250F_GCC_PHYS,GCC_CFG,v);
    b->barrier(b->ctx);
    v=rd(b,J250F_GCC_PHYS,GCC_CMD);
    wr(b,J250F_GCC_PHYS,GCC_CMD,v|1u);
    b->barrier(b->ctx);
    if (wait_clear(b,GCC_CMD,1u))return -1;
    v=rd(b,J250F_GCC_PHYS,GCC_AHB);
    wr(b,J250F_GCC_PHYS,GCC_AHB,v|1u);
    v=rd(b,J250F_GCC_PHYS,GCC_APPS);
    wr(b,J250F_GCC_PHYS,GCC_APPS,v|1u);
    b->barrier(b->ctx);
    if(wait_clear(b,GCC_AHB,1u<<31))return -1;
    if(wait_clear(b,GCC_APPS,1u<<31))return -1;
    return 0;
}

int j250f_tlmm_sdcc1_pins(j250f_board *b,int active) {
    uint32_t v;
    if(!valid(b))return -1;
    /* MSM8917 SDC_QDSD_PINGROUP entries all share 0x10A000:
     * SDC1_CLK: pull bit 13, drive bit 6
     * SDC1_CMD: pull bit 11, drive bit 3
     * SDC1_DATA: pull bit 9, drive bit 0
     * SDC1_RCLK: pull bit 15, no drive
     * pull: 0 none, 1 pulldown, 3 pullup;
     * drive strength code = milliamps/2 - 1.
     * Board DTS active: clk 16mA none, cmd/data 10mA up, rclk down.
     * Board DTS sleep: clk/cmd/data 2mA, cmd/data up, rclk down. */
    v=rd(b,J250F_TLMM_PHYS,TLMM_SDC1_CTL);
    v&=~((3u<<13)|(7u<<6)|(3u<<11)|(7u<<3)|
         (3u<<9)|(7u<<0)|(3u<<15));
    v|=(3u<<11)|(3u<<9)|(1u<<15);
    if(active)v|=(7u<<6)|(4u<<3)|(4u<<0);
    wr(b,J250F_TLMM_PHYS,TLMM_SDC1_CTL,v);
    b->barrier(b->ctx);
    /* Do not claim success when TLMM write did not latch. */
    return rd(b,J250F_TLMM_PHYS,TLMM_SDC1_CTL)==v?0:-1;
}
