/* Host-side unit test for the private log-sector write primitive. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../Platforms/Samsung/J250fPkg/J250fEmmcDxe/Include/j250f_emmc.h"

enum { BLOCK=0x04, ARG=0x08, MODE=0x0c, COMMAND=0x0e, RESPONSE=0x10,
       BUFFER=0x20, PRESENT=0x24, INTERRUPT=0x30 };
static uint32_t regs[64];
static uint8_t card[128u*512u];
static unsigned command_count, cmd24_count, cmd13_count, cmd17_count;
static uint32_t last_arg, cmd24_arg, cmd24_mode, current_cmd, cursor;
static int fail_cmd24, timeout_cmd24;

static uint8_t rd8(void *c,j250f_region r,uint32_t a){(void)c;(void)r;(void)a;return 0;}
static uint16_t rd16(void *c,j250f_region r,uint32_t a){(void)c;(void)r;return (uint16_t)regs[a/4];}
static uint32_t rd32(void *c,j250f_region r,uint32_t a)
{
    (void)c;
    if(r==J250F_REGION_CORE)return 0;
    if(a==PRESENT) return current_cmd==24 ? 0x400 : current_cmd==17 ? 0x800 : 0;
    if(a==BUFFER) {
        uint32_t v=0; unsigned i;
        for(i=0;i<4;i++) v|=(uint32_t)card[last_arg*512u+cursor+i]<<(8*i);
        cursor+=4; return v;
    }
    if(a==INTERRUPT &&
       ((current_cmd==24 && cursor==512) || (current_cmd==17 && cursor==512)))
        return 2;
    return regs[a/4];
}
static void wr8(void*c,j250f_region r,uint32_t a,uint8_t v){(void)c;(void)r;(void)a;(void)v;}
static void wr16(void*c,j250f_region r,uint32_t a,uint16_t v)
{
    (void)c;(void)r;
    if(a==MODE){regs[a/4]=v;return;}
    if(a==COMMAND) {
        current_cmd=v>>8; command_count++; cursor=0;
        if(current_cmd==24){cmd24_count++;cmd24_arg=last_arg;cmd24_mode=regs[MODE/4];}
        if(current_cmd==13)cmd13_count++;
        if(current_cmd==17)cmd17_count++;
        regs[ARG/4]=last_arg;
        regs[INTERRUPT/4]=timeout_cmd24 && current_cmd==24 ? 0 :
            fail_cmd24 && current_cmd==24 ? 0x10000 : 1;
        regs[RESPONSE/4]=current_cmd==13 ? ((1u<<8)|(4u<<9)) : 0;
    }
}
static void wr32(void*c,j250f_region r,uint32_t a,uint32_t v)
{
    unsigned i;(void)c;(void)r;
    if(a==ARG){last_arg=v;regs[a/4]=v;return;}
    if(a==BUFFER && current_cmd==24){for(i=0;i<4;i++)card[last_arg*512u+cursor+i]=(uint8_t)(v>>(8*i));cursor+=4;return;}
    if(a==INTERRUPT){regs[a/4]=0;return;}
    regs[a/4]=v;
}
static void barrier(void*c){(void)c;}
static void delay(void*c,uint32_t u){(void)c;(void)u;}
static int clock_hz(void*c,uint32_t h){(void)c;(void)h;return 0;}
static int rails(void*c,int on,uint32_t a,uint32_t b){(void)c;(void)on;(void)a;(void)b;return 0;}
static int pins(void*c,int on){(void)c;(void)on;return 0;}

int main(void)
{
    j250f_emmc h; j250f_ops ops={rd8,rd16,rd32,wr8,wr16,wr32,barrier,delay,clock_hz,rails,pins};
    uint8_t input[512]; unsigned i,before;
    memset(&h,0,sizeof(h));memset(regs,0,sizeof(regs));memset(card,0,sizeof(card));
    h.io=ops;h.host_ready=h.card_ready=h.high_capacity=1;h.capacity_sectors=128;h.rca=1;
    for(i=0;i<sizeof(input);i++)input[i]=(uint8_t)(i*37u);
    assert(j250f_log_write_sector(&h,7,input)==J250F_OK);
    assert(cmd24_count==1 && cmd13_count==1 && cmd17_count==1);
    assert(cmd24_arg==7 && cmd24_mode==2); /* single-block write: no TRANS_READ */
    for(i=0;i<512;i++)assert(card[7*512u+i]==input[i]);
    before=command_count;assert(j250f_log_write_sector(&h,128,input)==J250F_E_RANGE);
    assert(command_count==before);
    fail_cmd24=1;before=cmd24_count;
    assert(j250f_log_write_sector(&h,8,input)==J250F_E_IO && cmd24_count==before+1);
    assert(cmd13_count==1); /* failed write was not retried or followed */
    puts("PASS: CMD24 PIO write, CMD13 readiness, readback and bounds/failure cases");
    return 0;
}
