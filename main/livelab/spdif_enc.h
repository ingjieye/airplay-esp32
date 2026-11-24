// S/PDIF (IEC 60958) table-driven encoder: PCM -> biphase-mark code, fed to I2S (32-bit stereo, sample rate = audio rate x 2)
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t cs[24];   // 192-bit channel-status block
    int frame;        // position of the current frame within the block, 0..191
} spdif_enc_t;

// Set channel status (consumer PCM) from sample rate and bit depth
void spdif_enc_init(spdif_enc_t *e, int sample_rate, int bits);
// pcm: interleaved stereo, each sample a signed 24-bit value in the low 24 bits of int32; out: 4 uint32 per frame (128 level cells)
void spdif_enc_frames(spdif_enc_t *e, const int32_t *pcm, int frames, uint32_t *out);
