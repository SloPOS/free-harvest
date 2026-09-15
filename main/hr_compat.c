#include "hr_compat.h"

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <stdatomic.h>

static const char *TAG = "hr_compat";

#define COMPAT_NVS_NS  "hrcompat"
#define COMPAT_NVS_KEY "fw644170"

static atomic_bool s_on;
static atomic_bool s_changed;
static atomic_bool s_explicit;   /* a value was stored, not just defaulted */

void hr_compat_init(void)
{
#ifdef CONFIG_HR_COMPAT_644170
    bool on = true;
#else
    bool on = false;
#endif
    const char *src = "kconfig";
    nvs_handle_t nh;
    if (nvs_open(COMPAT_NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nh, COMPAT_NVS_KEY, &v) == ESP_OK) {
            on = (v != 0);
            src = "nvs";
            atomic_store(&s_explicit, true);
        }
        nvs_close(nh);
    }
    atomic_store(&s_on, on);
    atomic_store(&s_changed, true);
    ESP_LOGI(TAG, "6.0.644170 handshake: %s (%s)", on ? "ON" : "off", src);
}

bool hr_compat_644170(void)
{
    return atomic_load(&s_on);
}

bool hr_compat_set_644170(bool on)
{
    atomic_store(&s_on, on);
    atomic_store(&s_changed, true);
    /* However it got here - the web UI, /api/compat, or auto-detection - this
     * is now a decision, and auto-detection must not override it later. */
    atomic_store(&s_explicit, true);
    nvs_handle_t nh;
    if (nvs_open(COMPAT_NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed; setting not persisted");
        return false;
    }
    bool ok = nvs_set_u8(nh, COMPAT_NVS_KEY, on ? 1 : 0) == ESP_OK &&
              nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    ESP_LOGW(TAG, "6.0.644170 handshake set %s%s", on ? "ON" : "off",
             ok ? "" : " (NOT persisted)");
    return ok;
}

bool hr_compat_take_changed(void)
{
    return atomic_exchange(&s_changed, false);
}

bool hr_compat_explicit(void)
{
    return atomic_load(&s_explicit);
}
