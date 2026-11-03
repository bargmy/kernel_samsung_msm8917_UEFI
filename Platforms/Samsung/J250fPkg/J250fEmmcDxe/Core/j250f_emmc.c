/* SPDX-License-Identifier: GPL-2.0-only */
#include "j250f_emmc.h"


/* See original/sdhci-msm.c, original/sdhci.h (both GPL-licensed). */
#define INT_ERROR_MASK UINT32_C(0xFFFF8000)
#define INT_ALL UINT32_C(0xFFFFFFFF)
enum {
    HC_BLOCK_SIZE=0x04, HC_BLOCK_COUNT=0x06, HC_ARGUMENT=0x08,
    HC_TRANSFER_MODE=0x0C, HC_COMMAND=0x0E, HC_RESPONSE=0x10,
    HC_BUFFER=0x20, HC_PRESENT=0x24, HC_HOST_CONTROL=0x28,
    HC_POWER=0x29, HC_CLOCK=0x2C, HC_TIMEOUT=0x2E,
    HC_RESET=0x2F, HC_INTERRUPT=0x30, HC_INT_ENABLE=0x34,
    HC_SIGNAL_ENABLE=0x38, HC_HOST_CONTROL2=0x3E, HC_CAPS=0x40, HC_VERSION=0xFE,
    HC_DLL_CONFIG=0x100, HC_VENDOR_SPEC=0x10C, HC_VENDOR_CAPS=0x11C,
    CORE_VERSION=0x50, CORE_MODE=0x78, CORE_PWR_STATUS=0xDC,
    CORE_PWR_MASK=0xE0, CORE_PWR_CLEAR=0xE4, CORE_PWR_CTL=0xE8
};
enum {
    CMD_INHIBIT=1, DATA_INHIBIT=2,
    SPACE_AVAILABLE=0x400, DATA_AVAILABLE=0x800,
    INT_COMMAND=1, INT_TRANSFER=2, INT_WRITE_READY=0x10,
    INT_READ_READY=0x20, INT_ERROR=0x8000,
    INT_DATA_TIMEOUT=(1u<<20),

    POWER_BUS_OFF=1, POWER_BUS_ON=2, POWER_IO_LOW=4,
    POWER_IO_HIGH=8, POWER_BUS_OK=1, POWER_BUS_FAIL=2,
    POWER_IO_OK=4, POWER_IO_FAIL=8,
    CLOCK_INT_EN=1, CLOCK_INT_STABLE=2, CLOCK_CARD_EN=4,
    RESET_ALL=1, RESET_CMD=2, RESET_DAT=4,
    RSP_NONE=0, RSP_136=1, RSP_48=2, RSP_48_BUSY=3,
    CMD_CRC=8, CMD_INDEX=16, CMD_DATA=32,
    TRANS_BLOCKCNT=2, TRANS_READ=16,
    VENDOR_PAD_SWITCH_EN=(1u<<15), VENDOR_PAD_SWITCH=(1u<<16),
    VENDOR_MCLK_DEFAULT=(2u<<8), VENDOR_MCLK_MASK=(3u<<8)
};
static uint8_t r8(j250f_emmc *h,j250f_region r,uint32_t a){return h->io.read8(h->context,r,a);}
static uint32_t r32(j250f_emmc *h,j250f_region r,uint32_t a){return h->io.read32(h->context,r,a);}
static void w8(j250f_emmc *h,j250f_region r,uint32_t a,uint8_t v){h->io.write8(h->context,r,a,v);}
static void w16(j250f_emmc *h,j250f_region r,uint32_t a,uint16_t v){h->io.write16(h->context,r,a,v);}
static void w32(j250f_emmc *h,j250f_region r,uint32_t a,uint32_t v){h->io.write32(h->context,r,a,v);}
static void sync_io(j250f_emmc *h){h->io.barrier(h->context);}
static void pause_us(j250f_emmc *h,uint32_t us){h->io.delay_us(h->context,us);}

