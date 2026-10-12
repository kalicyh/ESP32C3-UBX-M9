#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "ubx.h"
#include "gnss_web.h"
#include "ble_link.h"

static const char *TAG = "m9";
static ubx_parser parser;
static QueueHandle_t output_queue;
#ifdef CONFIG_GNSS_I2C
static i2c_master_dev_handle_t device;
#else
static QueueHandle_t uart_events;
#endif
static atomic_uint dropped;
static unsigned io_errors, uart_overflows;
static bool version_seen, response_seen;
static uint8_t wanted_class, wanted_id;
typedef struct { size_t len; uint8_t data[]; } packet;

static void received(const uint8_t *f, size_t n, void *context) {
    (void)context;
    gnss_web_receive(f, n);
    if (f[2] == 0x0a && f[3] == 4 && n >= 48) version_seen = true;
    if (f[2] == wanted_class && f[3] == wanted_id) response_seen = true;
    packet *p = malloc(sizeof(*p) + n);
    if (!p) { atomic_fetch_add(&dropped, 1); return; }
    p->len = n;
    memcpy(p->data, f, n);
    if (xQueueSend(output_queue, &p, 0) != pdTRUE) {
        free(p); atomic_fetch_add(&dropped, 1);
    }
}
static void json_string(const uint8_t *s, size_t n) {
    putchar('"');
    for (size_t i = 0; i < n && s[i]; ++i) {
        if (s[i] == '"' || s[i] == '\\') printf("\\%c", s[i]);
        else if (s[i] < 32 || s[i] >= 127) printf("\\u%04x", s[i]);
        else putchar(s[i]);
    }
    putchar('"');
}
static void output_task(void *arg) {
    (void)arg;
    packet *pkt;
    for (;;) {
        if (xQueueReceive(output_queue, &pkt, portMAX_DELAY) != pdTRUE) continue;
        const uint8_t *f = pkt->data, *p = f + 6;
        size_t n = ubx_u16(f + 4);
        /* One stdio lock keeps frames intact when ESP-IDF logs concurrently. */
        flockfile(stdout);
        printf("{\"type\":\"UBX\",\"class\":%u,\"id\":%u,\"length\":%u,\"frame_hex\":\"",
               f[2], f[3], (unsigned)n);
        static const char hex[] = "0123456789ABCDEF";
        char encoded[256];
        for (size_t offset = 0; offset < pkt->len;) {
            size_t count = pkt->len - offset;
            if (count > sizeof(encoded) / 2) count = sizeof(encoded) / 2;
            for (size_t i = 0; i < count; ++i) {
                encoded[2*i] = hex[f[offset+i] >> 4];
                encoded[2*i+1] = hex[f[offset+i] & 15];
            }
            fwrite(encoded, 2, count, stdout); offset += count;
        }
        printf("\"}\n");
        if (f[2] == 1 && f[3] == 7 && n >= 92) {
            printf("{\"type\":\"PVT\",\"iTOW_ms\":%"PRIu32",\"utc\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\","
                   "\"valid_flags\":%u,\"time_accuracy_ns\":%"PRIu32",\"nano\":%"PRId32","
                   "\"fix_type\":%u,\"fix_ok\":%s,\"flags\":%u,\"flags2\":%u,\"satellites\":%u,"
                   "\"lon_deg\":%.7f,\"lat_deg\":%.7f,\"height_m\":%.3f,\"msl_m\":%.3f,"
                   "\"hacc_m\":%.3f,\"vacc_m\":%.3f,\"vel_n_mps\":%.3f,\"vel_e_mps\":%.3f,\"vel_d_mps\":%.3f,"
                   "\"ground_speed_mps\":%.3f,\"heading_deg\":%.5f,\"speed_accuracy_mps\":%.3f,"
                   "\"heading_accuracy_deg\":%.5f,\"pdop\":%.2f,\"flags3\":%u,\"vehicle_heading_deg\":%.5f}\n",
                   ubx_u32(p), ubx_u16(p+4), p[6], p[7], p[8], p[9], p[10], p[11], ubx_u32(p+12), ubx_i32(p+16),
                   p[20], (p[21]&1) ? "true":"false", p[21], p[22], p[23],
                   ubx_i32(p+24)*1e-7, ubx_i32(p+28)*1e-7, ubx_i32(p+32)*1e-3, ubx_i32(p+36)*1e-3,
                   ubx_u32(p+40)*1e-3, ubx_u32(p+44)*1e-3, ubx_i32(p+48)*1e-3, ubx_i32(p+52)*1e-3,
                   ubx_i32(p+56)*1e-3, ubx_i32(p+60)*1e-3, ubx_i32(p+64)*1e-5, ubx_u32(p+68)*1e-3,
                   ubx_u32(p+72)*1e-5, ubx_u16(p+76)*0.01, ubx_u16(p+78), ubx_i32(p+84)*1e-5);
        } else if (f[2] == 1 && f[3] == 0x35 && n >= 8 && p[4] == 1 && n >= 8u + 12u*p[5]) {
            for (unsigned i = 0; i < p[5]; ++i) {
                const uint8_t *s = p + 8 + 12*i;
                int elevation = s[3] < 128 ? s[3] : (int)s[3]-256;
                int azimuth = ubx_u16(s+4); if (azimuth >= 32768) azimuth -= 65536;
                int residual = ubx_u16(s+6); if (residual >= 32768) residual -= 65536;
                printf("{\"type\":\"SAT\",\"iTOW_ms\":%"PRIu32",\"gnss_id\":%u,\"sv_id\":%u,\"cno_dbhz\":%u,"
                       "\"elevation_deg\":%d,\"azimuth_deg\":%d,\"pseudorange_residual_m\":%.1f,\"flags\":%"PRIu32"}\n",
                       ubx_u32(p), s[0], s[1], s[2], elevation, azimuth, residual*0.1, ubx_u32(s+8));
            }
        } else if (f[2] == 1 && f[3] == 4 && n >= 18) {
            printf("{\"type\":\"DOP\",\"iTOW_ms\":%"PRIu32",\"g\":%.2f,\"p\":%.2f,\"t\":%.2f,\"v\":%.2f,\"h\":%.2f,\"n\":%.2f,\"e\":%.2f}\n",
                   ubx_u32(p), ubx_u16(p+4)*.01, ubx_u16(p+6)*.01, ubx_u16(p+8)*.01,
                   ubx_u16(p+10)*.01, ubx_u16(p+12)*.01, ubx_u16(p+14)*.01, ubx_u16(p+16)*.01);
        } else if (f[2] == 0x0a && f[3] == 4 && n >= 40) {
            printf("{\"type\":\"VERSION\",\"software\":"); json_string(p,30);
            printf(",\"hardware\":"); json_string(p+30,10);
            printf(",\"extensions\":[");
            for (size_t i = 40; i + 30 <= n; i += 30) { if (i != 40) putchar(','); json_string(p+i,30); }
            printf("]}\n");
        } else if (f[2] == 5 && n == 2) {
            printf("{\"type\":\"%s\",\"class\":%u,\"id\":%u}\n", f[3] == 1 ? "ACK":"NAK", p[0], p[1]);
        }
        funlockfile(stdout);
        free(pkt);
    }
}
static esp_err_t send_message(uint8_t cls, uint8_t id, const uint8_t *payload, size_t n) {
    uint8_t frame[64];
    size_t len = ubx_encode(frame,sizeof(frame),cls,id,payload,n);
    if (!len) return ESP_ERR_INVALID_SIZE;
#ifdef CONFIG_GNSS_I2C
    return i2c_master_transmit(device,frame,len,200);
#else
    return uart_write_bytes(UART_NUM_1,frame,len) == (int)len ? ESP_OK : ESP_FAIL;
#endif
}
static void pump(unsigned duration_ms) {
    int64_t until = esp_timer_get_time() + duration_ms*1000LL;
    uint8_t bytes[512];
    static int64_t last_rx;
    do {
        int count = 0;
#ifdef CONFIG_GNSS_I2C
        uint8_t reg = 0xfd, pending[2];
        esp_err_t err = i2c_master_transmit_receive(device,&reg,1,pending,2,100);
        if (err == ESP_OK) {
            unsigned available = ((unsigned)pending[0]<<8) | pending[1];
            if (available && available != 0xffff) {
                count = available > sizeof(bytes) ? sizeof(bytes) : available;
                reg = 0xff;
                err = i2c_master_transmit_receive(device,&reg,1,bytes,count,100);
            }
        }
        if (err != ESP_OK) { ++io_errors; count = 0; }
        if (!count) vTaskDelay(pdMS_TO_TICKS(10));
#else
        uart_event_t event;
        while (xQueueReceive(uart_events,&event,0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
                ++uart_overflows; parser.used = 0;
                uart_flush_input(UART_NUM_1);
            } else if (event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) ++io_errors;
        }
        count = uart_read_bytes(UART_NUM_1,bytes,sizeof(bytes),pdMS_TO_TICKS(20));
        if (count < 0) { ++io_errors; count = 0; }
#endif
        if (count) {
            if (last_rx && esp_timer_get_time() - last_rx > 2000000) parser.used = 0;
            last_rx = esp_timer_get_time();
            ubx_feed(&parser,bytes,count);
        }
    } while (esp_timer_get_time() < until);
}
static bool poll_message(uint8_t cls, uint8_t id, unsigned timeout) {
    wanted_class = cls; wanted_id = id; response_seen = false;
    if (send_message(cls,id,NULL,0) != ESP_OK) { ++io_errors; return false; }
    int64_t deadline = esp_timer_get_time() + timeout*1000LL;
    do { pump(20); } while (!response_seen && esp_timer_get_time() < deadline);
    return response_seen;
}
static void transport_init(void) {
#ifdef CONFIG_GNSS_I2C
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t config = {
        .i2c_port=I2C_NUM_0, .sda_io_num=4, .scl_io_num=3,
        .clk_source=I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt=7,
        .flags.enable_internal_pullup=true
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&config,&bus));
    i2c_device_config_t dev = { .dev_addr_length=I2C_ADDR_BIT_LEN_7, .device_address=0x42, .scl_speed_hz=100000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus,&dev,&device));
    ESP_LOGI(TAG,"I2C SCL=3 SDA=4 address=0x42");
