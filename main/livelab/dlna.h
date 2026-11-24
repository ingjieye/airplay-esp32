#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

// Register the DLNA handlers on an already running HTTP server and start SSDP
void dlna_start(httpd_handle_t h, const char *ip, int port);

// Volume from the web page (0-100), synced with NetEase
void dlna_set_volume(int volume, bool mute);