int j250f_service_power(j250f_emmc *h)
{
    uint32_t status, ack=0;
    unsigned tries;
    int result=J250F_OK;
    if (!h || !h->io.read8) return J250F_E_ARGUMENT;
    status=r32(h,J250F_REGION_CORE,CORE_PWR_STATUS);
    if (!status) return J250F_OK;
    /* Samsung's threaded interrupt routine: write1-to-clear and retry (lost
     * first clear pulse on certain reset/power races). NO unconditional ack. */
    for (tries=0;tries<10;tries++) {
        w32(h,J250F_REGION_CORE,CORE_PWR_CLEAR,status);
        sync_io(h);
        if (!(r32(h,J250F_REGION_CORE,CORE_PWR_STATUS)&status)) break;
        pause_us(h,10);
    }
    if (tries==10) return J250F_E_TIMEOUT;
    if (status&POWER_BUS_ON) {
        int ok = !h->io.set_rails(h->context,1,J250F_VDD_UV,J250F_VQMMC_UV);
        if (ok) {
            h->rails_on=1;
            ok = !h->io.set_pins(h->context,1);
            if (ok) h->pins_active=1;
        }
        if (ok) ack|=POWER_BUS_OK;
        else {ack|=POWER_BUS_FAIL;result=J250F_E_POWER;}
    }
    if (status&POWER_BUS_OFF) {
        int ok = !h->io.set_pins(h->context,0);
        if (ok) {
            h->pins_active=0;
            ok = !h->io.set_rails(h->context,0,J250F_VDD_UV,J250F_VQMMC_UV);
            if (ok) h->rails_on=0;
        }
        if (ok) ack|=POWER_BUS_OK;
        else {ack|=POWER_BUS_FAIL;result=J250F_E_POWER;}
    }
    if (status&(POWER_IO_LOW|POWER_IO_HIGH)) {
        /* On J250F the two DT qcom,vdd-io-voltage-level entries are
         * BOTH 1,800,000uV. Never pretend to switch to 3.0V. */
        int ok = !h->io.set_rails(h->context,h->rails_on,
                                  J250F_VDD_UV,J250F_VQMMC_UV);
        if (ok) ack|=POWER_IO_OK;
        else {ack|=POWER_IO_FAIL;result=J250F_E_POWER;}
    }
    w32(h,J250F_REGION_CORE,CORE_PWR_CTL,ack);
    sync_io(h);
    if (result==J250F_OK && h->rails_on &&
        (status&(POWER_BUS_ON|POWER_IO_HIGH|POWER_IO_LOW))) {
        uint32_t vendor=r32(h,J250F_REGION_HC,HC_VENDOR_SPEC);
        /* Samsung BUS_ON sets io_level=REQ_IO_HIGH. On this fixed-1.8V
         * board the pad selector must also be set without a separate IO IRQ.
         * Do not touch it if regulators/pins failed or logical rails are off. */
        w32(h,J250F_REGION_HC,HC_VENDOR_SPEC,
            vendor|VENDOR_PAD_SWITCH_EN|VENDOR_PAD_SWITCH);
        sync_io(h);
    }
    return result;
}

static int poll_mask(j250f_emmc *h,j250f_region region,uint32_t offset,
                     uint32_t mask,uint32_t expected,uint32_t timeout_us)
{
    uint32_t n;
    for (n=0;n<timeout_us;n+=10) {
        int r=j250f_service_power(h);
        if (r) return r;
        if ((r32(h,region,offset)&mask)==expected) return J250F_OK;
        pause_us(h,10);
    }
    return J250F_E_TIMEOUT;
}

/* Samsung sdhci_msm_set_clock() selects CORE_HC_MCLK_SEL_DFLT (2 << 8)
 * outside HS400. Repair an inherited/reset selector only after the command
 * line is idle, and preserve pad-voltage, DLL, and unrelated vendor bits. */
static int ensure_legacy_mclk(j250f_emmc *h)
{
    uint32_t vendor=r32(h,J250F_REGION_HC,HC_VENDOR_SPEC);
    uint32_t desired=(vendor&~(VENDOR_MCLK_MASK|2u))|VENDOR_MCLK_DEFAULT;
    uint32_t actual;

    if (desired!=vendor) {
        w32(h,J250F_REGION_HC,HC_VENDOR_SPEC,desired);
        sync_io(h);
    }
    actual=r32(h,J250F_REGION_HC,HC_VENDOR_SPEC);
    if ((actual&VENDOR_MCLK_MASK)!=VENDOR_MCLK_DEFAULT) {
        /* The caller will abort before a command; retain the failed readback
         * and adjacent controller state so the DXE log explains why. */
        h->last_vendor=actual;
        h->last_present=r32(h,J250F_REGION_HC,HC_PRESENT);
        h->last_dll=r32(h,J250F_REGION_HC,HC_DLL_CONFIG);
        h->last_clock=h->io.read16(h->context,J250F_REGION_HC,HC_CLOCK);
        return J250F_E_IO;
    }
    return J250F_OK;
}

