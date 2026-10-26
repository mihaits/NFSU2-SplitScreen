"""Generate a self-contained HTML map explorer of the tracks' position markers (trackmarkers.py) on the game's own
TRACKMAP textures, for picking Free Run spawn spots. Read-only; needs numpy and Pillow.

python trackmap_explorer.py [-o path]   (default <repo>/build/trackmap.html, then open it in a browser)

In the page: drag to pan, wheel to zoom, hover a marker for its values, click a marker to pick it or click anywhere
to place a spot (z estimated from the nearest markers), Shift+drag or right-drag from the spot (or Q/E) to turn it.
The panel gives the kFreeRunStarts line for free_run.cpp and the [DEBUG] FreeRunStart line for the ini.
"""
import argparse, base64, io, json, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import trackmarkers as tm

# Maps to embed (texture and calibration of that track). 4701 and 4711 share a calibration but each draws only its own
# speedway; the 4300 and 4600 overview textures are nearly empty.
MAPS = [(4000, 'Bayview (city)'), (4701, 'Speedway Industrial'), (4711, 'Speedway Airport'),
        (4311, 'Stadium drift'), (4301, 'Vacant lot drift'), (4200, 'Drag strips'),
        (4601, 'Street X city'), (4605, 'Street X industrial')]
# Track word for kFreeRunStarts per region (the venue base tracks free_run.cpp uses).
BASE_TRACK = {'L4RA': 'FREE_RUN_TRACK', 'L4RB': '4200', 'L4RC': '4300', 'L4RD': '4400', 'L4RF': '4600',
              'L4RG': '4700'}
DEFAULT_GRID = [[0, 0], [0, -3.5], [0, 3.5], [0, -7], [5.7, 0], [5.7, -3.5]]


def start_grid():
    """kStartGrid from free_run.cpp when the checkout has it, so the preview follows the plugin."""
    src = os.path.join(HERE, '..', 'src', 'parts', 'free_run.cpp')
    try:
        m = re.search(r'kStartGrid\[\d+\]\[2\]\s*=\s*\{(.*?)\};', open(src).read(), re.S)
        pairs = re.findall(r'\{\s*([-\d.]+)f?\s*,\s*([-\d.]+)f?\s*\}', m.group(1))
        return [[float(a), float(b)] for a, b in pairs] or DEFAULT_GRID
    except (OSError, AttributeError):
        return DEFAULT_GRID


def png_data_url(rgb):
    from PIL import Image
    buf = io.BytesIO()
    Image.fromarray(rgb).save(buf, 'PNG', optimize=True)
    return 'data:image/png;base64,' + base64.b64encode(buf.getvalue()).decode()


