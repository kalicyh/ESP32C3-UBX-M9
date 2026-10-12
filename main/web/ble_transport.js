'use strict';
const GNSS_SERVICE='8c7b0001-6b8c-4a36-9f86-3ecfe03b6a91';
const GNSS_DATA='8c7b0002-6b8c-4a36-9f86-3ecfe03b6a91';
const GNSS_REQUEST='8c7b0003-6b8c-4a36-9f86-3ecfe03b6a91';
class SnapshotAssembler {
  constructor(){this.reset();}
  reset(){this.bytes=null;this.offset=0;this.seq=0;this.startedAt=0;}
  push(value){
    const v=value instanceof DataView?value:new DataView(value.buffer,value.byteOffset,value.byteLength);
    if(v.byteLength<=8 || v.getUint8(0)!==1)throw new Error('蓝牙分包版本或长度无效');
    const flags=v.getUint8(1),seq=v.getUint16(2,true),offset=v.getUint16(4,true),total=v.getUint16(6,true);
    if(flags&~3 || total<2 || total>49152)throw new Error('蓝牙快照长度无效');
    if(flags&1){if(offset!==0)throw new Error('蓝牙首包偏移无效');this.bytes=new Uint8Array(total);this.offset=0;this.seq=seq;this.startedAt=performance.now();}
    if(!this.bytes || seq!==this.seq || total!==this.bytes.length || offset!==this.offset)throw new Error('蓝牙分包缺失或乱序');
    const data=new Uint8Array(v.buffer,v.byteOffset+8,v.byteLength-8);
    if(offset+data.length>total)throw new Error('蓝牙快照越界');
    this.bytes.set(data,offset);this.offset+=data.length;
    if(Boolean(flags&2)!==(this.offset===total))throw new Error('蓝牙结束标志无效');
    if(!(flags&2))return null;
    const json=new TextDecoder('utf-8',{fatal:true}).decode(this.bytes);
    const snapshot=JSON.parse(json),elapsed=performance.now()-this.startedAt;this.reset();
    if(snapshot.error)throw new Error(snapshot.error);
    if(!Number.isFinite(snapshot.uptime_ms) || !snapshot.stats || !Array.isArray(snapshot.satellites) || !Array.isArray(snapshot.raw))throw new Error('蓝牙快照格式错误');
    // Include transfer time in freshness checks; never present a delayed fix as current.
    snapshot.uptime_ms+=elapsed;
    return snapshot;
  }
}
class GnssBLE {
  constructor(onDisconnect){this.onDisconnect=onDisconnect;this.assembler=new SnapshotAssembler();this.pending=null;this.device=null;this.data=null;this.request=null;this.onValue=this.onValue.bind(this);this.onLost=this.onLost.bind(this);}
  async connect(){
    if(!globalThis.isSecureContext)throw new Error('请通过 HTTPS 或 localhost 打开页面');
    if(!navigator.bluetooth)throw new Error('当前浏览器不支持 Web Bluetooth，请使用电脑或安卓 Chrome / Edge');
    try{
      this.device=await navigator.bluetooth.requestDevice({filters:[{services:[GNSS_SERVICE]}]});
      this.device.addEventListener('gattserverdisconnected',this.onLost);
      const server=await this.device.gatt.connect(),service=await server.getPrimaryService(GNSS_SERVICE);
      this.data=await service.getCharacteristic(GNSS_DATA);this.request=await service.getCharacteristic(GNSS_REQUEST);
      this.data.addEventListener('characteristicvaluechanged',this.onValue);
      await this.data.startNotifications();
    }catch(error){this.disconnect();throw new Error(error.name==='NotFoundError'?'未选择设备或未发现 BLE 广播。确认 C3 已刷入蓝牙固件、开启手机蓝牙并靠近设备。':error.message);}
  }
  finish(error,value){
    const p=this.pending;if(!p)return;
    this.pending=null;clearTimeout(p.timer);clearTimeout(p.deadline);
    if(error){this.assembler.reset();p.reject(error);}else p.resolve(value);
  }
  onValue(event){
    if(!this.pending)return;
    try{const value=this.assembler.push(event.target.value);clearTimeout(this.pending.timer);
      if(value)this.finish(null,value);else this.pending.timer=setTimeout(()=>this.finish(new Error('蓝牙分包接收超时')),10000);
    }catch(error){this.finish(error);}
  }
  snapshot(includeRaw=false){
    if(this.pending)return Promise.reject(new Error('上一份快照尚未完成'));
    if(!this.device?.gatt.connected || !this.request)return Promise.reject(new Error('蓝牙未连接'));
    this.assembler.reset();
    return new Promise((resolve,reject)=>{
      this.pending={resolve,reject,timer:setTimeout(()=>this.finish(new Error('蓝牙响应超时')),10000),deadline:setTimeout(()=>this.finish(new Error('蓝牙快照传输过慢')),120000)};
      this.request.writeValueWithResponse(new Uint8Array([includeRaw?2:1])).catch(error=>this.finish(error));
    });
  }
  onLost(){
    this.finish(new Error('设备已断开'));this.assembler.reset();
    this.data?.removeEventListener('characteristicvaluechanged',this.onValue);
    this.device?.removeEventListener('gattserverdisconnected',this.onLost);
    this.data=null;this.request=null;this.onDisconnect?.();
  }
  disconnect(){if(this.device?.gatt.connected)this.device.gatt.disconnect();this.onLost();}
}
if(typeof module!=='undefined')module.exports={SnapshotAssembler,GnssBLE};
