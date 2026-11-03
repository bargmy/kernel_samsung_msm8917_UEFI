/* Sector-framed text with exact byte lengths and a torn-record checksum.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef J250F_LOG_FORMAT_H
#define J250F_LOG_FORMAT_H
#include <stdint.h>
#include <stddef.h>

static inline uint16_t j250f_log_crc(uint16_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        unsigned bit;
        crc ^= *p++;
        for (bit=0; bit<8; ++bit)
            crc=(uint16_t)((crc>>1)^((crc&1)?0xa001:0));
    }
    return crc;
}
static inline void j250f_log_hex(uint8_t *out, uint32_t value, unsigned digits)
{
    unsigned i;
    for (i=0; i<digits; ++i)
        out[i]=(uint8_t)"0123456789abcdef"[(value>>((digits-i-1)*4))&15];
}
static inline void j250f_log_marker(uint8_t *out, unsigned index, int used, unsigned length)
{
    unsigned i;
    const char *state=used?"USED ":"READY";
    for (i=0; i<32; ++i) out[i]=' ';
    for (i=0; i<10; ++i) out[i]=(uint8_t)"J250FLOG2 "[i];
    j250f_log_hex(out+10,0,4);
    j250f_log_hex(out+14,index,4);
    for (i=0; i<5; ++i) out[19+i]=(uint8_t)state[i];
    if (used) j250f_log_hex(out+25,length,6);
    out[31]='\n';
}
static inline uint16_t j250f_log_record_crc(const uint8_t *record)
{
    uint16_t crc=j250f_log_crc(0xffff,record+14,18);
    return j250f_log_crc(crc,record+32,480);
}
static inline void j250f_log_seal(uint8_t *record, unsigned index, unsigned length)
{
    j250f_log_marker(record,index,1,length);
    j250f_log_hex(record+10,j250f_log_record_crc(record),4);
}
static inline int j250f_log_valid_used(const uint8_t *record, unsigned index)
{
    uint32_t length=0;
    uint8_t expected[32];
    unsigned i;
    for (i=25; i<31; ++i) {
        unsigned ch=record[i];
        if (ch>='0' && ch<='9') ch-='0';
        else if (ch>='a' && ch<='f') ch=ch-'a'+10;
        else return 0;
        length=(length<<4)|ch;
    }
    if (!length || length>480 || index>=2048) return 0;
    j250f_log_marker(expected,index,1,length);
    j250f_log_hex(expected+10,j250f_log_record_crc(record),4);
    for (i=0; i<32; ++i) if (record[i]!=expected[i]) return 0;
    return 1;
}
#endif
