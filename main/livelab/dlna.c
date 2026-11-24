#include "esp_heap_caps.h"
// DLNA renderer (MediaRenderer): SSDP discovery + UPnP device/service descriptions + SOAP control; playback is handed to player
#include "dlna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "gena.h"
#include "output.h"
#include "player.h"
#include "settings.h"

#define TAG "dlna"
#define SERVER_STR "ESP32/1.0 UPnP/1.0 ESP32Player/1.0"

#define SVC_AVT "urn:schemas-upnp-org:service:AVTransport:1"
#define SVC_RC  "urn:schemas-upnp-org:service:RenderingControl:1"
#define SVC_CM  "urn:schemas-upnp-org:service:ConnectionManager:1"
#define DEV_MR  "urn:schemas-upnp-org:device:MediaRenderer:1"

#define SINK_PROTOCOLS                                                                 \
    "http-get:*:audio/flac:*,http-get:*:audio/x-flac:*,http-get:*:audio/mpeg:*,"         \
    "http-get:*:audio/mp3:*,http-get:*:audio/*:*"

static char g_ip[16];
static int g_port;
static char g_name[65];  // device name (same as AirPlay / Bluetooth, editable on the web page)
static char g_uuid[48];
static char *g_uri_esc, *g_meta_esc;  // escaped copies to send back to the controller
static SemaphoreHandle_t g_uri_mx;    // the GENA task reads the two above too; must not read freed ones during a track change
static int g_volume = 100, g_mute;    // applies to the 3.5mm output; optical uses fixed volume by default (bit-perfect), can be turned off on the web page

static char *xml_escape(const char *s)
{
    if (!s) return strdup("");
    size_t n = 1;
    for (const char *p = s; *p; p++) n += (*p == '&') ? 5 : (*p == '<' || *p == '>') ? 4 : (*p == '"') ? 6 : 1;
    char *o = malloc(n), *q = o;
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '&': memcpy(q, "&amp;", 5); q += 5; break;
        case '<': memcpy(q, "&lt;", 4); q += 4; break;
        case '>': memcpy(q, "&gt;", 4); q += 4; break;
        case '"': memcpy(q, "&quot;", 6); q += 6; break;
        default: *q++ = *p;
        }
    }
    *q = 0;
    return o;
}

