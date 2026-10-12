#include "ubx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned seen;
static size_t last_size;
static void receive(const uint8_t *f, size_t n, void *ctx) {
    (void)ctx; assert(f[0] == 0xb5 && f[1] == 0x62); ++seen; last_size=n;
}
int main(void) {
    static ubx_parser p = {.callback=receive};
    /* Independent known MON-VER poll checksum. */
    const uint8_t known[] = {0xb5,0x62,0x0a,4,0,0,0x0e,0x34};
    uint8_t frame[UBX_MAX_FRAME], payload[UBX_MAX_PAYLOAD];
    assert(ubx_encode(frame,sizeof(frame),0x0a,4,NULL,0)==sizeof(known));
    assert(!memcmp(frame,known,sizeof(known)));
    for (size_t i=0;i<sizeof(known);++i) ubx_feed(&p,known+i,1);
    assert(seen==1);
    const uint8_t noise[]={0,0xff,0xb5,0xb5};
    ubx_feed(&p,noise,sizeof(noise)); ubx_feed(&p,known,sizeof(known));
    assert(seen==2);
    frame[7]^=1; ubx_feed(&p,frame,8); ubx_feed(&p,known,8);
    assert(seen==3 && p.bad_checksum==1);
    const uint8_t oversize[]={0xb5,0x62,1,7,0xff,0xff};
    ubx_feed(&p,oversize,sizeof(oversize)); ubx_feed(&p,known,8);
    assert(seen==4 && p.oversized==1);
    memset(payload,0xb5,sizeof(payload));
    size_t n=ubx_encode(frame,sizeof(frame),2,0x15,payload,sizeof(payload));
    assert(n==UBX_MAX_FRAME);
    ubx_feed(&p,frame,n); assert(seen==5 && last_size==n);
    assert(ubx_encode(frame,7,1,1,NULL,0)==0);
    assert(ubx_encode(frame,sizeof(frame),1,1,NULL,1)==0);
    const uint8_t negative[]={0,0,0,0x80}, minus1[]={255,255,255,255};
    assert(ubx_i32(negative)==INT32_MIN && ubx_i32(minus1)==-1);
    ubx_feed(&p,known,8); ubx_feed(&p,known,8); assert(seen==7);
    puts("UBX parser tests passed");
}