#else
    uart_config_t config = { .baud_rate=CONFIG_GNSS_BAUD, .data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE, .stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE, .source_clk=UART_SCLK_DEFAULT };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1,32768,0,32,&uart_events,0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1,&config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1,2,1,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG,"UART1 ESP TX=2 RX=1");
#endif
}
void app_main(void) {
    gnss_web_init();
    ble_link_start();
    output_queue = xQueueCreate(12,sizeof(packet *));
    configASSERT(output_queue);
    parser.callback = received;
    configASSERT(xTaskCreate(output_task,"gnss_output",6144,NULL,4,NULL) == pdPASS);
    transport_init();
    vTaskDelay(pdMS_TO_TICKS(500));
    while (!version_seen) {
#ifdef CONFIG_GNSS_UART
        const int rates[] = {CONFIG_GNSS_BAUD,9600,38400,115200,57600,19200,230400,460800,921600,4800};
        for (unsigned i=0; i<sizeof(rates)/sizeof(rates[0]) && !version_seen; ++i) {
            ESP_ERROR_CHECK(uart_wait_tx_done(UART_NUM_1,pdMS_TO_TICKS(200)));
            ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_1,rates[i]));
            uart_flush_input(UART_NUM_1);
            parser.used = 0;
            poll_message(0x0a,4,1200);
            if (version_seen) ESP_LOGI(TAG,"Detected GNSS baud=%d",rates[i]);
        }