static void xml_unescape(char *s)
{
    static const struct { const char *e; char c; } T[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    char *w = s;
    for (char *r = s; *r;) {
        bool hit = false;
        if (*r == '&') {
            for (int i = 0; i < 5; i++) {
                size_t l = strlen(T[i].e);
                if (!strncmp(r, T[i].e, l)) { *w++ = T[i].c; r += l; hit = true; break; }
            }
            if (!hit && r[1] == '#') {
                char *end;
                long v = (r[2] == 'x' || r[2] == 'X') ? strtol(r + 3, &end, 16) : strtol(r + 2, &end, 10);
                if (*end == ';' && v > 0 && v < 128) { *w++ = (char)v; r = end + 1; hit = true; }
            }
        }
        if (!hit) *w++ = *r++;
    }
    *w = 0;
}

// Get <tag>content</tag> (allows <tag attrs>, <tag/>), returns an unescaped malloc'd string
static char *xml_get(const char *body, const char *tag)
{
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *p = body;
    while ((p = strstr(p, open))) {
        char c = p[strlen(open)];
        if (c == '>' || c == ' ' || c == '/') break;
        p++;
    }
    if (!p) return NULL;
    const char *gt = strchr(p, '>');
    if (!gt) return NULL;
    if (gt[-1] == '/') return strdup("");
    const char *e = strstr(gt + 1, close);
    if (!e) return NULL;
    char *o = strndup(gt + 1, e - gt - 1);
    xml_unescape(o);
    return o;
}

static double parse_hms(const char *s)
{
    int h = 0, m = 0;
    double sec = 0;
    if (s && sscanf(s, "%d:%d:%lf", &h, &m, &sec) == 3) return h * 3600 + m * 60 + sec;
    return 0;
}

static void fmt_hms(char *o, size_t n, int s)
{
    if (s < 0) s = 0;
    snprintf(o, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
}

static esp_err_t soap_reply(httpd_req_t *r, const char *svc, const char *act, const char *inner)
{
    char *x = NULL;
    int n = asprintf(&x,
                     "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                     "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                     "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                     "<u:%sResponse xmlns:u=\"%s\">%s</u:%sResponse></s:Body></s:Envelope>",
                     act, svc, inner ? inner : "", act);
    if (n < 0) return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "text/xml; charset=\"utf-8\"");
    esp_err_t e = httpd_resp_send(r, x, n);
    free(x);
    return e;
}

static esp_err_t soap_fault(httpd_req_t *r, int code, const char *desc)
{
    char x[600];
    int n = snprintf(x, sizeof x,
                     "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                     "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                     "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault>"
                     "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
                     "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>%d</errorCode>"
                     "<errorDescription>%s</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>",
                     code, desc);
    httpd_resp_set_status(r, "500 Internal Server Error");
    httpd_resp_set_type(r, "text/xml; charset=\"utf-8\"");
    return httpd_resp_send(r, x, n);
}

// Polling requests are only logged the first time, to avoid flooding the log
static bool log_once(const char *act)
{
    static const char *quiet[] = {"GetPositionInfo", "GetTransportInfo", "GetVolume", "GetMute", "GetMediaInfo"};
    static bool seen[5];
    for (int i = 0; i < 5; i++)
        if (!strcmp(act, quiet[i])) { bool first = !seen[i]; seen[i] = true; return first; }
    return true;
}

static esp_err_t handle_avt(httpd_req_t *r, const char *act, const char *body)
{
    char x[320], el[16], du[16];
    if (!strcmp(act, "SetAVTransportURI")) {
        char *uri = xml_get(body, "CurrentURI");
        char *meta = xml_get(body, "CurrentURIMetaData");
        player_set_uri(uri, meta);
        xSemaphoreTake(g_uri_mx, portMAX_DELAY);
        free(g_uri_esc);
        free(g_meta_esc);
        g_uri_esc = xml_escape(uri);
        g_meta_esc = xml_escape(meta);
        xSemaphoreGive(g_uri_mx);
        free(uri);
        free(meta);
        return soap_reply(r, SVC_AVT, act, NULL);
    }
    if (!strcmp(act, "SetNextAVTransportURI")) return soap_reply(r, SVC_AVT, act, NULL);  // no gapless playback, ignore
    if (!strcmp(act, "Play")) { player_play(); return soap_reply(r, SVC_AVT, act, NULL); }
    if (!strcmp(act, "Pause")) {
        // Pause received while not playing: the controller thinks we're still playing (e.g. it missed an event); re-push the actual state so it corrects itself
        if (strcmp(player_state(), "PLAYING")) gena_resync();
        player_pause();
        return soap_reply(r, SVC_AVT, act, NULL);
    }
    if (!strcmp(act, "Stop")) { player_stop(); return soap_reply(r, SVC_AVT, act, NULL); }
    if (!strcmp(act, "Seek")) {
        char *unit = xml_get(body, "Unit"), *t = xml_get(body, "Target");
        if (unit && (!strcmp(unit, "REL_TIME") || !strcmp(unit, "ABS_TIME"))) player_seek(parse_hms(t));
        ESP_LOGI(TAG, "Seek %s %s", unit ? unit : "?", t ? t : "?");
        free(unit);
        free(t);
        return soap_reply(r, SVC_AVT, act, NULL);
    }
    if (!strcmp(act, "Next") || !strcmp(act, "Previous")) return soap_reply(r, SVC_AVT, act, NULL);
    if (!strcmp(act, "GetTransportInfo")) {
        static const char *last;
        static int polls;
        polls++;
        if (last != player_state()) {
            ESP_LOGI(TAG, "GetTransportInfo returns %s (polled %d times so far)", player_state(), polls);
            last = player_state();
        }
        snprintf(x, sizeof x,
                 "<CurrentTransportState>%s</CurrentTransportState>"
                 "<CurrentTransportStatus>%s</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>",
                 player_state(), player_has_error() ? "ERROR_OCCURRED" : "OK");
        return soap_reply(r, SVC_AVT, act, x);
    }
    if (!strcmp(act, "GetPositionInfo")) {
        fmt_hms(el, sizeof el, (int)player_position());
        fmt_hms(du, sizeof du, (int)player_duration());
        char *o = NULL;
        asprintf(&o,
                 "<Track>1</Track><TrackDuration>%s</TrackDuration><TrackMetaData>%s</TrackMetaData>"
                 "<TrackURI>%s</TrackURI><RelTime>%s</RelTime><AbsTime>%s</AbsTime>"
                 "<RelCount>2147483647</RelCount><AbsCount>2147483647</AbsCount>",
                 du, g_meta_esc ? g_meta_esc : "", g_uri_esc ? g_uri_esc : "", el, el);
        esp_err_t e = soap_reply(r, SVC_AVT, act, o);
        free(o);
        return e;
    }
    if (!strcmp(act, "GetMediaInfo")) {
        fmt_hms(du, sizeof du, (int)player_duration());
        char *o = NULL;
        asprintf(&o,
                 "<NrTracks>%d</NrTracks><MediaDuration>%s</MediaDuration><CurrentURI>%s</CurrentURI>"
                 "<CurrentURIMetaData>%s</CurrentURIMetaData><NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
                 "<PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                 "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
                 g_uri_esc ? 1 : 0, du, g_uri_esc ? g_uri_esc : "", g_meta_esc ? g_meta_esc : "");
        esp_err_t e = soap_reply(r, SVC_AVT, act, o);
        free(o);
        return e;
    }
    if (!strcmp(act, "GetDeviceCapabilities"))
        return soap_reply(r, SVC_AVT, act,
                          "<PlayMedia>NETWORK</PlayMedia><RecMedia>NOT_IMPLEMENTED</RecMedia>"
                          "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
    if (!strcmp(act, "GetTransportSettings"))
        return soap_reply(r, SVC_AVT, act, "<PlayMode>NORMAL</PlayMode><RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>");
    if (!strcmp(act, "GetCurrentTransportActions"))
        return soap_reply(r, SVC_AVT, act, "<Actions>Play,Stop,Pause,Seek</Actions>");
    return soap_fault(r, 401, "Invalid Action");
}

// Volume from the web page: same variable as NetEase's SetVolume; state changes are synced to NetEase via events
void dlna_set_volume(int volume, bool mute)
{
    g_volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    g_mute = mute;
    output_set_volume(g_volume, g_mute);
}

static esp_err_t handle_rc(httpd_req_t *r, const char *act, const char *body)
{
    char x[96];
    if (!strcmp(act, "GetVolume")) {
        snprintf(x, sizeof x, "<CurrentVolume>%d</CurrentVolume>", g_volume);
        return soap_reply(r, SVC_RC, act, x);
    }
    if (!strcmp(act, "SetVolume")) {
        char *v = xml_get(body, "DesiredVolume");
        if (v) g_volume = atoi(v);
        free(v);
        output_set_volume(g_volume, g_mute);
        ESP_LOGI(TAG, "SetVolume %d", g_volume);
        return soap_reply(r, SVC_RC, act, NULL);
    }
    if (!strcmp(act, "GetMute")) {
        snprintf(x, sizeof x, "<CurrentMute>%d</CurrentMute>", g_mute);
        return soap_reply(r, SVC_RC, act, x);
    }
    if (!strcmp(act, "SetMute")) {
        char *v = xml_get(body, "DesiredMute");
        g_mute = v && (v[0] == '1' || v[0] == 't');
        free(v);
        output_set_volume(g_volume, g_mute);
        return soap_reply(r, SVC_RC, act, NULL);
    }
    return soap_fault(r, 401, "Invalid Action");
}

static esp_err_t handle_cm(httpd_req_t *r, const char *act, const char *body)
{
    if (!strcmp(act, "GetProtocolInfo"))
        return soap_reply(r, SVC_CM, act, "<Source></Source><Sink>" SINK_PROTOCOLS "</Sink>");
    if (!strcmp(act, "GetCurrentConnectionIDs"))
        return soap_reply(r, SVC_CM, act, "<ConnectionIDs>0</ConnectionIDs>");
    if (!strcmp(act, "GetCurrentConnectionInfo"))
        return soap_reply(r, SVC_CM, act,
                          "<RcsID>0</RcsID><AVTransportID>0</AVTransportID><ProtocolInfo></ProtocolInfo>"
                          "<PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
                          "<Direction>Input</Direction><Status>OK</Status>");
    return soap_fault(r, 401, "Invalid Action");
}

static esp_err_t control_handler(httpd_req_t *r)
{
    char sa[200] = {0}, act[64] = {0};
    httpd_req_get_hdr_value_str(r, "SOAPACTION", sa, sizeof sa);
    const char *h = strrchr(sa, '#');
    if (h) {
        int i = 0;
        for (h++; *h && *h != '"' && i < (int)sizeof act - 1; h++) act[i++] = *h;
    }
    int len = r->content_len;
    if (len <= 0 || len > 64 * 1024) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad length");
    char *body = malloc(len + 1);
    if (!body) return httpd_resp_send_500(r);
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(r, body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { free(body); return ESP_FAIL; }
        got += n;
    }
    body[len] = 0;

    char from[16] = "?";
    struct sockaddr_in6 peer;
    socklen_t pl = sizeof peer;
    if (getpeername(httpd_req_to_sockfd(r), (struct sockaddr *)&peer, &pl) == 0) {
        struct sockaddr_in *p4 = (struct sockaddr_in *)&peer;
        if (p4->sin_family == AF_INET) inet_ntoa_r(p4->sin_addr, from, sizeof from);
        else inet_ntoa_r(*(struct in_addr *)&peer.sin6_addr.un.u32_addr[3], from, sizeof from);
    }
    const char *svc = r->user_ctx;
    if (log_once(act)) ESP_LOGI(TAG, "SOAP %s #%s (from %s)", svc + strlen("urn:schemas-upnp-org:service:"), act, from);

    esp_err_t e;
    if (!strcmp(svc, SVC_AVT)) e = handle_avt(r, act, body);
    else if (!strcmp(svc, SVC_RC)) e = handle_rc(r, act, body);
    else e = handle_cm(r, act, body);
    free(body);
    return e;
}

// ---------- Device description / service descriptions ----------

typedef struct { const char *name, *args; } action_t;
// args format: "+argName=stateVar;-argName=stateVar", + is input, - is output
// vars format: "name:type,...", a leading ! on the name means sendEvents=yes

#define IID "+InstanceID=A_ARG_TYPE_InstanceID"
static const action_t AVT_ACTS[] = {
    {"SetAVTransportURI", IID ";+CurrentURI=AVTransportURI;+CurrentURIMetaData=AVTransportURIMetaData"},
    {"SetNextAVTransportURI", IID ";+NextURI=NextAVTransportURI;+NextURIMetaData=NextAVTransportURIMetaData"},
    {"GetMediaInfo", IID ";-NrTracks=NumberOfTracks;-MediaDuration=CurrentMediaDuration;-CurrentURI=AVTransportURI;"
                     "-CurrentURIMetaData=AVTransportURIMetaData;-NextURI=NextAVTransportURI;"
                     "-NextURIMetaData=NextAVTransportURIMetaData;-PlayMedium=PlaybackStorageMedium;"
                     "-RecordMedium=RecordStorageMedium;-WriteStatus=RecordMediumWriteStatus"},
    {"GetTransportInfo", IID ";-CurrentTransportState=TransportState;-CurrentTransportStatus=TransportStatus;"
                         "-CurrentSpeed=TransportPlaySpeed"},
    {"GetPositionInfo", IID ";-Track=CurrentTrack;-TrackDuration=CurrentTrackDuration;-TrackMetaData=CurrentTrackMetaData;"
                        "-TrackURI=CurrentTrackURI;-RelTime=RelativeTimePosition;-AbsTime=AbsoluteTimePosition;"
                        "-RelCount=RelativeCounterPosition;-AbsCount=AbsoluteCounterPosition"},
    {"GetDeviceCapabilities", IID ";-PlayMedia=PossiblePlaybackStorageMedia;-RecMedia=PossibleRecordStorageMedia;"
                              "-RecQualityModes=PossibleRecordQualityModes"},
    {"GetTransportSettings", IID ";-PlayMode=CurrentPlayMode;-RecQualityMode=CurrentRecordQualityMode"},
    {"GetCurrentTransportActions", IID ";-Actions=CurrentTransportActions"},
    {"Stop", IID},
    {"Play", IID ";+Speed=TransportPlaySpeed"},
    {"Pause", IID},
    {"Seek", IID ";+Unit=A_ARG_TYPE_SeekMode;+Target=A_ARG_TYPE_SeekTarget"},
    {"Next", IID},
    {"Previous", IID},
};
static const char AVT_VARS[] =
    "!LastChange:string,A_ARG_TYPE_InstanceID:ui4,AVTransportURI:string,AVTransportURIMetaData:string,"
    "NextAVTransportURI:string,NextAVTransportURIMetaData:string,NumberOfTracks:ui4,CurrentMediaDuration:string,"
    "PlaybackStorageMedium:string,RecordStorageMedium:string,RecordMediumWriteStatus:string,TransportState:string,"
    "TransportStatus:string,TransportPlaySpeed:string,CurrentTrack:ui4,CurrentTrackDuration:string,"
    "CurrentTrackMetaData:string,CurrentTrackURI:string,RelativeTimePosition:string,AbsoluteTimePosition:string,"
    "RelativeCounterPosition:i4,AbsoluteCounterPosition:i4,PossiblePlaybackStorageMedia:string,"
    "PossibleRecordStorageMedia:string,PossibleRecordQualityModes:string,CurrentPlayMode:string,"
    "CurrentRecordQualityMode:string,A_ARG_TYPE_SeekMode:string,A_ARG_TYPE_SeekTarget:string,"
    "CurrentTransportActions:string";

static const action_t RC_ACTS[] = {
    {"GetVolume", IID ";+Channel=A_ARG_TYPE_Channel;-CurrentVolume=Volume"},
    {"SetVolume", IID ";+Channel=A_ARG_TYPE_Channel;+DesiredVolume=Volume"},
    {"GetMute", IID ";+Channel=A_ARG_TYPE_Channel;-CurrentMute=Mute"},
    {"SetMute", IID ";+Channel=A_ARG_TYPE_Channel;+DesiredMute=Mute"},
};
static const char RC_VARS[] = "!LastChange:string,A_ARG_TYPE_InstanceID:ui4,A_ARG_TYPE_Channel:string,Volume:ui2,Mute:boolean";

static const action_t CM_ACTS[] = {
    {"GetProtocolInfo", "-Source=SourceProtocolInfo;-Sink=SinkProtocolInfo"},
    {"GetCurrentConnectionIDs", "-ConnectionIDs=CurrentConnectionIDs"},
    {"GetCurrentConnectionInfo", "+ConnectionID=A_ARG_TYPE_ConnectionID;-RcsID=A_ARG_TYPE_RcsID;"
                                 "-AVTransportID=A_ARG_TYPE_AVTransportID;-ProtocolInfo=A_ARG_TYPE_ProtocolInfo;"
                                 "-PeerConnectionManager=A_ARG_TYPE_ConnectionManager;"
                                 "-PeerConnectionID=A_ARG_TYPE_ConnectionID;-Direction=A_ARG_TYPE_Direction;"
                                 "-Status=A_ARG_TYPE_ConnectionStatus"},
};
static const char CM_VARS[] =
    "!SourceProtocolInfo:string,!SinkProtocolInfo:string,!CurrentConnectionIDs:string,A_ARG_TYPE_ConnectionID:i4,"
    "A_ARG_TYPE_RcsID:i4,A_ARG_TYPE_AVTransportID:i4,A_ARG_TYPE_ProtocolInfo:string,"
    "A_ARG_TYPE_ConnectionManager:string,A_ARG_TYPE_Direction:string,A_ARG_TYPE_ConnectionStatus:string";

typedef struct { const action_t *acts; int n; const char *vars; } scpd_t;
static const scpd_t SCPD_AVT = {AVT_ACTS, sizeof AVT_ACTS / sizeof AVT_ACTS[0], AVT_VARS};
static const scpd_t SCPD_RC = {RC_ACTS, sizeof RC_ACTS / sizeof RC_ACTS[0], RC_VARS};
static const scpd_t SCPD_CM = {CM_ACTS, sizeof CM_ACTS / sizeof CM_ACTS[0], CM_VARS};

#define APPEND(...) do { o += snprintf(b + o, o < (int)n ? n - o : 0, __VA_ARGS__); } while (0)

static int scpd_build(char *b, size_t n, const scpd_t *s)
{
    int o = 0;
    APPEND("<?xml version=\"1.0\" encoding=\"utf-8\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
           "<specVersion><major>1</major><minor>0</minor></specVersion><actionList>");
    for (int i = 0; i < s->n; i++) {
        APPEND("<action><name>%s</name><argumentList>", s->acts[i].name);
        for (const char *p = s->acts[i].args; *p;) {
            char dir = *p++;
            const char *eq = strchr(p, '='), *end = strchr(p, ';');
            if (!end) end = p + strlen(p);
            APPEND("<argument><name>%.*s</name><direction>%s</direction>"
                   "<relatedStateVariable>%.*s</relatedStateVariable></argument>",
                   (int)(eq - p), p, dir == '+' ? "in" : "out", (int)(end - eq - 1), eq + 1);
            p = *end ? end + 1 : end;
        }
        APPEND("</argumentList></action>");
    }
    APPEND("</actionList><serviceStateTable>");
    for (const char *p = s->vars; *p;) {
        bool ev = *p == '!';
        if (ev) p++;
        const char *c = strchr(p, ':'), *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        APPEND("<stateVariable sendEvents=\"%s\"><name>%.*s</name><dataType>%.*s</dataType></stateVariable>",
               ev ? "yes" : "no", (int)(c - p), p, (int)(end - c - 1), c + 1);
        p = *end ? end + 1 : end;
    }
    APPEND("</serviceStateTable></scpd>");
    return o;
}

static esp_err_t scpd_handler(httpd_req_t *r)
{
    const size_t cap = 12 * 1024;
    char *b = malloc(cap);
    if (!b) return httpd_resp_send_500(r);
    int n = scpd_build(b, cap, r->user_ctx);
    if (n >= (int)cap) { free(b); ESP_LOGE(TAG, "SCPD buffer too small"); return httpd_resp_send_500(r); }
    httpd_resp_set_type(r, "text/xml; charset=\"utf-8\"");
    esp_err_t e = httpd_resp_send(r, b, n);
    free(b);
    return e;
}

static esp_err_t desc_handler(httpd_req_t *r)
{
    char *x = NULL;
#define SVC_XML(type, id, name)                                                                        \
    "<service><serviceType>" type "</serviceType><serviceId>urn:upnp-org:serviceId:" id "</serviceId>" \
    "<SCPDURL>/" name ".xml</SCPDURL><controlURL>/" name "/control</controlURL>"                       \
    "<eventSubURL>/" name "/event</eventSubURL></service>"
    int n = asprintf(&x,
                     "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                     "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
                     "<specVersion><major>1</major><minor>0</minor></specVersion><device>"
                     "<deviceType>" DEV_MR "</deviceType><friendlyName>%s</friendlyName>"
                     "<manufacturer>DIY</manufacturer><modelName>ESP32 Player</modelName>"
                     "<modelNumber>1</modelNumber><modelDescription>DLNA audio renderer</modelDescription>"
                     "<UDN>%s</UDN><dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC><serviceList>"
                     SVC_XML(SVC_AVT, "AVTransport", "AVTransport")
                     SVC_XML(SVC_RC, "RenderingControl", "RenderingControl")
                     SVC_XML(SVC_CM, "ConnectionManager", "ConnectionManager")
                     "</serviceList></device></root>",
                     g_name, g_uuid);
    if (n < 0) return httpd_resp_send_500(r);
    struct sockaddr_in6 pa;  // lwip's httpd uses IPv6 sockets, with the IPv4 address mapped at the end
    socklen_t pl = sizeof pa;
    char peer[16] = "?";
    if (getpeername(httpd_req_to_sockfd(r), (struct sockaddr *)&pa, &pl) == 0)
        inet_ntoa_r(((struct in_addr *)&pa.sin6_addr.un.u32_addr[3])[0], peer, sizeof peer);
    ESP_LOGI(TAG, "%s fetched description.xml", peer);
    httpd_resp_set_type(r, "text/xml; charset=\"utf-8\"");
    esp_err_t e = httpd_resp_send(r, x, n);
    free(x);
    return e;
}

// Event content (for GENA notifications): AVTransport / RenderingControl use LastChange, ConnectionManager lists state variables directly
static char *event_body(int svc)
{
    char *inner = NULL, *out = NULL;
    if (svc == GENA_AVT) {
        char du[16];
        fmt_hms(du, sizeof du, (int)player_duration());
        // Include the track URL and metadata: lets the controller match what the board is playing (also after a reboot restore);
        // on a playback error TransportStatus reports ERROR_OCCURRED, so the controller knows it wasn't a normal stop
        xSemaphoreTake(g_uri_mx, portMAX_DELAY);
        const char *u = g_uri_esc ? g_uri_esc : "", *m = g_meta_esc ? g_meta_esc : "";
        asprintf(&inner,
                 "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"><InstanceID val=\"0\">"
                 "<TransportState val=\"%s\"/><TransportStatus val=\"%s\"/><TransportPlaySpeed val=\"1\"/>"
                 "<NumberOfTracks val=\"%d\"/><CurrentTrack val=\"%d\"/>"
                 "<CurrentTrackDuration val=\"%s\"/><CurrentMediaDuration val=\"%s\"/>"
                 "<AVTransportURI val=\"%s\"/><AVTransportURIMetaData val=\"%s\"/>"
                 "<CurrentTrackURI val=\"%s\"/><CurrentTrackMetaData val=\"%s\"/>"
                 "<CurrentTransportActions val=\"Play,Stop,Pause,Seek\"/></InstanceID></Event>",
                 player_state(), player_has_error() ? "ERROR_OCCURRED" : "OK", g_uri_esc ? 1 : 0, g_uri_esc ? 1 : 0,
                 du, du, u, m, u, m);
        xSemaphoreGive(g_uri_mx);
    } else if (svc == GENA_RC) {
        asprintf(&inner,
                 "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\"><InstanceID val=\"0\">"
                 "<Volume channel=\"Master\" val=\"%d\"/><Mute channel=\"Master\" val=\"%d\"/></InstanceID></Event>",
                 g_volume, g_mute);
    } else {
        asprintf(&out,
                 "<?xml version=\"1.0\" encoding=\"utf-8\"?><e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
                 "<e:property><SourceProtocolInfo></SourceProtocolInfo></e:property>"
                 "<e:property><SinkProtocolInfo>" SINK_PROTOCOLS "</SinkProtocolInfo></e:property>"
                 "<e:property><CurrentConnectionIDs>0</CurrentConnectionIDs></e:property></e:propertyset>");
        return out;
    }
    char *esc = xml_escape(inner);
    free(inner);
    asprintf(&out,
             "<?xml version=\"1.0\" encoding=\"utf-8\"?><e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
             "<e:property><LastChange>%s</LastChange></e:property></e:propertyset>",
             esc);
    free(esc);
    return out;
}

// ---------- SSDP (lets phones discover this device) ----------

static bool hdr_val(const char *msg, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    for (const char *p = msg; (p = strstr(p, "\r\n"));) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ') p++;
            const char *e = strstr(p, "\r\n");
            if (!e) e = p + strlen(p);
            size_t l = (size_t)(e - p) < n - 1 ? (size_t)(e - p) : n - 1;
            memcpy(out, p, l);
            out[l] = 0;
            return true;
        }
    }
    return false;
}

