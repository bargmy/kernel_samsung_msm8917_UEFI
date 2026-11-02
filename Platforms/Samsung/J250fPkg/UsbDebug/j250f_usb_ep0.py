#!/usr/bin/env python3
"""Experimental J250F USB EP0 gadget client. Requires PyUSB and libusb."""
import argparse, json, struct, time
VID, PID = 0x1209, 0x8917
WORDS=('magic','version','result_signed','host_ready','card_ready',
       'frequency_hz','last_command','last_interrupt','last_r1',
       'last_present','last_vendor','last_dll','last_clock',
       'irq_live','present_live','vendor_live')
def decode(data):
    if len(data)!=64: raise ValueError("Unexpected packet length")
    values=struct.unpack('<16I',bytes(data))
    if values[:2]!=(0x4742444a,1): raise ValueError("Invalid magic/version")
    obj=dict(zip(WORDS,values))
    obj['result_signed']=struct.unpack('<i',struct.pack('<I',values[2]))[0]
    return obj
def main():
    p=argparse.ArgumentParser()
    p.add_argument('action',choices=['ping','snapshot','probe','monitor'])
    p.add_argument('--rate',choices=[400000],type=int,default=400000)
    p.add_argument('--yes',action='store_true',help='Allow controller/card reinitialization')
    p.add_argument('--vid',type=lambda x:int(x,0),default=VID)
    p.add_argument('--pid',type=lambda x:int(x,0),default=PID)
    p.add_argument('--count',type=int,default=30)
    p.add_argument('--interval',type=float,default=1.0)
    p.add_argument('--log')
    a=p.parse_args()
    import usb.core
    dev=usb.core.find(idVendor=a.vid,idProduct=a.pid)
    if dev is None: p.error("J250F gadget not detected")
    if a.action=='probe' and not a.yes: p.error("probe resets card state; pass --yes")
    if a.count<0 or a.interval<0: p.error("count and interval must be nonnegative")
    dev.set_configuration(1)
    f=open(a.log,'a',encoding='utf-8') if a.log else None
    try:
        for i in range(a.count if a.action=='monitor' else 1):
            if a.action=='ping':
                out={'reply':bytes(dev.ctrl_transfer(0xc0,0x70,0,0,12,timeout=30000)).rstrip(b'\0').decode('ascii')}
            else:
                req=0x72 if a.action=='probe' else 0x71
                value={400000:1}[a.rate] if req==0x72 else 0
                out=decode(dev.ctrl_transfer(0xc0,req,value,0,64,timeout=30000))
            out['timestamp']=time.time()
            line=json.dumps(out,sort_keys=True)
            print(line)
            if f: f.write(line+'\n');f.flush()
            if a.action=='monitor' and i+1<a.count: time.sleep(a.interval)
    finally:
        if f: f.close()
if __name__=='__main__':main()
