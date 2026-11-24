// UPnP eventing (GENA): records controller subscriptions and actively sends NOTIFY when state changes.
// NetEase updates its view of the device state from these events (without events, pressing "resume" after a pause makes it send Pause).
#pragma once
#include "esp_http_server.h"

enum { GENA_AVT, GENA_RC, GENA_CM, GENA_NSVC };

// State snapshot provided by dlna: builds the event content (propertyset XML) for a service, returns a malloc'd string
typedef char *(*gena_body_fn)(int svc);

// At startup restore the previous subscriptions from NVS and push the current state right away
void gena_start(gena_body_fn body_fn);
// Call before a normal reboot: save subscriptions (with accurate SEQ) to NVS
void gena_save(void);
// Push the current state to all subscribers immediately in the caller's task (used before reboot: the GENA task wouldn't make it in time)
void gena_notify_now(void);
// Have the GENA task re-push the current state to all subscribers (used when the controller's view doesn't match the actual state)
void gena_resync(void);
// SUBSCRIBE / UNSUBSCRIBE handlers; registered with user_ctx = (void *)service number
esp_err_t gena_handler(httpd_req_t *r);
