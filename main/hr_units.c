#include "hr_units.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdatomic.h>

static const char *TAG = "hr_units";

#define UNITS_NVS_NS  "hrunits"
#define UNITS_NVS_KEY "temp"     /* u8: 0 = auto, 1 = F, 2 = C (hr_temp_pref_t) */

static atomic_int  s_pref = HR_TEMP_PREF_F;
static atomic_bool s_changed;

static hr_temp_pref_t pref_from_u8(uint8_t v)
{
    switch (v) {
    case HR_TEMP_PREF_C:
        return HR_TEMP_PREF_C;
    case HR_TEMP_PREF_AUTO:
        return HR_TEMP_PREF_AUTO;
    case HR_TEMP_PREF_F:
    default:
        return HR_TEMP_PREF_F;
    }
}

void hr_units_init(void)
{
    hr_temp_pref_t pref = HR_TEMP_PREF_F;
    const char *src = "default";
    nvs_handle_t nh;
    if (nvs_open(UNITS_NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nh, UNITS_NVS_KEY, &v) == ESP_OK) {
            pref = pref_from_u8(v);
            src = "nvs";
        }
        nvs_close(nh);
    }
    atomic_store(&s_pref, (int)pref);
    atomic_store(&s_changed, true);
    ESP_LOGI(TAG, "temperature unit: %s -> degrees %s (%s)",
             hr_temp_pref_str(pref), hr_temp_unit_letter(hr_units_temp()),
             src);
}

hr_temp_pref_t hr_units_pref(void)
{
    return (hr_temp_pref_t)atomic_load(&s_pref);
}

hr_temp_unit_t hr_units_temp(void)
{
    /* The dryer has never reported a unit; AUTO is the F default. */
    return hr_temp_resolve(hr_units_pref(), HR_TEMP_DRYER_UNKNOWN, HR_TEMP_F);
}

bool hr_units_metric(void)
{
    return hr_units_temp() == HR_TEMP_C;
}

bool hr_units_set_pref(hr_temp_pref_t pref)
{
    hr_temp_pref_t old = (hr_temp_pref_t)atomic_exchange(&s_pref, (int)pref);
    if (old != pref) {
        atomic_store(&s_changed, true);
    }
    nvs_handle_t nh;
    if (nvs_open(UNITS_NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed; setting not persisted");
        return false;
    }
    bool ok = nvs_set_u8(nh, UNITS_NVS_KEY, (uint8_t)pref) == ESP_OK &&
              nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    ESP_LOGI(TAG, "temperature unit set: %s -> degrees %s%s",
             hr_temp_pref_str(pref), hr_temp_unit_letter(hr_units_temp()),
             ok ? "" : " (NOT persisted)");
    return ok;
}

bool hr_units_take_changed(void)
{
    return atomic_exchange(&s_changed, false);
}
