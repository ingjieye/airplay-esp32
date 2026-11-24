#include "netstream.h"
#include <string.h>

// How far ahead to "download and discard" before sending a new Range request instead.
// Assuming WiFi ~1-2 MB/s and ~100 ms per new request, the two cost about the same around 128 KB
#define DISCARD_MAX (128 * 1024)

// Ring history window read/write (positioned by file offset modulo size)
static void hist_put(netstream_t *s, int64_t off, const uint8_t *src, size_t n)
{
    if (n > (size_t)s->hist_cap) { src += n - s->hist_cap; off += n - s->hist_cap; n = s->hist_cap; }
    size_t i = off % s->hist_cap, first = n < s->hist_cap - i ? n : s->hist_cap - i;
    memcpy(s->hist + i, src, first);
    memcpy(s->hist, src + first, n - first);
}

static void hist_get(netstream_t *s, int64_t off, uint8_t *dst, size_t n)
{
    size_t i = off % s->hist_cap, first = n < s->hist_cap - i ? n : s->hist_cap - i;
    memcpy(dst, s->hist + i, first);
    memcpy(dst + first, s->hist, n - first);
}

// Receive data from the transport, filling the head cache and history window on the way
static size_t net_get(netstream_t *s, uint8_t *buf, size_t n)
{
    size_t r = s->recv(s->ctx, buf, n);
    if (r && s->cache && s->stream_pos == s->cache_len && s->cache_len < s->cache_cap) {
        size_t k = r < (size_t)(s->cache_cap - s->cache_len) ? r : (size_t)(s->cache_cap - s->cache_len);
        memcpy(s->cache + s->cache_len, buf, k);
        s->cache_len += k;
    }
    if (r && s->hist) {
        hist_put(s, s->stream_pos, buf, r);
        if (s->stream_pos + (int64_t)r - s->hist_start > s->hist_cap) s->hist_start = s->stream_pos + r - s->hist_cap;
    }
    s->stream_pos += r;
    return r;
}

static bool skip(netstream_t *s, int64_t n)
{
    uint8_t tmp[512];
    while (n > 0) {
        size_t got = net_get(s, tmp, n > (int64_t)sizeof tmp ? sizeof tmp : (size_t)n);
        if (got == 0) return false;
        n -= got;
    }
    return true;
}

size_t ns_read(netstream_t *s, void *buf, size_t n)
{
    if (s->size >= 0) {
        if (s->pos >= s->size) return 0;
        if ((int64_t)n > s->size - s->pos) n = (size_t)(s->size - s->pos);
    }
    size_t pre = 0;
    while (n > 0) {
        size_t k;
        if (s->pos < s->cache_len) {  // head-cache hit
            k = s->cache_len - s->pos < (int64_t)n ? (size_t)(s->cache_len - s->pos) : n;
            memcpy(buf, s->cache + s->pos, k);
        } else if (s->hist && s->pos < s->stream_pos && s->pos >= s->hist_start) {  // history-window hit
            k = s->stream_pos - s->pos < (int64_t)n ? (size_t)(s->stream_pos - s->pos) : n;
            hist_get(s, s->pos, buf, k);
        } else {
            break;
        }
        s->pos += k;
        s->consumed += k;
        pre += k;
        buf = (uint8_t *)buf + k;
        n -= k;
    }
    if (n == 0) return pre;
    if (s->pos != s->stream_pos) {
        int64_t ahead = s->pos - s->stream_pos;
        if (ahead < 0 || ahead > DISCARD_MAX || !skip(s, ahead)) {
            s->restart(s->ctx, s->pos);
            s->restarts++;
            s->stream_pos = s->pos;
            s->hist_start = s->pos;  // history window restarts at the new position
        }
    }
    size_t got = 0;
    while (got < n) {
        size_t r = net_get(s, (uint8_t *)buf + got, n - got);
        if (r == 0) break;
        got += r;
    }
    s->pos += got;
    s->consumed += got;
    return pre + got;
}

bool ns_seek(netstream_t *s, int offset, int origin)
{
    int64_t p;
    if (origin == NS_SEEK_SET) p = offset;
    else if (origin == NS_SEEK_CUR) p = s->pos + offset;
    else {
        if (!s->allow_seek_end || s->size < 0) return false;
        p = s->size + offset;
    }
    if (p < 0) return false;
    if (s->size >= 0 && p > s->size) p = s->size;
    s->pos = p;
    return true;
}

bool ns_tell(netstream_t *s, int64_t *cursor)
{
    *cursor = s->pos;
    return true;
}

int ns_peek(netstream_t *s, uint8_t *out, int n)
{
    if (s->pos != 0 || !s->cache || n > s->cache_cap) return 0;
    int64_t c = s->consumed;
    int got = (int)ns_read(s, out, n);
    s->pos = 0;  // data is now in the head cache; reading from 0 afterwards hits the cache
    s->consumed = c;
    return got;
}
