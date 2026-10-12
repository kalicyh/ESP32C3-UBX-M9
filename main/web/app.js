'use strict';
const $ = id => document.getElementById(id);
let latest=null, connected=false, history=[], track=[], lastEpoch=null, lastUptime=0, rawPaused=false;
let ble=null, pollTimer=0, lastReceivedAt=0, connectingBle=false;
const names=['GPS','SBAS','Galileo','北斗','IMES','QZSS','GLONASS'];
const fixNames=['未定位','航位推算','2D 定位','3D 定位','GNSS + 推算','仅授时'];
function text(id,value){$(id).textContent=value;}
function age(ms){return ms<1000?'刚刚':`${Math.floor(ms/1000)} 秒前`;}
function num(value,digits=1){return Number.isFinite(value)?value.toFixed(digits):'—';}
function validPosition(p){return Boolean(p?.fix_ok && [2,3,4].includes(p.fix_type) && !(p.flags3&1) && Number.isFinite(p.lat_deg) && Number.isFinite(p.lon_deg) && Math.abs(p.lat_deg)<=90 && Math.abs(p.lon_deg)<=180);}
function freshPvt(d){return Boolean(d?.pvt_ms && d.uptime_ms-d.pvt_ms<5000);}
function canvas(id){
  const el=$(id),rect=el.getBoundingClientRect(),ratio=window.devicePixelRatio||1;
  if(!rect.width)return null;
  el.width=Math.round(rect.width*ratio);el.height=Math.round(rect.height*ratio);
  const c=el.getContext('2d');c.scale(ratio,ratio);c.font='11px system-ui';c.lineWidth=1;c.clearRect(0,0,rect.width,rect.height);
  return {c,w:rect.width,h:rect.height};
}
function empty(c,w,h,message){c.fillStyle='#5d707b';c.textAlign='center';c.fillText(message,w/2,h/2);}
function drawSpeed(){
  const ctx=canvas('speedChart');if(!ctx)return;const {c,w,h}=ctx;
  if(history.length<2){empty(c,w,h,'等待至少两次有效速度采样');return;}
  const left=36,bottom=h-25,right=w-10,top=12,max=Math.max(5,...history.map(p=>p.v))*1.15;
  c.textAlign='right';for(let i=0;i<=3;i++){const y=bottom-(bottom-top)*i/3;c.strokeStyle='#e4ecef';c.beginPath();c.moveTo(left,y);c.lineTo(right,y);c.stroke();c.fillStyle='#5d707b';c.fillText(num(max*i/3,0),left-8,y+4);}
  const start=history[0].t,span=Math.max(1000,history.at(-1).t-start);
  c.strokeStyle='#087c70';c.lineWidth=2;c.beginPath();history.forEach((p,i)=>{const x=left+(right-left)*(p.t-start)/span,y=bottom-(bottom-top)*p.v/max;if(!i || p.t-history[i-1].t>5000)c.moveTo(x,y);else c.lineTo(x,y);});c.stroke();
  c.fillStyle='#5d707b';c.textAlign='left';c.fillText(`${Math.round(span/1000)} 秒前`,left,h-5);c.textAlign='right';c.fillText('最新有效采样',right,h-5);
}
function drawSky(){
  const ctx=canvas('sky');if(!ctx)return;const {c,w,h}=ctx,cx=w/2,cy=h/2,r=Math.min(w,h)/2-28;
  c.strokeStyle='#dce5e8';c.fillStyle='#5d707b';c.textAlign='center';
  for(const f of [1,2/3,1/3]){c.beginPath();c.arc(cx,cy,r*f,0,Math.PI*2);c.stroke();}
  c.beginPath();c.moveTo(cx-r,cy);c.lineTo(cx+r,cy);c.moveTo(cx,cy-r);c.lineTo(cx,cy+r);c.stroke();
  for(const [label,x,y] of [['N',cx,cy-r-12],['S',cx,cy+r+20],['W',cx-r-16,cy+4],['E',cx+r+16,cy+4]])c.fillText(label,x,y);
  c.font='9px system-ui';c.fillText('60°',cx+10,cy-r/3+12);c.fillText('30°',cx+10,cy-r*2/3+12);
  const sats=latest?.satellites||[],stale=!connected || !latest?.sat_ms || latest.uptime_ms-latest.sat_ms>60000;
  if(!sats.length){empty(c,w,h,'等待卫星方位数据');return;}
  c.globalAlpha=stale?.35:1;
  for(const s of sats){if(s.elevation_deg<0 || s.elevation_deg>90 || s.azimuth_deg<0 || s.azimuth_deg>360)continue;
    const a=s.azimuth_deg*Math.PI/180,d=r*(1-s.elevation_deg/90),x=cx+Math.sin(a)*d,y=cy-Math.cos(a)*d;
    c.fillStyle=(s.flags&8)?'#087c70':'#537ba3';c.beginPath();c.arc(x,y,10,0,Math.PI*2);c.fill();c.fillStyle='#fff';c.font='9px system-ui';c.fillText(String(s.sv_id),x,y+3);
  }c.globalAlpha=1;
}
function drawTrack(){
  const ctx=canvas('track');if(!ctx)return;const {c,w,h}=ctx;
  if(!track.length){empty(c,w,h,'定位有效后开始记录轨迹');return;}
  const origin=track[0],points=track.map(p=>({x:(p.lon-origin.lon)*111320*Math.cos(origin.lat*Math.PI/180),y:(p.lat-origin.lat)*111320}));
  const xmin=Math.min(0,...points.map(p=>p.x)),xmax=Math.max(0,...points.map(p=>p.x)),ymin=Math.min(0,...points.map(p=>p.y)),ymax=Math.max(0,...points.map(p=>p.y));
  const scale=Math.min((w-80)/Math.max(xmax-xmin,10),(h-70)/Math.max(ymax-ymin,10)),mx=(xmin+xmax)/2,my=(ymin+ymax)/2;
  const map=p=>({x:w/2+(p.x-mx)*scale,y:h/2-(p.y-my)*scale});
  c.strokeStyle='#e4ecef';for(let i=1;i<5;i++){c.beginPath();c.moveTo(i*w/5,20);c.lineTo(i*w/5,h-25);c.stroke();}
  c.strokeStyle='#087c70';c.lineWidth=2;c.beginPath();points.forEach((p,i)=>{const a=map(p);if(!i || track[i].t-track[i-1].t>5000)c.moveTo(a.x,a.y);else c.lineTo(a.x,a.y);});c.stroke();
  const end=map(points.at(-1));c.fillStyle='#087c70';c.beginPath();c.arc(end.x,end.y,5,0,Math.PI*2);c.fill();
  c.fillStyle='#5d707b';c.textAlign='left';c.fillText('↑ 北',10,15);c.fillText(`视野宽度约 ${num(w/scale,0)} m`,10,h-5);
}
function renderSatellites(d){
  const rows=$('satRows');rows.replaceChildren();
  const sats=[...(d.satellites||[])].sort((a,b)=>b.cno_dbhz-a.cno_dbhz);
  text('satCount',`${sats.length} 颗${d.sat_reported>sats.length?'（明细已限量）':''}`);
  text('satTotal',d.sat_ms?`明细 ${sats.length} 颗 · ${age(d.uptime_ms-d.sat_ms)}`:'卫星明细尚未返回');
  text('satAge',d.sat_ms?`${age(d.uptime_ms-d.sat_ms)}${d.uptime_ms-d.sat_ms>60000?' · 已过期':''}`:'尚无 SAT');
  if(!sats.length){const row=rows.insertRow(),cell=row.insertCell();cell.colSpan=5;cell.className='empty';cell.textContent=d.sat_ms?'模块报告的卫星列表为空':'等待卫星明细';return;}
  for(const s of sats){const row=rows.insertRow();row.insertCell().textContent=`${names[s.gnss_id]||`GNSS ${s.gnss_id}`} · ${s.sv_id}`;
    const cell=row.insertCell(),bar=document.createElement('div');bar.className='signal';const meter=document.createElement('meter');meter.min=0;meter.max=60;meter.value=s.cno_dbhz;meter.setAttribute('aria-label',`${s.cno_dbhz} dB-Hz`);const value=document.createElement('span');value.textContent=s.cno_dbhz;bar.append(meter,value);cell.append(bar);
    row.insertCell().textContent=`${s.elevation_deg}°`;row.insertCell().textContent=`${s.azimuth_deg}°`;row.insertCell().textContent=s.flags&8?'是':'否';}
}
function renderRaw(){
  if(!latest || rawPaused)return;const el=$('rawFrames');el.replaceChildren();
  for(const f of [...latest.raw].reverse()){const item=document.createElement('details');item.className='raw-item';const title=document.createElement('summary');title.textContent=`#${f.seq} · UBX ${f.class.toString(16).padStart(2,'0').toUpperCase()}/${f.id.toString(16).padStart(2,'0').toUpperCase()} · ${f.length} B payload${f.truncated?' · 已截断':''}`;const pre=document.createElement('pre');pre.textContent=f.frame_hex.match(/.{1,2}/g)?.join(' ')||'';item.append(title,pre);el.append(item);}
  text('rawCount',`累计 ${latest.stats.frames} 帧`);text('pvtJson',JSON.stringify(latest.pvt,null,2));
}
function renderData(d){
  const p=d.pvt,fresh=connected && freshPvt(d),valid=fresh && validPosition(p),fix=fresh && p?.fix_ok;
  text('fix',!connected?'连接中断':!p?'等待数据':!fresh?'数据过期':!p.fix_ok?(p.fix_type===0?'未定位':'解算无效'):(p.flags3&1)?'位置无效':fixNames[p.fix_type]||'未知状态');
  text('fixDetail',!p?'正在等待 NAV-PVT':fix?'接收机确认有效解算':'尚无有效位置解算');
  text('used',fresh?p.satellites:'—');text('speed',valid?num(p.ground_speed_mps*3.6):'—');
  text('accuracy',valid?num(p.hacc_m):'—');text('pdop',valid?`PDOP ${num(p.pdop,2)}`:'PDOP —');
  text('speedAccuracy',valid?`速度精度 ±${num(p.speed_accuracy_mps,2)} m/s`:'等待有效速度');
  text('latitude',valid?num(p.lat_deg,7):'—');text('longitude',valid?num(p.lon_deg,7):'—');
  const heightValid=valid && [3,4].includes(p.fix_type);
  text('altitude',heightValid?`${num(p.msl_m,2)} m`:'—');text('height',heightValid?`${num(p.height_m,2)} m`:'—');
  text('vacc',heightValid?`±${num(p.vacc_m)} m`:'—');text('heading',valid && p.ground_speed_mps>.2?`${num(p.heading_deg)}°`:'—');
  text('utc',fresh && (p.valid_flags&3)===3?p.utc.replace('T',' ').replace('Z',' UTC'):'时间无效 / 等待卫星授时');
  text('pvtAge',d.pvt_ms?age(d.uptime_ms-d.pvt_ms):'尚无 PVT');
  text('version',d.version?[d.version.software,...d.version.extensions.filter(v=>v.startsWith('PROTVER'))].join(' · '):'等待版本信息');
  $('fixNotice').classList.toggle('warning',!valid);
  text('fixNotice',valid?'定位有效。图表仅记录有效位置；各类消息更新时间不同。':!connected?'设备离线。以下历史图表不代表实时位置。':!p?'等待接收机数据；蓝牙与 GNSS 接线可分别检查。':!fresh?'PVT 超过 5 秒未更新。请检查模块供电和接线。':'尚未获得有效位置。请把 GNSS 天线放到开阔处；坐标、速度与精度暂不展示。');
  renderSatellites(d);
  const s=d.stats;for(const [id,key] of [['frames','frames'],['checksum','checksum'],['dropped','output_drop'],['ioErrors','io_errors'],['overflows','uart_overflow']])text(id,s[key]);
  drawSpeed();drawSky();drawTrack();if(!$('raw').hidden)renderRaw();
}
function receiveData(d){
  if(d.uptime_ms<lastUptime){history=[];track=[];lastEpoch=null;}lastUptime=d.uptime_ms;latest=d;
  const p=d.pvt;
  if(freshPvt(d) && validPosition(p) && p.iTOW_ms!==lastEpoch){lastEpoch=p.iTOW_ms;history.push({t:d.pvt_ms,v:p.ground_speed_mps*3.6});track.push({t:d.pvt_ms,lat:p.lat_deg,lon:p.lon_deg});history=history.slice(-120);track=track.slice(-300);}
  text('speedSummary',history.length?`${history.length} 个有效采样 · 最新 ${num(history.at(-1).v)} km/h`:'尚无有效速度采样');
  text('trackSummary',track.length?`${track.length} 个有效定位点 · 相对首点的局部平面投影，非道路地图`:'等待有效定位；最多保留 300 个点，不加载在线地图。');
  $('export').disabled=false;renderData(d);
}
function setLinkState(online,message){
  connected=online;$('offline').hidden=online || !latest;
  $('liveDot').className=online?'dot online':'dot offline';text('liveLabel',message);
  $('connectBle').disabled=connectingBle || online;
  $('disconnectBle').hidden=!ble?.device?.gatt?.connected;
  if(latest)renderData(latest);
}
async function pollBle(){
  clearTimeout(pollTimer);
  if(!ble?.device?.gatt?.connected)return;
  try {
    const d=await ble.snapshot(!$('raw').hidden && !rawPaused);
    lastReceivedAt=performance.now();setLinkState(true,'蓝牙已连接');receiveData(d);
    text('bleHint',`已连接 ${ble.device.name}。每次快照完成后约 1 秒请求下一次；速度随蓝牙协商和数据量变化。`);
    pollTimer=setTimeout(pollBle,1000);
  } catch(e) {
    text('bleHint',`读取中断：${e.message}。请重新连接蓝牙。`);ble.disconnect();setLinkState(false,'蓝牙连接中断');
  }
}
$('connectBle').addEventListener('click',async()=>{
  if(connectingBle)return;
  connectingBle=true;$('connectBle').disabled=true;text('bleHint','请选择 UBX-M9-BLE 设备，并允许浏览器连接。');
  try {
    ble=new GnssBLE(()=>{clearTimeout(pollTimer);setLinkState(false,'蓝牙已断开');});
    await ble.connect();connected=true;history=[];track=[];lastEpoch=null;lastUptime=0;
    text('deviceAddress',ble.device.name||'UBX M9 BLE');setLinkState(true,'蓝牙已连接');pollBle();
  } catch(e) {text('bleHint',e.message);setLinkState(false,'蓝牙未连接');}
  finally{connectingBle=false;$('connectBle').disabled=connected;}
});
$('disconnectBle').addEventListener('click',()=>{clearTimeout(pollTimer);ble?.disconnect();setLinkState(false,'蓝牙已断开');text('bleHint','已断开，可以重新选择设备。');});
// A slow transfer must not leave an old position looking live.
setInterval(()=>{if(connected && latest && lastReceivedAt){const elapsed=performance.now()-lastReceivedAt;if(elapsed>5000)renderData({...latest,uptime_ms:latest.uptime_ms+elapsed});}},1000);
for(const button of document.querySelectorAll('.tab'))button.addEventListener('click',()=>{
  for(const b of document.querySelectorAll('.tab')){b.classList.toggle('active',b===button);if(b===button)b.setAttribute('aria-current','page');else b.removeAttribute('aria-current');}
  for(const panel of document.querySelectorAll('.panel'))panel.hidden=panel.id!==button.dataset.panel;
  if(latest)renderData(latest);
});
$('pauseRaw').addEventListener('click',()=>{rawPaused=!rawPaused;text('pauseRaw',rawPaused?'继续刷新':'暂停原始数据刷新');if(!rawPaused)renderRaw();});
$('clearTrack').addEventListener('click',()=>{track=[];text('trackSummary','轨迹已清空，等待下一个有效定位点。');drawTrack();});
$('export').addEventListener('click',()=>{
  if(!latest)return;const blob=new Blob([JSON.stringify(latest,null,2)],{type:'application/json'}),url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download='ubx-m9-snapshot.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
});
window.addEventListener('resize',()=>{drawSpeed();drawSky();drawTrack();});
text('deviceAddress','未选择设备');drawSpeed();drawSky();drawTrack();
if(!window.isSecureContext){text('bleHint','蓝牙需要安全页面。电脑请通过 localhost 打开，安卓请使用 HTTPS 地址；普通局域网 HTTP 页面无法连接蓝牙。');$('connectBle').disabled=true;}
else if(!navigator.bluetooth){text('bleHint','当前浏览器不支持 Web Bluetooth。请在电脑或安卓的 Chrome / Edge 中打开；Safari 和内置预览浏览器可能不支持。');$('connectBle').disabled=true;}