#else
        poll_message(0x0a,4,1500);
#endif
        if (!version_seen) {
            ESP_LOGW(TAG,"No UBX MON-VER response: check power, wiring, UBX input/output and selected transport; retry in 5s");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
#ifdef CONFIG_GNSS_RAWX
    const uint8_t ids[] = {0x15,0x13};
    for (unsigned i=0; i<sizeof(ids); ++i) {
        uint8_t cfg[] = {2,ids[i],1};
        ESP_LOGI(TAG,"Requesting RXM %02x; inspect ACK/NAK for CFG-MSG",ids[i]);
        ESP_ERROR_CHECK(send_message(6,1,cfg,sizeof(cfg)));
        pump(1000);
    }
#endif
    /* Only data polls: never poll command IDs such as NAV-RESETODO. */
    const uint8_t messages[][2] = {
        {1,0x35},{1,0x43},{1,4},{1,3},{1,1},{1,2},{1,0x11},{1,0x12},
        {1,0x20},{1,0x21},{1,0x22},{1,0x23},{1,0x24},{1,0x25},{1,0x26},{1,0x27},
        {1,0x34},{1,0x36},{1,0x32},{1,0x42},{1,0x39},{1,9},{1,0x60},
        {0x0a,0x36},{0x0a,0x28},{0x0a,9},{0x0a,0x0b},{0x0a,0x37},
        {0x0a,0x38},{0x0a,0x31},{0x0a,0x27},{0x0a,4}
    };
    unsigned misses[sizeof(messages)/sizeof(messages[0])] = {0};
    size_t index = 0;
    int64_t next_pvt = 0, next_stats = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= next_pvt) {
            if (!poll_message(1,7,1000)) ESP_LOGW(TAG,"NAV-PVT timeout (no response, not proof of no fix)");
            next_pvt = esp_timer_get_time() + 1000000;
        }
        if (!poll_message(messages[index][0],messages[index][1],700)) {
            if (++misses[index] == 1 || misses[index] % 10 == 0)
                ESP_LOGW(TAG,"Poll %02x/%02x timeout: unsupported, unavailable or transport loss",messages[index][0],messages[index][1]);
        } else misses[index] = 0;
        index = (index+1) % (sizeof(messages)/sizeof(messages[0]));
        pump(200);
        if (esp_timer_get_time() >= next_stats) {
            gnss_web_stats(parser.bad_checksum, parser.oversized, atomic_load(&dropped), io_errors, uart_overflows);
            ESP_LOGI(TAG,"stats checksum=%"PRIu32" oversize=%"PRIu32" output_drop=%u io_errors=%u uart_overflow=%u",
                parser.bad_checksum,parser.oversized,atomic_load(&dropped),io_errors,uart_overflows);
            next_stats = esp_timer_get_time()+10000000;
        }
    }
}