/* J250F hardware experiment: a clock-gated, explicit legacy HC2 write
 * before each identification command succeeded even when HC2 read zero.
 * Reproduce that ordering without requiring an ADB diagnostic profile.
 * Preserve signal-voltage and other HC2 bits; restore the original clock
 * on both success and failed readback. Only call while both lines are idle.
 */
static int latch_legacy_timing(j250f_emmc *h)
{
    uint16_t clock=h->io.read16(h->context,J250F_REGION_HC,HC_CLOCK);
    uint16_t ctrl2,actual;
    w16(h,J250F_REGION_HC,HC_CLOCK,(uint16_t)(clock&~CLOCK_CARD_EN));
    sync_io(h);
    ctrl2=h->io.read16(h->context,J250F_REGION_HC,HC_HOST_CONTROL2);
    w16(h,J250F_REGION_HC,HC_HOST_CONTROL2,(uint16_t)(ctrl2&~7u));
    sync_io(h);
    actual=h->io.read16(h->context,J250F_REGION_HC,HC_HOST_CONTROL2);
    w16(h,J250F_REGION_HC,HC_CLOCK,clock);
    sync_io(h);
    return (actual&7u)?J250F_E_IO:J250F_OK;
}

int j250f_set_frequency(j250f_emmc *h,uint32_t hz)
{
    uint32_t vendor;
    int r;
    if (!h || !h->io.set_clock_hz || hz>50000000u ||
        (hz && hz!=144000u && hz!=400000u && hz!=20000000u && hz!=25000000u))
        return J250F_E_ARGUMENT;
    /* Disable the card clock before changing Qualcomm root frequency. */
    w16(h,J250F_REGION_HC,HC_CLOCK,0);
    sync_io(h);
    /* Legacy timing is already selected for our supported <=25MHz rates.
     * Verify the inherited mode bits are cleared, preserving 1.8V and other
     * Host Control 2 fields. */
    if (hz && hz<=25000000u) {
        uint16_t ctrl2=h->io.read16(h->context,J250F_REGION_HC,HC_HOST_CONTROL2);
        if (ctrl2&7u) {
            w16(h,J250F_REGION_HC,HC_HOST_CONTROL2,(uint16_t)(ctrl2&~7u));
            sync_io(h);
        }
        if (h->io.read16(h->context,J250F_REGION_HC,HC_HOST_CONTROL2)&7u) {
            h->last_present=r32(h,J250F_REGION_HC,HC_PRESENT);
            h->last_vendor=r32(h,J250F_REGION_HC,HC_VENDOR_SPEC);
            h->last_dll=r32(h,J250F_REGION_HC,HC_DLL_CONFIG);
            h->last_clock=h->io.read16(h->context,J250F_REGION_HC,HC_CLOCK);
            return J250F_E_IO;
        }
    }
    vendor=r32(h,J250F_REGION_HC,HC_VENDOR_SPEC);
    vendor=(vendor&~(VENDOR_MCLK_MASK|2u|(1u<<18)|(7u<<19)))|VENDOR_MCLK_DEFAULT;
    w32(h,J250F_REGION_HC,HC_VENDOR_SPEC,vendor);
    sync_io(h);
    if (hz && hz<=100000000u) {
        /* Samsung sdhci_msm_set_uhs_signaling: below 100MHz use feedback
         * clock, not the inherited HS200/HS400 DLL sampling path. Preserve
         * all unrelated vendor bits, assert DLL_RST then DLL_PDN. */
        w32(h,J250F_REGION_HC,HC_DLL_CONFIG,
            r32(h,J250F_REGION_HC,HC_DLL_CONFIG)|(1u<<30));
        w32(h,J250F_REGION_HC,HC_DLL_CONFIG,
            r32(h,J250F_REGION_HC,HC_DLL_CONFIG)|(1u<<29));
        sync_io(h);
    }
    r=h->io.set_clock_hz(h->context,hz);
    if (r) return J250F_E_IO;
    if (!hz) {h->frequency_hz=0;return J250F_OK;}
    /* Quirk ALWAYS_USE_BASE_CLOCK: SDHCI divider = 0, no CAP base clock. */
    w16(h,J250F_REGION_HC,HC_CLOCK,CLOCK_INT_EN);
    r=poll_mask(h,J250F_REGION_HC,HC_CLOCK,CLOCK_INT_STABLE,CLOCK_INT_STABLE,20000);
    if (r) return r;
    w16(h,J250F_REGION_HC,HC_CLOCK,CLOCK_INT_EN|CLOCK_CARD_EN);
    sync_io(h);
    h->frequency_hz=hz;
    return ensure_legacy_mclk(h);
}

