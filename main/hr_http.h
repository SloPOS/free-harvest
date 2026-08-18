/*
 * HTTP server: the decoding web UI + WiFi setup, plus a live Server-Sent
 * Events stream of frames.
 *
 * Endpoints
 *   GET  /                 the single-page UI (embedded index.html)
 *   GET  /events           text/event-stream; one event per frame (SSE)
 *   GET  /api/state        JSON: link status, counters, wifi info
 *   GET  /api/history?since=N   JSON array of frames with seq > N
 *   GET  /api/verbs        JSON: per-verb latest body, count, changed mask
 *   GET  /api/capture      text/plain download of all retained frames
 *   GET  /api/scan         start a WiFi scan / return last results
 *   POST /api/wifi         set credentials  {ssid,password}  (form-encoded)
 *   POST /api/forget       clear stored credentials
 *
 * hr_http_notify() is called by the app on every inbound frame so the SSE
 * stream can push it immediately.
 */
#ifndef HR_HTTP_H
#define HR_HTTP_H

#include "hr_history.h"
#include "hr_session.h"
#include "hr_telemetry.h"
#include "hr_trend.h"

/* Free Harvest release version, shown in Settings so users can report it.
 * Keep this in step with the git tag when cutting a release. */
#define FREEHARVEST_VERSION "0.3.5"

/*
 * Provide the mutex that guards the shared history object. MUST be called
 * before hr_http_start(). The app owns this lock (it also writes history from
 * the USB RX task), so both sides serialise on the same mutex.
 */
void hr_http_use_lock(void *mutex);

/*
 * Provide the 30s temperature/pressure series for GET /api/trend. Owned by the
 * app; read under the same lock passed to hr_http_use_lock(). Must be called
 * before hr_http_start().
 */
void hr_http_set_trend(hr_trend_t *tr);

void hr_http_start(hr_session_t *session, hr_history_t *history);

/* Notify the server a new frame (already recorded in history) is available. */
void hr_http_notify(uint32_t seq);

/*
 * Publish the latest decoded telemetry so /api/state can report the cycle
 * phase and live readings. Called from the frame observer.
 */
void hr_http_set_telemetry(const hr_telemetry_t *t);

/*
 * Publish the running/idle tracker so /api/state can distinguish "batch
 * running" from "stale elapsed counter left over from the last batch".
 */
void hr_http_set_tracker(const hr_phase_tracker_t *tr);

#endif /* HR_HTTP_H */