static void ssdp_send(int s, const struct sockaddr_in *to, const char *st, bool notify)
{
    char usn[160], b[600];
    if (!strncmp(st, "uuid:", 5)) snprintf(usn, sizeof usn, "%s", g_uuid);
    else snprintf(usn, sizeof usn, "%s::%s", g_uuid, st);
    int n;
    if (notify)
        n = snprintf(b, sizeof b,
                     "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=1800\r\n"
                     "LOCATION: http://%s:%d/description.xml\r\nNT: %s\r\nNTS: ssdp:alive\r\n"
                     "SERVER: " SERVER_STR "\r\nUSN: %s\r\n\r\n",
                     g_ip, g_port, st, usn);
    else
        n = snprintf(b, sizeof b,
                     "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                     "LOCATION: http://%s:%d/description.xml\r\nSERVER: " SERVER_STR "\r\n"
                     "ST: %s\r\nUSN: %s\r\n\r\n",
                     g_ip, g_port, st, usn);
    sendto(s, b, n, 0, (const struct sockaddr *)to, sizeof *to);
}

// Answer each search 3 times (immediately, after 1 s, after 2 s): UDP drops packets, especially WiFi multicast; if the phone misses one it can't find us
#define SSDP_REPEAT 3
#define SSDP_PENDING 8
typedef struct {
    struct sockaddr_in to;
    char st[160];
    int left;
    int64_t due_us;
} ssdp_reply_t;