static int reset(j250f_emmc *h,uint8_t bits)
{
    uint32_t i;
    w8(h,J250F_REGION_HC,HC_RESET,bits);
    for(i=0;i<100000;i+=10) {
        int rc=j250f_service_power(h);
        if(rc) return rc;
        if (!(r8(h,J250F_REGION_HC,HC_RESET)&bits)) return J250F_OK;
        pause_us(h,10);
    }
    return J250F_E_TIMEOUT;
}

/* Record the failing transaction before W1C and command-line reset destroy
 * evidence. Raw response words on CRC failure are not a validated CID. */
static void record_command(j250f_emmc *h,unsigned cmd,uint32_t arg,uint16_t flags,
                           uint32_t intr,int result)
{
    unsigned i; j250f_trace *t;
    uint32_t seq=++h->trace_sequence;
    t=&h->trace[(seq-1u)%J250F_TRACE_DEPTH];
    t->sequence=seq;t->command=cmd;t->argument=arg;t->flags=flags;
    t->result=(uint32_t)result;t->interrupt=intr;
    t->present=r32(h,J250F_REGION_HC,HC_PRESENT);
    for(i=0;i<4;i++)t->response[i]=r32(h,J250F_REGION_HC,HC_RESPONSE+i*4);
    t->vendor=r32(h,J250F_REGION_HC,HC_VENDOR_SPEC);
    t->dll=r32(h,J250F_REGION_HC,HC_DLL_CONFIG);
    t->hc2=h->io.read16(h->context,J250F_REGION_HC,HC_HOST_CONTROL2);
    t->clock=h->io.read16(h->context,J250F_REGION_HC,HC_CLOCK);
    t->frequency_hz=h->frequency_hz;
    t->core_mode=r32(h,J250F_REGION_CORE,CORE_MODE);
    t->power_status=r32(h,J250F_REGION_CORE,CORE_PWR_STATUS);
    t->power_mask=r32(h,J250F_REGION_CORE,CORE_PWR_MASK);
    t->power_control=r32(h,J250F_REGION_CORE,CORE_PWR_CTL);
    if(h->diagnostic_capture)h->diagnostic_capture(h->diagnostic_context,t);
}