def build():
    tracks, markers = tm.load_all()
    by_id = {t['id']: t for t in tracks}
    maps = []
    for tid, label in MAPS:
        rgb = tm.trackmap_rgb(tid)
        if rgb is None or tid not in by_id:
            continue
        t = by_id[tid]
        maps.append({'id': tid, 'label': label, 'map': t['map'], 'base': BASE_TRACK.get(t['region'], str(tid)),
                     'img': png_data_url(rgb)})
    # Tuning and replay venues (L4RH, L4RR) use their own local coordinates: not on these maps.
    markers = [m for m in markers if m['region'] in BASE_TRACK]
    types = sorted({m['type'] for m in markers})
    data = {
        'tracks': [{'id': t['id'], 'name': t['name'], 'region': t['region'], 'map': t['map']} for t in tracks
                   if t['region'] in BASE_TRACK],
        'types': types,
        'markers': [[m['track'], types.index(m['type']), m['index'], m['x'], m['y'], m['z'], m['heading']]
                    for m in markers],
        'maps': maps, 'base': BASE_TRACK, 'grid': start_grid(),
    }
    return PAGE.replace('/*DATA*/null', json.dumps(data, separators=(',', ':')))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('-o', '--out', default=os.path.normpath(os.path.join(HERE, '..', 'build', 'trackmap.html')))
    a = ap.parse_args()
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    html = build()
    with open(a.out, 'w', encoding='utf-8') as f:
        f.write(html)
    print('%s (%d KB)' % (a.out, len(html) // 1024))


PAGE = r'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>NFSU2 Track Markers</title>
<style>
:root{--bg:#15181b;--panel:#1d2125;--line:#2e343a;--text:#d9dde1;--muted:#8b949c;--accent:#6cb8ff;
--fwd:#4fd08a;--back:#f08b5a;--sline:#6cb8ff;--fline:#e05f8c;--shop:#f2d14b;--spot:#ffffff}
*{box-sizing:border-box}
html,body{margin:0;height:100%;background:var(--bg);color:var(--text);font:13px/1.4 system-ui,Segoe UI,sans-serif}
#app{display:flex;height:100%}
#side{width:340px;flex:none;overflow:auto;background:var(--panel);border-right:1px solid var(--line);padding:12px}
#view{flex:1;position:relative;overflow:hidden;cursor:crosshair}
canvas{display:block;width:100%;height:100%}
h1{font-size:15px;margin:0 0 8px}
h2{font-size:11px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:14px 0 6px}
label{display:flex;align-items:center;gap:6px;margin:2px 0;cursor:pointer}
select,input[type=text],input[type=number]{width:100%;background:#111417;color:var(--text);border:1px solid var(--line);
border-radius:4px;padding:4px 6px;font:inherit}
input[type=number]{width:auto;min-width:0}
.row{display:flex;gap:6px;align-items:center}
.row>*{flex:1}
.sw{width:10px;height:10px;border-radius:2px;flex:none}
.fields{display:grid;grid-template-columns:auto 1fr auto 1fr;gap:4px 6px;align-items:center}
.fields input{width:100%}
pre{background:#111417;border:1px solid var(--line);border-radius:4px;padding:6px;margin:4px 0;white-space:pre-wrap;
word-break:break-all;font:12px/1.35 Consolas,monospace;user-select:all}
button{background:#2a3138;color:var(--text);border:1px solid var(--line);border-radius:4px;padding:4px 8px;font:inherit;
cursor:pointer}
button:hover{border-color:var(--accent)}
.muted{color:var(--muted)}
.warn{color:#f0b35a}
#tip{position:absolute;pointer-events:none;background:rgba(17,20,23,.94);border:1px solid var(--line);border-radius:4px;
padding:5px 7px;font:12px/1.35 Consolas,monospace;display:none;white-space:pre;z-index:2}
#status{position:absolute;left:8px;bottom:8px;background:rgba(17,20,23,.85);border-radius:4px;padding:3px 7px;
font:12px Consolas,monospace;pointer-events:none}
#zoom{position:absolute;right:8px;top:8px;display:flex;flex-direction:column;gap:4px}
#zoom button{width:30px;height:30px;padding:0;font-size:16px}
textarea{width:100%;height:110px;background:#111417;color:var(--text);border:1px solid var(--line);border-radius:4px;
font:12px/1.35 Consolas,monospace}
@media (max-width:700px){#app{flex-direction:column}#side{width:auto;height:45%;border-right:0;
border-bottom:1px solid var(--line)}}
</style></head><body>
<div id="app">
<div id="side">
  <h1>NFSU2 track markers</h1>
  <div class="muted">Drag to pan, wheel to zoom. Click a marker to pick it, or click anywhere to place a spot.
  Shift+drag or right-drag turns the spot; Q / E turn it by 1&deg; (Shift: 15&deg;).</div>
  <h2>Map</h2>
  <select id="map"></select>
  <h2>Track</h2>
  <input type="text" id="search" placeholder="filter: id, name, SH, Drift...">
  <select id="track" size="7" style="margin-top:4px"></select>
  <h2>Markers</h2>
  <div id="cats"></div>
  <label><input type="checkbox" id="spot0" checked> starting positions: spot 0 only</label>
  <label><input type="checkbox" id="grid" checked> show the 6-car grid at the spot</label>
  <h2>Spot</h2>
  <div id="src" class="muted">nothing picked</div>
  <div class="fields" style="margin-top:6px">
    <span>x</span><input type="number" id="fx" step="0.1"><span>y</span><input type="number" id="fy" step="0.1">
    <span>z</span><input type="number" id="fz" step="0.1"><span>h</span><input type="number" id="fh" step="1" min="0" max="65535">
  </div>
  <input type="range" id="hslider" min="0" max="65535" step="1" style="width:100%;margin-top:6px">
  <div id="zinfo" class="muted"></div>
  <div class="row" style="margin-top:6px"><input type="text" id="name" placeholder="name for kFreeRunStarts" value="Spot"></div>
  <h2>free_run.cpp <button data-copy="outc" style="float:right;padding:0 6px">copy</button></h2>
  <pre id="outc"></pre>
  <h2>ini [DEBUG] <button data-copy="outi" style="float:right;padding:0 6px">copy</button></h2>
  <pre id="outi"></pre>
  <div class="row"><button id="add">Add to list</button><button data-copy="list">Copy list</button></div>
  <textarea id="list" spellcheck="false"></textarea>
</div>
<div id="view"><canvas id="cv"></canvas><div id="tip"></div><div id="status"></div>
<div id="zoom"><button id="zin">+</button><button id="zout">&minus;</button><button id="zfit" title="fit map">&#9635;</button></div></div>
</div>
<script>
const D=/*DATA*/null;
const CATS=[
 {key:'fwd',label:'Forward starting positions',color:'--fwd',test:t=>t==='ForwardStartingPosition'},
 {key:'back',label:'Backward starting positions',color:'--back',test:t=>t==='BackwardStartingPosition'},
 {key:'sline',label:'Start lines',color:'--sline',test:t=>/StartLine$/.test(t)},
 {key:'fline',label:'Finish lines',color:'--fline',test:t=>/FinishLine$/.test(t)},
 {key:'shop',label:'Shops and crib (track 4000)',color:'--shop',test:t=>!/(Position|Line)$/.test(t)}];
const css=n=>getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const typeCat=D.types.map(t=>CATS.findIndex(c=>c.test(t)));
const trackById=new Map(D.tracks.map(t=>[t.id,t]));
const M=D.markers.map(a=>({track:a[0],type:D.types[a[1]],cat:typeCat[a[1]],index:a[2],x:a[3],y:a[4],z:a[5],h:a[6]}));
const cv=document.getElementById('cv'),ctx=cv.getContext('2d'),view=document.getElementById('view');
const $=id=>document.getElementById(id);
let mapIdx=0,scale=1,cx=0,cy=0,trackSel=0,hover=null,spot=null,catOn=CATS.map(()=>true);
const imgs=D.maps.map(m=>{const i=new Image();i.src=m.img;i.onload=draw;return i});
const fmt=v=>(Math.round(v*10)/10).toFixed(1);
const deg=h=>(h*360/65536).toFixed(1);
const regionOf=id=>(trackById.get(id)||{}).region;
const sameMap=(a,b)=>a&&b&&a.every((v,i)=>Math.abs(v-b[i])<0.05);

// map and view
D.maps.forEach((m,i)=>$('map').add(new Option(m.label+' ('+m.id+')',i)));
$('map').onchange=()=>{mapIdx=+$('map').value;fit()};
function fit(){const m=D.maps[mapIdx].map,w=view.clientWidth,h=view.clientHeight;
 scale=Math.min(w,h)/m[2];cx=m[0]+m[2]/2;cy=m[1]-m[2]/2;draw()}
const toS=(x,y)=>[(x-cx)*scale+cv.width/devicePixelRatio/2,(cy-y)*scale+cv.height/devicePixelRatio/2];
const toW=(sx,sy)=>[(sx-cv.width/devicePixelRatio/2)/scale+cx,cy-(sy-cv.height/devicePixelRatio/2)/scale];
function resize(){cv.width=view.clientWidth*devicePixelRatio;cv.height=view.clientHeight*devicePixelRatio;
 if(!scale)fit();else draw()}   // the page can load hidden (zero size): fit once it has one
function zoomAt(f,sx,sy){const [wx,wy]=toW(sx,sy);scale*=f;const [nx,ny]=toW(sx,sy);cx+=wx-nx;cy+=wy-ny;draw()}

// filters
CATS.forEach((c,i)=>{const l=document.createElement('label');
 l.innerHTML=`<input type="checkbox" checked><span class="sw" style="background:var(${c.color})"></span>${c.label}`+
 ` <span class="muted">${M.filter(m=>m.cat===i).length}</span>`;
 l.querySelector('input').onchange=e=>{catOn[i]=e.target.checked;draw()};$('cats').append(l)});
function fillTracks(){const q=$('search').value.toLowerCase(),sel=$('track');sel.innerHTML='';
 sel.add(new Option('All tracks',0));
 D.tracks.filter(t=>!q||(t.id+' '+t.name).toLowerCase().includes(q)).forEach(t=>sel.add(new Option(t.name,t.id)));
 sel.value=trackSel;if(sel.selectedIndex<0)sel.selectedIndex=0}
$('search').oninput=fillTracks;
$('track').onchange=()=>{trackSel=+$('track').value;const t=trackById.get(trackSel);
 if(t&&!sameMap(t.map,D.maps[mapIdx].map)){const i=D.maps.findIndex(m=>sameMap(m.map,t.map));
  if(i>=0){mapIdx=i;$('map').value=i;fit()}}
 const ms=t?visible():[];
 if(ms.length){const xs=ms.map(m=>m.x),ys=ms.map(m=>m.y),x0=Math.min(...xs),x1=Math.max(...xs),y0=Math.min(...ys),
  y1=Math.max(...ys);cx=(x0+x1)/2;cy=(y0+y1)/2;   // frame the track's markers, at least 800 m across
  scale=Math.min(view.clientWidth,view.clientHeight)/Math.max(800,(x1-x0)*1.3,(y1-y0)*1.3)}
 draw()};
$('spot0').onchange=draw;$('grid').onchange=draw;
function visible(){const s0=$('spot0').checked;
 return M.filter(m=>catOn[m.cat]&&(!trackSel||m.track===trackSel)&&!(s0&&m.cat<2&&m.index!==0))}

// drawing
function arrow(x,y,h,col,r,sel){const a=h*Math.PI*2/65536,[sx,sy]=toS(x,y),fx=Math.cos(a),fy=-Math.sin(a);
 ctx.beginPath();ctx.moveTo(sx+fx*r*1.6,sy+fy*r*1.6);ctx.lineTo(sx-fx*r+fy*r,sy-fy*r-fx*r);
 ctx.lineTo(sx-fx*r*.4,sy-fy*r*.4);ctx.lineTo(sx-fx*r-fy*r,sy-fy*r+fx*r);ctx.closePath();
 ctx.fillStyle=col;ctx.fill();ctx.lineWidth=sel?2:1;ctx.strokeStyle=sel?'#fff':'rgba(0,0,0,.7)';ctx.stroke()}
function car(x,y,h){const a=h*Math.PI*2/65536,fx=Math.cos(a),fy=Math.sin(a),L=2.3,W=0.95;
 const pts=[[L,W],[L,-W],[-L,-W],[-L,W]].map(([f,s])=>toS(x+f*fx-s*fy,y+f*fy+s*fx));
 ctx.beginPath();pts.forEach((p,i)=>i?ctx.lineTo(...p):ctx.moveTo(...p));ctx.closePath();
 ctx.fillStyle='rgba(255,255,255,.35)';ctx.fill();ctx.strokeStyle='#fff';ctx.lineWidth=1;ctx.stroke()}
function draw(){if(!cv.width)return;ctx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);
 ctx.fillStyle=css('--bg');ctx.fillRect(0,0,cv.width,cv.height);
 const m=D.maps[mapIdx],[x0,y0]=toS(m.map[0],m.map[1]);ctx.imageSmoothingEnabled=true;
 if(imgs[mapIdx].complete)ctx.drawImage(imgs[mapIdx],x0,y0,m.map[2]*scale,m.map[2]*scale);
 const r=Math.max(5,Math.min(9,scale*3));
 visible().forEach(mk=>arrow(mk.x,mk.y,mk.h,css(CATS[mk.cat].color),r,mk===hover));
 if(spot){if($('grid').checked&&scale>0.6){const a=spot.h*Math.PI*2/65536,fx=Math.cos(a),fy=Math.sin(a);
   D.grid.forEach(([b,s])=>car(spot.x-b*fx-s*fy,spot.y-b*fy+s*fx,spot.h))}
  arrow(spot.x,spot.y,spot.h,css('--spot'),r+3,true)}
}