static void ssdp_reply(int s, const struct sockaddr_in *to, const char *st, const char *const *ST, int NST)
{
    bool all = !strcmp(st, "ssdp:all");
    for (int i = 0; i < NST; i++)
        if (all || !strcasecmp(st, ST[i])) ssdp_send(s, to, ST[i], false);
}

static void ssdp_task(void *arg)
{
    const char *ST[] = {"upnp:rootdevice", g_uuid, DEV_MR, SVC_AVT, SVC_RC, SVC_CM};
    const int NST = 6;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(1900), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { ESP_LOGE(TAG, "SSDP bind failed"); vTaskDelete(NULL); }
    struct ip_mreq m = {.imr_multiaddr.s_addr = inet_addr("239.255.255.250"), .imr_interface.s_addr = inet_addr(g_ip)};
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) < 0) ESP_LOGE(TAG, "Failed to join multicast group");
    uint8_t ttl = 4;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    struct timeval tv = {.tv_usec = 200 * 1000};  // repeat responses must go out on time
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in mc = {.sin_family = AF_INET, .sin_port = htons(1900), .sin_addr.s_addr = inet_addr("239.255.255.250")};
    ESP_LOGI(TAG, "SSDP started, waiting for phones to search");

    int64_t last_notify = 0;
    int boot_notify = SSDP_REPEAT;  // send 3 rounds of announcements at boot, one per second
    static ssdp_reply_t pend[SSDP_PENDING];
    struct { uint32_t ip; int64_t us; } seen[8] = {0};  // rate-limit logs per source: once per 30 s for the same source
    char buf[1024], st[160], from[16];
    while (1) {
        int64_t now = esp_timer_get_time();
        if (now - last_notify > (boot_notify > 0 ? 1000000LL : 30 * 1000000LL)) {
            for (int k = 0; k < 3; k++) {  // send each one 3 times, 100 ms apart (as upmpdcli does), against multicast loss
                if (k) vTaskDelay(pdMS_TO_TICKS(100));
                for (int i = 0; i < NST; i++) ssdp_send(s, &mc, ST[i], true);
            }
            last_notify = now;
            if (boot_notify > 0) boot_notify--;
        }
        for (int i = 0; i < SSDP_PENDING; i++) {
            if (pend[i].left > 0 && now >= pend[i].due_us) {
                ssdp_reply(s, &pend[i].to, pend[i].st, ST, NST);
                pend[i].left--;
                pend[i].due_us = now + 1000000LL;
            }
        }
        struct sockaddr_in src;
        socklen_t sl = sizeof src;
        int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&src, &sl);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strncmp(buf, "M-SEARCH", 8) || !hdr_val(buf, "ST", st, sizeof st)) continue;
        bool hit = !strcmp(st, "ssdp:all");
        for (int i = 0; i < NST && !hit; i++) hit = !strcasecmp(st, ST[i]);
        if (!hit) continue;
        ssdp_reply(s, &src, st, ST, NST);
        // Send 2 more repeats; if the same source and ST is already queued, just reset the count (so it doesn't pile up when the phone searches every second)
        ssdp_reply_t *q = NULL;
        for (int i = 0; i < SSDP_PENDING && !q; i++)
            if (pend[i].left > 0 && pend[i].to.sin_addr.s_addr == src.sin_addr.s_addr &&
                pend[i].to.sin_port == src.sin_port && !strcmp(pend[i].st, st))
                q = &pend[i];
        for (int i = 0; i < SSDP_PENDING && !q; i++)
            if (pend[i].left <= 0) q = &pend[i];
        if (q) {
            q->to = src;
            strlcpy(q->st, st, sizeof q->st);
            q->left = SSDP_REPEAT - 1;
            q->due_us = esp_timer_get_time() + 1000000LL;
        }
        int k = 0;
        for (int i = 0; i < 8; i++) {
            if (seen[i].ip == src.sin_addr.s_addr) { k = i; break; }
            if (seen[i].us < seen[k].us) k = i;
        }
        if (seen[k].ip != src.sin_addr.s_addr || now - seen[k].us > 30 * 1000000LL) {
            seen[k].ip = src.sin_addr.s_addr;
            seen[k].us = now;
            inet_ntoa_r(src.sin_addr, from, sizeof from);
            ESP_LOGI(TAG, "M-SEARCH from %s, ST=%s, answered (%d more repeats)", from, st, SSDP_REPEAT - 1);
        }
    }
}