/* Commands use legacy, non-DMA PIO transfer with bounded polling. */
static int command(j250f_emmc *h,unsigned cmd,uint32_t arg,unsigned response,
                   uint8_t *buffer,int write_data,size_t data_len,uint32_t *out)
{
    uint32_t intr,mask=CMD_INHIBIT;
    uint16_t command_bits,mode=0;
    unsigned n;
    int rc;
    h->last_command=(uint8_t)cmd;
    if (!h->host_ready) return J250F_E_NOT_READY;
    if (data_len && (!buffer || data_len!=512)) return J250F_E_ARGUMENT;
    if (data_len || response==RSP_48_BUSY || !h->card_ready) mask|=DATA_INHIBIT;
    rc=poll_mask(h,J250F_REGION_HC,HC_PRESENT,mask,0,1000000);
    if (rc) return rc;
    /* A reset/feedback-clock path may have cleared the Qualcomm MCLK mux.
     * Check it while idle before issuing any MMC command. */
    if (h->frequency_hz && h->frequency_hz<=50000000u) {
        rc=ensure_legacy_mclk(h);
        if (rc) return rc;
    }
    if(!h->card_ready){
        rc=latch_legacy_timing(h);
        if(rc)return rc;
    }
    if(h->diagnostic_apply){
        rc=h->diagnostic_apply(h->diagnostic_context);
        if(rc)return rc;
    }
    /* Remove stale command and data interrupts before issuing a command. */
    w32(h,J250F_REGION_HC,HC_INTERRUPT,INT_ALL);
    if (data_len) {
        w16(h,J250F_REGION_HC,HC_BLOCK_SIZE,512);
        w16(h,J250F_REGION_HC,HC_BLOCK_COUNT,1);
        mode=TRANS_BLOCKCNT | (write_data?0:TRANS_READ);
        w16(h,J250F_REGION_HC,HC_TRANSFER_MODE,mode);
    }
    w32(h,J250F_REGION_HC,HC_ARGUMENT,arg);
    command_bits=(uint16_t)response;
    if (response!=RSP_NONE) {
        if (response!=RSP_136 && cmd!=1) command_bits|=CMD_INDEX;
        if (response!=RSP_136 && cmd!=1) command_bits|=CMD_CRC;
        /* CID/CSD 136-bit response has CRC but no command index. */
        if (response==RSP_136) command_bits|=CMD_CRC;
    }
    if (data_len) command_bits|=CMD_DATA;
    w16(h,J250F_REGION_HC,HC_COMMAND,(uint16_t)((cmd<<8)|command_bits));
    sync_io(h);
    /* Wait for command response, or any error, without assuming IRQ wire. */
    intr=0;
    for(n=0;n<100000;n++) {
        rc=j250f_service_power(h);
        if (rc) return rc;
        intr=r32(h,J250F_REGION_HC,HC_INTERRUPT);
        /* SDHCI_QUIRK2_IGNORE_DATATOUT_FOR_R1BCMD from Samsung Linux:
         * Qualcomm can raise DAT timeout (and summary error) on an R1b
         * command with NO DATA. Clear only those two error bits. An actual
         * CRC/index/end-bit error must still abort the transaction. */
        if (response==RSP_48_BUSY && !data_len &&
            (intr & INT_DATA_TIMEOUT) &&
            !(intr & (INT_ERROR_MASK & ~(INT_DATA_TIMEOUT|INT_ERROR)))) {
            w32(h,J250F_REGION_HC,HC_INTERRUPT,INT_DATA_TIMEOUT|INT_ERROR);
            intr &= ~(INT_DATA_TIMEOUT|INT_ERROR);
        }
        if (intr&(INT_COMMAND|INT_ERROR_MASK)) break;
        pause_us(h,10);
    }
    record_command(h,cmd,arg,(uint16_t)((cmd<<8)|command_bits),intr,
                   (intr&INT_ERROR_MASK)?J250F_E_IO:
                   ((intr&INT_COMMAND)?J250F_OK:J250F_E_TIMEOUT));
    h->last_interrupt=intr;
    h->last_present=r32(h,J250F_REGION_HC,HC_PRESENT);
    h->last_vendor=r32(h,J250F_REGION_HC,HC_VENDOR_SPEC);
    h->last_dll=r32(h,J250F_REGION_HC,HC_DLL_CONFIG);
    h->last_clock=h->io.read16(h->context,J250F_REGION_HC,HC_CLOCK);
    if (intr&INT_ERROR_MASK) {
        w32(h,J250F_REGION_HC,HC_INTERRUPT,intr);
        (void)reset(h,RESET_CMD|(data_len?RESET_DAT:0));
        return J250F_E_IO;
    }
    if (!(intr&INT_COMMAND)) {
        (void)reset(h,RESET_CMD|(data_len?RESET_DAT:0));
        return J250F_E_TIMEOUT;
    }
    if (out) {
        if (response==RSP_136) {
            /* SDHCI R2 register response is 8 bits shifted relative to the
             * 128 useful CID/CSD bits, as in Linux sdhci_finish_command. */
            uint32_t raw[4];
            unsigned i;
            for(i=0;i<4;i++) raw[i]=r32(h,J250F_REGION_HC,HC_RESPONSE+i*4);
            out[0]=(raw[3]<<8)|(raw[2]>>24);
            out[1]=(raw[2]<<8)|(raw[1]>>24);
            out[2]=(raw[1]<<8)|(raw[0]>>24);
            out[3]=(raw[0]<<8);
        } else out[0]=response==RSP_NONE?0:r32(h,J250F_REGION_HC,HC_RESPONSE);
    }
    if (response==RSP_48 || response==RSP_48_BUSY) {
        h->last_r1=r32(h,J250F_REGION_HC,HC_RESPONSE);
        if (cmd!=1 && (h->last_r1 & (UINT32_C(0xFDFFA080) | (UINT32_C(1) << 25))))
            return J250F_E_PROTOCOL;
    }
    w32(h,J250F_REGION_HC,HC_INTERRUPT,INT_COMMAND);
    if (data_len) {
        /* SDHCI may indicate DATA_AVAILABLE via INT_READ_READY or
         * PRESENT_STATE. PIO transfer is always one 512-byte block. */
        uint32_t done=0, elapsed=0;
        while(done<data_len && elapsed<1000000) {
            uint32_t st=r32(h,J250F_REGION_HC,HC_PRESENT);
            intr=r32(h,J250F_REGION_HC,HC_INTERRUPT);
            if(intr&INT_ERROR_MASK) break;
            if ((st&(write_data?SPACE_AVAILABLE:DATA_AVAILABLE)) ||
                (intr&(write_data?INT_WRITE_READY:INT_READ_READY))) {
                uint32_t v;
                unsigned j;
                if (write_data) {
                    v=0;
                    for(j=0;j<4;j++) v|=((uint32_t)buffer[done+j])<<(j*8);
                    w32(h,J250F_REGION_HC,HC_BUFFER,v);
                } else {
                    v=r32(h,J250F_REGION_HC,HC_BUFFER);
                    for(j=0;j<4;j++) buffer[done+j]=(uint8_t)(v>>(j*8));
                }
                done+=4;
            } else {pause_us(h,10);elapsed+=10;}
        }
        w32(h,J250F_REGION_HC,HC_INTERRUPT,INT_READ_READY|INT_WRITE_READY);
        if (intr&INT_ERROR_MASK || done!=512) {
            h->last_interrupt=intr;
            (void)reset(h,RESET_CMD|RESET_DAT);
            return (intr&INT_ERROR_MASK)?J250F_E_IO:J250F_E_TIMEOUT;
        }
        for(n=0;n<100000;n++) {
            intr=r32(h,J250F_REGION_HC,HC_INTERRUPT);
            if (intr&(INT_TRANSFER|INT_ERROR_MASK)) break;
            pause_us(h,10);
        }
        h->last_interrupt=intr;
        w32(h,J250F_REGION_HC,HC_INTERRUPT,INT_TRANSFER|INT_ERROR_MASK);
        if (intr&INT_ERROR_MASK) { (void)reset(h,RESET_DAT);return J250F_E_IO; }
        if (!(intr&INT_TRANSFER)) { (void)reset(h,RESET_DAT);return J250F_E_TIMEOUT; }
    }
    if (response==RSP_48_BUSY) {
        rc=poll_mask(h,J250F_REGION_HC,HC_PRESENT,DATA_INHIBIT,0,1000000);
        if (rc) return rc;
    }
    return J250F_OK;
}

