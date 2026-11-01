/* SPDX-License-Identifier: GPL-2.0-only */
#include "j250f_emmc.h"
#include "j250f_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(x) do { if(!(x)) {fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);exit(1);} }while(0)
#define HC 0
#define CORE 1

typedef struct emu {
    uint8_t hc[0x500], core[0x800];
    uint8_t sectors[32][512];
    uint8_t ext_csd[512];
    uint8_t pio[512];
    unsigned cmd_count[64], cmd1_attempts;
    unsigned data_pos;
    unsigned pwr_clear_count;
    unsigned frequency;
    unsigned pins_on, rails_on;
    unsigned last_pwr_ack;
    unsigned op;
    unsigned sector;
    unsigned time_us;
    int fail_rails, fail_pins, suppress_command, fail_data_crc;
} emu;

static uint32_t raw32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void wr32(uint8_t *p,uint32_t v){
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);
}
static uint8_t *ap(emu *e,j250f_region r,uint32_t offset,size_t size) {
    ASSERT((r==J250F_REGION_HC && offset+size<=sizeof(e->hc)) ||
           (r==J250F_REGION_CORE && offset+size<=sizeof(e->core)));
    return (r==J250F_REGION_HC?e->hc:e->core)+offset;
}
static uint8_t read8(void *ctx,j250f_region r,uint32_t a){return *ap(ctx,r,a,1);}
static uint16_t read16(void *ctx,j250f_region r,uint32_t a){
    uint8_t *p=ap(ctx,r,a,2);
    ASSERT(a%2==0);
    return (uint16_t)(p[0]|(p[1]<<8));
}
static uint32_t read32(void *ctx,j250f_region r,uint32_t a){
    emu *e=ctx;
    uint8_t *p=ap(ctx,r,a,4);
    ASSERT(a%4==0);
    if(r==J250F_REGION_HC && a==0x20 && e->op==8) {
        uint32_t v;
        ASSERT(e->data_pos+4<=512);
        v=raw32(e->pio+e->data_pos);
        e->data_pos+=4;
        if(e->data_pos==512) {
            wr32(e->hc+0x24,raw32(e->hc+0x24)&~0x800u);
            wr32(e->hc+0x30,raw32(e->hc+0x30)|2);
        }
        return v;
    }
    if(r==J250F_REGION_HC && a==0x20 && e->op==17) {
        uint32_t v;
        ASSERT(e->data_pos+4<=512);
        v=raw32(e->sectors[e->sector]+e->data_pos);
        e->data_pos+=4;
        if(e->data_pos==512) {
            wr32(e->hc+0x24,raw32(e->hc+0x24)&~0x800u);
            wr32(e->hc+0x30,raw32(e->hc+0x30)|2);
        }
        return v;
    }
    return raw32(p);
}
static void write8(void *ctx,j250f_region r,uint32_t a,uint8_t v){
    emu *e=ctx;
    uint8_t *p=ap(ctx,r,a,1);
    if(r==J250F_REGION_CORE && a==0xE4){
        e->pwr_clear_count++;
        e->core[0xDC]&=(uint8_t)~v;
        return;
    }
    if(r==J250F_REGION_CORE && a==0xE8) e->last_pwr_ack=v;
    if(r==J250F_REGION_HC && a==0x2F){
        *p=0;/* reset completed */
        return;
    }
    if(r==J250F_REGION_HC && a==0x29){
        if((raw32(e->core+0xE0)&0x0Fu)==0x0Fu) e->core[0xDC]|=0x0A;
    }
    *p=v;
}
static void write32(void *ctx,j250f_region r,uint32_t a,uint32_t v){
    emu *e=ctx;
    uint8_t *p=ap(ctx,r,a,4);
    ASSERT(a%4==0);
    if(r==J250F_REGION_CORE && a==0xE4) {
        e->pwr_clear_count++;
        wr32(e->core+0xDC,raw32(e->core+0xDC)&~v);
        return;
    }
    if(r==J250F_REGION_CORE && a==0xE8) e->last_pwr_ack=v;
    if(r==J250F_REGION_HC && a==0x30){
        wr32(p,raw32(p)&~v);
        return;
    }
    if(r==J250F_REGION_HC && a==0x20 && e->op==24){
        ASSERT(e->data_pos+4<=512);
        wr32(e->sectors[e->sector]+e->data_pos,v);
        e->data_pos+=4;
        if(e->data_pos==512){
            wr32(e->hc+0x24,raw32(e->hc+0x24)&~0x400u);
            wr32(e->hc+0x30,raw32(e->hc+0x30)|2);
        }
        return;
    }
    wr32(p,v);
}
static void command_received(emu *e,uint16_t command){
    unsigned cmd=command>>8;
    uint32_t arg=raw32(e->hc+0x08);
    uint32_t resp=0;
    e->data_pos=0;
    e->op=0;
    ASSERT(cmd<64);
    e->cmd_count[cmd]++;
    if(e->suppress_command && cmd==(unsigned)e->suppress_command) return;
    switch(cmd){
    case 0: break;
    case 1:
        e->cmd1_attempts++;
        resp=e->cmd1_attempts<3?0x40ff8080u:0xC0ff8080u;
        break;
    case 2:case 9:
        for(unsigned i=0;i<4;i++) wr32(e->hc+0x10+i*4,0x12345678u+i);
        break;
    case 3:case 7:break;
    case 8:
        e->op=8;
        memcpy(e->pio,e->ext_csd,512);
        wr32(e->hc+0x24,raw32(e->hc+0x24)|0x800u);
        break;
    case 6:
        e->ext_csd[(arg>>16)&255u]=(uint8_t)(arg>>8);
        break;
    case 17:
        ASSERT(arg<32);
        e->op=17;e->sector=arg;
        wr32(e->hc+0x24,raw32(e->hc+0x24)|0x800u);
        if(e->fail_data_crc) {
            wr32(e->hc+0x30,raw32(e->hc+0x30)|0x200000u);
        }
        break;
    case 24:
        ASSERT(arg<32);
        e->op=24;e->sector=arg;
        wr32(e->hc+0x24,raw32(e->hc+0x24)|0x400u);
        break;
    default:ASSERT(0);
    }
    if(cmd!=2 && cmd!=9) wr32(e->hc+0x10,resp);
    wr32(e->hc+0x30,raw32(e->hc+0x30)|1u);
}
static void write16(void *ctx,j250f_region r,uint32_t a,uint16_t v){
    emu *e=ctx;
    uint8_t *p=ap(ctx,r,a,2);
    ASSERT(a%2==0);
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);
    if(r==J250F_REGION_HC && a==0x2C && (v&1)) p[0]|=2u;
    if(r==J250F_REGION_HC && a==0x0E) command_received(e,v);
}
static void barrier(void *ctx){(void)ctx;}
static void delay_us(void *ctx,uint32_t v){((emu *)ctx)->time_us+=v;}
static int clock_hz(void *ctx,uint32_t hz){((emu *)ctx)->frequency=hz;return 0;}
static int rails(void *ctx,int on,uint32_t vdd,uint32_t io){
    emu *e=ctx;
    ASSERT(vdd==2900000u && io==1800000u);
    if(e->fail_rails)return -1;
    e->rails_on=(unsigned)!!on;
    return 0;
}
static int pins(void *ctx,int on){
    emu *e=ctx;
    if(e->fail_pins)return -1;
    e->pins_on=(unsigned)!!on;
    return 0;
}
static const j250f_ops ops={read8,read16,read32,write8,write16,write32,
                            barrier,delay_us,clock_hz,rails,pins};