// picking
function nearest(sx,sy){let best=null,bd=12*12;
 for(const m of visible()){const [px,py]=toS(m.x,m.y),d=(px-sx)**2+(py-sy)**2;if(d<bd){bd=d;best=m}}return best}
function zEstimate(x,y){const ns=M.map(m=>[Math.hypot(m.x-x,m.y-y),m]).sort((a,b)=>a[0]-b[0]).slice(0,6);
 if(ns[0][0]<0.5)return{z:ns[0][1].z,near:ns[0]};let w=0,z=0;
 for(const [d,m] of ns){const k=1/(d*d);w+=k;z+=k*m.z}return{z:z/w,near:ns[0]}}
function info(m){const t=trackById.get(m.track);
 return `${m.track} ${t?t.name:''}\n${m.type} #${m.index}\nx ${fmt(m.x)}  y ${fmt(m.y)}  z ${fmt(m.z)}\nheading ${m.h} (${deg(m.h)}°)`}
function setSpot(s){spot=s;update();draw()}
function update(){if(!spot)return;$('fx').value=fmt(spot.x);$('fy').value=fmt(spot.y);$('fz').value=fmt(spot.z);
 $('fh').value=spot.h;$('hslider').value=spot.h;
 $('src').innerHTML=spot.marker?`marker: ${info(spot.marker).split('\n').slice(0,2).join(', ')}`:
  `placed on the ${D.maps[mapIdx].label} map`;
 const e=spot.est;$('zinfo').innerHTML=e?`z estimated from the nearest markers; nearest ${fmt(e.near[0])} m away `+
  `(${e.near[1].track} ${e.near[1].type}, z ${fmt(e.near[1].z)})`+(e.near[0]>40?
  ' <span class="warn">far from any marker: check z in game</span>':''):'';
 const base=spot.base,name=($('name').value||'Spot').replace(/"/g,''),src=spot.marker?
  `   // ${spot.marker.track} ${spot.marker.type} ${spot.marker.index}`:'';
 $('outc').textContent=`{"${name}", ${base}, ${fmt(spot.x)}f, ${fmt(spot.y)}f, ${fmt(spot.z)}f, ${spot.h}},${src}`;
 $('outi').textContent=`FreeRunStart = ${fmt(spot.x)} ${fmt(spot.y)} ${fmt(spot.z)} ${spot.h}`}
['fx','fy','fz','fh'].forEach((id,i)=>$(id).onchange=()=>{if(!spot)return;const v=+$(id).value;
 if(i===3)spot.h=((Math.round(v)%65536)+65536)%65536;else spot['xyz'[i]]=v;
 if(i<2){spot.est=zEstimate(spot.x,spot.y);}spot.marker=null;update();draw()});
$('hslider').oninput=()=>{if(spot){spot.h=+$('hslider').value;update();draw()}};
$('name').oninput=update;
$('add').onclick=()=>{if(spot)$('list').value+=$('outc').textContent+'\n'};
document.querySelectorAll('[data-copy]').forEach(b=>b.onclick=()=>{const el=$(b.dataset.copy);
 navigator.clipboard.writeText(el.value!==undefined?el.value:el.textContent);b.textContent='copied';
 setTimeout(()=>b.textContent=b.dataset.copy==='list'?'Copy list':'copy',900)});

// mouse
let drag=null;
cv.addEventListener('contextmenu',e=>e.preventDefault());
cv.addEventListener('mousedown',e=>{const turn=spot&&(e.shiftKey||e.button===2);
 drag={x:e.offsetX,y:e.offsetY,moved:false,turn}});
window.addEventListener('mousemove',e=>{const r=cv.getBoundingClientRect(),sx=e.clientX-r.left,sy=e.clientY-r.top;
 if(drag){if(Math.abs(sx-drag.x)+Math.abs(sy-drag.y)>3)drag.moved=true;
  if(drag.turn){const [wx,wy]=toW(sx,sy),a=Math.atan2(wy-spot.y,wx-spot.x);
   spot.h=Math.round(((a/(Math.PI*2))*65536+65536)%65536);spot.marker=null;update();draw()}
  else if(drag.moved){cx-=(sx-drag.x)/scale;cy+=(sy-drag.y)/scale;drag.x=sx;drag.y=sy;draw()}return}
 if(e.target!==cv){$('tip').style.display='none';return}
 const h=nearest(sx,sy);if(h!==hover){hover=h;draw()}
 const tip=$('tip');if(h){tip.textContent=info(h);tip.style.display='block';
  tip.style.left=Math.min(sx+14,view.clientWidth-tip.offsetWidth-4)+'px';tip.style.top=(sy+14)+'px'}
 else tip.style.display='none';
 const [wx,wy]=toW(sx,sy),z=zEstimate(wx,wy);
 $('status').textContent=`x ${fmt(wx)}  y ${fmt(wy)}  z~${fmt(z.z)} (nearest marker ${Math.round(z.near[0])} m)`});
window.addEventListener('mouseup',e=>{if(!drag)return;const d=drag;drag=null;if(d.moved||d.turn)return;
 const r=cv.getBoundingClientRect(),sx=e.clientX-r.left,sy=e.clientY-r.top,m=nearest(sx,sy);
 if(m)setSpot({x:m.x,y:m.y,z:m.z,h:m.h,marker:m,base:D.base[regionOf(m.track)]||m.track});
 else{const [wx,wy]=toW(sx,sy),est=zEstimate(wx,wy);
  setSpot({x:wx,y:wy,z:est.z,h:spot?spot.h:0,marker:null,est,base:D.maps[mapIdx].base})}});
cv.addEventListener('wheel',e=>{e.preventDefault();zoomAt(Math.exp(-e.deltaY*0.0015),e.offsetX,e.offsetY)},{passive:false});
$('zin').onclick=()=>zoomAt(1.5,view.clientWidth/2,view.clientHeight/2);
$('zout').onclick=()=>zoomAt(1/1.5,view.clientWidth/2,view.clientHeight/2);
$('zfit').onclick=fit;
window.addEventListener('keydown',e=>{if(!spot||e.target.tagName==='INPUT'||e.target.tagName==='TEXTAREA')return;
 const k=e.key.toLowerCase();if(k!=='q'&&k!=='e')return;const step=Math.round(65536/360*(e.shiftKey?15:1));
 spot.h=((spot.h+(k==='q'?step:-step))%65536+65536)%65536;spot.marker=null;update();draw()});
window.addEventListener('resize',resize);
fillTracks();resize();fit();
</script></body></html>
'''

if __name__ == '__main__':
    main()