static int short_cmd(j250f_emmc *h,unsigned cmd,uint32_t arg,
                     unsigned typ,uint32_t *res)
{
    uint32_t tmp[4]={0,0,0,0};
    int rc=command(h,cmd,arg,typ,0,0,0,tmp);
    if (rc) return rc;
    if (res) *res=tmp[0];
    return J250F_OK;
}

int j250f_init_host(j250f_emmc *h,const j250f_ops *ops,void *context)
{
    uint32_t vendor,caps,version;
    int rc;
    if(!h||!ops||!ops->read8||!ops->read16||!ops->read32||
       !ops->write8||!ops->write16||!ops->write32||!ops->barrier||
       !ops->delay_us||!ops->set_clock_hz||!ops->set_rails||!ops->set_pins)
        return J250F_E_ARGUMENT;
    {
        size_t i;
        uint8_t *p=(uint8_t *)h;
        for(i=0;i<sizeof(*h);i++) p[i]=0;
    }
    h->io=*ops;
    h->context=context;
    h->last_command=255;
    /* Keep the clock running while resetting the Qualcomm core. */
    rc=h->io.set_clock_hz(context,400000);
    if(rc) return J250F_E_IO;
    /* Vendor-specific Qualcomm initialization in Samsung probe(). */
    w32(h,J250F_REGION_HC,HC_VENDOR_SPEC,0xA1Cu);
    w32(h,J250F_REGION_CORE,CORE_MODE,1u|(1u<<13));
    version=r32(h,J250F_REGION_CORE,CORE_VERSION);
    caps=r32(h,J250F_REGION_HC,HC_CAPS);
    /* SDCC5 revision requires caps override, see Samsung source. */
    if ((version>>28)>=1 && (version&0xff)!=0x11 && (version&0xff)!=0x12) {
        caps|=(1u<<18)|(1u<<26); /* 8-bit, 1.8V */
        w32(h,J250F_REGION_HC,HC_VENDOR_CAPS,caps);
    }
    vendor=r32(h,J250F_REGION_HC,HC_VENDOR_SPEC);
    w32(h,J250F_REGION_HC,HC_VENDOR_SPEC,vendor|VENDOR_PAD_SWITCH_EN);
    /* Before enabling PWRCTL requests, clear stale status but do not lie
     * about power state. The normal handler can then validate the rails. */
    rc=j250f_service_power(h);
    if(rc) return rc;
    h->host_ready=1;
    rc=reset(h,RESET_ALL);
    if (rc) return rc;
    /* Qualcomm PWRCTL status requests are masked until CORE_PWR_MASK=0xF;
     * see Samsung probe() after registration of threaded pwr_irq. */
    w32(h,J250F_REGION_CORE,CORE_PWR_MASK,0xFu);
    sync_io(h);
    w32(h,J250F_REGION_HC,HC_INT_ENABLE,INT_ALL);
    w32(h,J250F_REGION_HC,HC_SIGNAL_ENABLE,0); /* polling */
    w8(h,J250F_REGION_HC,HC_TIMEOUT,0x0E);
    rc=j250f_set_frequency(h,400000);
    if(rc) return rc;
    /* VDD card supply is 2.9V (SDHCI 3.0V encoding = 0x0C);
     * VQMMC signalling is separately fixed at 1.8V via PM8917 L5. */
    w8(h,J250F_REGION_HC,HC_POWER,0x0D);
    sync_io(h);
    for (unsigned tries=0;tries<1000;tries++) {
        rc=j250f_service_power(h);
        if(rc) return rc;
        if(h->rails_on && h->pins_active) break;
        pause_us(h,100);
    }
    if (!h->rails_on || !h->pins_active) return J250F_E_POWER;
    pause_us(h,5000);
    return J250F_OK;
}

