#include "http_server_app.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_check.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include <ctype.h>
#include "nvs_flash.h"
#include <json_generator.h>
#include <json_parser.h>
#define NVS_NAMESPACE "thresholds"
#define NVS_KEY "nodes_blob"

#define EXAMPLE_HTTP_QUERY_KEY_MAX_LEN  (64)
static httpd_handle_t server = NULL;
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
static const char *TAG = "HTPP_SERVER_APP";

#define MAX_NODES  2

static node_info_t g_nodes[MAX_NODES] = {
    { "0001", 10, 20, 30},
    { "0002", 40, 50, 30},
};
node_threshold_t g_thresholds[MAX_NODES] = {
    { "0001", 30, 50, 70, 40 },
    { "0002", 30, 50, 70, 40 },
};
static esp_err_t save_node_to_flash(void){
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(nvs, NVS_KEY, g_thresholds, sizeof(g_thresholds));
    if(err != ESP_OK){
        ESP_LOGE(TAG, "NVS set blob failed: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return err;
    }
    err = nvs_commit(nvs);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    }
    else{
        ESP_LOGI(TAG, "Node thresholds saved to NVS");
    }
    nvs_close(nvs);
    return err;
}
static esp_err_t load_node_from_flash(void){
    nvs_handle_t nvs;
    size_t required_size = sizeof(g_thresholds);
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS not found, using default thresholds");
        return err;
    }
    err = nvs_get_blob(nvs, NVS_KEY, g_thresholds, &required_size);
    if (err == ESP_OK && required_size == sizeof(g_thresholds)) {
        ESP_LOGI(TAG, "Thresholds loaded from NVS");
    } else {
        ESP_LOGW(TAG, "No valid thresholds in NVS, using defaults");
    }
    nvs_close(nvs);
    return err;
}
static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (strcmp("/interface", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/hello URI is not available");
        /* Return ESP_OK to keep underlying socket open */
        return ESP_OK;
    } 
    /* For any other URI send 404 and close socket */
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}
static int generate_data_json(char *buffer, size_t buf_size){
    json_gen_str_t jstr;
    json_gen_str_start(&jstr, buffer, buf_size, NULL, NULL);
    json_gen_start_array(&jstr);
    for(int i=0; i<MAX_NODES; i++){
        json_gen_start_object(&jstr);
        json_gen_obj_set_string(&jstr, "id", g_nodes[i].id);
        json_gen_obj_set_string(&jstr, "type", "data");
        json_gen_obj_set_float(&jstr, "temp", g_nodes[i].temp);
        json_gen_obj_set_float(&jstr, "hum", g_nodes[i].hum);
        json_gen_obj_set_float(&jstr, "soil", g_nodes[i].soil);
        json_gen_end_object(&jstr);
    }
    json_gen_end_array(&jstr);
    return json_gen_str_end(&jstr); 
}
static esp_err_t parse_config_from_req(char *buf, int len, char *id, float *temp, float *hum, float *soil, int *period_sec)
{
    jparse_ctx_t jctx;
    char type[16];
    if (json_parse_start(&jctx, buf, len) != OS_SUCCESS) {
        return ESP_FAIL; 
    }
    if (json_obj_get_string(&jctx, "type", type, sizeof(type)) != OS_SUCCESS || strcmp(type, "config") != 0) {
        json_parse_end(&jctx);
        return ESP_FAIL;
    }
    if( json_obj_get_string(&jctx, "id", id, 5 ) != OS_SUCCESS || 
        json_obj_get_float(&jctx, "temp",temp) != OS_SUCCESS ||
        json_obj_get_float(&jctx, "hum", hum) != OS_SUCCESS ||
        json_obj_get_float(&jctx, "soil", soil) != OS_SUCCESS ||
        json_obj_get_int(&jctx, "period_sec", period_sec) != OS_SUCCESS ){
        json_parse_end(&jctx);
        return ESP_FAIL;
    }
    json_parse_end(&jctx);
    return ESP_OK;
}
static esp_err_t get_server_interface_handler(httpd_req_t *req)
{
    // const char* resp_str = (const char*) "Hello world";
    // httpd_resp_send(req, resp_str, strlen(resp_str));
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,(const char*)index_html_start,index_html_end-index_html_start);
    return ESP_OK;
}
static const httpd_uri_t get_server_interface = {
    .uri       = "/interface",
    .method    = HTTP_GET,
    .handler   = get_server_interface_handler,
    .user_ctx  = "NULL"
};
static esp_err_t get_data_handler(httpd_req_t *req)
{
    char json_buffer[512];
    int json_len = generate_data_json(json_buffer, sizeof(json_buffer));
    if (json_len < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate JSON");
        return ESP_FAIL;
    }
    printf("Sending data: %s\n", json_buffer);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_buffer, json_len);
    return ESP_OK;
}
static const httpd_uri_t get_data_uri = {
    .uri       = "/data",
    .method    = HTTP_GET,
    .handler   = get_data_handler,
    .user_ctx  = "NULL"
};
static esp_err_t post_config_handler(httpd_req_t *req)
{
    int content_len = req->content_len;
    if(content_len <=0 || content_len>256){
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad POST length");
        return ESP_FAIL;
    }
    char buf[257];
    int ret = httpd_req_recv(req, buf, content_len);
    if(ret <=0){
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
        return ESP_FAIL;
    }
    buf[ret]=0;
    printf("Received config: %s\n", buf);
    char id_config[5];
    float temp_config, hum_config, soil_config;
    int period_sec_config;
    if (parse_config_from_req(buf, ret, id_config, &temp_config, &hum_config, &soil_config,&period_sec_config ) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    bool node_found = false;
    for(int i=0; i<MAX_NODES; i++){
        if(strcmp(g_thresholds[i].id, id_config) == 0){
            g_thresholds[i].temp_th = temp_config;
            g_thresholds[i].hum_th = hum_config;
            g_thresholds[i].soil_th = soil_config;
            g_thresholds[i].period_sec = period_sec_config;
            save_node_to_flash();
            node_found = true;
            break;
        }
    }
    if(!node_found){
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown node ID");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}
static const httpd_uri_t post_config_uri ={
    .uri       = "/config",
    .method    = HTTP_POST,
    .handler   = post_config_handler,
    .user_ctx  = "NULL"
};
static int generate_thresholds_json(char *buffer, size_t buf_size){
    json_gen_str_t jstr;
    json_gen_str_start(&jstr, buffer, buf_size, NULL, NULL);
    json_gen_start_array(&jstr);

    for (int i = 0; i < 2; i++) {
        json_gen_start_object(&jstr);
        json_gen_obj_set_string(&jstr, "id", g_thresholds[i].id);
        json_gen_obj_set_float(&jstr, "temp_th", g_thresholds[i].temp_th);
        json_gen_obj_set_float(&jstr, "hum_th",  g_thresholds[i].hum_th);
        json_gen_obj_set_float(&jstr, "soil_th", g_thresholds[i].soil_th);
        json_gen_obj_set_int(&jstr, "period_sec", g_thresholds[i].period_sec);
        json_gen_end_object(&jstr);
    }

    json_gen_end_array(&jstr);
    return json_gen_str_end(&jstr);
}
static esp_err_t get_thresholds_handler(httpd_req_t *req)
{
    char json_buffer[512];
    int json_len = generate_thresholds_json(json_buffer, sizeof(json_buffer));
    if (json_len < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate JSON");
        return ESP_FAIL;
    }
    printf("Sending thresholds: %s\n", json_buffer);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_buffer, json_len);
    return ESP_OK;
}
static const httpd_uri_t get_thresholds_uri ={
    .uri       = "/thresholds",
    .method    = HTTP_GET,
    .handler   = get_thresholds_handler,
    .user_ctx  = "NULL"
};
void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    // Start the httpd server
    load_node_from_flash();
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server,&get_server_interface);
        httpd_register_uri_handler(server,&get_data_uri);
        httpd_register_uri_handler(server,&post_config_uri);
        httpd_register_uri_handler(server, &get_thresholds_uri);
        httpd_register_err_handler(server,HTTPD_404_NOT_FOUND,http_404_error_handler);
        //httpd_register_uri_handler(server, &echo);
    }
    else{
        ESP_LOGI(TAG, "Error starting server!");
    }
}
void stop_webserver(void)
{
    // Stop the httpd server
    httpd_stop(server);
}