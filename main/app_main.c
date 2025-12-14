/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "string.h"
#include "wifi_sta.h"
#include "http_server_app.h"
/**
 * This is an example which echos any data it receives on configured UART back to the sender,
 * with hardware flow control turned off. It does not use UART driver event queue.
 *
 * - Port: configured UART
 * - Receive (Rx) buffer: on
 * - Transmit (Tx) buffer: off
 * - Flow control: off
 * - Event queue: off
 * - Pin assignment: see defines below (See Kconfig)
 */
// ---- Biến toàn cục ----
float temp = 0.0f;
float humd = 0.0f;
bool  device_connected = false;
char  current_node_id[8] = "0001";

#define TAG       "GATEWAY"
#define TAG_LORA  "LORA"
#define BUF_SIZE  256
// Semaphore / Mutex
SemaphoreHandle_t lora_tx_mutex;       // bảo vệ UART TX
SemaphoreHandle_t config_sem;          // có config mới
// ID 4 node
#define NODE1 "0001"
#define NODE2 "0002"
#define NODE3 "0003"
#define NODE4 "0004"

// Dữ liệu 4 node
static const char *g_node_ids[2] = { NODE1, NODE2 };
static float g_node_temp[2] = {0};
static float g_node_humd[2] = {0};
static float g_node_soil[2] = {0};

typedef struct{
    bool intialized;
    float temperature_config;
    float humidity_config;
    float soil_config;
}node_threshold_cache_t;
static node_threshold_cache_t g_saved_thresholds[2];

extern node_threshold_t g_thresholds[2];
// Semaphore for config message
void lora_uart_send(const uint8_t *data, size_t len){
    if (xSemaphoreTake(lora_tx_mutex, pdMS_TO_TICKS(1000)) == pdTRUE){
        uart_write_bytes(UART_NUM_1, (const char *)data, len);
        xSemaphoreGive(lora_tx_mutex);
    }
    else{
        ESP_LOGW(TAG_LORA, "TX mutex timeout");
    }
}

// ==================== Tìm index node theo ID =================
int find_node_index_by_id(const char *node_id){
    for (int i = 0; i < 2; i++){
        if (strcmp(node_id, g_node_ids[i]) == 0){
            return i;
        }
    }
    return -1;
}