int j250f_identify_card(j250f_emmc *h)
{
    uint32_t ocr=0;
    int rc,tries;
    if(!h||!h->host_ready||!h->rails_on||!h->pins_active) return J250F_E_NOT_READY;
    if(h->card_ready)return J250F_E_NOT_READY;
    h->cid_valid=0;h->ocr=0;
    /* Fixed 1.8V VQMMC (not a runtime 3V switch); eMMC CMD1 is MMC not SD. */
    rc=short_cmd(h,0,0,RSP_NONE,0);
    if(rc) return rc;
    for(tries=0;tries<100;tries++) {
        rc=short_cmd(h,1,0x40FF8080u,RSP_48,&ocr);
        if(rc) return rc;
        if(ocr&0x80000000u) break;
        pause_us(h,10000);
    }
    if (!(ocr&0x80000000u)) return J250F_E_TIMEOUT;
    h->ocr=ocr;
    h->high_capacity=(uint8_t)!!(ocr&(1u<<30));
    rc=command(h,2,0,RSP_136,0,0,0,h->cid);
    if(rc) return rc;
    h->cid_valid=1;
    return J250F_OK;
}

int j250f_init_card(j250f_emmc *h)
{
    uint32_t rsp=0;
    int rc=j250f_identify_card(h);
    if(rc)return rc;
    h->rca=1;
    rc=short_cmd(h,3,1u<<16,RSP_48,&rsp);
    if(rc) return rc;
    rc=command(h,9,h->rca<<16,RSP_136,0,0,0,h->csd);
    if(rc) return rc;
    rc=short_cmd(h,7,h->rca<<16,RSP_48_BUSY,&rsp);
    if(rc) return rc;
    rc=command(h,8,0,RSP_48,h->ext_csd,0,512,&rsp);
    if(rc) return rc;
    /* Do not address an unknown/zero-sized eMMC: SEC_COUNT in EXT_CSD. */
    h->capacity_sectors=(uint32_t)h->ext_csd[212]|
        ((uint32_t)h->ext_csd[213]<<8)|
        ((uint32_t)h->ext_csd[214]<<16)|
        ((uint32_t)h->ext_csd[215]<<24);
    if (!h->capacity_sectors || !h->high_capacity) {
        /* This transport intentionally does not implement byte addressing
         * for old MMC cards, which are not the target eMMC. */
        return J250F_E_UNSUPPORTED;
    }
    /* Recognition-first integration: keep one-bit, 400kHz mode. No CMD6,
     * partition switching, or GPLL0 vote is needed for CID/capacity/read access.
     * Refuse a hardware partition rather than silently changing selection. */
    if (h->ext_csd[179] & 7u) return J250F_E_UNSUPPORTED;
    h->card_ready=1;
    return J250F_OK;
}

