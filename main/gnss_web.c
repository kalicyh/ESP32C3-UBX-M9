#include "gnss_web.h"
#include "ubx.h"
#include "web_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#define SAT_LIMIT 128
#define RAW_COUNT 8
#define RAW_BYTES 512
#define JSON_CAPACITY 49152

typedef struct { uint32_t seq; int64_t ms; uint16_t len, kept; uint8_t bytes[RAW_BYTES]; } raw_frame;
typedef struct {
    uint8_t pvt[92], sat[8 + SAT_LIMIT*12], dop[18], version[340];
    size_t sat_len, version_len;
    int64_t pvt_ms, sat_ms, dop_ms, version_ms, last_ms;
    uint32_t frames, checksum, oversized;
    unsigned dropped, io_errors, overflows;
    raw_frame raw[RAW_COUNT];
} gnss_snapshot;
static gnss_snapshot state;
static SemaphoreHandle_t lock;
void gnss_web_init(void) { lock = xSemaphoreCreateMutex(); configASSERT(lock); }
void gnss_web_receive(const uint8_t *f, size_t len) {
    const uint8_t *p = f + 6;
    size_t n = ubx_u16(f + 4);
    int64_t now = esp_timer_get_time()/1000;
    xSemaphoreTake(lock, portMAX_DELAY);
    state.last_ms = now;
    raw_frame *r = &state.raw[state.frames % RAW_COUNT];
    r->seq = ++state.frames; r->ms = now; r->len = len;
    r->kept = len < RAW_BYTES ? len : RAW_BYTES;
    memcpy(r->bytes, f, r->kept);
    if (f[2] == 1 && f[3] == 7 && n >= sizeof(state.pvt)) {
        memcpy(state.pvt,p,sizeof(state.pvt)); state.pvt_ms = now;
    } else if (f[2] == 1 && f[3] == 0x35 && n >= 8 && p[4] == 1 && n >= 8u + 12u*p[5]) {
        size_t count = p[5] > SAT_LIMIT ? SAT_LIMIT : p[5];
        state.sat_len = 8 + 12*count;
        memcpy(state.sat,p,state.sat_len); state.sat_ms = now;
    } else if (f[2] == 1 && f[3] == 4 && n >= sizeof(state.dop)) {
        memcpy(state.dop,p,sizeof(state.dop)); state.dop_ms = now;
    } else if (f[2] == 10 && f[3] == 4 && n >= 40) {
        state.version_len = n < sizeof(state.version) ? n : sizeof(state.version);
        memcpy(state.version,p,state.version_len); state.version_ms = now;
    }
    xSemaphoreGive(lock);
}
void gnss_web_stats(uint32_t checksum, uint32_t oversized, unsigned dropped, unsigned io_errors, unsigned overflows) {
    xSemaphoreTake(lock,portMAX_DELAY);
    state.checksum=checksum; state.oversized=oversized; state.dropped=dropped;
    state.io_errors=io_errors; state.overflows=overflows;
    xSemaphoreGive(lock);
}
static void pvt_json(json_writer *w, const uint8_t *p) {
    jw_printf(w,"{\"type\":\"PVT\",\"iTOW_ms\":%"PRIu32",\"utc\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\","
        "\"valid_flags\":%u,\"time_accuracy_ns\":%"PRIu32",\"nano\":%"PRId32",\"fix_type\":%u,"
        "\"fix_ok\":%s,\"flags\":%u,\"flags2\":%u,\"satellites\":%u,\"lon_deg\":%.7f,\"lat_deg\":%.7f,"
        "\"height_m\":%.3f,\"msl_m\":%.3f,\"hacc_m\":%.3f,\"vacc_m\":%.3f,\"vel_n_mps\":%.3f,"
        "\"vel_e_mps\":%.3f,\"vel_d_mps\":%.3f,\"ground_speed_mps\":%.3f,\"heading_deg\":%.5f,"
        "\"speed_accuracy_mps\":%.3f,\"heading_accuracy_deg\":%.5f,\"pdop\":%.2f,\"flags3\":%u,\"vehicle_heading_deg\":%.5f}",
        ubx_u32(p),ubx_u16(p+4),p[6],p[7],p[8],p[9],p[10],p[11],ubx_u32(p+12),ubx_i32(p+16),p[20],
        (p[21]&1)?"true":"false",p[21],p[22],p[23],ubx_i32(p+24)*1e-7,ubx_i32(p+28)*1e-7,
        ubx_i32(p+32)*1e-3,ubx_i32(p+36)*1e-3,ubx_u32(p+40)*1e-3,ubx_u32(p+44)*1e-3,
        ubx_i32(p+48)*1e-3,ubx_i32(p+52)*1e-3,ubx_i32(p+56)*1e-3,ubx_i32(p+60)*1e-3,ubx_i32(p+64)*1e-5,
        ubx_u32(p+68)*1e-3,ubx_u32(p+72)*1e-5,ubx_u16(p+76)*.01,ubx_u16(p+78),ubx_i32(p+84)*1e-5);
}
char *gnss_snapshot_json(bool include_raw) {
    gnss_snapshot *s = malloc(sizeof(*s));
    char *json = malloc(JSON_CAPACITY);
    if (!s || !json) { free(s); free(json); return NULL; }
    /* Copy once; never hold the receiver lock while formatting or sending a snapshot. */
    xSemaphoreTake(lock,portMAX_DELAY); memcpy(s,&state,sizeof(*s)); xSemaphoreGive(lock);
    json_writer w = {.data=json,.capacity=JSON_CAPACITY};
    jw_printf(&w,"{\"uptime_ms\":%"PRId64",\"last_frame_ms\":%"PRId64",\"pvt_ms\":%"PRId64",\"sat_ms\":%"PRId64",\"dop_ms\":%"PRId64",\"pvt\":",
        esp_timer_get_time()/1000,s->last_ms,s->pvt_ms,s->sat_ms,s->dop_ms);
    if (s->pvt_ms) pvt_json(&w,s->pvt); else jw_printf(&w,"null");
    jw_printf(&w,",\"sat_reported\":%u,\"satellites\":[",s->sat_ms?s->sat[5]:0);
    for (size_t i=0; s->sat_len>=8 && i<(s->sat_len-8)/12; ++i) {
        const uint8_t *p=s->sat+8+12*i;
        int elev=p[3]<128?p[3]:(int)p[3]-256;
        int az=ubx_u16(p+4); if(az>=32768) az-=65536;
        int residual=ubx_u16(p+6); if(residual>=32768) residual-=65536;
        jw_printf(&w,"%s{\"type\":\"SAT\",\"iTOW_ms\":%"PRIu32",\"gnss_id\":%u,\"sv_id\":%u,\"cno_dbhz\":%u,\"elevation_deg\":%d,\"azimuth_deg\":%d,\"pseudorange_residual_m\":%.1f,\"flags\":%"PRIu32"}",
            i?",":"",ubx_u32(s->sat),p[0],p[1],p[2],elev,az,residual*.1,ubx_u32(p+8));
    }
    jw_printf(&w,"],\"dop\":");
    if (s->dop_ms) {
        const uint8_t *p=s->dop;
        jw_printf(&w,"{\"type\":\"DOP\",\"iTOW_ms\":%"PRIu32",\"g\":%.2f,\"p\":%.2f,\"t\":%.2f,\"v\":%.2f,\"h\":%.2f,\"n\":%.2f,\"e\":%.2f}",
            ubx_u32(p),ubx_u16(p+4)*.01,ubx_u16(p+6)*.01,ubx_u16(p+8)*.01,ubx_u16(p+10)*.01,ubx_u16(p+12)*.01,ubx_u16(p+14)*.01,ubx_u16(p+16)*.01);
    } else jw_printf(&w,"null");
    jw_printf(&w,",\"version\":");
    if (s->version_ms) {
        jw_printf(&w,"{\"software\":"); jw_string(&w,(char*)s->version,30);
        jw_printf(&w,",\"hardware\":"); jw_string(&w,(char*)s->version+30,10);
        jw_printf(&w,",\"extensions\":[");
        for (size_t i=40;i+30<=s->version_len;i+=30) {
            if(i!=40) jw_printf(&w,",");
            jw_string(&w,(char*)s->version+i,30);
        }
        jw_printf(&w,"]}");
    } else jw_printf(&w,"null");
    jw_printf(&w,",\"stats\":{\"frames\":%"PRIu32",\"checksum\":%"PRIu32",\"oversize\":%"PRIu32",\"output_drop\":%u,\"io_errors\":%u,\"uart_overflow\":%u},\"raw\":[",
        s->frames,s->checksum,s->oversized,s->dropped,s->io_errors,s->overflows);
    uint32_t count=include_raw?(s->frames<RAW_COUNT?s->frames:RAW_COUNT):0;
    for(uint32_t i=0;i<count;++i) {
        const raw_frame *r=&s->raw[(s->frames-count+i)%RAW_COUNT];
        jw_printf(&w,"%s{\"seq\":%"PRIu32",\"received_ms\":%"PRId64",\"class\":%u,\"id\":%u,\"length\":%u,\"truncated\":%s,\"frame_hex\":\"",
            i?",":"",r->seq,r->ms,r->bytes[2],r->bytes[3],r->len-8,r->len>r->kept?"true":"false");
        for(size_t k=0;k<r->kept;++k) jw_printf(&w,"%02X",r->bytes[k]);
        jw_printf(&w,"\"}");
    }
    jw_printf(&w,"]}"); free(s);
    if(w.failed) { free(json); return NULL; }
    return json;
}

char *gnss_web_json(void) { return gnss_snapshot_json(true); }
