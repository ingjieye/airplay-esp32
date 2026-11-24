// LiveLab: wires the DLNA player, web page and source arbitration into airplay-esp32.
#pragma once
#include "output.h"

// Call once after the network is up: start DLNA (separate HTTP server), register web handlers, start source arbitration
void livelab_start(void);
// Call before OTA: pause DLNA and free the playback buffer (downloading while playing competes with the OTA for the network, and with the buffer occupying PSRAM the OTA has to write as it receives)
void livelab_ota_prepare(void);

// Source pre-emption: the latest source wins. take pauses/stops/disconnects other playing sources, then hands the output to src
void livelab_take(out_src_t src);
// Source finished (played to the end, stopped): if it still holds the output, return it to idle (AirPlay pulls on demand, silence when there's no session)
void livelab_release(out_src_t src);

// For the output adapter: each source remembers its own sample rate, switched when it gets the output
void livelab_output_take(out_src_t src);

#include "playback_events.h"
// For the web page: the source currently "in use" (none / dlna / airplay / bluetooth), and the latest AirPlay/Bluetooth playback events
const char *livelab_active_source(void);
bool livelab_source_playing(playback_source_t s);
