// Player: DLNA control entry points + playback info (for the web front end)
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *state;          // DLNA transport state: NO_MEDIA_PRESENT / STOPPED / PLAYING / PAUSED_PLAYBACK / TRANSITIONING
    char title[128], artist[128], album[128], art[300];
    char codec[8];              // "FLAC" / "MP3" / ""
    bool lossless;
    int sample_rate, bits, channels;
    double duration, position;  // seconds
    int64_t file_size, downloaded, consumed;
    int buf_bytes, buf_cap;     // compressed-data buffer
    int pcm_ms;                 // PCM buffer duration
    int avg_kbps;
    uint32_t underruns;
    int start_ms;               // latency from Play received to first audible frame
    int restarts;               // number of HTTP re-requests
    int dec_load;               // decoder CPU1 usage in percent (between two queries)
    char error[96];
    char host[64];              // audio source host
} player_info_t;

void player_init(void);
void player_set_uri(const char *uri, const char *meta);
void player_play(void);
void player_pause(void);
void player_stop(void);
void player_seek(double seconds);
// Used when restoring a track after reboot: state becomes PAUSED_PLAYBACK, next Play starts from this position (invalidated by track change or stop)
void player_restore_paused(double seconds);
// Call before OTA: stop download and decoding, free the compressed-data buffer, set state to paused (track and position kept)
void player_suspend_for_ota(void);
bool player_has_error(void);  // current track failed (cleared on track change or a new Play)
const char *player_state(void);
double player_position(void);
double player_duration(void);
void player_get_info(player_info_t *out);
