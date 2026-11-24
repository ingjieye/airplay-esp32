// M4A (AAC in an MP4 container): parse moov ourselves to get each frame's position and size, then feed frames one by one to Espressif's AAC decoder.
// Espressif's built-in M4A parser can only decode sequentially from the start (no seeking, and not when moov is at the end of the file), so it isn't used.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "netstream.h"

typedef struct m4a m4a_t;

// Parse the file and decode the first frame (to get the actual sample rate and channel count). Returns NULL on failure, with the reason in err
m4a_t *m4a_open(netstream_t *ns, char *err, int errlen);
void m4a_info(const m4a_t *m, int *rate, int *channels, int64_t *total_frames);
// Read up to max frames (channels samples per frame, int32 left-aligned to 32 bit, same as dr_flac's s32); returns 0 at the end
int m4a_read(m4a_t *m, int32_t *out, int max);
bool m4a_seek(m4a_t *m, int64_t frame);
void m4a_close(m4a_t *m);