// ---------- Keep the track across reboots ----------
// Before a normal reboot (OTA, web restart) save the track and position to NVS and restore as PAUSED_PLAYBACK at boot
// (Macast is in this state right after SetAVTransportURI; NetEase ignores STOPPED after boot but accepts PAUSED):
// on "play" the controller only sends Play after the event, it won't push the URL again, so without the track nothing plays.
// Written only once at reboot; abnormal reboots (crash, power loss) aren't saved and boot into idle.
static void transport_save(void)
{
    if (esp_ptr_external_ram(__builtin_frame_address(0))) return;  // tasks with stacks in PSRAM can't write flash
    nvs_handle_t h;
    if (nvs_open("dlna", NVS_READWRITE, &h) != ESP_OK) return;
    if (g_uri_esc && *g_uri_esc) {
        nvs_set_blob(h, "uri", g_uri_esc, strlen(g_uri_esc) + 1);
        nvs_set_blob(h, "meta", g_meta_esc ? g_meta_esc : "", g_meta_esc ? strlen(g_meta_esc) + 1 : 1);
        nvs_set_u32(h, "pos_ms", (uint32_t)(player_position() * 1000));
    }
    nvs_commit(h);
    nvs_close(h);
}

static char *nvs_get_str_blob(nvs_handle_t h, const char *key)
{
    size_t n = 0;
    if (nvs_get_blob(h, key, NULL, &n) != ESP_OK || !n) return NULL;
    char *v = malloc(n);
    if (v && nvs_get_blob(h, key, v, &n) != ESP_OK) { free(v); return NULL; }
    if (v) v[n - 1] = 0;
    return v;
}

