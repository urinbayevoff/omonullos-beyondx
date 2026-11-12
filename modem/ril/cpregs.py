import mmap,os,struct
# Read the CP mailbox (0x15920000) and the PMU CP registers, read only.
fd=os.open("/dev/mem",os.O_RDONLY|os.O_SYNC)
m=mmap.mmap(fd,0x1000,mmap.MAP_SHARED,mmap.PROT_READ,offset=0x15920000)
r=lambda o: struct.unpack_from("<I",m,o)[0]
p=mmap.mmap(fd,0x4000,mmap.MAP_SHARED,mmap.PROT_READ,offset=0x15860000)
q=lambda o: struct.unpack_from("<I",p,o)[0]
print("INTSR0 %08x INTSR1 %08x ISSR0-3 %x %x %x %x | CP_STATUS %x CP_OUT %08x CP_IN %08x"%(r(0x14),r(0x28),r(0x80),r(0x84),r(0x88),r(0x8c),q(0x3004),q(0x3020),q(0x3024)))
