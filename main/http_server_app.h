#ifndef __HTTP_SERVER_APP_H
#define __HTTP_SERVER_APP_H
#include "esp_err.h"
#include <esp_http_server.h>
typedef struct {
    char  id[5];        // "0001", "0002"
    float temp;
    float hum;
    float soil;
} node_info_t;
typedef struct {
    char  id[5];     // "0001", "0002"
    float temp_th;
    float hum_th;
    float soil_th;
} node_threshold_t;
void start_webserver(void);
void stop_webserver(void);
#endif