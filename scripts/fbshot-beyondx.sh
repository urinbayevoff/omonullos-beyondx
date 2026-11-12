#!/bin/sh
# fbshot-beyondx.sh <out.png>: the Galaxy S10 5G's screen, read from the
# bootloader framebuffer that simpledrm copies every frame into (0xca000000,
# 1440x3040 BGRA; /dev/mem read() refuses it, mmap works), shrunk to 480 wide.
# Needs the root system up (SSH key ~/.ssh/omonullos_ed25519). ztMETRO has no
# wlr-screencopy, so grim cannot do this. Added for OmonullOS, October 2026.
K=~/.ssh/omonullos_ed25519; O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o BatchMode=yes -o LogLevel=ERROR"
ssh $O -i $K root@172.16.42.1 'python3 -c "
import mmap,os,sys,zlib
f=os.open(\"/dev/mem\",os.O_RDONLY|os.O_SYNC)
n=1440*3040*4
m=mmap.mmap(f,n,mmap.MAP_SHARED,mmap.PROT_READ,offset=0xca000000)
sys.stdout.buffer.write(zlib.compress(bytes(m[:n]),1))
"' > "$1.z"
python3 -c "
import zlib,sys
from PIL import Image
d=zlib.decompress(open(sys.argv[1]+'.z','rb').read())
Image.frombuffer('RGBA',(1440,3040),d,'raw','BGRA',0,1).convert('RGB').resize((480,1013)).save(sys.argv[1])
" "$1" && rm -f "$1.z"
