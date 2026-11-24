#pragma once
#include "esp_http_server.h"

// Web front end: / page, /api/status playback info, /api/output output switching
void web_start(httpd_handle_t h, const char *ip);
