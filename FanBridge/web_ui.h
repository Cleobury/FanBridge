// Single-page web UI served by FanBridge.
#pragma once
static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FanBridge</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1c1e21;--mut:#6b7280;--line:#e3e5e8;--acc:#0b7a75;--acc-fg:#fff;--warn:#b45309;--bad:#b91c1c}
@media (prefers-color-scheme:dark){:root{--bg:#131518;--card:#1d2024;--fg:#e8eaed;--mut:#9aa0a6;--line:#30343a;--acc:#2bb3a8;--acc-fg:#0b1513;--warn:#f59e0b;--bad:#f87171}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:720px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:4px 0 12px}h2{font-size:17px;margin:0 0 10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.sp{flex:1}
.mut{color:var(--mut);font-size:13px}.warn{color:var(--warn)}
button{font:inherit;border:1px solid var(--line);background:var(--bg);color:var(--fg);border-radius:8px;padding:8px 12px;cursor:pointer;min-height:38px}
button.on,button.pri{background:var(--acc);color:var(--acc-fg);border-color:var(--acc)}
button.bad{color:var(--bad)}button:disabled{opacity:.5}
input,select{font:inherit;color:var(--fg);background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:8px;min-height:38px;width:100%}
input[type=range]{padding:0;border:0;background:none}input[type=checkbox]{width:auto;min-height:0}
label{display:block;margin:6px 0}label.inl{display:inline-flex;gap:6px;align-items:center;margin-right:12px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}@media(max-width:480px){.grid{grid-template-columns:1fr}}
.speeds button{min-width:42px}.sl{display:grid;grid-template-columns:80px 1fr;gap:8px;align-items:center;margin:8px 0}
.banner{background:var(--warn);color:#fff;border-radius:10px;padding:10px 14px;margin-bottom:12px;display:flex;gap:8px;align-items:center}
.banner button{background:#fff;color:#000;border:0}
.badge{font-size:12px;border:1px solid var(--line);border-radius:999px;padding:2px 8px}.badge.low{color:var(--bad);border-color:var(--bad)}
details summary{cursor:pointer;color:var(--mut);margin-top:8px}code{font-size:13px;word-break:break-all}
</style></head><body><main>
<div class="row"><h1 class="sp">FanBridge</h1><span id="mat" class="mut"></span></div>
<div id="banner"></div>
<div id="devs"></div>

<div class="card"><h2 id="ftitle">Add a device</h2>
<label>Type<select id="f_kind"><option value="0">Fan / light (FanLamp, e.g. Philips Bliss)</option><option value="1">Tuiss smart blind</option></select></label>
<label>Name<input id="f_name" maxlength="22" placeholder="Bedroom fan"></label>
<div id="fanf"><div class="grid">
<label>Packet type<select id="f_var"></select></label>
<label>Remote ID (hex, blank = new random ID)<input id="f_id" placeholder="e.g. 034A5A9D"></label>
<label>Group index (0-255)<input id="f_idx" type="number" min="0" max="255" value="0"></label>
<label>Fan speeds<select id="f_sp"><option>6</option><option>3</option></select></label>
</div>
<label class="inl"><input type="checkbox" id="f_light" checked>Light</label>
<label class="inl"><input type="checkbox" id="f_ct" checked>Dimmable / colour temp</label>
<label class="inl"><input type="checkbox" id="f_fan" checked>Fan</label>
<p class="mut">New random ID: add the device, then turn the fan off at the wall, back on, and press <b>Pair</b> within ~5 s.
Cloned ID: use <b>Learn</b> below while pressing a button on the existing remote or app &mdash; no pairing needed.</p></div>
<div id="blindf" hidden><div class="grid">
<label>Blind ID (MAC printed on the blind's label)<input id="f_mac" placeholder="AA:BB:CC:DD:EE:FF"></label>
<label>Model<select id="f_model"></select></label>
</div>
<p class="mut">The blind ID is on the label on the motor / bracket (and in the Tuiss app). Blinds need no pairing: FanBridge connects to them directly. Each command takes a few seconds.</p></div>
<div class="row" style="margin-top:10px"><button class="pri" id="f_save">Add device</button><button id="f_cancel" hidden>Cancel</button></div>
</div>

<div class="card"><div class="row"><h2 class="sp">Learn from a remote or app</h2><button id="learn">Listen 20 s</button></div>
<div id="learned" class="mut">Press Listen, then press any button on the fan remote (or in the FanLamp Pro app). Tuiss blinds in range are listed too.</div>
<div id="blinds"></div></div>

<div class="card"><h2>System</h2><div id="sys" class="mut"></div>
<div class="row" style="margin-top:10px"><button id="reboot">Reboot</button><button class="bad" id="mreset">Remove from Google Home</button></div></div>
</main>
<script>
const $=id=>document.getElementById(id);let S=null,editing=-1,busy=0;
const api=(p,q={})=>fetch(p+'?'+new URLSearchParams(q),{method:'POST'}).then(r=>r.ok?r.text():r.text().then(t=>{throw t})).then(load).catch(e=>alert(e));
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function hex(n){return n.toString(16).toUpperCase().padStart(8,'0')}
function load(){if(busy)return;return fetch('/api/state').then(r=>r.json()).then(s=>{S=s;render()}).catch(()=>{})}
function render(){
 const m=S.matter;$('mat').textContent=m.enabled?(m.commissioned?'Google Home: connected':'Google Home: not added'):'Matter off';
 $('banner').innerHTML=S.rebootNeeded?'<div class="banner"><span class="sp">Device list changed. Reboot so Google Home sees it.</span><button onclick="api(\'/api/reboot\')">Reboot now</button></div>':'';
 if(!$('f_var').options.length){$('f_var').innerHTML=S.variants.map((v,i)=>`<option value="${i}">${esc(v)}</option>`).join('');
  $('f_model').innerHTML=S.models.map((v,i)=>`<option value="${i}">${esc(v)}</option>`).join('');$('f_model').value=5}
 $('devs').innerHTML=S.devices.length?S.devices.map(d=>d.kind==1?blindCard(d):fanCard(d)).join(''):'<div class="card mut">No devices yet. Add one below.</div>';
 const L=S.learned;$('learn').disabled=S.learning;$('learn').textContent=S.learning?'Listening…':'Listen 20 s';
 const fans=S.devices.filter(d=>d.kind!=1);
 if(L.length)$('learned').innerHTML='<b>Fan remotes</b>'+L.map((l,i)=>`<div class="row" style="margin:6px 0"><span class="sp"><code>${hex(l.id)}</code> idx ${l.index} · ${esc(S.variants[l.variant])} · cmd 0x${l.cmd.toString(16)}</span>${fans.map(d=>`<button onclick="applyL(${i},${d.i})">Apply to ${esc(d.name)}</button>`).join('')}<button onclick="useL(${i})">Add as new</button></div>`).join('');
 $('blinds').innerHTML=S.blinds.length?'<p class="mut" style="margin-bottom:0"><b>Tuiss blinds in range</b> (find your label\'s ID in the data)</p>'+S.blinds.map(b=>`<div class="row mut" style="margin:6px 0"><span class="sp"><b>${esc(b.name||'?')}</b> · ${b.rssi} dBm · ${b.age}s ago<br><code>${esc(b.mfr)}</code></span><button onclick="useB('${esc(b.name)}')">Add blind</button></div>`).join(''):'';
 $('sys').innerHTML=`IP: <code>${esc(S.ip)}</code> · encoder self-test: ${S.selftest?'PASS':'<b class=warn>FAIL</b>'}`+(m.enabled&&!m.commissioned?`<br>Add to Google Home: + → Device → Matter-enabled device. Code <code>${esc(m.code)}</code> · <a href="${esc(m.qr)}" target="_blank">QR code</a>`:'');
}
function settings(d,extra){return `<details><summary>Settings</summary><div class="row" style="margin-top:8px">${extra}<span class="sp"></span><button onclick="edit(${d.i})">Edit</button><button class="bad" onclick="del(${d.i})">Delete</button></div>`}
function fanCard(d){
 const st=d.st;let h=`<div class="card"><div class="row"><h2 class="sp">${esc(d.name)}</h2><span class="mut"><code>${hex(d.id)}</code></span></div>`;
 if(d.light){h+=`<div class="row"><span class="sp">Light</span><button class="${st.on?'on':''}" onclick="cmd(${d.i},'light',1)">On</button><button class="${st.on?'':'on'}" onclick="cmd(${d.i},'light',0)">Off</button><button onclick="cmd(${d.i},'toggle')">Toggle</button></div>`;
  if(d.ct)h+=`<div class="sl"><span>Brightness</span><input type="range" min="1" max="255" value="${st.bri}" onpointerdown="busy=1" onchange="busy=0;cmd(${d.i},'bri',this.value)"></div>
  <div class="sl"><span>Warmth</span><input type="range" min="${S.mcold}" max="${S.mwarm}" value="${st.mired}" onpointerdown="busy=1" onchange="busy=0;cmd(${d.i},'ct',this.value)"></div>`}
 if(d.fan){let b='';for(let s=0;s<=d.speeds;s++)b+=`<button class="${st.fan==s?'on':''}" onclick="cmd(${d.i},'fan',${s})">${s?s:'Off'}</button>`;
  h+=`<div class="row speeds" style="margin-top:8px"><span class="sp">Fan</span>${b}</div><div class="row" style="margin-top:8px"><span class="sp">Direction</span><button class="${st.dir?'':'on'}" onclick="cmd(${d.i},'dir',0)">Forward</button><button class="${st.dir?'on':''}" onclick="cmd(${d.i},'dir',1)">Reverse</button></div>`}
 h+=settings(d,`<button onclick="cmd(${d.i},'pair')">Pair</button><button onclick="cmd(${d.i},'unpair')">Unpair</button>`)+`<p class="mut">${esc(S.variants[d.variant])} · index ${d.index} · ${d.speeds} speeds</p></details></div>`;return h}
function blindCard(d){
 const st=d.st,pos=st.pos<0?'unknown':st.pos+'% open',tgt=st.target>=0?` → ${st.target}%`:'';
 const bat=st.bat==2?'<span class="badge low">Battery low</span>':st.bat==1?'<span class="badge">Battery OK</span>':'';
 const v=st.target>=0?st.target:(st.pos<0?0:st.pos);
 return `<div class="card"><div class="row"><h2 class="sp">${esc(d.name)}</h2><span class="mut">${esc(S.models[d.model])} · <code>${esc(d.mac)}</code></span></div>
 <div class="row"><span class="sp">Position: <b>${pos}</b>${tgt}</span>${bat}</div>
 <div class="sl"><span>Open %</span><input type="range" min="0" max="100" step="5" value="${v}" onpointerdown="busy=1" onchange="busy=0;cmd(${d.i},'pos',this.value)"></div>
 <div class="row"><button onclick="cmd(${d.i},'open')">Open</button><button onclick="cmd(${d.i},'stop')">Stop</button><button onclick="cmd(${d.i},'close')">Close</button><span class="sp"></span><button onclick="cmd(${d.i},'refresh')">Refresh</button></div>
 <p class="mut">${st.busy?'Working… (blinds take a few seconds to connect)':esc(st.msg)}</p>`+settings(d,'')+`</details></div>`}
function cmd(d,c,v){api('/api/cmd',{d,c,v:v??''})}
function del(i){if(confirm('Delete '+S.devices[i].name+'?'))api('/api/dev/del',{d:i})}
function kind(k){$('f_kind').value=k;$('fanf').hidden=k==1;$('blindf').hidden=k!=1}
function fill(n,v,id,idx,sp,l,ct,f){$('f_name').value=n;$('f_var').value=v;$('f_id').value=id;$('f_idx').value=idx;$('f_sp').value=sp;$('f_light').checked=l;$('f_ct').checked=ct;$('f_fan').checked=f}
function edit(i){const d=S.devices[i];editing=i;kind(d.kind);fill(d.name,d.variant,hex(d.id),d.index,d.speeds,d.light,d.ct,d.fan);$('f_mac').value=d.mac;$('f_model').value=d.model;
 $('ftitle').textContent='Edit '+d.name;$('f_save').textContent='Save';$('f_cancel').hidden=false;$('ftitle').scrollIntoView({behavior:'smooth'})}
function reset(){editing=-1;kind(0);fill('',1,'',0,6,true,true,true);$('f_mac').value='';$('f_model').value=5;$('ftitle').textContent='Add a device';$('f_save').textContent='Add device';$('f_cancel').hidden=true}
function applyL(i,di){const l=S.learned[i],d=S.devices[di];if(!confirm(`Use remote ${hex(l.id)} for ${d.name}?`))return;
 api('/api/dev/edit',{d:di,kind:0,name:d.name,variant:l.variant,id:hex(l.id),index:l.index,speeds:d.speeds,light:+d.light,ct:+d.ct,fan:+d.fan})}
function useL(i){const l=S.learned[i];reset();fill('Fan '+(S.devices.length+1),l.variant,hex(l.id),l.index,6,true,true,true);$('ftitle').scrollIntoView({behavior:'smooth'})}
function useB(name){reset();kind(1);$('f_name').value='Blind '+(S.devices.filter(d=>d.kind==1).length+1);const m=S.models.indexOf(name);if(m>=0)$('f_model').value=m;$('ftitle').scrollIntoView({behavior:'smooth'});$('f_mac').focus()}
$('f_kind').onchange=e=>kind(+e.target.value);
$('f_save').onclick=()=>{const k=+$('f_kind').value,q={kind:k,name:$('f_name').value||(k?'Blind':'Fan')};
 if(k==1)Object.assign(q,{mac:$('f_mac').value.trim(),model:$('f_model').value});
 else Object.assign(q,{variant:$('f_var').value,id:$('f_id').value.trim(),index:$('f_idx').value,speeds:$('f_sp').value,light:+$('f_light').checked,ct:+$('f_ct').checked,fan:+$('f_fan').checked});
 if(editing>=0)q.d=editing;api(editing>=0?'/api/dev/edit':'/api/dev/add',q).then(reset)};
$('f_cancel').onclick=reset;$('learn').onclick=()=>api('/api/learn');
$('reboot').onclick=()=>api('/api/reboot');$('mreset').onclick=()=>{if(confirm('Remove FanBridge from Google Home? You will need to add it again.'))api('/api/matter/reset')};
load();setInterval(load,3000);
</script></body></html>)rawliteral";
