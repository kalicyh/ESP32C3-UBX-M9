const assert=require('node:assert/strict');
const {SnapshotAssembler}=require('../main/web/ble_transport.js');
const sample={uptime_ms:1000,pvt:{fix_ok:false,utc:'时间无效'},satellites:[],raw:[],stats:{frames:2}};
function packets(data,chunk=12,seq=17){
  const bytes=new TextEncoder().encode(JSON.stringify(data)),result=[];
  for(let offset=0;offset<bytes.length;offset+=chunk){const n=Math.min(chunk,bytes.length-offset),b=new Uint8Array(8+n),v=new DataView(b.buffer);b[0]=1;b[1]=(offset===0?1:0)|(offset+n===bytes.length?2:0);v.setUint16(2,seq,true);v.setUint16(4,offset,true);v.setUint16(6,bytes.length,true);b.set(bytes.slice(offset,offset+n),8);result.push(b);}
  return result;
}
for(const size of [1,12,236]){
  const assembler=new SnapshotAssembler();let result;
  for(const packet of packets(sample,size))result=assembler.push(packet);
  assert.deepEqual(result.pvt,sample.pvt);assert.ok(result.uptime_ms>=1000);assert.equal(assembler.bytes,null);
}
let assembler=new SnapshotAssembler(),chunks=packets(sample);
assert.throws(()=>assembler.push(chunks[1]),/缺失|乱序/);
assembler=new SnapshotAssembler();assembler.push(chunks[0]);assert.throws(()=>assembler.push(chunks[2]),/缺失|乱序/);
assembler=new SnapshotAssembler();assembler.push(chunks[0]);assembler.push(chunks[1]);assert.throws(()=>assembler.push(chunks[1]),/缺失|乱序/);
let bad=chunks[0].slice();bad[0]=9;assert.throws(()=>new SnapshotAssembler().push(bad),/版本/);
bad=chunks[0].slice();bad[7]=255;assert.throws(()=>new SnapshotAssembler().push(bad),/长度/);
bad=chunks[0].slice();bad[1]=3;assert.throws(()=>new SnapshotAssembler().push(bad),/结束/);
assembler=new SnapshotAssembler();assembler.push(chunks[0]);assembler.reset();assert.throws(()=>assembler.push(chunks[1]),/缺失|乱序/);
assembler=new SnapshotAssembler();let result;for(const p of packets(sample,236,65535))result=assembler.push(p);for(const p of packets(sample,236,0))result=assembler.push(p);assert.equal(result.stats.frames,2);
console.log('PASS: MTU23 fragmentation, UTF-8 across chunks, ordering, duplicate/missing chunks, bounds, reset and sequence wrap');
