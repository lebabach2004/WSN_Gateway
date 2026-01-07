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

#define CMD_SEND_REQUEST   0x01  
#define CMD_SEND_OK        0x02  
#define CMD_SENSOR_DATA    0x03  
#define CMD_DATA_ACK       0x04  
#define CMD_CFG_REQUEST    0x05  
#define CMD_CFG_DATA       0x07  
#define CMD_CFG_NOCHANGE   0x06 
#define HEADER_BYTE        0xAA

// Dữ liệu 4 node
static const char *g_node_ids[2] = { NODE1, NODE2 };

typedef struct{
    bool intialized;
    float temperature_config;
    float humidity_config;
    float soil_config;
    int period_sec;
}node_threshold_cache_t;
static node_threshold_cache_t g_saved_thresholds[2];

extern node_threshold_t g_thresholds[2];
extern node_info_t g_nodes[2];
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
int find_node_index_by_id(uint16_t id){
    for (int i = 0; i < 2; i++)
    {
        uint16_t stored_id = (uint16_t)strtol(g_node_ids[i], NULL, 16);
        if (stored_id == id)
        {
            return i;
        }
    }
    return -1;
}

// ==================== Gửi phản hồi =================
void send_ok_for_send_req(uint16_t id)
{
    uint8_t ok[4];
    ok[0] = HEADER_BYTE;
    ok[1] = (id >> 8) & 0xFF;
    ok[2] = id & 0xFF;
    ok[3] = CMD_SEND_OK;
    uart_write_bytes(UART_NUM_1, (const char*)ok, 4);
}
// ==================== Gửi ACK cho DATA =================
void send_ack_for_data(uint16_t id)
{
    uint8_t ack[4];
    ack[0] = HEADER_BYTE;
    ack[1] = (id >> 8) & 0xFF;
    ack[2] = id & 0xFF;
    ack[3] = CMD_DATA_ACK;
    uart_write_bytes(UART_NUM_1, (const char*)ack, 4);
}
void process_cfg_request(uint16_t id, int idx){
    if(idx < 0 || idx >=2){
        ESP_LOGW(pcTaskGetName(NULL), "Invalid node index for CONFIG: %d", idx);
        return;
    }
    float t_high = g_thresholds[idx].temp_th;
    float h_high = g_thresholds[idx].hum_th ;
    float s_high  = g_thresholds[idx].soil_th; 
    int period = g_thresholds[idx].period_sec;
    bool is_new = false;
    if(!g_saved_thresholds[idx].intialized){
        is_new = true;
    }
    else{
        if(g_saved_thresholds[idx].temperature_config != t_high ||
           g_saved_thresholds[idx].humidity_config    != h_high ||
           g_saved_thresholds[idx].soil_config        != s_high || 
           g_saved_thresholds[idx].period_sec         != period){
            is_new = true;
        }
    }
    char msg[64];
    if(!is_new){
        uint8_t nochange[4];
        nochange[0] = HEADER_BYTE;
        nochange[1] = (id >> 8) & 0xFF;
        nochange[2] = id & 0xFF;
        nochange[3] = CMD_CFG_NOCHANGE;
        uart_write_bytes(UART_NUM_1, (const char*)nochange, 4);
        ESP_LOGI(TAG_LORA, "Node %s: No CONFIG change, sent CFG_NOCHANGE", g_node_ids[idx]);
        ESP_LOG_BUFFER_HEX(TAG_LORA, nochange, 4);
    }
    else{
        uint8_t cfg[20];
        cfg[0] = HEADER_BYTE;
        cfg[1] = (id >> 8) & 0xFF;
        cfg[2] = id & 0xFF;
        cfg[3] = CMD_CFG_DATA;
        memcpy(&cfg[4],  &t_high, 4);
        memcpy(&cfg[8],  &h_high, 4);
        memcpy(&cfg[12], &s_high, 4);
        memcpy(&cfg[16], &period, 4);
        uart_write_bytes(UART_NUM_1, (const char*)cfg, 20);
        ESP_LOGI(TAG_LORA, "Sent CONFIG to %s: TempTh=%.1f HumTh=%.1f SoilTh=%.1f Period=%d",
                 g_node_ids[idx], t_high, h_high, s_high, period);
        ESP_LOG_BUFFER_HEX(TAG_LORA, cfg, 20);
        g_saved_thresholds[idx].intialized = true;
        g_saved_thresholds[idx].temperature_config = t_high;
        g_saved_thresholds[idx].humidity_config    = h_high;
        g_saved_thresholds[idx].soil_config        = s_high;
        g_saved_thresholds[idx].period_sec         = period;

    }
}
static void handle_one_line(uint8_t *buf,uint16_t len)
{
    if (len < 4) return;
    if (buf[0] != HEADER_BYTE) return;
    uint16_t id = ((uint16_t)buf[1] << 8) | buf[2];
    int idx = find_node_index_by_id(id);
    uint8_t cmd = buf[3];
    if (cmd == CMD_SEND_REQUEST && len == 4 && idx >= 0)
    {
        ESP_LOGI(pcTaskGetName(NULL), "Node %s request SEND", g_node_ids[idx]);
        send_ok_for_send_req(id);
    }
    else if (cmd == CMD_SENSOR_DATA && len == 16 && idx >= 0)
    {
        float hum, temp, soil;
        memcpy(&hum,  &buf[4],  4);
        memcpy(&temp, &buf[8],  4);
        memcpy(&soil, &buf[12], 4);

        g_nodes[idx].hum = hum;
        g_nodes[idx].temp = temp;
        g_nodes[idx].soil = soil;
        ESP_LOGI(pcTaskGetName(NULL), "Node %s DATA: Hum=%.1f Tmp=%.1f Soil=%.1f", g_node_ids[idx], hum, temp, soil);
        send_ack_for_data(id);
    }
    else if (cmd == CMD_CFG_REQUEST && len == 4 && idx >= 0)
    {
        ESP_LOGI(pcTaskGetName(NULL), "Node %s request CONFIG", g_node_ids[idx]);
        process_cfg_request(id, idx);
    }
    else
    {
        ESP_LOGW(pcTaskGetName(NULL), "Unknown packet or invalid length from %s", g_node_ids[idx]);
    }
}
void task_rx(void *pvParameters)
{
    ESP_LOGI(pcTaskGetName(NULL), "Start RX");

    uint8_t tmp[64];
    static uint8_t frame[20];
    static int fidx = 0;

    while (1)
    {
        int rxLen = uart_read_bytes(UART_NUM_1, tmp, sizeof(tmp), pdMS_TO_TICKS(30));
        if (rxLen > 0)
        {
            for (int i = 0; i < rxLen; i++)
            {
                uint8_t b = tmp[i];

                if (fidx == 0)
                {
                    if (b == HEADER_BYTE){
                        frame[0] = b;
                        fidx = 1;
                    }
                    continue;
                }
                else {
                    frame[fidx++] = b;
                }

                // nếu là gói 4 byte
                if (fidx == 4)
                {
                    uint8_t cmd = frame[3];
                    if (cmd == CMD_SEND_REQUEST || cmd == CMD_CFG_REQUEST)
                    {
                        ESP_LOG_BUFFER_HEX(TAG_LORA, frame, 4);
                        handle_one_line(frame, 4);
                        fidx = 0;
                    }
                }
                // nếu là sensor 16 byte
                else if (fidx == 16)
                {
                    ESP_LOG_BUFFER_HEX(TAG_LORA, frame, 16);
                    handle_one_line(frame, 16);
                    fidx = 0;
                }
                // nếu là config 20 byte
                else if (fidx == 20)
                {
                    handle_one_line(frame, 20);
                    fidx = 0;
                }

                if (fidx >= 20) fidx = 0;
            }
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
