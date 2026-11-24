#!/usr/bin/env python3
"""ESP32 LiveLab (AirPlay + DLNA + Bluetooth) simulator tests: run the simulator firmware in QEMU, playing the roles of NetEase + audio source server + audio sink.

Usage:
  python3 sim/test_sim.py            # run all scenarios
  python3 sim/test_sim.py basic seek # run only the given scenarios
Prerequisites: build_sim built (see "Build, flash, test" in README.md), sim/media generated (sim/make_media.sh).
"""
import http.server
import os
import re
import socket
import socketserver
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from xml.sax.saxutils import escape

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MEDIA = os.path.join(ROOT, "sim", "media")
SERIAL_LOG = os.path.join(ROOT, "build_sim", "qemu_serial.log")
BOARD = "http://127.0.0.1:18080"   # web page (board port 80)
DLNA = "http://127.0.0.1:19152"    # separate DLNA HTTP server (board port 49152)
MEDIA_PORT, SINK_PORT, EVENT_PORT = 8000, 9000, 8001  # the analog-output sink is on SINK_PORT + 1
GUEST_HOST = "10.0.2.2"  # the host's address as seen by the simulated board in QEMU user networking

# ---------------------------------------------------------------- Audio source server (supports Range)
NET_DELAY_S, NET_RATE = 0.08, 2 * 1024 * 1024  # simulate CDN + WiFi: 80 ms time to first byte, 2 MB/s


class MediaHandler(http.server.BaseHTTPRequestHandler):
    drop_once = {}  # file name -> disconnect after this many bytes on the first request (simulates a network drop)
    requests = []

    def log_message(self, *a):
        pass

    def do_GET(self):
        path, _, query = self.path.partition("?")
        path = path.lstrip("/")
        q = dict(kv.split("=", 1) for kv in query.split("&") if "=" in kv)
        MediaHandler.requests.append((path, self.headers.get("Range")))
        f = os.path.join(MEDIA, path)
        if not os.path.isfile(f):
            self.send_error(404)
            return
        data = open(f, "rb").read()
        start, end = 0, len(data) - 1
        m = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range") or "")
        if m:
            start = int(m.group(1))
            if m.group(2):
                end = min(end, int(m.group(2)))
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
        else:
            self.send_response(200)
        ctype = q.get("ctype", "audio/flac" if path.endswith(".flac") else "audio/mpeg").replace("%2F", "/")
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        body = data[start:end + 1]
        drop = MediaHandler.drop_once.pop(path, None)
        if drop is not None and drop < len(body):
            body = body[:drop]
        try:
            time.sleep(NET_DELAY_S)
            t0, chunk = time.time(), 16384
            for i in range(0, len(body), chunk):
                self.wfile.write(body[i:i + chunk])
                ahead = (i + chunk) / NET_RATE - (time.time() - t0)
                if ahead > 0:
                    time.sleep(ahead)
            if drop is not None:
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR)  # disconnect midway
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


class ThreadingServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


