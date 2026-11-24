// Audio output: the output task owns I2S and takes PCM from the current source -> optical S/PDIF or 3.5mm DAC.
// Outputs silence continuously when there is no data so the receiver stays locked. Only one source at a time.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const char *id;      // "spdif" / "analog"
    const char *name;    // display name
    bool available;      // connected and usable
    const char *note;    // explanation when unavailable
} output_desc_t;

typedef enum {
    OUT_SRC_NONE = 0,    // silence
    OUT_SRC_DLNA,        // pushes into the PCM buffer (large buffer, prebuffering)
    OUT_SRC_AIRPLAY,     // the output task pulls from the AirPlay timeline on demand (low latency, for sync)
    OUT_SRC_BT,          // pushes into the PCM buffer (small buffer)
} out_src_t;

void output_init(void);

void output_set_source(out_src_t src);   // switch source (clears the buffer)
out_src_t output_source(void);
const char *output_source_name(out_src_t src);

// Call when sample rate / bit depth changes (waits for the buffer to drain before switching). Returns false if the current output doesn't support the rate
bool output_set_format(int sample_rate, int bits);
int output_rate(void);
int output_bits(void);
// Write interleaved stereo PCM (signed 24-bit in the low 24 bits of int32); blocks when the buffer is full; returns early when *abort is set
// max_pending: max frames to keep in the buffer (DLNA uses 0 for the whole buffer, Bluetooth a small value for lower latency)
int output_write(const int32_t *frames, int n, volatile bool *abort, int max_pending);
void output_flush(void);                 // drop unplayed PCM, reset the play counter, prebuffer again
void output_pause(bool pause);
void output_set_active(bool active);     // whether the decoder is continuously feeding data (for counting underruns and deciding whether to prebuffer)
uint64_t output_played_frames(void);     // frames actually played since the flush
int output_pending_frames(void);
uint32_t output_underruns(void);
int64_t output_first_audio_us(void);     // time the first frame after a flush was actually played, 0 if not yet

// For AirPlay sync: frames handed to I2S but not yet played (including the block being assembled), and the sample time
bool output_pipeline(int64_t *now_us, uint32_t *frames);
uint32_t output_dma_frames(void);        // DMA ring capacity (frames)

int output_count(void);
const output_desc_t *output_get(int i);
int output_current(void);
esp_err_t output_select(const char *id);  // switch immediately (also while playing) and remember the choice

// Volume (0-100, 0.5 dB per step). Optical uses fixed volume by default (bit-perfect); can be turned off in settings
void output_set_volume(int volume, bool mute);
int output_volume(void);
bool output_muted(void);
void output_set_fixed_optical(bool fixed);
bool output_fixed_optical(void);
bool output_volume_applies(void);        // whether the current output applies software volume
int32_t output_gain_q16(void);           // gain currently in effect (65536 when not applied)