static void transport_restore(void)
{
    nvs_handle_t h;
    if (nvs_open("dlna", NVS_READWRITE, &h) != ESP_OK) return;
    char *uri = nvs_get_str_blob(h, "uri"), *meta = nvs_get_str_blob(h, "meta");
    uint32_t pos_ms = 0;
    nvs_get_u32(h, "pos_ms", &pos_ms);
    nvs_erase_key(h, "uri");  // restore only once: URLs expire, a later abnormal reboot must not bring back a stale one
    nvs_erase_key(h, "meta");
    nvs_erase_key(h, "pos_ms");
    nvs_commit(h);
    nvs_close(h);
    if (uri) {
        g_uri_esc = uri;
        g_meta_esc = meta ? meta : strdup("");
        char *u = strdup(uri), *m = strdup(g_meta_esc);
        xml_unescape(u);
        xml_unescape(m);
        player_set_uri(u, m);
        player_restore_paused(pos_ms / 1000.0);
        free(u);
        free(m);
        ESP_LOGI(TAG, "Restoring track from before reboot, position %.1f s", pos_ms / 1000.0);
    } else {
        free(meta);
    }
}

static void on_shutdown(void)
{
    // Pause first and push PAUSED to the controller: controllers ignore STOPPED after boot (tested with NetEase),
    // so tell it we're paused before the reboot; after the reboot pressing play on the controller sends Play and resumes from the same position
    if (!strcmp(player_state(), "PLAYING")) {
        player_pause();
        gena_notify_now();
    }
    transport_save();
    gena_save();
}

