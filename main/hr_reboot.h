/*
 * The one way to restart the adapter on purpose.
 *
 * Every deliberate restart - after an OTA, an OTA rollback, anything added
 * later - goes through hr_reboot_request(), so they all get the same
 * shutdown order: tell Wi-Fi a restart is coming, quiesce and unmount the
 * capture filesystem, then esp_restart(). Calling esp_restart() directly
 * from module code is what produced a panic on the reboot path (see
 * hr_capture_shutdown()); do not add new callers of it.
 */
#ifndef HR_REBOOT_H
#define HR_REBOOT_H

#include <stdint.h>

/*
 * Restart after `delay_ms` (so an HTTP reply can reach the browser first).
 * Returns at once; the work happens on a dedicated task with enough stack
 * for the filesystem teardown and esp_restart()'s own shutdown handlers.
 * A second request while one is pending is ignored. `why` is for the log.
 *
 * Never blocks the restart indefinitely: every wait inside is bounded, and
 * if the helper task cannot be created the sequence runs on the caller.
 */
void hr_reboot_request(const char *why, uint32_t delay_ms);

#endif /* HR_REBOOT_H */