static void setup(emu *e){
    memset(e,0,sizeof(*e));
    wr32(e->core+0x50,0x10000049u);
    wr32(e->hc+0x40,0x00000000u);
    /* Simulated user area SIZE = 32, use >32 to test overflow without read */
    e->ext_csd[212]=32;e->ext_csd[179]=0x49;
    for(unsigned s=0;s<32;s++)for(unsigned i=0;i<512;i++)
        e->sectors[s][i]=(uint8_t)(s^i);
}
static void test_success(void){
    emu e;
    j250f_emmc h;
    uint8_t data[3*512];
    setup(&e);
    ASSERT(j250f_init_host(&h,&ops,&e)==J250F_OK);
    ASSERT(e.pwr_clear_count && e.last_pwr_ack==5 && e.rails_on && e.pins_on);
    ASSERT(raw32(e.core+0xE0)==0x0Fu);
    ASSERT(e.hc[0x29]==0x0Du);
    ASSERT(h.host_ready && !h.card_ready && h.frequency_hz==400000);
    ASSERT((read32(&e,J250F_REGION_CORE,0x78)&1)==1);
    ASSERT(j250f_init_card(&h)==J250F_OK);
    ASSERT(h.card_ready && h.bus_8bit && h.high_capacity);
    ASSERT(h.capacity_sectors==32 && h.ext_csd[179]==0x48);
    ASSERT(e.ext_csd[183]==2 && e.ext_csd[179]==0x48);
    ASSERT(h.frequency_hz==20000000u && e.frequency==20000000u);
    ASSERT(e.cmd_count[1]==3 && e.cmd_count[6]==2 && e.cmd_count[8]==1);
    ASSERT(j250f_read_blocks(&h,7,3,data)==J250F_OK);
    for(unsigned s=0;s<3;s++)for(unsigned i=0;i<512;i++)
        ASSERT(data[s*512+i]==(uint8_t)((7+s)^i));
    ASSERT(j250f_read_blocks(&h,31,2,data)==J250F_E_RANGE);
    ASSERT(j250f_read_blocks(&h,0,0,data)==J250F_OK);
    ASSERT(j250f_read_blocks(&h,0,1,0)==J250F_E_ARGUMENT);
    ASSERT(j250f_read_blocks(&h,UINT32_MAX,1,data)==J250F_E_RANGE);
    ASSERT(j250f_write_blocks(&h,0,1,data)==J250F_E_READ_ONLY);
    j250f_allow_writes(&h,1);
    ASSERT(j250f_write_blocks(&h,11,1,data)==J250F_OK);
    ASSERT(memcmp(e.sectors[11],data,512)==0);
    ASSERT(j250f_read_blocks(&h,11,1,data)==J250F_OK);
    ASSERT(j250f_set_frequency(&h,30000000)==J250F_E_ARGUMENT);
    puts("PASS: initialization, power handshake, 8-bit eMMC, partition, PIO I/O, sector safety");
}
static void test_fail_rails(void){
    emu e;
    j250f_emmc h;
    setup(&e);
    e.fail_rails=1;
    ASSERT(j250f_init_host(&h,&ops,&e)==J250F_E_POWER);
    ASSERT(e.last_pwr_ack==10); /* BUS_FAIL | IO_FAIL */
    ASSERT(!h.card_ready);
    puts("PASS: failed PMIC callback never acknowledges power success");
}
static void test_fail_pins(void){
    emu e;
    j250f_emmc h;
    setup(&e);
    e.fail_pins=1;
    ASSERT(j250f_init_host(&h,&ops,&e)==J250F_E_POWER);
    ASSERT((e.last_pwr_ack&2)!=0 && (e.last_pwr_ack&1)==0);
    puts("PASS: failed TLMM pin callback returns BUS_FAIL");
}
static void test_timeouts(void){
    emu e;
    j250f_emmc h;
    setup(&e);
    ASSERT(j250f_init_host(&h,&ops,&e)==J250F_OK);
    e.suppress_command=1;
    ASSERT(j250f_init_card(&h)==J250F_E_TIMEOUT);
    ASSERT(!h.card_ready);
    puts("PASS: command timeout without infinite loop");
}
static void test_crc(void){
    emu e;
    j250f_emmc h;
    uint8_t data[512];
    setup(&e);
    ASSERT(j250f_init_host(&h,&ops,&e)==J250F_OK);
    ASSERT(j250f_init_card(&h)==J250F_OK);
    e.fail_data_crc=1;
    ASSERT(j250f_read_blocks(&h,0,1,data)==J250F_E_IO);
    puts("PASS: data CRC error reports failure");
}

