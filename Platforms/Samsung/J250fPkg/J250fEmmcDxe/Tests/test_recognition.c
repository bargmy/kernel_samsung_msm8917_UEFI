/* Integration regressions for the one-bit 400kHz, read-only build.
 * Includes original simulator; its old main is not this build's test oracle. */
#define main original_test_main
#include "test_core.c"
#undef main
static uint32_t card_timeout(void *c,j250f_region r,uint32_t a) {
 emu *e=c;
 if(r==J250F_REGION_HC && a==0x30 && e->cmd_count[0])return 0;
 return read32(c,r,a);
}
static uint32_t cmd2_crc_error(void *c,j250f_region r,uint32_t a) {
 emu *e=c;
 if(r==J250F_REGION_HC && a==0x30 && e->cmd_count[2])return 0x00028000;
 return read32(c,r,a);
}
int main(void) {
 emu e;j250f_emmc h;uint8_t data[512];j250f_ops o=ops;
 setup(&e);e.ext_csd[179]=0x48;e.hc[0x3e]=3;wr32(e.hc+0x100,0x00070000);
 ASSERT(j250f_init_host(&h,&o,&e)==J250F_OK);
 ASSERT((raw32(e.hc+0x100)&0x60000000u)==0x60000000u && (e.hc[0x3e]&7)==0);
 ASSERT((raw32(e.hc+0x100)&0x70000)==0x70000);
 ASSERT(j250f_init_card(&h)==J250F_OK);
 ASSERT(h.card_ready && !h.bus_8bit && h.frequency_hz==400000);
 ASSERT(e.cmd_count[6]==0 && e.cmd_count[24]==0 && e.cmd_count[2]==1);
 ASSERT(h.capacity_sectors==32);
 ASSERT(j250f_read_blocks(&h,7,1,data)==J250F_OK);
 for(unsigned i=0;i<512;i++)ASSERT(data[i]==(uint8_t)(7^i));
 ASSERT(j250f_read_blocks(&h,31,2,data)==J250F_E_RANGE);
 j250f_allow_writes(&h,1);
 ASSERT(j250f_write_blocks(&h,0,1,data)==J250F_E_READ_ONLY && e.cmd_count[24]==0);
 setup(&e);ASSERT(j250f_init_host(&h,&o,&e)==J250F_OK);
 ASSERT(j250f_init_card(&h)==J250F_E_UNSUPPORTED && e.cmd_count[6]==0 && !h.card_ready);
 setup(&e);e.ext_csd[179]=0x48;o.read32=card_timeout;
 ASSERT(j250f_init_host(&h,&o,&e)==J250F_OK);
 ASSERT(j250f_init_card(&h)==J250F_E_TIMEOUT && !h.card_ready && e.cmd_count[0]==1);
 ASSERT(e.cmd_count[1]==0 && e.cmd_count[2]==0);
 setup(&e);e.ext_csd[179]=0x48;o=ops;o.read32=cmd2_crc_error;
 ASSERT(j250f_init_host(&h,&o,&e)==J250F_OK);
 ASSERT(j250f_init_card(&h)==J250F_E_IO && !h.card_ready);
 ASSERT(h.last_command==2 && h.last_interrupt==0x28000);
 setup(&e);o=ops;ASSERT(j250f_init_host(&h,&o,&e)==J250F_OK);
 wr32(e.hc+0x10c,0x0a1c);e.core[0xdc]=2;
 ASSERT(j250f_service_power(&h)==J250F_OK);
 ASSERT((raw32(e.hc+0x10c)&0x18000)==0x18000);
 ASSERT((raw32(e.hc+0x10c)&~0x18000u)==0x0a1c);
 ASSERT(e.last_pwr_ack==1); // BUS_ON-only, no separate IO event
 wr32(e.hc+0x10c,0x0a1c);e.core[0xdc]=2;e.fail_rails=1;
 ASSERT(j250f_service_power(&h)==J250F_E_POWER);
 ASSERT(raw32(e.hc+0x10c)==0x0a1c && e.last_pwr_ack==2);
 e.fail_rails=0;e.fail_pins=1;e.core[0xdc]=2;
 ASSERT(j250f_service_power(&h)==J250F_E_POWER);
 ASSERT(raw32(e.hc+0x10c)==0x0a1c && e.last_pwr_ack==2);
 e.fail_pins=0;e.core[0xdc]=1;ASSERT(j250f_service_power(&h)==J250F_OK);
 ASSERT(!h.rails_on && raw32(e.hc+0x10c)==0x0a1c);
 puts("PASS: BUS_ON-only selects 1.8V pads; regulator/pin failures and BUS_OFF do not falsely select pads");
 puts("PASS: Samsung low-speed DLL reset/power-down and feedback mode; genuine CMD2 CRC errors remain fatal");
 puts("PASS: 400kHz/one-bit recognition/read path, no CMD6 or write path, partition rejection, CMD0 timeout abort");
 return 0;
}