void dlna_start(httpd_handle_t h, const char *ip, int port)
{
    strlcpy(g_ip, ip, sizeof g_ip);
    g_port = port;
    settings_get_device_name(g_name, sizeof g_name);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_uuid, sizeof g_uuid, "uuid:5c3a1d2e-8b7f-4e10-9a6b-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    static const struct { const char *name, *svc; const scpd_t *scpd; } S[] = {
        {"AVTransport", SVC_AVT, &SCPD_AVT},
        {"RenderingControl", SVC_RC, &SCPD_RC},
        {"ConnectionManager", SVC_CM, &SCPD_CM},
    };
    static char uris[3][3][48];
    httpd_uri_t d = {.uri = "/description.xml", .method = HTTP_GET, .handler = desc_handler};
    httpd_register_uri_handler(h, &d);
    for (int i = 0; i < 3; i++) {
        snprintf(uris[i][0], 48, "/%s.xml", S[i].name);
        snprintf(uris[i][1], 48, "/%s/control", S[i].name);
        snprintf(uris[i][2], 48, "/%s/event", S[i].name);
        httpd_uri_t u1 = {.uri = uris[i][0], .method = HTTP_GET, .handler = scpd_handler, .user_ctx = (void *)S[i].scpd};
        httpd_uri_t u2 = {.uri = uris[i][1], .method = HTTP_POST, .handler = control_handler, .user_ctx = (void *)S[i].svc};
        httpd_uri_t u3 = {.uri = uris[i][2], .method = HTTP_SUBSCRIBE, .handler = gena_handler, .user_ctx = (void *)(intptr_t)i};
        httpd_uri_t u4 = {.uri = uris[i][2], .method = HTTP_UNSUBSCRIBE, .handler = gena_handler, .user_ctx = (void *)(intptr_t)i};
        httpd_register_uri_handler(h, &u1);
        httpd_register_uri_handler(h, &u2);
        httpd_register_uri_handler(h, &u3);
        httpd_register_uri_handler(h, &u4);
    }
    xTaskCreatePinnedToCoreWithCaps(ssdp_task, "ssdp", 4096, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM);  // stack in PSRAM, saves internal RAM
    g_uri_mx = xSemaphoreCreateMutex();
    transport_restore();  // restore the track first, so what GENA pushes after restoring subscriptions is PAUSED_PLAYBACK
    gena_start(event_body);
    esp_register_shutdown_handler(on_shutdown);
    ESP_LOGI(TAG, "DLNA started: \"%s\" http://%s:%d/description.xml", g_name, g_ip, g_port);
}
