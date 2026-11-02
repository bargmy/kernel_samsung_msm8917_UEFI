#!/usr/bin/env python3
"""Exercise the real portable ADB engine with an actual adb host over TCP.
USB DMA/PHY and EFI execution are not emulated by this check.
"""
import argparse, ctypes, pathlib, socket, struct, subprocess, tempfile, threading
ROOT=pathlib.Path(__file__).resolve().parents[1]
CORE=ROOT/'Platforms/Samsung/J250fPkg/J250fEmmcDxe/Core'
BRIDGE=r'''
#include "j250f_adb.h"
#include <string.h>
static jadb a; static unsigned char file[JADB_FILE_MAX]; static size_t used; static int complete;
static size_t exec(void*c,const char*s,char*b,size_t cap){const char*r=!strcmp(s,"emmc status")?"fixture: host=1 card=0 CMD=2 IRQ=00028000\r\n":"fixture command\r\n";size_t n=strlen(r);if(n>cap)n=cap;memcpy(b,r,n);return n;}
static int begin(void*c,const char*p){used=0;complete=0;return strncmp(p,"/ram/",5)?-1:0;}
static int data(void*c,const uint8_t*p,size_t n){if(n>sizeof(file)-used)return -1;memcpy(file+used,p,n);used+=n;return 0;}
static int done(void*c){complete=1;return 0;}
static int stat(void*c,const char*p,uint32_t*m,uint32_t*s){*m=!strcmp(p,"/ram")?0040777:0;*s=0;return 0;}
void reset(void){jadb_ops o={0,exec,begin,data,done,stat};jadb_init(&a,&o);}
int feed(const void*p,size_t n){return jadb_feed(&a,p,n);}
size_t tx(void*b){size_t n;const void*p=jadb_tx(&a,&n);if(n){memcpy(b,p,n);jadb_tx_done(&a);}return n;}
size_t size(void){return complete?used:0;}
int match(const void*p,size_t n){return complete&&used==n&&!memcmp(p,file,n);}
'''
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--adb',required=True);ap.add_argument('--ubsan',action='store_true');args=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='j250f-adb-test-') as td:
        td=pathlib.Path(td);(td/'bridge.c').write_text(BRIDGE)
        subprocess.run(['cc']+(['-fsanitize=undefined','-fno-sanitize-recover=all'] if args.ubsan else [])+['-shared','-fPIC','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-I'+str(CORE),str(CORE/'j250f_adb.c'),str(td/'bridge.c'),'-o',str(td/'bridge.so')],check=True)
        lib=ctypes.CDLL(str(td/'bridge.so'));lib.feed.argtypes=[ctypes.c_void_p,ctypes.c_size_t];lib.tx.argtypes=[ctypes.c_void_p];lib.tx.restype=ctypes.c_size_t;lib.size.restype=ctypes.c_size_t;lib.match.argtypes=[ctypes.c_void_p,ctypes.c_size_t]
        # Fragments, invalid magic and oversized advertised payloads.
        lib.reset();assert lib.feed(bytes(24),24)==-1
        lib.reset();hdr=struct.pack('<6I',0x4e584e43,0x1000000,4096,4097,0,0x4e584e43^0xffffffff);assert lib.feed(hdr,len(hdr))==-1
        server=socket.socket();server.bind(('127.0.0.1',0));server.listen();port=server.getsockname()[1];errors=[];stop=threading.Event()
        def serve():
            try:
                conn,_=server.accept();conn.settimeout(10);lib.reset();buf=ctypes.create_string_buffer(8192)
                with conn:
                    while not stop.is_set():
                        data=conn.recv(8192)
                        if not data:break
                        # Every packet is fragmented to exercise streaming parser.
                        for start in range(0,len(data),7):
                            chunk=data[start:start+7]
                            if lib.feed(chunk,len(chunk)):raise AssertionError('engine rejected host packet')
                            while True:
                                n=lib.tx(buf)
                                if not n:break
                                conn.sendall(buf.raw[:n])
            except Exception as e:
                if not stop.is_set():errors.append(e)
        thread=threading.Thread(target=serve,daemon=True);thread.start()
        control=socket.socket();control.bind(('127.0.0.1',0));control_port=control.getsockname()[1];control.close()
        adb=[args.adb,'-P',str(control_port)];serial=f'127.0.0.1:{port}'
        def run(*cmd):
            p=subprocess.run(adb+list(cmd),capture_output=True,text=True,timeout=20)
            if p.returncode:raise AssertionError((cmd,p.stdout,p.stderr,errors))
            return p.stdout
        try:
            assert 'connected' in run('connect',serial)
            out=run('-s',serial,'shell','emmc status');assert 'IRQ=00028000' in out,out
            # Large file forces many ADB WRTE frames and fragmented SYNC DATA.
            payload=bytes(range(256))*1031;(td/'test.efi').write_bytes(payload)
            run('-s',serial,'push',str(td/'test.efi'),'/ram/test.efi')
            assert lib.match(payload,len(payload))==1,(lib.size(),len(payload))
            p=subprocess.run(adb+['-s',serial,'shell'],input='emmc status\nexit\n',capture_output=True,text=True,timeout=20)
            assert 'IRQ=00028000' in p.stdout,(p.stdout,p.stderr)
            assert not errors,errors
            print('PASS: real adb CNXN, one-shot and interactive shell, multi-frame SYNC push, fragmented packets, malformed headers')
        finally:
            stop.set();subprocess.run(adb+['disconnect',serial],capture_output=True);subprocess.run(adb+['kill-server'],capture_output=True);server.close();thread.join(timeout=2)
if __name__=='__main__':main()
