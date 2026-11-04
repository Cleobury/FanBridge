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
.speeds button{min-width:42px}.sl{display:grid;grid-template-columns:70px 1fr;gap:8px;align-items:center;margin:8px 0}
.banner{background:var(--warn);color:#fff;border-radius:10px;padding:10px 14px;margin-bottom:12px;display:flex;gap:8px;align-items:center}
.banner button{background:#fff;color:#000;border:0}
details summary{cursor:pointer;color:var(--mut);margin-top:8px}code{font-size:13px}
</style></head><body><main>
<div class="row"><h1 class="sp">FanBridge</h1><span id="mat" class="mut"></span></div>
<div id="banner"></div>
<div id="devs"></div>

<div class="card"><h2 id="ftitle">Add a device</h2>
<div class="grid">
<label>Name<input id="f_name" maxlength="22" placeholder="Bedroom fan"></label>
<label>Packet type<select id="f_var"></select></label>
<label>Remote ID (hex, blank = new random ID)<input id="f_id" placeholder="e.g. 034A5A9D"></label>
<label>Group index (0-255)<input id="f_idx" type="number" min="0" max="255" value="0"></label>
<label>Fan speeds<select id="f_sp"><option>6</option><option>3</option></select></label>
</div>
<label class="inl"><input type="checkbox" id="f_light" checked>Light</label>
<label class="inl"><input type="checkbox" id="f_ct" checked>Dimmable / colour temp</label>
<label class="inl"><input type="checkbox" id="f_fan" checked>Fan</label>
<div class="row" style="margin-top:10px"><button class="pri" id="f_save">Add device</button><button id="f_cancel" hidden>Cancel</button></div>
<p class="mut">New random ID: add the device, then turn the fan off at the wall, back on, and press <b>Pair</b> within ~5 s.
Cloned ID: use <b>Learn</b> below while pressing a button on the existing remote or app &mdash; no pairing needed.</p>
</div>

<div class="card"><div class="row"><h2 class="sp">Learn from a remote or app</h2><button id="learn">Listen 20 s</button></div>
<div id="learned" class="mut">Press Listen, then press any button on the remote (or in the FanLamp Pro app).</div></div>

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
 if(!$('f_var').options.length)$('f_var').innerHTML=S.variants.map((v,i)=>`<option value="${i}">${esc(v)}</option>`).join('');
 $('devs').innerHTML=S.devices.length?S.devices.map(card).join(''):'<div class="card mut">No devices yet. Add one below.</div>';
 const L=S.learned;$('learn').disabled=S.learning;$('learn').textContent=S.learning?'Listening…':'Listen 20 s';
 if(L.length)$('learned').innerHTML=L.map((l,i)=>`<div class="row" style="margin:6px 0"><span class="sp"><code>${hex(l.id)}</code> idx ${l.index} · ${esc(S.variants[l.variant])} · cmd 0x${l.cmd.toString(16)}</span>${S.devices.map(d=>`<button onclick="applyL(${i},${d.i})">Apply to ${esc(d.name)}</button>`).join('')}<button onclick="useL(${i})">Add as new</button></div>`).join('');
 $('sys').innerHTML=`IP: <code>${esc(S.ip)}</code> · encoder self-test: ${S.selftest?'PASS':'<b class=warn>FAIL</b>'}`+(m.enabled&&!m.commissioned?`<br>Add to Google Home: + → Device → Matter-enabled device. Code <code>${esc(m.code)}</code> · <a href="${esc(m.qr)}" target="_blank">QR code</a>`:'');
}
function card(d){
 const st=d.st;let h=`<div class="card"><div class="row"><h2 class="sp">${esc(d.name)}</h2><span class="mut"><code>${hex(d.id)}</code></span></div>`;
 if(d.light){h+=`<div class="row"><span class="sp">Light</span><button class="${st.on?'on':''}" onclick="cmd(${d.i},'light',1)">On</button><button class="${st.on?'':'on'}" onclick="cmd(${d.i},'light',0)">Off</button><button onclick="cmd(${d.i},'toggle')">Toggle</button></div>`;
  if(d.ct)h+=`<div class="sl"><span>Brightness</span><input type="range" min="1" max="255" value="${st.bri}" onpointerdown="busy=1" onchange="busy=0;cmd(${d.i},'bri',this.value)"></div>
  <div class="sl"><span>Warmth</span><input type="range" min="${S.mcold}" max="${S.mwarm}" value="${st.mired}" onpointerdown="busy=1" onchange="busy=0;cmd(${d.i},'ct',this.value)"></div>`}
 if(d.fan){let b='';for(let s=0;s<=d.speeds;s++)b+=`<button class="${st.fan==s?'on':''}" onclick="cmd(${d.i},'fan',${s})">${s?s:'Off'}</button>`;
  h+=`<div class="row speeds" style="margin-top:8px"><span class="sp">Fan</span>${b}</div><div class="row" style="margin-top:8px"><span class="sp">Direction</span><button class="${st.dir?'':'on'}" onclick="cmd(${d.i},'dir',0)">Forward</button><button class="${st.dir?'on':''}" onclick="cmd(${d.i},'dir',1)">Reverse</button></div>`}
 h+=`<details><summary>Pairing &amp; settings</summary><div class="row" style="margin-top:8px"><button onclick="cmd(${d.i},'pair')">Pair</button><button onclick="cmd(${d.i},'unpair')">Unpair</button><span class="sp"></span><button onclick="edit(${d.i})">Edit</button><button class="bad" onclick="del(${d.i})">Delete</button></div>
 <p class="mut">${esc(S.variants[d.variant])} · index ${d.index} · ${d.speeds} speeds</p></details></div>`;return h}
function cmd(d,c,v){api('/api/cmd',{d,c,v:v??''})}
function del(i){if(confirm('Delete '+S.devices[i].name+'?'))api('/api/dev/del',{d:i})}
function fill(n,v,id,idx,sp,l,ct,f){$('f_name').value=n;$('f_var').value=v;$('f_id').value=id;$('f_idx').value=idx;$('f_sp').value=sp;$('f_light').checked=l;$('f_ct').checked=ct;$('f_fan').checked=f}
function edit(i){const d=S.devices[i];editing=i;fill(d.name,d.variant,hex(d.id),d.index,d.speeds,d.light,d.ct,d.fan);$('ftitle').textContent='Edit '+d.name;$('f_save').textContent='Save';$('f_cancel').hidden=false;$('ftitle').scrollIntoView({behavior:'smooth'})}
function reset(){editing=-1;fill('',1,'',0,6,true,true,true);$('ftitle').textContent='Add a device';$('f_save').textContent='Add device';$('f_cancel').hidden=true}
function applyL(i,di){const l=S.learned[i],d=S.devices[di];if(!confirm(`Use remote ${hex(l.id)} for ${d.name}?`))return;
 api('/api/dev/edit',{d:di,name:d.name,variant:l.variant,id:hex(l.id),index:l.index,speeds:d.speeds,light:+d.light,ct:+d.ct,fan:+d.fan})}
function useL(i){const l=S.learned[i];reset();fill('Fan '+(S.devices.length+1),l.variant,hex(l.id),l.index,6,true,true,true);$('ftitle').scrollIntoView({behavior:'smooth'})}
$('f_save').onclick=()=>{const q={name:$('f_name').value||'Fan',variant:$('f_var').value,id:$('f_id').value.trim(),index:$('f_idx').value,speeds:$('f_sp').value,light:+$('f_light').checked,ct:+$('f_ct').checked,fan:+$('f_fan').checked};
 if(editing>=0)q.d=editing;api(editing>=0?'/api/dev/edit':'/api/dev/add',q).then(reset)};
$('f_cancel').onclick=reset;$('learn').onclick=()=>api('/api/learn');
$('reboot').onclick=()=>api('/api/reboot');$('mreset').onclick=()=>{if(confirm('Remove FanBridge from Google Home? You will need to add it again.'))api('/api/matter/reset')};
load();setInterval(load,3000);
</script></body></html>)rawliteral";
