#include "web_codec.h"
#include "gnss_web.h"
#include "ubx.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int64_t esp_timer_get_time(void) { return 1000000; }
static void send(uint8_t cls,uint8_t id,uint8_t *p,size_t n) {
    uint8_t frame[UBX_MAX_FRAME]; size_t size=ubx_encode(frame,sizeof(frame),cls,id,p,n);
    assert(size); gnss_web_receive(frame,size);
}
int main(int argc,char **argv) {
    char json[128]; json_writer w={.data=json,.capacity=sizeof(json)};
    jw_string(&w,"\"\\\n中",20);
    assert(!w.failed && !strcmp(json,"\"\\\"\\\\\\u000a中\""));
    char small[3]; w=(json_writer){.data=small,.capacity=sizeof(small)};
    jw_string(&w,"long",4); assert(w.failed);
    gnss_web_init();
    char *snapshot=gnss_web_json(); assert(snapshot && strstr(snapshot,"\"pvt\":null")); free(snapshot);
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb"); assert(f);
        uint8_t frame[UBX_MAX_FRAME]; size_t n=fread(frame,1,sizeof(frame),f); fclose(f);
        assert(n>=8 && n==(size_t)ubx_u16(frame+4)+8); gnss_web_receive(frame,n);
    } else {
        uint8_t pvt[92]={0};pvt[20]=3;pvt[21]=1;pvt[23]=12;send(1,7,pvt,sizeof(pvt));
        uint8_t sat[8+128*12]={0};sat[4]=1;sat[5]=128;
        for(unsigned i=0;i<128;++i){sat[8+i*12]=i%7;sat[9+i*12]=i;sat[10+i*12]=45;sat[11+i*12]=40;}
        send(1,0x35,sat,sizeof(sat));
        uint8_t malformed[8]={0};malformed[4]=1;malformed[5]=200;send(1,0x35,malformed,sizeof(malformed));
        uint8_t version[340];memset(version,'X',sizeof(version));send(10,4,version,sizeof(version));
        uint8_t raw[8192];memset(raw,0xff,sizeof(raw));for(int i=0;i<10;++i)send(2,0x15,raw,sizeof(raw));
    }
    snapshot=gnss_web_json();assert(snapshot);puts(snapshot);free(snapshot);
}
