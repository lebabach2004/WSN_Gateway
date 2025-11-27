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
static const char *g_node_ids[4] = { NODE1, NODE2, NODE3, NODE4 };
float g_node_temp[4] = {0};
float g_node_humd[4] = {0};

char   config_msg[64];
size_t config_len = 0;

// Semaphore for config message
void lora_uart_send(const uint8_t *data, size_t len)
{
    if (xSemaphoreTake(lora_tx_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        uart_write_bytes(UART_NUM_1, (const char *)data, len);
        xSemaphoreGive(lora_tx_mutex);
    }
    else
    {
        ESP_LOGW(TAG_LORA, "TX mutex timeout");
    }
}
// ================= Gửi cấu hình ngưỡng cho Node =================
void send_threshold_to_node(const char *node_id,
                            float t_high, float t_low,
                            float h_high, float h_low)
{
    strncpy(current_node_id, node_id, sizeof(current_node_id) - 1);
    current_node_id[sizeof(current_node_id) - 1] = '\0';

    config_len = snprintf(config_msg, sizeof(config_msg),"CFG|%s|TH=%.1f|TL=%.1f|HH=%.1f|HL=%.1f\r\n",
                          current_node_id, t_high, t_low, h_high, h_low);

    if (config_len <= 0 || config_len >= sizeof(config_msg))
    {
        ESP_LOGE(TAG_LORA, "Build config message error");
        return;
    }

    xSemaphoreGive(config_sem);   // báo cho task_tx_config
}
// ================= Task TX gửi CONFIG =================
void task_tx_config(void *pvParameters)
{
    ESP_LOGI(pcTaskGetName(NULL), "Start TX_CONFIG");

    while (1)
    {
        if (xSemaphoreTake(config_sem, portMAX_DELAY) == pdTRUE)
        {
            if (device_connected && config_len > 0)
            {
                lora_uart_send((uint8_t *)config_msg, config_len);
                ESP_LOGI(pcTaskGetName(NULL), "Sent CONFIG: %.*s",
                         config_len, config_msg);
            }
        }
    }
}

// ==================== Tìm index node theo ID =================
int find_node_index_by_id(const char *node_id)
{
    for (int i = 0; i < 4; i++)
    {
        if (strcmp(node_id, g_node_ids[i]) == 0)
        {
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
// ==================== Task RX nhận dữ liệu từ Node =================
void task_rx(void *pvParameters)
{
    ESP_LOGI(pcTaskGetName(NULL), "Start RX");
    uint8_t buf[256];

    while (1)
    {
        int rxLen = uart_read_bytes(UART_NUM_1,
                                    buf,
                                    sizeof(buf) - 1,
                                    pdMS_TO_TICKS(10000)); // 10s timeout

        if (rxLen > 0)
        {
            buf[rxLen] = '\0';
            ESP_LOGI(pcTaskGetName(NULL), "RX(%d): %s", rxLen, buf);

            // 1) Node xin gửi dữ liệu: "SEND|0001"
            if (strncmp((char *)buf, "SEND|", 5) == 0)
            {
                char node_id[8] = {0};

                // ví dụ buffer: "SEND|0001\r\n"
                if (sscanf((char*)buf, "SEND|%7s", node_id) == 1)
                {
                    int idx = find_node_index_by_id(node_id);
                    if (idx >= 0)
                    {
                        ESP_LOGI(pcTaskGetName(NULL),
                                 "Node %s request SEND (idx=%d)", node_id, idx);
                        // trả lời OK, cho phép node gửi DATA
                        send_ok_for_send_req(node_id);
                    }
                    else
                    {
                        ESP_LOGW(pcTaskGetName(NULL),
                                 "SEND from unknown node_id: %s", node_id);
                        // có thể gửi ERR|id nếu muốn
                    }
                }
                else
                {
                    ESP_LOGW(pcTaskGetName(NULL),
                             "Parse SEND error: %s", buf);
                }
            }
            // 2) Node gửi dữ liệu: "DATA|<id>|Hum: xx.x Tmp: yy.y"
            else if (strncmp((char *)buf, "DATA|", 5) == 0)
            {
                char node_id[8] = {0};
                float h, t;

                int matched = sscanf((char*)buf,
                                     "DATA|%7[^|]|Hum: %f Tmp: %f",
                                     node_id, &h, &t);
                if (matched == 3)
                {
                    int idx = find_node_index_by_id(node_id);
                    if (idx >= 0)
                    {
                        g_node_humd[idx] = h;
                        g_node_temp[idx] = t;

                        ESP_LOGI(pcTaskGetName(NULL),
                                 "Node %s (idx=%d): Hum=%.1f Tmp=%.1f",
                                 node_id, idx, h, t);

                        // gửi ACK cho DATA (để node biết đã nhận)
                        send_ack_for_data(node_id);
                    }
                    else
                    {
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
            // 3) Loại gói khác (sau này thêm nếu cần)
            else
            {
                ESP_LOGW(pcTaskGetName(NULL),
                         "Unknown packet: %s", buf);
            }
        }
        else
        {
            // không nhận được gì trong 10s, chỉ log chơi
            ESP_LOGD(pcTaskGetName(NULL), "No data for 10s");
        }
    }
}
// static void echo_task(void *arg)
// {
//     // Configure a temporary buffer for the incoming data
//     uint8_t *data = (uint8_t *) malloc(BUF_SIZE);

//     while (1) {
//         // Read data from the UART
//         int len = uart_read_bytes(UART_NUM_1, data, (BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);
//         // Write data back to the UART
//         uart_write_bytes(UART_NUM_1, (const char *) data, len);
//         if (len) {
//             data[len] = '\0';
//             ESP_LOGI(TAG, "Recv str: %s", (char *) data);
//             printf("Do dai:%d\n",len);
//         }
//     }
// }
// static void uart_write_task(void *arg)
// {
//     const char *msg = "Hello\n";
//     while (1) {
//         uart_write_bytes(UART_NUM_1, msg, strlen(msg));
//         ESP_LOGI(TAG, "Sent: Hello");
//         vTaskDelay(pdMS_TO_TICKS(5000));   // gửi mỗi 2 giây
//     }
// }
void app_main(void)
{
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
}
