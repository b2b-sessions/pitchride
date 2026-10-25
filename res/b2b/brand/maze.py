import random,math,collections,json
RINGS=[12,12,24,24,24]; RC=1.6; W=1.0
NR=len(RINGS); ROUT=RC+NR*W
def off(r): return math.pi/2-math.pi/RINGS[-1]   # same start for every ring so cells nest; outer cell 0 centred on the bottom
def span(r,k): n=RINGS[r]; a=off(r)+k*2*math.pi/n; return a,a+2*math.pi/n
def mid(r,k): a,b=span(r,k); return (a+b)/2
def rin(r): return RC+r*W
def parent(r,k): return (r-1,k*RINGS[r-1]//RINGS[r]) if r>0 else None
cells=[(r,k) for r in range(NR) for k in range(RINGS[r])]
def nbrs(r,k):
    n=RINGS[r]; out=[(r,(k+1)%n),(r,(k-1)%n)]
    if r>0: out.append(parent(r,k))
    if r<NR-1:
        f=RINGS[r+1]//RINGS[r]; out+= [(r+1,k*f+t) for t in range(f)]
    return out
ENT=(NR-1,0); GOAL='C'
def build(seed):
    R=random.Random(seed); adj=collections.defaultdict(set)
    s=R.choice(cells); seen={s}; st=[s]
    while st:
        u=st[-1]; nb=[v for v in nbrs(*u) if v not in seen]
        if not nb: st.pop(); continue
        v=R.choice(nb); adj[u].add(v); adj[v].add(u); seen.add(v); st.append(v)
    g=(0,R.randrange(RINGS[0])); adj[g].add(GOAL); adj[GOAL].add(g)
    return adj
def solve(adj):
    prev={ENT:None}; q=collections.deque([ENT])
    while q:
        u=q.popleft()
        if u==GOAL: break
        for v in adj[u]:
            if v not in prev: prev[v]=u; q.append(v)
    p=[];u=GOAL
    while u is not None: p.append(u); u=prev[u]
    return p[::-1]
def sdelta(a,b):
    d=(b-a+math.pi)%(2*math.pi)-math.pi; return d
def travel(p):
    t=0
    for a,b in zip(p,p[1:-1]):
        if a[0]==b[0]: t+=abs(sdelta(mid(*a),mid(*b)))
    return t
def turns(p):
    kinds=[]
    for a,b in zip(p,p[1:]):
        if b==GOAL or a==GOAL: kinds.append('in')
        elif a[0]==b[0]: kinds.append('cw' if sdelta(mid(*a),mid(*b))>0 else 'ccw')
        else: kinds.append('in' if b[0]<a[0] else 'out')
    return sum(1 for x,y in zip(kinds,kinds[1:]) if x!=y)
def path_ops(p):
    rm=lambda r:rin(r)+W/2
    ops=[('line',rm(p[0][0]),mid(*p[0]))]; ang=mid(*p[0])
    for i,(A,B) in enumerate(zip(p,p[1:])):
        if B==GOAL: ops.append(('line',0,ang)); break
        ra=rm(A[0]); rb=rm(B[0]); ab=mid(*B)
        nxt=p[i+2] if i+2<len(p) else None
        if A[0]==B[0]:
            ops.append(('arc',ra,ang,ang+sdelta(ang,ab))); ang=ab
        elif B[0]<A[0]:
            ops.append(('line',rb,ang))
        else:
            ops.append(('arc',ra,ang,ang+sdelta(ang,ab))); ang=ab; ops.append(('line',rb,ang))
    return ops
def one_way(p):
    sg=set()
    for o in path_ops(p):
        if o[0]=='arc' and abs(o[3]-o[2])>1e-6: sg.add(o[3]>o[2])
    return len(sg)<=1
def monotonic(p):
    sign=None
    for A,B in zip(p,p[1:]):
        if B==GOAL: continue
        if B[0]>A[0]: return False          # never back out
        if A[0]==B[0]:
            d=1 if sdelta(mid(*A),mid(*B))>0 else -1
            if sign is None: sign=d
            elif d!=sign: return False      # never turn round
    return True
def pick():
    for seed in range(1,200000):
        adj=build(seed); p=solve(adj)
        if 10<=len(p)<=14 and 3<=turns(p)<=4 and monotonic(p) and one_way(p) and travel(p)<1.1*math.pi:
            return seed,adj,p
seed,adj,path=pick()
def svg(size,flower,anim=False,uid="m",lit=True,COP="#E3A06B",ORA="#F2511E"):
    sc=(size/2*0.94)/ROUT; cx=cy=size/2; sw=0.15*sc
    P=lambda r,a:(cx+r*sc*math.cos(a),cy+r*sc*math.sin(a))
    def arc(r,a,b):
        x0,y0=P(r,a); x1,y1=P(r,b); d=b-a
        return f'M{x0:.1f} {y0:.1f}A{r*sc:.1f} {r*sc:.1f} 0 {1 if abs(d)>math.pi else 0} {1 if d>0 else 0} {x1:.1f} {y1:.1f}'
    d=[]
    for (r,k) in cells:
        a,b=span(r,k)
        # inner wall (towards centre / parent)
        inner_open = (GOAL in adj[(r,k)]) if r==0 else (parent(r,k) in adj[(r,k)])
        if not inner_open: d.append(arc(rin(r),a,b))
        # radial wall on its far edge
        if (r,(k+1)%RINGS[r]) not in adj[(r,k)]:
            x0,y0=P(rin(r),b); x1,y1=P(rin(r)+W,b); d.append(f'M{x0:.1f} {y0:.1f}L{x1:.1f} {y1:.1f}')
        if r==NR-1 and (r,k)!=ENT: d.append(arc(ROUT,a,b))
    walls=''.join(d)
    # solution path
    seg=[]; x,y=P(ROUT+0.7,math.pi/2); seg.append(f'M{x:.1f} {y:.1f}')
    rm=lambda r:rin(r)+W/2
    cur=(ROUT+0.7,math.pi/2)
    def lineto(r,a):
        x,y=P(r,a); seg.append(f'L{x:.1f} {y:.1f}')
    def arcto(r,a0,a1):
        if abs(a1-a0)<1e-6: return
        x,y=P(r,a1); d=a1-a0
        seg.append(f'A{r*sc:.1f} {r*sc:.1f} 0 {1 if abs(d)>math.pi else 0} {1 if d>0 else 0} {x:.1f} {y:.1f}')
    fs=RC*0.5*sc/10
    ops=path_ops(path); end_ang=ops[-1][2]
    ops[-1]=('line',6.0*fs/sc,end_ang)        # stop inside the petal that points back along the line
    for o in ops:
        if o[0]=='line': lineto(o[1],o[2])
        else: arcto(o[1],o[2],o[3])
    rot=math.degrees(end_ang)+90
    sp=''.join(seg)
    pcls=' class="pp"' if anim else ''; fcls=' class="fl"' if anim else ''
    pl=f'<path{pcls} d="{sp}" pathLength="1" fill="none" stroke="{ORA}" stroke-width="{sw*1.1:.2f}" stroke-linecap="round" stroke-linejoin="round"/>' if lit else ''
    return (f'<svg viewBox="0 0 {size} {size}" width="100%" height="100%" xmlns="http://www.w3.org/2000/svg" overflow="visible">'
            f'<path d="{walls}" fill="none" stroke="{COP}" stroke-width="{sw:.2f}" stroke-linecap="round" stroke-linejoin="round"/>{pl}'
            f'<g transform="translate({cx} {cy}) rotate({rot:.2f}) scale({fs:.3f})"><g{fcls} fill="{ORA}">{flower}</g></g></svg>')
json.dump({"rings":RINGS,"rc":RC,"seed":seed,"path":[p if p!=GOAL else "C" for p in path]},open('/dev/null','w'))
if __name__=="__main__": print('seed',seed,'steps',len(path),'turns',turns(path),path)
