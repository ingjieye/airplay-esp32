#include "m4a.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decoder/impl/esp_aac_dec.h"
#include "esp_audio_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define TAG "m4a"
#define MOOV_MAX (4 * 1024 * 1024)
#define OUT_CAP (2048 * 8 * 2)  // at most 2048 samples per frame (HE-AAC) x 8 channels x 16 bit

struct m4a {
    netstream_t *ns;
    void *dec;
    // Position and size of each frame (an MP4 sample) in the file, in PSRAM
    uint32_t n, *off, *size;
    uint32_t *tstart;          // start time of each frame (mdhd time units), binary-searched when seeking
    uint32_t timescale;
    uint64_t duration;         // mdhd time units
    int aot, sfi, ch_cfg;      // for the ADTS header: audio object type, sampling frequency index, channel config
    int rate, ch;              // actual decoder output
    uint32_t next;             // next frame to decode
    uint8_t *in;               // ADTS header + one frame of data
    uint32_t in_cap;
    int16_t *pcm;              // PCM decoded from the current frame
    int pcm_frames, pcm_pos;
    int skip;                  // leading frames to drop after a seek
    int errors;
};

static const int SFI_RATE[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

// Find a child box of the given type in [p, end); returns the content start and length
static const uint8_t *child(const uint8_t *p, const uint8_t *end, const char *type, uint32_t *len)
{
    while (end - p >= 8) {
        uint64_t sz = be32(p);
        int h = 8;
        if (sz == 1 && end - p >= 16) { sz = be64(p + 8); h = 16; }
        else if (sz == 0) sz = end - p;
        if (sz < (uint64_t)h || sz > (uint64_t)(end - p)) return NULL;
        if (!memcmp(p + 4, type, 4)) { *len = sz - h; return p + h; }
        p += sz;
    }
    return NULL;
}

static const uint8_t *path(const uint8_t *p, uint32_t len, const char *const *types, uint32_t *out_len)
{
    for (; *types; types++) {
        p = child(p, p + len, *types, &len);
        if (!p) return NULL;
    }
    *out_len = len;
    return p;
}

// Length of an MPEG-4 descriptor (1-4 bytes, the high bit of each byte means more follow)
static uint32_t desc_len(const uint8_t **p, const uint8_t *end)
{
    uint32_t l = 0;
    for (int i = 0; i < 4 && *p < end; i++) {
        uint8_t b = *(*p)++;
        l = l << 7 | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return l;
}

// Get the AudioSpecificConfig from esds
static bool parse_esds(m4a_t *m, const uint8_t *p, uint32_t len)
{
    const uint8_t *end = p + len;
    p += 4;  // version + flags
    while (p < end) {
        uint8_t tag = *p++;
        uint32_t l = desc_len(&p, end);
        if (tag == 0x03) { p += 3; continue; }           // ES_Descriptor: ES_ID(2) + flags(1), followed by sub-descriptors
        if (tag == 0x04) { p += 13; continue; }          // DecoderConfig: objectType(1) streamType(1) buffer(3) bitrates(8)
        if (tag == 0x05 && l >= 2 && p + 2 <= end) {     // DecoderSpecificInfo = AudioSpecificConfig
            m->aot = p[0] >> 3;
            m->sfi = ((p[0] & 7) << 1) | (p[1] >> 7);
            m->ch_cfg = (p[1] >> 3) & 0xF;
            if ((m->aot == 5 || m->aot == 29) && l >= 4) {  // HE-AAC explicit signalling: followed by the extension sample rate, then the core object type
                // Explicit signalling: the preceding sample rate is the core layer's; after skipping the extension rate (4 bits) comes the core object type.
                // SBR is not enabled; decode as core-layer AAC-LC (less high-frequency content, but it plays)
                uint32_t v = be32(p);
                m->aot = (v >> 10) & 0x1F;  // the 5 bits after 5(aot)+4(sfi)+4(ch)+4(ext sfi)
                return true;
            }
            return true;
        }
        p += l;
    }
    return false;
}

static bool parse_moov(m4a_t *m, const uint8_t *moov, uint32_t mlen, char *err, int errlen)
{
    // Find the sound track
    const uint8_t *trak = NULL, *stbl = NULL, *mdia = NULL;
    uint32_t tl = 0, sl = 0, ml = 0;
    for (const uint8_t *p = moov, *end = moov + mlen; end - p >= 8;) {
        uint32_t sz = be32(p);
        if (sz < 8 || sz > (uint32_t)(end - p)) break;
        if (!memcmp(p + 4, "trak", 4)) {
            uint32_t hl;
            const uint8_t *hdlr = path(p + 8, sz - 8, (const char *const[]){"mdia", "hdlr", NULL}, &hl);
            if (hdlr && hl >= 12 && !memcmp(hdlr + 8, "soun", 4)) { trak = p + 8; tl = sz - 8; break; }
        }
        p += sz;
    }
    if (!trak) { snprintf(err, errlen, "No audio track in M4A"); return false; }
    mdia = child(trak, trak + tl, "mdia", &ml);
    stbl = path(trak, tl, (const char *const[]){"mdia", "minf", "stbl", NULL}, &sl);
    uint32_t l;
    const uint8_t *mdhd = mdia ? child(mdia, mdia + ml, "mdhd", &l) : NULL;
    if (!stbl || !mdhd) { snprintf(err, errlen, "Incomplete M4A structure"); return false; }
    if (mdhd[0] == 1) { m->timescale = be32(mdhd + 20); m->duration = be64(mdhd + 24); }
    else { m->timescale = be32(mdhd + 12); m->duration = be32(mdhd + 16); }

    // Codec: only AAC (mp4a + esds) is supported
    const uint8_t *stsd = child(stbl, stbl + sl, "stsd", &l);
    if (!stsd || l < 16) { snprintf(err, errlen, "M4A missing stsd"); return false; }
    const uint8_t *ent = stsd + 8;  // version/flags + entry count
    uint32_t el = be32(ent);
    if (memcmp(ent + 4, "mp4a", 4)) {
        snprintf(err, errlen, "Unsupported M4A codec: %.4s", ent + 4);
        return false;
    }
    int ver = ent[8 + 8] << 8 | ent[8 + 9];  // sound entry version (QuickTime extension)
    int skip = 8 + 28 + (ver == 1 ? 16 : ver == 2 ? 36 : 0);
    const uint8_t *esds = el > (uint32_t)skip ? child(ent + skip, ent + el, "esds", &l) : NULL;
    if (!esds || !parse_esds(m, esds, l)) { snprintf(err, errlen, "M4A missing AAC config (esds)"); return false; }
    if (m->aot != 2 || m->sfi >= 13 || m->ch_cfg < 1 || m->ch_cfg > 7) {
        snprintf(err, errlen, "Unsupported AAC type (object type %d, sampling index %d, channels %d)", m->aot, m->sfi, m->ch_cfg);
        return false;
    }

    // Frame sizes (stsz)
    const uint8_t *stsz = child(stbl, stbl + sl, "stsz", &l);
    if (!stsz || l < 12) { snprintf(err, errlen, "M4A missing stsz"); return false; }
    uint32_t fixed = be32(stsz + 4);
    m->n = be32(stsz + 8);
    if (!m->n || (!fixed && l < 12 + 4 * m->n)) { snprintf(err, errlen, "Incomplete M4A frame table"); return false; }
    m->off = heap_caps_malloc(m->n * 4, MALLOC_CAP_SPIRAM);
    m->size = heap_caps_malloc(m->n * 4, MALLOC_CAP_SPIRAM);
    m->tstart = heap_caps_malloc(m->n * 4, MALLOC_CAP_SPIRAM);
    if (!m->off || !m->size || !m->tstart) { snprintf(err, errlen, "Out of memory (%lu frames)", (unsigned long)m->n); return false; }
    uint32_t maxsz = 0;
    for (uint32_t i = 0; i < m->n; i++) {
        m->size[i] = fixed ? fixed : be32(stsz + 12 + 4 * i);
        if (m->size[i] > maxsz) maxsz = m->size[i];
    }

    // Frame positions: chunk offsets (stco / co64) + frames per chunk (stsc)
    bool co64 = false;
    const uint8_t *stco = child(stbl, stbl + sl, "stco", &l);
    if (!stco) { stco = child(stbl, stbl + sl, "co64", &l); co64 = true; }
    uint32_t sc_l;
    const uint8_t *stsc = child(stbl, stbl + sl, "stsc", &sc_l);
    if (!stco || !stsc || l < 8 || sc_l < 8) { snprintf(err, errlen, "M4A missing chunk tables"); return false; }
    uint32_t nchunk = be32(stco + 4), nsc = be32(stsc + 4);
    if (l < 8 + nchunk * (co64 ? 8 : 4) || sc_l < 8 + nsc * 12) { snprintf(err, errlen, "Incomplete M4A chunk tables"); return false; }
    uint32_t s = 0;
    for (uint32_t e = 0; e < nsc && s < m->n; e++) {
        const uint8_t *q = stsc + 8 + e * 12;
        uint32_t first = be32(q), per = be32(q + 4);
        uint32_t last = e + 1 < nsc ? be32(q + 12) : nchunk + 1;  // first chunk of the next entry (chunk numbers start at 1)
        for (uint32_t c = first; c < last && c <= nchunk && s < m->n; c++) {
            uint64_t o = co64 ? be64(stco + 8 + (c - 1) * 8) : be32(stco + 8 + (c - 1) * 4);
            for (uint32_t k = 0; k < per && s < m->n; k++) {
                m->off[s] = (uint32_t)o;
                o += m->size[s++];
            }
        }
    }
    if (s < m->n) { snprintf(err, errlen, "M4A chunk table doesn't match frame count (%lu / %lu)", (unsigned long)s, (unsigned long)m->n); return false; }

    // Time (stts): start time of each frame
    const uint8_t *stts = child(stbl, stbl + sl, "stts", &l);
    uint32_t t = 0;
    s = 0;
    if (stts && l >= 8) {
        uint32_t ne = be32(stts + 4);
        for (uint32_t e = 0; e < ne && 8 + e * 8 + 8 <= l; e++)
            for (uint32_t k = 0, cnt = be32(stts + 8 + e * 8), d = be32(stts + 12 + e * 8); k < cnt && s < m->n; k++) {
                m->tstart[s++] = t;
                t += d;
            }
    }
    for (; s < m->n; s++) { m->tstart[s] = t; t += 1024; }  // if the table is short, assume 1024 samples per frame

    m->in_cap = maxsz + 7;
    m->in = heap_caps_malloc(m->in_cap, MALLOC_CAP_8BIT);
    if (!m->in) { snprintf(err, errlen, "Out of memory"); return false; }
    return true;
}

static void adts(uint8_t *h, int aot, int sfi, int ch, int len)
{
    len += 7;
    h[0] = 0xFF;
    h[1] = 0xF1;
    h[2] = ((aot - 1) & 3) << 6 | (sfi & 0xF) << 2 | (ch >> 2 & 1);
    h[3] = (ch & 3) << 6 | (len >> 11 & 3);
    h[4] = len >> 3 & 0xFF;
    h[5] = (len & 7) << 5 | 0x1F;
    h[6] = 0xFC;
}

static bool open_dec(m4a_t *m)
{
    if (m->dec) esp_aac_dec_close(m->dec);
    m->dec = NULL;
    esp_aac_dec_cfg_t c = ESP_AAC_DEC_CONFIG_DEFAULT();
    c.sample_rate = SFI_RATE[m->sfi];
    c.channel = m->ch_cfg;
    c.bits_per_sample = 16;
    c.no_adts_header = false;
    c.aac_plus_enable = false;
    return esp_aac_dec_open(&c, sizeof c, &m->dec) == ESP_AUDIO_ERR_OK;
}

// Decode the next frame into m->pcm; returns false at the end
static bool decode_next(m4a_t *m)
{
    while (m->next < m->n) {
        uint32_t i = m->next++;
        uint32_t sz = m->size[i];
        if (!ns_seek(m->ns, (int)m->off[i], NS_SEEK_SET)) return false;
        if (ns_read(m->ns, m->in + 7, sz) != sz) return false;
        adts(m->in, m->aot, m->sfi, m->ch_cfg, sz);
        esp_audio_dec_in_raw_t raw = {.buffer = m->in, .len = sz + 7};
        esp_audio_dec_out_frame_t fr = {.buffer = (uint8_t *)m->pcm, .len = OUT_CAP};
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t e = esp_aac_dec_decode(m->dec, &raw, &fr, &info);
        if (e != ESP_AUDIO_ERR_OK || !fr.decoded_size) {
            if (m->errors++ < 5) ESP_LOGW(TAG, "Frame %lu failed to decode (%d), skipped", (unsigned long)i, e);
            if (e != ESP_AUDIO_ERR_OK) open_dec(m);  // once the decoder errors it keeps failing; recreate it (upstream does the same)
            continue;
        }
        if (info.sample_rate) m->rate = info.sample_rate;
        if (info.channel) m->ch = info.channel;
        m->pcm_frames = fr.decoded_size / (2 * m->ch);
        m->pcm_pos = 0;
        return true;
    }
    return false;
}

m4a_t *m4a_open(netstream_t *ns, char *err, int errlen)
{
    m4a_t *m = heap_caps_calloc(1, sizeof *m, MALLOC_CAP_8BIT);
    uint8_t *moov = NULL;
    if (!m) { snprintf(err, errlen, "Out of memory"); return NULL; }
    m->ns = ns;
    // Scan top-level boxes in order to find moov (it may be after mdat; skipping there costs one Range request)
    int64_t pos = 0;
    uint32_t mlen = 0;
    while (1) {
        uint8_t h[16];
        if (!ns_seek(ns, (int)pos, NS_SEEK_SET) || ns_read(ns, h, 8) != 8) { snprintf(err, errlen, "moov not found in M4A"); goto fail; }
        uint64_t sz = be32(h);
        int hl = 8;
        if (sz == 1) { if (ns_read(ns, h + 8, 8) != 8) goto fail; sz = be64(h + 8); hl = 16; }
        if (!memcmp(h + 4, "moov", 4)) {
            if (sz - hl > MOOV_MAX) { snprintf(err, errlen, "M4A moov too large (%llu bytes)", sz); goto fail; }
            mlen = sz - hl;
            moov = heap_caps_malloc(mlen, MALLOC_CAP_SPIRAM);
            if (!moov || ns_read(ns, moov, mlen) != mlen) { snprintf(err, errlen, "Failed to read moov"); goto fail; }
            break;
        }
        if (sz < 8 || (ns->size > 0 && pos + (int64_t)sz > ns->size)) { snprintf(err, errlen, "moov not found in M4A"); goto fail; }
        pos += sz;
    }
    if (!parse_moov(m, moov, mlen, err, errlen)) goto fail;
    free(moov);
    moov = NULL;
    m->pcm = heap_caps_malloc(OUT_CAP, MALLOC_CAP_8BIT);
    if (!m->pcm || !open_dec(m)) { snprintf(err, errlen, "AAC decoder init failed"); goto fail; }
    m->rate = SFI_RATE[m->sfi];
    m->ch = m->ch_cfg;
    if (!decode_next(m)) { snprintf(err, errlen, "AAC decode failed"); goto fail; }
    ESP_LOGI(TAG, "AAC %d Hz / %d ch, %lu frames, moov %lu bytes", m->rate, m->ch, (unsigned long)m->n, (unsigned long)mlen);
    return m;
fail:
    free(moov);
    m4a_close(m);
    return NULL;
}

void m4a_info(const m4a_t *m, int *rate, int *channels, int64_t *total_frames)
{
    *rate = m->rate;
    *channels = m->ch;
    *total_frames = m->timescale ? (int64_t)(m->duration * m->rate / m->timescale) : 0;
}

int m4a_read(m4a_t *m, int32_t *out, int max)
{
    int w = 0;
    while (w < max) {
        if (m->pcm_pos >= m->pcm_frames && !decode_next(m)) break;
        int n = m->pcm_frames - m->pcm_pos;
        if (m->skip) {
            int d = n < m->skip ? n : m->skip;
            m->pcm_pos += d;
            m->skip -= d;
            continue;
        }
        if (n > max - w) n = max - w;
        const int16_t *src = m->pcm + m->pcm_pos * m->ch;
        for (int i = 0; i < n * m->ch; i++) out[w * m->ch + i] = (int32_t)src[i] << 16;
        m->pcm_pos += n;
        w += n;
    }
    return w;
}

bool m4a_seek(m4a_t *m, int64_t frame)
{
    if (frame < 0) frame = 0;
    uint64_t t = (uint64_t)frame * m->timescale / m->rate;  // convert to mdhd time units
    uint32_t lo = 0, hi = m->n;
    while (hi - lo > 1) {  // last frame whose start time <= t
        uint32_t mid = (lo + hi) / 2;
        if (m->tstart[mid] <= t) lo = mid; else hi = mid;
    }
    // Start decoding from the previous frame: AAC frames overlap, so the first decoded frame is incomplete and is dropped
    uint32_t start = lo ? lo - 1 : 0;
    open_dec(m);
    m->next = start;
    m->pcm_frames = m->pcm_pos = 0;
    m->skip = (int)(((uint64_t)t - m->tstart[start]) * m->rate / m->timescale);
    return true;
}

void m4a_close(m4a_t *m)
{
    if (!m) return;
    if (m->dec) esp_aac_dec_close(m->dec);
    heap_caps_free(m->off);
    heap_caps_free(m->size);
    heap_caps_free(m->tstart);
    free(m->in);
    free(m->pcm);
    free(m);
}