# ---------------------------------------------------------------- UPnP event receiver (plays NetEase receiving NOTIFY)
class EventHandler(http.server.BaseHTTPRequestHandler):
    events = []  # (time, SID, SEQ, service path, body)

    def log_message(self, *a):
        pass

    def do_NOTIFY(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode("utf8", "replace")
        EventHandler.events.append((time.time(), self.headers.get("SID"), int(self.headers.get("SEQ", -1)), self.path, body))
        self.send_response(200)
        self.end_headers()


def event_state(path="/avt"):
    """Get TransportState from the last AVTransport event received (LastChange holds escaped XML)"""
    for _, _, _, p, body in reversed(EventHandler.events):
        if p == path:
            m = re.search(r"TransportState val=&quot;(\w+)&quot;", body) or re.search(r'TransportState val="(\w+)"', body)
            if m:
                return m.group(1)
    return None


def subscribe(svc, path):
    req = urllib.request.Request(f"{DLNA}/{svc}/event", method="SUBSCRIBE", headers={
        "CALLBACK": f"<http://{GUEST_HOST}:{EVENT_PORT}{path}>", "NT": "upnp:event", "TIMEOUT": "Second-1800"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status, r.headers.get("SID"), r.headers.get("TIMEOUT")


# ---------------------------------------------------------------- Audio sink (receives the S/PDIF bitstream, or raw I2S data from the analog output)
class Sink:
    analog = None  # analog-output sink

    def __init__(self, port=SINK_PORT):
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.connected = threading.Event()
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", port))
        s.listen(1)
        threading.Thread(target=self._accept, args=(s,), daemon=True).start()

    def _accept(self, s):
        while True:
            c, _ = s.accept()
            self.connected.set()
            while True:
                d = c.recv(65536)
                if not d:
                    break
                with self.lock:
                    self.buf += d

    def mark(self):
        with self.lock:
            return len(self.buf) // 16 * 16  # align to S/PDIF frames (4 32-bit words)

    def since(self, mark):
        with self.lock:
            n = (len(self.buf) - mark) // 16 * 16
            return bytes(self.buf[mark:mark + n])


def decode_spdif(raw):
    """Decode the S/PDIF bitstream (4 little-endian uint32 per frame, MSB is the first level cell).
    Returns (samples int32[N,2], list of channel-status blocks, number of structural errors)"""
    w = np.frombuffer(raw, dtype="<u4").astype(">u4")
    bits = np.unpackbits(w.view(np.uint8)).reshape(-1, 64)  # one subframe per row
    errors = 0
    # Every subframe should start at level 0 (encoder invariant), preambles B/M/W
    pre = np.packbits(bits[:, :8], axis=1)[:, 0]
    errors += int(np.sum(~np.isin(pre, [0xE8, 0xE2, 0xE4])))
    cells = bits[:, 8:].reshape(-1, 28, 2)
    data = (cells[:, :, 0] != cells[:, :, 1]).astype(np.int64)  # data bits of the 28 slots
    # Every slot must toggle at its start
    prev = np.concatenate([bits[:, 7:8], cells[:, :-1, 1]], axis=1)
    errors += int(np.sum(cells[:, :, 0] == prev))
    errors += int(np.sum(data.sum(axis=1) % 2))  # even parity
    weights = (1 << np.arange(24)).astype(np.int64)
    v = (data[:, :24] * weights).sum(axis=1)
    v = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.int32)
    n = len(v) // 2
    samples = v[:2 * n].reshape(n, 2)
    # Channel status: C bits of 192 frames starting at each B preamble (left channel)
    left_pre = pre[0:2 * n:2]
    cbits = data[0:2 * n:2, 26]
    blocks = []
    for s in np.nonzero(left_pre == 0xE8)[0]:
        if s + 192 <= n:
            blocks.append(bytes(np.packbits(cbits[s:s + 192].reshape(24, 8)[:, ::-1], axis=1)[:, 0]))
    decode_spdif.block_starts = [int(s) for s in np.nonzero(left_pre == 0xE8)[0] if s + 192 <= n]
    return samples, blocks, errors


def reference_pcm(name):
    """Decode reference PCM with ffmpeg and convert to the same 24-bit representation as the firmware"""
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", os.path.join(MEDIA, name), "-f", "s32le", "-ac", "2", "-"],
                         capture_output=True, check=True).stdout
    return (np.frombuffer(raw, dtype="<i4") >> 8).reshape(-1, 2)


def strip_silence(x):
    return x[np.any(x != 0, axis=1)]


# ---------------------------------------------------------------- Playing NetEase (UPnP controller)
SVC = {"AVTransport": "urn:schemas-upnp-org:service:AVTransport:1",
       "RenderingControl": "urn:schemas-upnp-org:service:RenderingControl:1",
       "ConnectionManager": "urn:schemas-upnp-org:service:ConnectionManager:1"}


def soap(svc, action, **args):
    body = "".join(f"<{k}>{escape(str(v))}</{k}>" for k, v in args.items())
    xml = (f'<?xml version="1.0" encoding="utf-8"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           f's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
           f'<u:{action} xmlns:u="{SVC[svc]}">{body}</u:{action}></s:Body></s:Envelope>')
    req = urllib.request.Request(f"{DLNA}/{svc}/control", data=xml.encode(), method="POST", headers={
        "Content-Type": 'text/xml; charset="utf-8"', "SOAPACTION": f'"{SVC[svc]}#{action}"'})
    t = time.time()
    with urllib.request.urlopen(req, timeout=10) as r:
        text = r.read().decode()
    Stats.soap_lat.append(time.time() - t)
    return dict(re.findall(r"<(\w+)>([^<]*)</\1>", text))


def status():
    t = time.time()
    with urllib.request.urlopen(f"{BOARD}/api/ll/status", timeout=10) as r:
        import json
        d = json.loads(r.read())
    Stats.web_lat.append(time.time() - t)
    return d


def didl(title, artist="Test Artist", album="Test Album"):
    # Same format as measured from NetEase: no duration/size in res
    return (f'<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" xmlns:dc="http://purl.org/dc/elements/1.1/" '
            f'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/"><item id="f-0" parentID="0" restricted="0">'
            f'<dc:title>{escape(title)}</dc:title><upnp:artist>{escape(artist)}</upnp:artist><upnp:album>{escape(album)}</upnp:album>'
            f'<upnp:class>object.item.audioItem</upnp:class>'
            f'<res protocolInfo="http-get:*:audio/*:DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000">x</res>'
            f'</item></DIDL-Lite>')


def netease_push(name, title=None):
    """In the measured NetEase order: Seek(0) -> SetAVTransportURI -> Play -> Play"""
    soap("AVTransport", "Seek", InstanceID=0, Unit="REL_TIME", Target="00:00:00")
    soap("AVTransport", "SetAVTransportURI", InstanceID=0, CurrentURI=f"http://{GUEST_HOST}:{MEDIA_PORT}/{name}",
         CurrentURIMetaData=didl(title or name))
    soap("AVTransport", "Play", InstanceID=0, Speed=1)
    soap("AVTransport", "Play", InstanceID=0, Speed=1)


def poll(seconds, until=None):
    """Poll position and state every second like NetEase, also requesting the web API; returns the last (transport state, web status)"""
    end = time.time() + seconds
    last = None
    while time.time() < end:
        ti = soap("AVTransport", "GetTransportInfo", InstanceID=0)
        soap("AVTransport", "GetPositionInfo", InstanceID=0)
        st = status()
        last = (ti["CurrentTransportState"], st)
        if until and until(*last):
            break
        time.sleep(1)
    return last


# ---------------------------------------------------------------- Test framework
class Stats:
    soap_lat, web_lat = [], []


class Fail(Exception):
    pass


def check(cond, msg):
    print(("  ✓ " if cond else "  ✗ ") + msg)
    if not cond:
        raise Fail(msg)


def serial_errors(since):
    log = open(SERIAL_LOG, "rb").read()
    log = log[since if since <= len(log) else 0:].decode("utf8", "replace")  # the reboot scenario switches to a new log file
    bad = [l for l in log.splitlines() if re.search(r"task_wdt|Guru Meditation|abort\(\)|assert failed|Backtrace|stack overflow", l)]
    return bad, log


def verify_audio(sink, mark, name, bits, rate, offset_s=None, lead_s=0):
    """lead_s: allowed tail of the previous track at the start (the old track keeps playing until the track-change command takes effect)"""
    samples, blocks, errs = decode_spdif(sink.since(mark))
    check(errs == 0, f"S/PDIF bitstream well-formed (preamble/transition/parity errors {errs})")
    rate_code = {44100: 0x0, 48000: 0x2, 88200: 0x8, 96000: 0xA}[rate]
    cs = [b for b in blocks if b[3] == rate_code]
    check(len(cs) > 0, f"channel status says {rate} Hz")
    check(all(b[4] == (0x0B if bits >= 24 else 0x02) for b in cs), f"channel status says {24 if bits >= 24 else 16} bit")
    got = strip_silence(samples)
    ref = strip_silence(reference_pcm(name))
    if offset_s is None:
        extra = len(got) - len(ref)
        if 0 < extra <= lead_s * rate:
            print(f"  · first {extra} frames ({extra / rate * 1000:.0f} ms) are the tail of the previous track, ignored")
            got = got[extra:]
        if lead_s:
            check(extra <= 0.05 * rate, f"previous track left over on track change {max(extra, 0) / rate * 1000:.0f} ms (must be < 50 ms)")
        same = len(got) == len(ref) and np.array_equal(got, ref)
        detail = f"received {len(got)} frames, reference {len(ref)} frames"
        if not same and len(got) and len(ref):
            k = min(len(got), len(ref))
            diff = np.nonzero(np.any(got[:k] != ref[:k], axis=1))[0]
            detail += f", first differing frame {diff[0] if len(diff) else k}"
        check(same, f"bit-perfect: whole track identical sample by sample ({detail})")
    else:
        # After a seek: the end of what was received should match the end of the reference from offset
        k = len(ref) - int(offset_s * rate) - int(0.5 * rate)
        check(k > 0 and len(got) >= k and np.array_equal(got[-k:], ref[-k:]), f"after seek, last {k} frames identical sample by sample")
    return got


# ---------------------------------------------------------------- Scenarios
def sc_basic(sink):
    """44.1k/16bit full playback: start, progress, automatic STOPPED at the end, bit-perfect"""
    mark = sink.mark()
    t0 = time.time()
    netease_push("t44_16.flac", "Basic test")
    state, st = poll(6, until=lambda s, w: w["startMs"] >= 0)
    check(state == "PLAYING", f"transport state PLAYING ({state})")
    check(st["codec"] == "FLAC" and st["rate"] == 44100 and st["bits"] == 16, f"detected as FLAC 44.1k/16 ({st['codec']} {st['rate']} {st['bits']})")
    check(abs(st["duration"] - 20) < 0.5, f"duration 20 s ({st['duration']})")
    check(st["title"] == "Basic test" and st["artist"] == "Test Artist", "web page shows metadata")
    check(0 <= st["startMs"] < 3000, f"start-up latency {st['startMs']} ms (slow in the simulator, for reference only)")
    state, st = poll(40, until=lambda s, w: s == "STOPPED")
    check(state == "STOPPED", f"automatically STOPPED after finishing (took {time.time() - t0:.1f} s)")
    check(st["underruns"] == 0, f"underruns {st['underruns']}")
    verify_audio(sink, mark, "t44_16.flac", 16, 44100)


def sc_wav(sink):
    """WAV (some NetEase tracks are WAV, labelled audio/mpeg by the server): detected by file content, 16bit/24bit bit-perfect"""
    for name, rate, bits in (("t44_16.wav", 44100, 16), ("t48_24.wav", 48000, 24)):
        print(f"  — {name}")
        mark = sink.mark()
        netease_push(name)
        state, st = poll(40, until=lambda s, w: s == "STOPPED" and w["codec"])
        check(st["codec"] == "WAV" and st["lossless"], f"detected as lossless WAV ({st['codec']}, {st['error'] or 'no error'})")
        check(st["rate"] == rate and st["bits"] == bits, f"{rate}/{bits} ({st['rate']}/{st['bits']})")
        verify_audio(sink, mark, name, bits, rate)


def sc_hires(sink):
    """48k/24bit (with 896KB cover art) and 96k/24bit: sample-rate switching, 24bit bit-perfect"""
    for name, rate in (("t48_24.flac", 48000), ("t96_24.flac", 96000)):
        print(f"  — {name}")
        mark = sink.mark()
        netease_push(name)
        state, st = poll(60, until=lambda s, w: s == "STOPPED" and w["codec"])
        check(st["rate"] == rate and st["bits"] == 24, f"detected as {rate}/24 ({st['rate']}/{st['bits']})")
        check(state == "STOPPED" and not st["error"], f"finished without error ({st['error'] or 'none'})")
        verify_audio(sink, mark, name, 24, rate)


def sc_seek(sink):
    """Pause / resume / seek: progress freezes, resumes, data correct after the jump"""
    mark = sink.mark()
    netease_push("t44_30s.flac", "Seek test")
    poll(8, until=lambda s, w: w["position"] > 3)
    soap("AVTransport", "Pause", InstanceID=0)
    p1 = status()["position"]
    time.sleep(3)
    st = status()
    check(st["state"] == "PAUSED_PLAYBACK", f"paused ({st['state']})")
    check(abs(st["position"] - p1) < 0.3, f"progress frozen while paused ({p1:.1f} -> {st['position']:.1f})")
    soap("AVTransport", "Play", InstanceID=0, Speed=1)
    time.sleep(2)
    st = status()
    check(st["state"] == "PLAYING" and st["position"] > p1 + 1, f"resumed, progress advancing ({st['position']:.1f})")
    pos = os.path.getsize(SERIAL_LOG)
    smark = sink.mark()
    t = time.time()
    soap("AVTransport", "Seek", InstanceID=0, Unit="REL_TIME", Target="00:00:20")
    poll(5, until=lambda s, w: w["position"] >= 20)
    print(f"  · seek command to position jump {time.time() - t:.2f} s")
    time.sleep(1)
    for l in open(SERIAL_LOG, "rb").read()[pos:].decode("utf8", "replace").splitlines():
        if "Seek to" in l:
            print("  · serial: " + l.split(": ", 1)[-1])
    seg, _, _ = decode_spdif(sink.since(smark))
    z = np.nonzero(np.all(seg == 0, axis=1))[0]
    tail = (z[0] if len(z) else len(seg)) / 44100
    check(tail < 0.05, f"old audio left over after seek {tail * 1000:.0f} ms (must be < 50 ms, should mute immediately)")
    st = status()
    check(20 <= st["position"] < 24, f"position after seeking to 20 s: {st['position']:.1f}")
    pi = soap("AVTransport", "GetPositionInfo", InstanceID=0)
    check(pi["RelTime"].startswith("0:00:2"), f"position seen by NetEase {pi['RelTime']}")
    state, st = poll(30, until=lambda s, w: s == "STOPPED")
    check(state == "STOPPED", "automatically STOPPED at the end after seeking")
    verify_audio(sink, mark, "t44_30s.flac", 16, 44100, offset_s=20)


def sc_switch(sink):
    """Change track mid-playback: old one stops, new one plays, no hang"""
    netease_push("t44_30s.flac", "First track")
    poll(8, until=lambda s, w: w["position"] > 2)
    mark = sink.mark()
    t = time.time()
    netease_push("t48_24.flac", "Second track")
    check(time.time() - t < 3, f"track-change commands all returned within {time.time() - t:.2f} s")
    state, st = poll(40, until=lambda s, w: s == "STOPPED")
    check(st["title"] == "Second track" and st["rate"] == 48000, "switched to the second track at 48k")
    verify_audio(sink, mark, "t48_24.flac", 24, 48000, lead_s=0.2)


def sc_mp3(sink):
    """MP3: plays, duration estimate, marked as lossy"""
    mark = sink.mark()
    netease_push("t44.mp3", "MP3 test")
    state, st = poll(10, until=lambda s, w: w["position"] > 6)
    check(st["codec"] == "MP3" and not st["lossless"], f"detected as lossy MP3 ({st['codec']})")
    check(abs(st["duration"] - 20) < 2, f"estimated duration {st['duration']:.1f} s (actual 20)")
    state, st = poll(30, until=lambda s, w: s == "STOPPED")
    samples, _, errs = decode_spdif(sink.since(mark))
    got = strip_silence(samples)
    check(errs == 0 and abs(len(got) / 44100 - 20) < 0.2, f"output {len(got) / 44100:.2f} s of audio, bitstream error-free")


def sc_errors(sink):
    """192kHz (beyond the optical module limit), 404, reconnect after a mid-download disconnect"""
    netease_push("t192_24.flac")
    state, st = poll(15, until=lambda s, w: w["error"] != "")
    check("Unsupported" in st["error"], f"192k error: {st['error']}")
    state, st = poll(10, until=lambda s, w: s == "STOPPED")
    check(state == "STOPPED", "state STOPPED after the 192k error, no hang")
    EventHandler.events.clear()
    code, sid, _ = subscribe("AVTransport", "/avt")
    netease_push("not_exist.flac")
    state, st = poll(15, until=lambda s, w: w["error"] != "")
    check("404" in st["error"], f"404 error: {st['error']}")
    time.sleep(1)
    body = next((e[4] for e in reversed(EventHandler.events) if e[1] == sid), "")
    check("ERROR_OCCURRED" in body, "event TransportStatus=ERROR_OCCURRED on error")
    ti = soap("AVTransport", "GetTransportInfo", InstanceID=0)
    check(ti.get("CurrentTransportStatus") == "ERROR_OCCURRED", f"GetTransportInfo also reports the error ({ti.get('CurrentTransportStatus')})")
    urllib.request.urlopen(urllib.request.Request(f"{DLNA}/AVTransport/event", method="UNSUBSCRIBE", headers={"SID": sid}), timeout=10)
    print("  — disconnect at 500KB of download")
    MediaHandler.drop_once["t44_16.flac"] = 500_000
    MediaHandler.requests.clear()
    mark = sink.mark()
    netease_push("t44_16.flac", "Disconnect test")
    state, st = poll(45, until=lambda s, w: s == "STOPPED")
    ranges = [r for p, r in MediaHandler.requests if p == "t44_16.flac"]
    check(len(ranges) >= 2, f"automatically reconnected after disconnect (Range requests: {ranges})")
    verify_audio(sink, mark, "t44_16.flac", 16, 44100)


def sc_events(sink):
    """Event subscription: the board pushes state; after pausing NetEase-style (Play or Pause chosen from the state in events) it can resume"""
    EventHandler.events.clear()
    code, sid, timeout = subscribe("AVTransport", "/avt")
    check(code == 200 and sid and sid.startswith("uuid:"), f"subscribed to AVTransport (SID {sid}, {timeout})")
    code2, sid2, _ = subscribe("RenderingControl", "/rc")
    check(code2 == 200 and sid2, "subscribed to RenderingControl")
    time.sleep(2)
    check(any(e[1] == sid and e[2] == 0 for e in EventHandler.events), "received AVTransport initial event (SEQ=0)")
    check(any(e[1] == sid2 for e in EventHandler.events), "received RenderingControl initial event")

    def toggle():
        # NetEase's play/pause button: looks at the device state it recorded (from events); sends Pause while playing, otherwise Play
        st = event_state()
        act = "Pause" if st == "PLAYING" else "Play"
        soap("AVTransport", act, InstanceID=0, **({"Speed": 1} if act == "Play" else {}))
        return st, act

    netease_push("t44_30s.flac", "Event test")
    poll(8, until=lambda s, w: w["position"] > 2)
    time.sleep(1)
    check(event_state() == "PLAYING", f"event reports PLAYING ({event_state()})")
    st, act = toggle()
    check(act == "Pause", f"button: device state {st} → sends {act}")
    time.sleep(1.5)
    check(event_state() == "PAUSED_PLAYBACK", f"event reports PAUSED_PLAYBACK ({event_state()})")
    p1 = status()["position"]
    time.sleep(2)
    st, act = toggle()
    check(act == "Play", f"button again: device state {st} → sends {act}")
    time.sleep(2)
    w = status()
    check(w["state"] == "PLAYING" and w["position"] > p1 + 1, f"resumed, position {p1:.1f} → {w['position']:.1f}")
    seqs = [e[2] for e in EventHandler.events if e[1] == sid]
    check(seqs == list(range(len(seqs))), f"event SEQ increments contiguously ({seqs})")
    soap("RenderingControl", "SetVolume", InstanceID=0, Channel="Master", DesiredVolume=37)
    time.sleep(1.5)
    check(any(e[1] == sid2 and "37" in e[4] for e in EventHandler.events), "volume change pushed a RenderingControl event")
    req = urllib.request.Request(f"{DLNA}/AVTransport/event", method="UNSUBSCRIBE", headers={"SID": sid})
    with urllib.request.urlopen(req, timeout=10) as r:
        check(r.status == 200, "unsubscribed")


def sc_sockets(sink):
    """Connections: hold 12 idle TCP connections first (phone, web page and PC may all keep connections alive), then control commands must still respond"""
    held = []
    for i in range(12):
        c = socket.create_connection(("127.0.0.1", 18080 if i % 2 else 19152), timeout=5)  # 6 each on the web page and DLNA
        c.sendall(b"GET /api/ll/status HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n")
        held.append(c)
    time.sleep(2)
    ok, err = 0, ""
    for _ in range(3):
        try:
            status()
            soap("AVTransport", "GetTransportInfo", InstanceID=0)
            ok += 1
        except Exception as e:
            err = f"{type(e).__name__}: {e}"
        time.sleep(1)
    for c in held:
        c.close()
    check(ok == 3, f"with connections exhausted, {ok} of 3 requests succeeded{(' (' + err + ')') if err else ''}")
    bad, _ = serial_errors(0)
    acc = sum("error in accept" in l for l in open(SERIAL_LOG, errors="replace"))
    check(acc == 0, f"no accept failures on serial ({acc})")


def sc_mislabel(sink):
    """Server mislabels the type (NetEase's old storage labels FLAC as audio/mpeg, as measured): detect by file content; unrecognisable data must fail quickly"""
    mark = sink.mark()
    netease_push("t44_16.flac?ctype=audio%2Fmpeg", "Wrong type")
    state, st = poll(15, until=lambda s, w: w["codec"] != "" or w["error"] != "")
    check(st["codec"] == "FLAC", f"FLAC with Content-Type audio/mpeg detected as {st['codec'] or '(unrecognised)'}{(', error: ' + st['error']) if st['error'] else ''}")
    state, st = poll(40, until=lambda s, w: s == "STOPPED")
    verify_audio(sink, mark, "t44_16.flac", 16, 44100)
    t = time.time()
    netease_push("garbage.bin?ctype=audio%2Fmpeg", "Garbage")
    state, st = poll(20, until=lambda s, w: w["error"] != "")
    check(st["error"] != "", f"unrecognisable data reported an error within {time.time() - t:.1f} s: {st['error'] or '(none)'}")
    state, st = poll(10, until=lambda s, w: s == "STOPPED")
    check(state == "STOPPED", "state STOPPED after the error")


def sc_startup(sink):
    """Track-change start-up (simulated 80 ms latency, 2 MB/s network): start-up time with a 400KB cover; bit-depth-only change doesn't restart I2S; sample-rate change outputs silence first for the speaker to lock"""
    netease_push("t44_16.flac", "16bit")
    poll(10, until=lambda s, w: w["position"] > 2)
    pos = os.path.getsize(SERIAL_LOG)
    netease_push("t44_cover24.flac", "24bit with 400KB cover")
    state, st = poll(10, until=lambda s, w: w["startMs"] >= 0)
    print(f"  · start-up latency {st['startMs']} ms, {st['restarts']} HTTP re-requests")
    check(0 <= st["startMs"] < 500, f"start-up latency with a 400KB cover {st['startMs']} ms (must be < 500 ms)")
    log = open(SERIAL_LOG, "rb").read()[pos:].decode("utf8", "replace")
    for l in log.splitlines():
        if "Start-up phases" in l or "Output format" in l or "Channel status" in l:
            print("  · serial: " + l.split(": ", 1)[-1])
    check("I2S not restarted" in log, "bit-depth-only change (16→24bit) doesn't restart I2S (stated explicitly in the serial log)")
    poll(5, until=lambda s, w: w["position"] > 2)
    mark = sink.mark()
    netease_push("t48_24.flac", "48k")
    poll(10, until=lambda s, w: w["position"] > 2)
    samples, blocks, _ = decode_spdif(sink.since(mark))
    starts = decode_spdif.block_starts
    sw = next((starts[i] for i, b in enumerate(blocks) if b[3] == 0x2), None)
    check(sw is not None, "bitstream switched to 48kHz")
    nz = np.nonzero(np.any(samples[sw:] != 0, axis=1))[0]
    lead = (nz[0] if len(nz) else 0) / 48000
    check(lead >= 0.25, f"after switching to 48kHz, {lead * 1000:.0f} ms of silence before audio (must be >= 250 ms, for the speaker to relock)")


def set_output(oid):
    req = urllib.request.Request(f"{BOARD}/api/ll/output", data=oid.encode(), method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status


def decode_i2s(raw):
    """Analog output: 32-bit stereo I2S, 24-bit samples in the high bits"""
    return (np.frombuffer(raw[:len(raw) // 8 * 8], dtype="<i4") >> 8).reshape(-1, 2)


def sc_analog(sink):
    """3.5mm analog output: switching, bit-perfect I2S data, NetEase volume/mute only affect analog, switching back to optical while playing"""
    asink = Sink.analog
    try:
        soap("RenderingControl", "SetVolume", InstanceID=0, Channel="Master", DesiredVolume=100)  # the events scenario changed the volume
        soap("RenderingControl", "SetMute", InstanceID=0, Channel="Master", DesiredMute=0)
        t = time.time()
        check(set_output("analog") == 200 and status()["output"] == "analog", f"switched to analog output ({time.time() - t:.2f} s)")
        mark, smark = asink.mark(), sink.mark()
        netease_push("t48_24.flac", "Analog output")
        state, st = poll(40, until=lambda s, w: s == "STOPPED")
        check(state == "STOPPED" and not st["error"] and st["underruns"] == 0, f"playback finished (error {st['error'] or 'none'}, underruns {st['underruns']})")
        got, ref = strip_silence(decode_i2s(asink.since(mark))), strip_silence(reference_pcm("t48_24.flac"))
        same = len(got) == len(ref) and np.array_equal(got, ref)
        detail = f"received {len(got)} frames, reference {len(ref)} frames"
        if not same and len(got) == len(ref):
            d = np.nonzero(np.any(got != ref, axis=1))[0]
            detail += f", {len(d)} frames differ, first at frame {d[0]}: {got[d[0]].tolist()} vs {ref[d[0]].tolist()}"
        check(same, f"I2S data bit-perfect at volume 100 ({detail})")
        check(len(sink.since(smark)) == 0, "no optical output while on analog")

        soap("RenderingControl", "SetVolume", InstanceID=0, Channel="Master", DesiredVolume=50)
        mark = asink.mark()
        netease_push("t44_16.flac", "Volume 50")
        poll(40, until=lambda s, w: s == "STOPPED")
        got, ref = decode_i2s(asink.since(mark)).astype(np.float64), reference_pcm("t44_16.flac").astype(np.float64)
        ratio, want = np.sum(got ** 2) / np.sum(ref ** 2), 10 ** (-25 / 10)
        check(abs(ratio / want - 1) < 0.05, f"volume 50 → energy {10 * np.log10(ratio):.1f} dB (expected -25 dB)")

        soap("RenderingControl", "SetVolume", InstanceID=0, Channel="Master", DesiredVolume=100)
        netease_push("t44_30s.flac", "Mute")
        poll(8, until=lambda s, w: w["position"] > 2)
        soap("RenderingControl", "SetMute", InstanceID=0, Channel="Master", DesiredMute=1)
        time.sleep(0.5)
        mark = asink.mark()
        time.sleep(2)
        x = decode_i2s(asink.since(mark))
        check(len(x) > 0.5 * 44100 and not np.any(x), f"all zeros after mute ({len(x)} frames)")
        soap("RenderingControl", "SetMute", InstanceID=0, Channel="Master", DesiredMute=0)
        time.sleep(0.5)
        mark = asink.mark()
        time.sleep(1)
        check(np.any(decode_i2s(asink.since(mark))), "audio comes back after unmute")

        smark = sink.mark()
        t = time.time()
        check(set_output("spdif") == 200, f"switched back to optical while playing ({time.time() - t:.2f} s)")
        mark = asink.mark()
        time.sleep(2)
        st = status()
        check(st["output"] == "spdif" and st["state"] == "PLAYING", f"still playing ({st['state']}), output {st['output']}")
        samples, _, errs = decode_spdif(sink.since(smark))
        check(errs == 0 and np.any(samples), f"optical bitstream correct and audible (structural errors {errs})")
        check(len(asink.since(mark)) == 0, "analog output stops after switching back to optical")
        soap("RenderingControl", "SetVolume", InstanceID=0, Channel="Master", DesiredVolume=50)
        smark = sink.mark()
        time.sleep(1)
        samples, _, _ = decode_spdif(sink.since(smark))
        ref = reference_pcm("t44_30s.flac")
        check(np.abs(samples).max() >= np.abs(ref).max() * 0.5, "optical unaffected by volume (stays bit-perfect)")
    finally:
        soap("RenderingControl", "SetVolume", InstanceID=0, Channel="Master", DesiredVolume=100)
        soap("RenderingControl", "SetMute", InstanceID=0, Channel="Master", DesiredMute=0)
        set_output("spdif")


QEMU = []  # the current QEMU process (the reboot scenario replaces it)


def start_qemu(keep_flash=False):
    """Start QEMU and wait until fully booted; keep_flash reuses the previous flash (keeps NVS)"""
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)
    env = dict(os.environ, KEEP_FLASH="1" if keep_flash else "0")
    QEMU[:] = [subprocess.Popen([os.path.join(ROOT, "sim", "qemu_run.sh"), SERIAL_LOG], env=env,
                                stdout=open(SERIAL_LOG + ".qemu", "w"), stderr=subprocess.STDOUT)]  # QEMU's own errors
    for _ in range(60):
        if os.path.exists(SERIAL_LOG) and all(k in open(SERIAL_LOG, errors="replace").read() for k in ("DLNA started", "AirPlay ready")):  # wait until fully booted (AirPlay RSA key generation is CPU-heavy)
            return
        time.sleep(1)
    raise SystemExit("simulator didn't boot within 60 s")


def sc_reboot(sink):
    """Restore after reboot: PAUSED pushed before reboot; subscriptions and track saved to NVS, restored as PAUSED at boot and pushed, controller resumes from the same position with just Play"""
    EventHandler.events.clear()
    code, sid, _ = subscribe("AVTransport", "/avt")
    check(code == 200 and sid, f"subscribed to AVTransport ({sid})")
    netease_push("t44_30s.flac", "Reboot test")
    poll(10, until=lambda s, w: w["position"] > 4)
    time.sleep(1)
    check(event_state() == "PLAYING", f"event PLAYING before reboot ({event_state()})")
    seq_before = max(e[2] for e in EventHandler.events if e[1] == sid)
    pos_before = status()["position"]
    try:
        urllib.request.urlopen(urllib.request.Request(f"{BOARD}/api/system/restart", data=b"", method="POST"), timeout=5).read()
    except Exception:
        pass  # the board may reboot before responding
    QEMU[0].wait(timeout=20)  # -no-reboot: QEMU exits after esp_restart
    check(event_state() == "PAUSED_PLAYBACK", f"PAUSED_PLAYBACK pushed before reboot ({event_state()})")
    n0 = len(EventHandler.events)
    sink.connected.clear()
    start_qemu(keep_flash=True)
    check(sink.connected.wait(10), "audio sink reconnected after reboot")
    time.sleep(3)
    new = [e for e in EventHandler.events[n0:] if e[1] == sid]
    check(new, f"no resubscribe needed after reboot, original SID receives events ({len(new)})")
    check(event_state() == "PAUSED_PLAYBACK", f"event PAUSED_PLAYBACK after reboot ({event_state()})")
    check("t44_30s.flac" in new[-1][4] and "Reboot test" in new[-1][4], "event after reboot carries the original track URL and metadata")
    check(new[0][2] > seq_before, f"SEQ keeps counting up (before reboot {seq_before}, after {new[0][2]})")
    pi = soap("AVTransport", "GetPositionInfo", InstanceID=0)
    check("t44_30s.flac" in pi.get("TrackURI", ""), "GetPositionInfo returns the original track")
    # The controller sees STOPPED; pressing play only sends Play
    soap("AVTransport", "Play", InstanceID=0, Speed=1)
    _, w = poll(10, until=lambda s, w: w["state"] == "PLAYING" and w["position"] > pos_before + 0.5)
    check(w["state"] == "PLAYING" and w["position"] >= pos_before - 0.5,
          f"just Play resumes from the same position (before reboot {pos_before:.1f} s, now {w['position']:.1f} s)")
    time.sleep(1)
    check(event_state() == "PLAYING", f"event PLAYING ({event_state()})")
    req = urllib.request.Request(f"{DLNA}/AVTransport/event", method="UNSUBSCRIBE", headers={"SID": sid})
    with urllib.request.urlopen(req, timeout=10) as r:
        check(r.status == 200, "unsubscribed")


def lossy_match(got, name, start_s=0.0, rate=44100):
    """Lossy formats can't be compared sample by sample: find the alignment in the reference and compute SNR (dB)"""
    ref = reference_pcm(name)[:, 0].astype(np.float64)
    g = got[:, 0].astype(np.float64)
    seg = g[rate // 2: rate // 2 + rate]  # take 0.5-1.5 s of what was received
    if len(seg) < rate:
        return -99, 0
    lo = max(0, int((start_s - 1.5) * rate))
    win = ref[lo: lo + int(4 * rate)]
    if len(win) < len(seg):
        return -99, 0
    c = np.correlate(win, seg, mode="valid")
    k = int(np.argmax(c))
    m = win[k:k + len(seg)]
    snr = 10 * np.log10(np.sum(m ** 2) / max(np.sum((m - seg) ** 2), 1e-9))
    return snr, (lo + k - rate // 2) / rate  # which second of the reference the received start corresponds to


def sc_m4a(sink):
    """M4A (AAC): plays with moov at the front and at the end; duration and format correct; audio matches the reference; seekable"""
    for name in ("t44_fast.m4a", "t44_tail.m4a"):
        mark = sink.mark()
        netease_push(name + "?ctype=audio%2Fmp4", name)
        state, w = poll(15, until=lambda s_, w: w["codec"] != "" or w["error"] != "")
        check(w["codec"] == "AAC" and w["rate"] == 44100, f"{name}: detected as AAC 44.1k ({w['codec']} {w['rate']} {w['error']})")
        check(abs(w["duration"] - 30) < 0.2, f"{name}: duration 30 s ({w['duration']:.2f})")
        state, w = poll(40, until=lambda s_, w: s_ == "STOPPED")
        check(state == "STOPPED" and w["underruns"] == 0 and not w["error"], f"{name}: played to the end, {w['underruns']} underruns")
        samples, _, errs = decode_spdif(sink.since(mark))
        got = strip_silence(samples)
        check(errs == 0 and abs(len(got) / 44100 - 30) < 0.2, f"{name}: received {len(got) / 44100:.2f} s of audio, {errs} bitstream errors")
        snr, at = lossy_match(got, name)
        check(snr > 30 and abs(at) < 0.1, f"{name}: aligned with reference (offset {at * 1000:.0f} ms), SNR {snr:.0f} dB")
    print("  — seek (file with moov at the end)")
    netease_push("t44_tail.m4a?ctype=audio%2Fmp4", "M4A seek")
    poll(10, until=lambda s_, w: w["position"] > 2)
    soap("AVTransport", "Seek", InstanceID=0, Unit="REL_TIME", Target="00:00:20")
    _, w = poll(5, until=lambda s_, w: w["position"] >= 20)
    check(20 <= w["position"] < 23, f"seeked to 20 s ({w['position']:.1f})")
    time.sleep(0.5)
    mark = sink.mark()
    time.sleep(3)
    got = strip_silence(decode_spdif(sink.since(mark))[0])
    snr, at = lossy_match(got, "t44_tail.m4a", start_s=w["position"] + 0.6)
    check(snr > 30, f"audio matches reference after seek (reference at {at:.2f} s, SNR {snr:.0f} dB)")
    soap("AVTransport", "Stop", InstanceID=0)


def sc_ota(sink):
    """OTA while playing: pause first (synced via events), free the buffer, receive the whole firmware image into PSRAM.
    QEMU hangs entirely at the flash-write step (also when idle; it's a simulator problem), so only verify up to receiving the firmware, then restart the simulator"""
    EventHandler.events.clear()
    code, sid, _ = subscribe("AVTransport", "/avt")
    netease_push("t44_30s.flac", "OTA test")
    poll(10, until=lambda s_, w: w["position"] > 4)
    log0 = os.path.getsize(SERIAL_LOG)
    fw = open(os.path.join(ROOT, "build_sim", "airplay2-receiver.bin"), "rb").read()
    req = urllib.request.Request(f"{BOARD}/api/ota/update", data=fw, method="POST")
    try:
        urllib.request.urlopen(req, timeout=20).read()
    except Exception:
        pass
    log = open(SERIAL_LOG, "rb").read()[log0:].decode("utf8", "replace")
    check("Stopped playback before OTA" in log, "DLNA playback stopped before OTA")
    check("Receiving firmware into PSRAM" in log and "SHA-256 verified OK" in log, "whole firmware image received into PSRAM and verified (not written as it arrives)")
    check(any(e[1] == sid and "PAUSED_PLAYBACK" in e[4] for e in EventHandler.events), "PAUSED_PLAYBACK pushed before OTA")
    QEMU[0].terminate()
    QEMU[0].wait()
    sink.connected.clear()
    start_qemu()  # rebuild flash; subsequent scenarios start from a clean state
    check(sink.connected.wait(10), "simulator restarted")


class Reject(http.server.BaseHTTPRequestHandler):
    hits = 0

    def log_message(self, *a):
        pass

    def do_NOTIFY(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        Reject.hits += 1
        self.send_response(412)
        self.end_headers()


def sc_offline(sink):
    """Controller temporarily can't receive events (locked screen/background): subscription kept, notifications resume afterwards; Pause while stopped re-pushes state; duplicate subscribe from the same callback keeps the SID; lease as requested; 412 reply removes the subscription"""
    port = EVENT_PORT + 1  # not listening yet, simulating a phone that can't receive
    req = urllib.request.Request(f"{DLNA}/AVTransport/event", method="SUBSCRIBE", headers={
        "CALLBACK": f"<http://{GUEST_HOST}:{port}/off>", "NT": "upnp:event", "TIMEOUT": "Second-1800"})
    with urllib.request.urlopen(req, timeout=10) as rr:
        sid = rr.headers.get("SID")
    netease_push("t44_30s.flac", "Offline test")
    poll(8, until=lambda s_, w: w["position"] > 1)
    for act in ("Pause", "Play", "Pause", "Play"):  # cause 4+ failed notifications
        soap("AVTransport", act, InstanceID=0, **({"Speed": 1} if act == "Play" else {}))
        time.sleep(1.5)
    srv = ThreadingServer(("127.0.0.1", port), EventHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        n0 = len(EventHandler.events)
        soap("AVTransport", "Pause", InstanceID=0)
        time.sleep(1.5)
        got = [e for e in EventHandler.events[n0:] if e[1] == sid]
        check(got and "PAUSED_PLAYBACK" in got[-1][4], f"subscription kept after repeated push failures, event received after recovery ({len(got)})")
        soap("AVTransport", "Stop", InstanceID=0)
        time.sleep(1.5)
        n1 = len(EventHandler.events)
        soap("AVTransport", "Pause", InstanceID=0)  # controller thinks we're still playing
        time.sleep(1.5)
        got = [e for e in EventHandler.events[n1:] if e[1] == sid]
        check(got and "STOPPED" in got[-1][4], f"Pause while stopped re-pushed STOPPED ({len(got)})")
        with urllib.request.urlopen(req, timeout=10) as rr:  # subscribe again from the same callback
            sid2 = rr.headers.get("SID")
        renew = urllib.request.Request(f"{DLNA}/AVTransport/event", method="SUBSCRIBE", headers={"SID": sid, "TIMEOUT": "Second-300"})
        with urllib.request.urlopen(renew, timeout=10) as rr:
            code, tmo = rr.status, rr.headers.get("TIMEOUT")
        check(sid2 == sid and code == 200, f"duplicate subscribe from the same callback: keeps the SID, renewing the old SID still works ({code})")
        check(tmo == "Second-300", f"lease as requested ({tmo})")
        urllib.request.urlopen(urllib.request.Request(f"{DLNA}/AVTransport/event", method="UNSUBSCRIBE", headers={"SID": sid}), timeout=10)
        # Controller replies 412 (no longer recognises this SID): remove the subscription
        Reject.hits = 0
        rsrv = ThreadingServer(("127.0.0.1", port + 1), Reject)
        threading.Thread(target=rsrv.serve_forever, daemon=True).start()
        try:
            rq = urllib.request.Request(f"{DLNA}/AVTransport/event", method="SUBSCRIBE", headers={
                "CALLBACK": f"<http://{GUEST_HOST}:{port + 1}/rej>", "NT": "upnp:event", "TIMEOUT": "Second-1800"})
            with urllib.request.urlopen(rq, timeout=10) as rr:
                sid3 = rr.headers.get("SID")
            time.sleep(1.5)  # the initial event gets a 412
            renew = urllib.request.Request(f"{DLNA}/AVTransport/event", method="SUBSCRIBE", headers={"SID": sid3})
            try:
                code = urllib.request.urlopen(renew, timeout=10).status
            except urllib.error.HTTPError as e:
                code = e.code
            check(Reject.hits >= 1 and code == 412, f"subscription removed after controller replied 412 ({Reject.hits} notifications received, renewal then returned {code})")
        finally:
            rsrv.shutdown()
            rsrv.server_close()
    finally:
        srv.shutdown()
        srv.server_close()


def sim_cmd(cmd):
    req = urllib.request.Request(f"{DLNA}/sim", data=cmd.encode(), method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status


def ramp_ok(samples):
    """Simulated AirPlay sawtooth: left channel +1 per frame (16 bit, in the high bits of 24 bit, i.e. +256), right channel negated. Returns (contiguous frames, number of breaks)"""
    left = samples[:, 0].astype(np.int64)
    d = np.diff(left) & 0xFFFFFF  # 24-bit wraparound
    good = (d == 256) | (d == (256 - 65536 * 256) & 0xFFFFFF)
    breaks = int(np.sum(~good))
    rs = np.all(samples[:, 1].astype(np.int64) == ((-(left >> 8)) & 0xFFFF).astype(np.int16).astype(np.int64) << 8)
    return len(left), breaks, bool(rs)


def sc_preempt(sink):
    """Source pre-emption (latest wins): AirPlay starts while DLNA plays → DLNA stops and notifies NetEase, output switches to AirPlay with no breaks; push DLNA again → takes the output back"""
    EventHandler.events.clear()
    code, sid, _ = subscribe("AVTransport", "/avt")
    check(code == 200, "NetEase subscribed to events")
    try:
        netease_push("t44_30s.flac", "Pre-empt test")
        poll(8, until=lambda s, w: w["position"] > 2)
        check(status()["source"] == "dlna", "DLNA playing, source dlna")
        sim_cmd("airplay_play")
        time.sleep(2)
        st = status()
        check(st["source"] == "airplay", f"source switched to airplay after AirPlay started playing ({st['source']})")
        check(st["state"] == "STOPPED", f"DLNA stopped ({st['state']})")
        check(event_state() == "STOPPED", f"NetEase received STOPPED event ({event_state()})")
        mark = sink.mark()
        time.sleep(3)
        samples, _, errs = decode_spdif(sink.since(mark))
        check(errs == 0, f"S/PDIF bitstream well-formed (errors {errs})")
        n, breaks, rs = ramp_ok(samples)
        check(n > 2 * 44100 and breaks == 0, f"AirPlay pull path contiguous: {n} frames, {breaks} breaks")
        check(rs, "AirPlay left/right channels correct")
        mark = sink.mark()
        netease_push("t48_24.flac", "Take back")
        poll(10, until=lambda s, w: w["position"] > 1)
        st = status()
        check(st["source"] == "dlna" and st["state"] == "PLAYING", f"new DLNA push takes the output back ({st['source']} {st['state']})")
        time.sleep(2)
        samples, blocks, _ = decode_spdif(sink.since(mark))
        tail = samples[-48000:]
        n, breaks, _ = ramp_ok(tail)
        check(breaks > 1000, "no more AirPlay data after taking it back")
        check(any(b[3] == 0x2 for b in blocks), "output switched to DLNA's 48kHz")
        sim_cmd("airplay_repeat")  # the PLAYING events AirPlay sends repeatedly while playing
        time.sleep(1.5)
        st = status()
        check(st["source"] == "dlna" and st["state"] == "PLAYING", f"repeated AirPlay playing events don't grab the output back ({st['source']} {st['state']})")
    finally:
        sim_cmd("airplay_stop")
        soap("AVTransport", "Stop", InstanceID=0)


def web_control(cmd):
    req = urllib.request.Request(f"{BOARD}/api/ll/control", data=cmd.encode(), method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status


def sc_local(sink):
    """Local web control: play/pause/stop/seek, state synced to NetEase via UPnP events"""
    EventHandler.events.clear()
    code, sid, _ = subscribe("AVTransport", "/avt")
    check(code == 200, "NetEase subscribed to events")
    netease_push("t44_30s.flac", "Local control")
    poll(8, until=lambda s, w: w["position"] > 2)
    web_control("pause")
    time.sleep(1.5)
    check(status()["state"] == "PAUSED_PLAYBACK", "web pause → board paused")
    check(event_state() == "PAUSED_PLAYBACK", f"NetEase received event {event_state()}")
    nxt = "Pause" if event_state() == "PLAYING" else "Play"
    check(nxt == "Play", f"NetEase's button would now send {nxt} (\"resume\" no longer sent as Pause)")
    web_control("play")
    time.sleep(1.5)
    check(status()["state"] == "PLAYING" and event_state() == "PLAYING", f"web resume → playing, NetEase received {event_state()}")
    web_control("seek:15")
    time.sleep(1.5)
    p = status()["position"]
    check(15 <= p < 18, f"web seek to 15 s → position {p:.1f}")
    pi = soap("AVTransport", "GetPositionInfo", InstanceID=0)
    check(pi["RelTime"].startswith("0:00:1"), f"position seen by NetEase {pi['RelTime']}")
    web_control("stop")
    time.sleep(1.5)
    check(status()["state"] == "STOPPED" and event_state() == "STOPPED", f"web stop → STOPPED, NetEase received {event_state()}")
    web_control("play")
    time.sleep(2.5)
    st = status()
    check(st["state"] == "PLAYING" and st["position"] < 3, f"play after stop starts from the beginning (position {st['position']:.1f})")
    try:
        web_control("next")
        check(False, "unknown command should return 400")
    except urllib.error.HTTPError as e:
        check(e.code == 400, f"unknown command (next) returned {e.code}")


SCENARIOS = {"basic": sc_basic, "hires": sc_hires, "seek": sc_seek, "switch": sc_switch, "mp3": sc_mp3, "errors": sc_errors,
             "events": sc_events, "sockets": sc_sockets, "mislabel": sc_mislabel, "startup": sc_startup, "local": sc_local,
             "analog": sc_analog, "preempt": sc_preempt, "wav": sc_wav, "reboot": sc_reboot, "offline": sc_offline, "ota": sc_ota, "m4a": sc_m4a}


def main():
    names = sys.argv[1:] or list(SCENARIOS)
    srv = ThreadingServer(("127.0.0.1", MEDIA_PORT), MediaHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    sink = Sink()
    Sink.analog = Sink(SINK_PORT + 1)
    esrv = ThreadingServer(("127.0.0.1", EVENT_PORT), EventHandler)
    threading.Thread(target=esrv.serve_forever, daemon=True).start()
    results = {}
    try:
        start_qemu()
        check(sink.connected.wait(10), "audio sink connected")
        for n in names:
            print(f"\n=== Scenario {n}: {SCENARIOS[n].__doc__}")
            pos = os.path.getsize(SERIAL_LOG)
            try:
                SCENARIOS[n](sink)
                bad, _ = serial_errors(pos)
                check(not bad, "no watchdog/crash on serial" + (": " + bad[0] if bad else ""))
                results[n] = "PASS"
            except Fail as e:
                results[n] = f"FAIL: {e}"
            except Exception as e:
                results[n] = f"ERROR: {type(e).__name__}: {e}"
                print("  ✗", results[n])
            soap("AVTransport", "Stop", InstanceID=0)
            time.sleep(1)
    finally:
        QEMU[0].terminate()
        QEMU[0].wait()
    print("\n=== Summary")
    for n, r in results.items():
        print(f"  {n:8s} {r}")
    if Stats.web_lat:
        print(f"  web API latency: max {max(Stats.web_lat) * 1000:.0f} ms, mean {np.mean(Stats.web_lat) * 1000:.0f} ms")
        print(f"  SOAP latency: max {max(Stats.soap_lat) * 1000:.0f} ms, mean {np.mean(Stats.soap_lat) * 1000:.0f} ms")
    sys.exit(0 if all(r == "PASS" for r in results.values()) else 1)


if __name__ == "__main__":
    main()