// ==================== Gửi phản hồi =================
void send_ok_for_send_req(const char *node_id)
{
    char msg[32];
    int len = snprintf(msg, sizeof(msg), "OK|%s\r\n", node_id);
    if (len > 0 && len < sizeof(msg))
    {
        lora_uart_send((uint8_t*)msg, len);
        ESP_LOGI("LORA", "Sent OK to %s", node_id);
    }
}
// ==================== Gửi ACK cho DATA =================
void send_ack_for_data(const char *node_id)
{
    char msg[32];
    int len = snprintf(msg, sizeof(msg), "ACK|%s\r\n", node_id);
    if (len > 0 && len < sizeof(msg))
    {
        lora_uart_send((uint8_t*)msg, len);
        ESP_LOGI("LORA", "Sent ACK to %s", node_id);
    }
}
void process_cfg_request(char *node_id, int idx){
    if(idx < 0 || idx >=2){
        ESP_LOGW(pcTaskGetName(NULL), "Invalid node index for CONFIG: %d", idx);
        return;
    }
    float t_high = g_thresholds[idx].temp_th;
    float h_high = g_thresholds[idx].hum_th ;
    float s_high  = g_thresholds[idx].soil_th; ;
    bool is_new = false;
    if(!g_saved_thresholds[idx].intialized){
        is_new = true;
    }
    else{
        if(g_saved_thresholds[idx].temperature_config != t_high ||
           g_saved_thresholds[idx].humidity_config    != h_high ||
           g_saved_thresholds[idx].soil_config        != s_high){
            is_new = true;
        }
    }
    char msg[64];
    if(!is_new){
        int n = snprintf(msg, sizeof(msg), "NOCHANGEDATA|%s\r\n", node_id);
        if (n > 0) lora_uart_send((uint8_t*)msg, (size_t)n);
        ESP_LOGI(TAG_LORA, "No change for %s, sent NOCHANGEDATA", node_id);
    }
    else{
        int n=snprintf(msg, sizeof(msg), "CFG|%s|TempTh:%.1f|HumTh:%.1f|SoilTh:%.1f\r\n",
                       node_id, t_high, h_high, s_high);
        if (n>0 && n < (int)sizeof(msg)){
            lora_uart_send((uint8_t*)msg, (size_t)n);
            ESP_LOGI(TAG_LORA, "Sent CONFIG to %s: TempTh=%.1f HumTh=%.1f SoilTh=%.1f",
                     node_id, t_high, h_high, s_high);
            g_saved_thresholds[idx].intialized = true;
            g_saved_thresholds[idx].temperature_config = t_high;
            g_saved_thresholds[idx].humidity_config    = h_high;
            g_saved_thresholds[idx].soil_config        = s_high;
        }
        else{
            ESP_LOGW(TAG_LORA, "Failed to build CFG frame for %s", node_id);
        }
    }
}
static void handle_one_line(char *buf)
{
    // 1) Node xin gửi dữ liệu: "SEND|0001"
    if (strncmp(buf, "SEND|", 5) == 0){
        char node_id[8] = {0};
        // ví dụ buffer: "SEND|0001\r\n"
        if (sscanf(buf, "SEND|%7s", node_id) == 1){
            int idx = find_node_index_by_id(node_id);
            if (idx >= 0){
                ESP_LOGI(pcTaskGetName(NULL),"Node %s request SEND (idx=%d)", node_id, idx);

                // trả lời OK, cho phép node gửi DATA
                send_ok_for_send_req(node_id);
            }
            else{
                ESP_LOGW(pcTaskGetName(NULL), "SEND from unknown node_id: %s", node_id);
                // nếu thích có thể gửi ERR|id ở đây
            }
        }
        else{
            ESP_LOGW(pcTaskGetName(NULL), "Parse SEND error: %s", buf);
        }
    }
    // 2) Node gửi dữ liệu: "DATA|<id>|Hum: xx.x Tmp: yy.y"
    else if (strncmp(buf, "DATA|", 5) == 0){
        char node_id[8] = {0};
        float h, t;

        int matched = sscanf(buf,"DATA|%7[^|]|Hum: %f Tmp: %f", node_id, &h, &t);
        if (matched == 3){
            int idx = find_node_index_by_id(node_id);
            if (idx >= 0){
                g_node_humd[idx] = h;
                g_node_temp[idx] = t;
                ESP_LOGI(pcTaskGetName(NULL),"Node %s (idx=%d): Hum=%.1f Tmp=%.1f", node_id, idx, h, t);
                // gửi ACK cho DATA (để node biết đã nhận)
                send_ack_for_data(node_id);
            }
            else{
                ESP_LOGW(pcTaskGetName(NULL),
                         "DATA from unknown node_id: %s", node_id);
            }
        }
        else
        {
            ESP_LOGW(pcTaskGetName(NULL),
                     "Parse DATA error: %s", buf);
        }
    }
    else if(strncmp(buf, "CONFIG|", 7) == 0){
        char node_id[8]={0};
        if(sscanf(buf, "CONFIG|%7s", node_id) == 1){
            int idx = find_node_index_by_id(node_id);
            if (idx >= 0){
                process_cfg_request(node_id, idx);
            }
            else{
                ESP_LOGW(pcTaskGetName(NULL),
                         "CONFIG from unknown node_id: %s", node_id);
            }
        }
    }
    // 3) Loại gói khác
    else
    {
        ESP_LOGW(pcTaskGetName(NULL),
                 "Unknown packet: %s", buf);
    }
}
void task_rx(void *pvParameters)
{
    ESP_LOGI(pcTaskGetName(NULL), "Start RX");

    static char line_buf[256];
    static int  line_idx = 0;

    uint8_t tmp[64];

    while (1)
    {
        // đọc 1 mẻ byte từ UART (có thể là 1 byte, 10 byte, 50 byte...)
        int rxLen = uart_read_bytes(UART_NUM_1, tmp, sizeof(tmp), pdMS_TO_TICKS(100)); // 10s timeout
        if (rxLen > 0)
        {
            for (int i = 0; i < rxLen; i++)
            {
                char c = (char)tmp[i];
                // tránh tràn line_buf
                if (line_idx < (int)sizeof(line_buf) - 1)
                {
                    line_buf[line_idx++] = c;
                }
                // gặp '\n' => kết thúc 1 dòng / 1 packet
                if (c == '\n')
                {
                    line_buf[line_idx] = '\0'; // đóng chuỗi

                    ESP_LOGI(pcTaskGetName(NULL),
                             "RX line (%d): %s", line_idx, line_buf);
                    handle_one_line(line_buf);  // xử lý SEND / DATA
                    line_idx = 0; // reset cho dòng tiếp theo
                }
            }
            // nếu bị tràn buffer mà chưa thấy '\n' => drop
            if (line_idx >= (int)sizeof(line_buf) - 1)
            {
                ESP_LOGW(pcTaskGetName(NULL),
                         "Line too long, drop");
                line_idx = 0;
            }
        }
        else
        {
            // không nhận được gì trong 10s, chỉ log chơi
            ESP_LOGD(pcTaskGetName(NULL), "No data for 10s");
        }
    }
}
void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    wifi_start();
    start_webserver();
    ESP_LOGI(TAG, "Gateway LoRa UART only - start");
    lora_tx_mutex = xSemaphoreCreateMutex();
    if (!lora_tx_mutex )
    {
        ESP_LOGE(TAG, "Create semaphore/mutex failed");
        return;
    }
        uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, 17, 16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // xTaskCreate(echo_task, "uart_echo_task", 4096, NULL, 10, NULL);
    // xTaskCreate(uart_write_task, "uart_write_task", 4096, NULL,  13, NULL);
    xTaskCreate(task_rx, "task_rx", 4096, NULL, 12, NULL);
}