int j250f_read_blocks(j250f_emmc *h,uint32_t sector,uint32_t count,void *buf)
{
    uint32_t rsp;
    uint32_t i;
    int rc;
    if(!h||(!buf&&count)) return J250F_E_ARGUMENT;
    if(!h->card_ready) return J250F_E_NOT_READY;
    if(sector>h->capacity_sectors || count>h->capacity_sectors-sector) return J250F_E_RANGE;
    if(count>UINT32_MAX/512u) return J250F_E_RANGE;
    for(i=0;i<count;i++) {
        rc=command(h,17,sector+i,RSP_48,(uint8_t *)buf+(size_t)i*512u,0,512,&rsp);
        if(rc) return rc;
        if(rsp & ((1u<<19)|(1u<<22))) return J250F_E_PROTOCOL;
    }
    return J250F_OK;
}

/* Private log-sector primitive. This deliberately does not alter the public
 * read-only BlockIo policy or retry a write whose completion is uncertain. */
int j250f_log_write_sector(j250f_emmc *h,uint32_t sector,const void *buffer)
{
    uint32_t rsp=0, status;
    unsigned elapsed;
    uint8_t verify[J250F_BLOCK_SIZE];
    int rc;
    if (!h || !buffer) return J250F_E_ARGUMENT;
    if (!h->host_ready || !h->card_ready) return J250F_E_NOT_READY;
    if (!h->high_capacity) return J250F_E_UNSUPPORTED;
    if (!h->capacity_sectors || sector>=h->capacity_sectors)
        return J250F_E_RANGE;
    rc=command(h,24,sector,RSP_48,(uint8_t *)(uintptr_t)buffer,1,
               J250F_BLOCK_SIZE,&rsp);
    if (rc) return rc;
    if (rsp & ((1u<<19)|(1u<<22))) return J250F_E_PROTOCOL;
    /* R1b programming may continue after the data transfer completes. Require
     * both READY_FOR_DATA and TRAN; never issue another write on a timeout. */
    for (elapsed=0;elapsed<1000000;elapsed+=1000) {
        rc=short_cmd(h,13,h->rca<<16,RSP_48,&status);
        if (rc) return rc;
        if ((status&(1u<<8)) && ((status>>9)&0xFu)==4u) break;
        pause_us(h,1000);
    }
    if (elapsed>=1000000) return J250F_E_TIMEOUT;
    rc=j250f_read_blocks(h,sector,1,verify);
    if (rc) return rc;
    {
        const uint8_t *source=(const uint8_t *)buffer;
        unsigned i;
        for (i=0;i<J250F_BLOCK_SIZE;i++)
            if (verify[i]!=source[i]) return J250F_E_IO;
    }
    return J250F_OK;
}

void j250f_allow_writes(j250f_emmc *h,int allow)
{
    (void)allow;
    if(h) h->writes_enabled=0;
}
int j250f_write_blocks(j250f_emmc *h,uint32_t sector,uint32_t count,const void *buf)
{
    (void)h;(void)sector;(void)count;(void)buf;
    return J250F_E_READ_ONLY;
}
