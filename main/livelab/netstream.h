// Sequential network stream with "lazy seek" and a "file-head cache": used as the read callback for dr_flac / dr_mp3.
// seek only changes the logical position; on the actual read:
//   within the head cache -> read from the cache (dr_flac jumps back to read the seek table when opening; this saves a request);
//   within the recent-download history window -> read from the window (dr_flac probes frame headers then steps back a few KB when seeking);
//   at the download position -> read directly; within DISCARD_MAX ahead -> discard the data in between; only otherwise send a new Range request.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int64_t pos;         // position as seen by the decoder
    int64_t stream_pos;  // file offset of the next byte received from the transport
    int64_t size;        // total file size, -1 if unknown
    bool allow_seek_end; // MP3 disallows SEEK_END (stops dr_mp3 issuing two extra requests to read trailing tags)
    // Transport
    void (*restart)(void *ctx, int64_t offset);              // restart download from offset (drop buffered data)
    size_t (*recv)(void *ctx, uint8_t *buf, size_t n);       // blocks until >= 1 byte arrives; returns 0 on end or abort
    void *ctx;
    // File-head cache (caller provides the buffer): data in [0, cache_len) is cached
    uint8_t *cache;
    int cache_cap, cache_len;
    // Ring history window over recent downloads (caller provides the buffer): [hist_start, stream_pos) can be read back
    uint8_t *hist;
    int hist_cap;
    int64_t hist_start;
    // Statistics
    int restarts;
    int64_t consumed;    // bytes actually read by the decoder (excluding discarded)
} netstream_t;

enum { NS_SEEK_SET, NS_SEEK_CUR, NS_SEEK_END };

size_t ns_read(netstream_t *s, void *buf, size_t n);
bool ns_seek(netstream_t *s, int offset, int origin);
bool ns_tell(netstream_t *s, int64_t *cursor);
// Read the first n bytes of the file into out (for format detection) without changing the logical position; returns bytes actually read
int ns_peek(netstream_t *s, uint8_t *out, int n);
