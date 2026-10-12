#include "ble_link.h"
#include "gnss_web.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* UUID: 8c7b0001-6b8c-4a36-9f86-3ecfe03b6a91; 0002=data, 0003=request. */
#define UUID_BYTES(id) 0x91,0x6a,0x3b,0xe0,0xcf,0x3e,0x86,0x9f,0x36,0x4a,0x8c,0x6b,id,0x00,0x7b,0x8c
static const ble_uuid128_t service_uuid=BLE_UUID128_INIT(UUID_BYTES(1));
static const ble_uuid128_t data_uuid=BLE_UUID128_INIT(UUID_BYTES(2));
static const ble_uuid128_t request_uuid=BLE_UUID128_INIT(UUID_BYTES(3));
static const char *TAG="ble_link";
static char name[24];
static uint8_t address_type;
static uint16_t data_handle;
static atomic_uint connection=BLE_HS_CONN_HANDLE_NONE, generation, mtu=23;
static atomic_bool subscribed, busy;
static atomic_int ack_status;
static QueueHandle_t requests;
static SemaphoreHandle_t ack;
typedef struct { uint16_t connection; unsigned generation; uint8_t command; } request;
static void advertise(void);

static int access_gatt(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg) {
    (void)attr; (void)arg;
    if(ctx->op!=BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    uint8_t command=0;
    if(OS_MBUF_PKTLEN(ctx->om)!=1 || ble_hs_mbuf_to_flat(ctx->om,&command,1,NULL)!=0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if(command!=1 && command!=2) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    if(!atomic_load(&subscribed) || conn!=atomic_load(&connection)) return BLE_ATT_ERR_UNLIKELY;
    if(atomic_exchange(&busy,true)) return BLE_ATT_ERR_INSUFFICIENT_RES;
    request req={conn,atomic_load(&generation),command};
    if(xQueueSend(requests,&req,0)!=pdTRUE) { atomic_store(&busy,false); return BLE_ATT_ERR_INSUFFICIENT_RES; }
    return 0;
}
static const struct ble_gatt_svc_def services[]={
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&service_uuid.u,
     .characteristics=(struct ble_gatt_chr_def[]){
         {.uuid=&data_uuid.u,.flags=BLE_GATT_CHR_F_INDICATE,.access_cb=access_gatt,.val_handle=&data_handle},
         {.uuid=&request_uuid.u,.flags=BLE_GATT_CHR_F_WRITE,.access_cb=access_gatt},
         {0}}},
    {0}
};
static bool current(const request *r) {
    return atomic_load(&connection)==r->connection && atomic_load(&generation)==r->generation && atomic_load(&subscribed);
}
static void transfer_task(void *arg) {
    (void)arg; request r; uint16_t sequence=0;
    for(;;) {
        if(xQueueReceive(requests,&r,portMAX_DELAY)!=pdTRUE) continue;
        if(!current(&r)) { atomic_store(&busy,false); continue; }
        char *json=gnss_snapshot_json(r.command==2);
        const char *payload=json?json:"{\"error\":\"设备内存不足，请重试\"}";
        size_t total=strlen(payload); ++sequence;
        bool failed=false;
        for(size_t offset=0; offset<total && current(&r);) {
            uint8_t packet[244];
            size_t size=atomic_load(&mtu);
            size=size>247?244:size-3;
            size_t count=total-offset;
            if(count>size-8) count=size-8;
            /* Version, start/end flags, sequence, byte offset, total length (LE). */
            packet[0]=1; packet[1]=(offset==0?1:0)|(offset+count==total?2:0);
            packet[2]=sequence; packet[3]=sequence>>8;
            packet[4]=offset; packet[5]=offset>>8; packet[6]=total; packet[7]=total>>8;
            memcpy(packet+8,payload+offset,count);
            while(xSemaphoreTake(ack,0)==pdTRUE) {}
            atomic_store(&ack_status,BLE_HS_EBUSY);
            struct os_mbuf *om=ble_hs_mbuf_from_flat(packet,count+8);
            int rc=om?ble_gatts_indicate_custom(r.connection,data_handle,om):BLE_HS_ENOMEM;
            /* NimBLE owns om on both success and failure. One in-flight indication. */
            if(rc || xSemaphoreTake(ack,pdMS_TO_TICKS(5000))!=pdTRUE || !current(&r) || atomic_load(&ack_status)!=BLE_HS_EDONE) {
                failed=true; break;
            }
            offset+=count;
        }
        free(json);
        if(failed && current(&r)) {
            ESP_LOGW(TAG,"Snapshot transfer failed; disconnecting to reset ATT state");
            ble_gap_terminate(r.connection,BLE_ERR_REM_USER_CONN_TERM);
        }
        atomic_store(&busy,false);
    }
}
static int gap_event(struct ble_gap_event *event,void *arg) {
    (void)arg;
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status) { advertise(); break; }
        atomic_fetch_add(&generation,1); atomic_store(&connection,event->connect.conn_handle);
        atomic_store(&subscribed,false); atomic_store(&mtu,23);
        ESP_LOGI(TAG,"Connected handle=%u",event->connect.conn_handle);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        atomic_store(&connection,BLE_HS_CONN_HANDLE_NONE); atomic_fetch_add(&generation,1);
        atomic_store(&subscribed,false); atomic_store(&ack_status,BLE_HS_ENOTCONN); xSemaphoreGive(ack);
        ESP_LOGI(TAG,"Disconnected reason=%d; advertising again",event->disconnect.reason);
        advertise(); break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if(event->subscribe.attr_handle==data_handle) atomic_store(&subscribed,event->subscribe.cur_indicate);
        break;
    case BLE_GAP_EVENT_MTU:
        atomic_store(&mtu,event->mtu.value); ESP_LOGI(TAG,"ATT MTU=%u",event->mtu.value); break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if(event->notify_tx.indication && event->notify_tx.attr_handle==data_handle && event->notify_tx.status!=0) {
            atomic_store(&ack_status,event->notify_tx.status); xSemaphoreGive(ack);
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise(); break;
    default: break;
    }
    return 0;
}
static void advertise(void) {
    struct ble_hs_adv_fields adv={0},response={0};
    adv.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128=(ble_uuid128_t*)&service_uuid; adv.num_uuids128=1; adv.uuids128_is_complete=1;
    /* A full 128-bit UUID plus name will not fit in one 31-byte advertisement. */
    response.name=(uint8_t*)name; response.name_len=strlen(name); response.name_is_complete=1;
    int rc=ble_gap_adv_set_fields(&adv);
    if(!rc) rc=ble_gap_adv_rsp_set_fields(&response);
    struct ble_gap_adv_params params={.conn_mode=BLE_GAP_CONN_MODE_UND,.disc_mode=BLE_GAP_DISC_MODE_GEN,
        .itvl_min=160,.itvl_max=240};
    if(!rc) rc=ble_gap_adv_start(address_type,NULL,BLE_HS_FOREVER,&params,gap_event,NULL);
    if(rc) ESP_LOGE(TAG,"Advertising failed rc=%d",rc);
    else ESP_LOGI(TAG,"Advertising %s (BLE; Wi-Fi disabled)",name);
}
static void on_sync(void) {
    int rc=ble_hs_util_ensure_addr(0);
    if(!rc) rc=ble_hs_id_infer_auto(0,&address_type);
    if(rc) { ESP_LOGE(TAG,"BLE address init failed rc=%d",rc); return; }
    advertise();
}
static void on_reset(int reason) {
    atomic_store(&connection,BLE_HS_CONN_HANDLE_NONE); atomic_fetch_add(&generation,1);
    atomic_store(&subscribed,false); atomic_store(&ack_status,BLE_HS_ENOTCONN); xSemaphoreGive(ack);
    ESP_LOGW(TAG,"Host reset reason=%d",reason);
}
static void host_task(void *arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }
void ble_link_start(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    requests=xQueueCreate(1,sizeof(request)); ack=xSemaphoreCreateBinary(); configASSERT(requests && ack);
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_BT));
    snprintf(name,sizeof(name),"UBX-M9-BLE-%02X%02X%02X",mac[3],mac[4],mac[5]);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb=on_sync; ble_hs_cfg.reset_cb=on_reset;
    ble_svc_gap_init(); ble_svc_gatt_init();
    configASSERT(ble_svc_gap_device_name_set(name)==0);
    configASSERT(ble_att_set_preferred_mtu(247)==0);
    configASSERT(ble_gatts_count_cfg(services)==0); configASSERT(ble_gatts_add_svcs(services)==0);
    configASSERT(xTaskCreate(transfer_task,"ble_data",6144,NULL,3,NULL)==pdPASS);
    nimble_port_freertos_init(host_task);
}
