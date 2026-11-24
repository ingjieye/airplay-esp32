#include "spdif_enc.h"
#include <string.h>

// Key property: the number of "1"s in the 28 slots of a subframe is even (guaranteed by the parity bit), and each "0" toggles the level once,
// so every subframe ends at the level it started with. Preambles don't change the level either. So the starting level is always 0 and preambles never need inverting.

#define PRE_B 0xE8  // 11101000 block start (left)
#define PRE_M 0xE2  // 11100010 left
#define PRE_W 0xE4  // 11100100 right

static uint16_t bmc8[256];  // with starting level 0, 8 data bits (LSB first) encode to 16 cells (MSB first)
static uint8_t bmc4[16];    // 4 data bits -> 8 cells
static int tables_ready;

static void build_tables(void)
{
    for (int v = 0; v < 256; v++) {
        int level = 0;
        uint16_t c = 0;
        for (int i = 0; i < 8; i++) {
            int b = (v >> i) & 1;
            level ^= 1; c = (c << 1) | level;       // always toggle at the start of a slot
            if (b) level ^= 1;                       // 1: toggle again in the middle
            c = (c << 1) | level;
        }
        bmc8[v] = c;
        if (v < 16) bmc4[v] = (uint8_t)(c >> 8);    // the 8 cells of the first 4 bits
    }
    tables_ready = 1;
}

// When the data bits contain an odd number of "0"s, the following cells must all be inverted
static inline int zeros_odd8(uint32_t v) { return (8 - __builtin_popcount(v & 0xFF)) & 1; }

static inline uint64_t subframe(uint8_t pre, uint32_t sample24, int c)
{
    uint32_t d = (sample24 & 0xFFFFFF) | ((uint32_t)c << 26);   // slots 4..27 audio, 28 V=0, 29 U=0, 30 C
    d |= (uint32_t)(__builtin_popcount(d) & 1) << 27;           // 31 P: even parity
    uint64_t acc = (uint64_t)pre << 56;
    int inv = 0;
    for (int k = 0; k < 3; k++) {
        uint32_t byte = (d >> (8 * k)) & 0xFF;
        uint16_t cells = inv ? (uint16_t)~bmc8[byte] : bmc8[byte];
        acc |= (uint64_t)cells << (40 - 16 * k);
        inv ^= zeros_odd8(byte);
    }
    uint32_t nib = (d >> 24) & 0xF;
    uint8_t cells = inv ? (uint8_t)~bmc4[nib] : bmc4[nib];
    acc |= cells;
    return acc;
}

void spdif_enc_init(spdif_enc_t *e, int sample_rate, int bits)
{
    if (!tables_ready) build_tables();
    memset(e, 0, sizeof *e);
    e->cs[0] = 0x04;  // consumer, PCM, copying permitted, no pre-emphasis
    uint8_t fs;
    switch (sample_rate) {
    case 32000: fs = 0x3; break;
    case 48000: fs = 0x2; break;
    case 88200: fs = 0x8; break;
    case 96000: fs = 0xA; break;
    case 176400: fs = 0xC; break;
    case 192000: fs = 0xE; break;
    default: fs = 0x0; break;  // 44100
    }
    e->cs[3] = fs;
    // Byte 4: word length. bit0=1 means max 24 bit; bit1..3 = 101 means 24 bit, 001 means 16 bit (when max is 20 bit)
    e->cs[4] = bits >= 24 ? 0x0B : 0x02;
}

void spdif_enc_frames(spdif_enc_t *e, const int32_t *pcm, int frames, uint32_t *out)
{
    for (int f = 0; f < frames; f++) {
        int c = (e->cs[e->frame >> 3] >> (e->frame & 7)) & 1;
        uint64_t l = subframe(e->frame == 0 ? PRE_B : PRE_M, (uint32_t)pcm[2 * f], c);
        uint64_t r = subframe(PRE_W, (uint32_t)pcm[2 * f + 1], c);
        out[0] = l >> 32; out[1] = (uint32_t)l;
        out[2] = r >> 32; out[3] = (uint32_t)r;
        out += 4;
        if (++e->frame == 192) e->frame = 0;
    }
}
