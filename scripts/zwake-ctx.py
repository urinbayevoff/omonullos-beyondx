import os,sys,time
def snap():
    d={}
    for p in os.listdir('/proc'):
        if not p.isdigit(): continue
        try:
            for t in os.listdir(f'/proc/{p}/task'):
                try:
                    s=open(f'/proc/{p}/task/{t}/status').read()
                    nm=s.split('\n',1)[0].split('\t',1)[1]
                    v=int(s.split('voluntary_ctxt_switches:\t')[1].split('\n')[0])
                    nv=int(s.split('nonvoluntary_ctxt_switches:\t')[1].split('\n')[0])
                    pn=open(f'/proc/{p}/comm').read().strip()
                    d[(p,t)]=(pn,nm,v+nv)
                except Exception: pass
        except Exception: pass
    return d
a=snap(); t0=time.time(); time.sleep(float(sys.argv[1])); b=snap(); dt=time.time()-t0
r=[]
for k,(pn,nm,v) in b.items():
    if k in a: 
        dv=v-a[k][2]
        if dv: r.append((dv/dt,pn,nm,k[0],k[1]))
r.sort(reverse=True)
print(f"total {sum(x[0] for x in r):.0f} switches/s over {dt:.1f}s")
for x in r[:30]: print(f"{x[0]:7.1f}/s  {x[1]:16} {x[2]:18} pid {x[3]} tid {x[4]}")
