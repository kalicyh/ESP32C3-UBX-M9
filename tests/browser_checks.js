async (page) => {
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.addInitScript(()=>{
    const data=new EventTarget(),device=new EventTarget();let seq=0;
    window.bleFixtureValid=false;window.bleRequests=[];
    data.startNotifications=async()=>data;
    const request={writeValueWithResponse:async command=>{
      window.bleRequests.push(command[0]);const valid=window.bleFixtureValid,t=10000+(++seq)*1000;
      const snapshot={uptime_ms:t,pvt_ms:t,sat_ms:t,sat_reported:1,stats:{frames:seq,checksum:0,output_drop:0,io_errors:0,uart_overflow:0},
        pvt:{iTOW_ms:t,fix_ok:valid,fix_type:valid?3:0,flags3:0,valid_flags:valid?3:0,lat_deg:31,lon_deg:121,satellites:1,ground_speed_mps:2,hacc_m:1.5,vacc_m:2,pdop:1,utc:'2026-10-12T02:00:00Z'},
        satellites:[{gnss_id:0,sv_id:3,cno_dbhz:42,elevation_deg:45,azimuth_deg:90,flags:8}],raw:command[0]===2?[{seq,class:1,id:7,length:2,frame_hex:'b5620107',truncated:false}]:[]};
      const bytes=new TextEncoder().encode(JSON.stringify(snapshot));
      setTimeout(()=>{for(let offset=0;offset<bytes.length;offset+=12){const payload=bytes.slice(offset,offset+12),v=new DataView(new ArrayBuffer(8+payload.length));v.setUint8(0,1);v.setUint8(1,(offset===0?1:0)|(offset+payload.length===bytes.length?2:0));v.setUint16(2,seq,true);v.setUint16(4,offset,true);v.setUint16(6,bytes.length,true);new Uint8Array(v.buffer,8).set(payload);data.value=v;data.dispatchEvent(new Event('characteristicvaluechanged'));}},20);
    }};
    device.name='UBX-M9-BLE-SYNTHETIC';device.gatt={connected:false,connect:async()=>{device.gatt.connected=true;return {getPrimaryService:async()=>({getCharacteristic:async uuid=>uuid.startsWith('8c7b0002')?data:request})}},disconnect:()=>{device.gatt.connected=false;device.dispatchEvent(new Event('gattserverdisconnected'))}};
    Object.defineProperty(navigator,'bluetooth',{value:{requestDevice:async()=>device},configurable:true});
  });
  await page.setViewportSize({width:390,height:844});await page.reload();
  await page.locator('#connectBle').click();await page.waitForFunction(()=>document.querySelector('#fix').textContent==='未定位');
  if(await page.locator('#latitude').textContent()!=='—')throw Error('Invalid fix shown');
  await page.evaluate(()=>window.bleFixtureValid=true);
  await page.waitForFunction(()=>document.querySelector('#speed').textContent==='7.2');
  if(await page.evaluate(()=>document.documentElement.scrollWidth)>390)throw Error('Mobile overflow');
  await page.screenshot({path:'output/playwright/ble-synthetic-mobile.png',fullPage:true});
  await page.getByRole('button',{name:'原始数据',exact:true}).click();
  await page.waitForFunction(()=>document.querySelectorAll('#rawFrames details').length===1);
  const [download]=await Promise.all([page.waitForEvent('download'),page.locator('#export').click()]);
  if(download.suggestedFilename()!=='ubx-m9-snapshot.json')throw Error('Export failed');
  await page.locator('#disconnectBle').click();
  if(await page.locator('#latitude').textContent()!=='—')throw Error('Disconnected coordinates shown');
  await page.locator('#connectBle').click();await page.waitForFunction(()=>document.querySelector('#speed').textContent==='7.2');
  if(errors.length)throw Error(errors.join('\n'));
  console.log('PASS synthetic BLE: connect, 12-byte fragments, invalid/valid fix, raw request, mobile, export, reconnect');
}