typedef struct board_emu {
    uint32_t gcc[0x80000/4], pin;
    int power_ok;
    unsigned time_us;
} board_emu;
static uint32_t board_r32(void *ctx,uint32_t phys){
    board_emu *b=ctx;
    if(phys==J250F_TLMM_PHYS+0x10A000)return b->pin;
    ASSERT(phys>=J250F_GCC_PHYS && phys<J250F_GCC_PHYS+J250F_GCC_SIZE);
    if(phys==J250F_GCC_PHYS+0x21000)return 1u<<30;
    return b->gcc[(phys-J250F_GCC_PHYS)/4];
}
static void board_w32(void *ctx,uint32_t phys,uint32_t v){
    board_emu *b=ctx;
    if(phys==J250F_TLMM_PHYS+0x10A000){b->pin=v;return;}
    ASSERT(phys>=J250F_GCC_PHYS && phys<J250F_GCC_PHYS+J250F_GCC_SIZE);
    if(phys==J250F_GCC_PHYS+0x42004) v&=~1u; /* RCG UPDATE settles */
    b->gcc[(phys-J250F_GCC_PHYS)/4]=v;
}
static void board_barrier(void *ctx){(void)ctx;}
static void board_delay(void *ctx,uint32_t us){((board_emu*)ctx)->time_us+=us;}
static int board_power_ok(void *ctx){return ((board_emu*)ctx)->power_ok?0:-1;}
static void test_board(void){
    board_emu sim;
    j250f_board b;
    memset(&sim,0,sizeof(sim));
    b.ctx=&sim;
    b.reg_read32=board_r32;b.reg_write32=board_w32;
    b.delay_us=board_delay;b.barrier=board_barrier;
    b.gpll0_power_ready=board_power_ok;
    sim.power_ok=1;
    sim.pin=(1u<<31);
    ASSERT(j250f_tlmm_sdcc1_pins(&b,1)==0);
    ASSERT((sim.pin&(1u<<31))!=0);
    ASSERT((sim.pin&(7u<<6))==(7u<<6));
    ASSERT((sim.pin&(7u<<3))==(4u<<3));
    ASSERT((sim.pin&7u)==4);
    ASSERT(j250f_tlmm_sdcc1_pins(&b,0)==0);
    ASSERT((sim.pin & ((7u<<6)|(7u<<3)|7u))==0);
    ASSERT((sim.pin & ((3u<<11)|(3u<<9)|(1u<<15)))==
                    ((3u<<11)|(3u<<9)|(1u<<15)));
    ASSERT(j250f_gcc_sdcc1_clock(&b,400000)==0);
    ASSERT((sim.gcc[0x42008/4]&0x371fu)==0x2017u);
    ASSERT(sim.gcc[0x4200C/4]==1);
    ASSERT(sim.gcc[0x42010/4]==~3u);
    ASSERT(sim.gcc[0x42014/4]==~4u);
    ASSERT(j250f_gcc_sdcc1_clock(&b,20000000)==0);
    ASSERT((sim.gcc[0x42008/4]&0x371fu)==0x2113u);
    ASSERT(sim.gcc[0x45000/4]&1u);
    ASSERT(j250f_gcc_sdcc1_clock(&b,25000000)==0);
    ASSERT((sim.gcc[0x42008/4]&0x371fu)==0x211fu);
    ASSERT(j250f_gcc_sdcc1_clock(&b,50000000)!=0);
    sim.power_ok=0;
    ASSERT(j250f_gcc_sdcc1_clock(&b,20000000)!=0);
    ASSERT(j250f_gcc_sdcc1_clock(&b,0)==0);
    ASSERT(!(sim.gcc[0x42018/4]&1u));
    puts("PASS: MSM8917 GCC M/N/D clocks + TLMM SDC1 active/sleep fields");
}
int main(void){
    test_success();test_fail_rails();test_fail_pins();test_timeouts();test_crc();test_board();
    puts("All tests passed.");
    return 0;
}
