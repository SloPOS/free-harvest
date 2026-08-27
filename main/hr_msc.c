#include "hr_msc.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#if CONFIG_TINYUSB_MSC_ENABLED
#include "tusb_msc_storage.h"
#endif
#include "wear_levelling.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "hr_msc";

#define PART_LABEL "msc"
#define BASE_PATH  "/msc"

static bool s_ready;

/*
 * The file the genuine adapter carries, captured verbatim from its volume:
 *
 *     HR_aabbccddeeff,v1.1.2,1,0/0
 *
 * Field 1 is the adapter identifier - the SAME string we send in WIFIINFO
 * field 4, so the dryer sees one consistent identity from both interfaces.
 * Field 2 is the adapter's firmware version.
 *
 * We write "v1.1.2" rather than our own version deliberately, and it is the
 * one dishonest byte in this file. The whole point of the volume is to look
 * like an adapter the dryer already accepts; if it gates on a version it
 * recognises, our own string would fail the test and tell us nothing about
 * whether mass storage was the problem. If the experiment shows the dryer
 * never reads this file, put the real version back.
 */
#define ADAPTER_FW "v1.1.2"

static void write_version_file(void)
{
    if (mkdir(BASE_PATH "/esp", 0777) != 0) {
        /* Almost always EEXIST on a volume we have already prepared. */
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    FILE *f = fopen(BASE_PATH "/esp/version.txt", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot write version.txt: %s", strerror(errno));
        return;
    }
    fprintf(f, "HR_%02x%02x%02x%02x%02x%02x,%s,1,0/0",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ADAPTER_FW);
    fclose(f);
    ESP_LOGI(TAG, "version.txt written as HR_%02x%02x%02x%02x%02x%02x,%s,1,0/0",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ADAPTER_FW);
}

#if !CONFIG_TINYUSB_MSC_ENABLED
/*
 * PARKED. See sdkconfig.defaults for why: the volume would gate something that
 * is demonstrably already running on the machine it was written for, and
 * enabling it costs a broken build and a partition OTA cannot deliver. The
 * implementation below is kept intact for when a dryer turns up whose CDC
 * thread really is stuck on MSC init.
 */
bool hr_msc_init(void) { return false; }
bool hr_msc_ready(void) { return false; }
#else

bool hr_msc_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, PART_LABEL);
    if (part == NULL) {
        ESP_LOGE(TAG, "no '%s' partition - volume not offered", PART_LABEL);
        return false;
    }

    /*
     * Pass one: mount through VFS so the filesystem is FORMATTED if it has
     * never been used, and so we can write the file. format_if_mount_failed
     * makes the first boot after a flash self-healing rather than a manual
     * step.
     */
    esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    wl_handle_t wl = WL_INVALID_HANDLE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(BASE_PATH, PART_LABEL,
                                                     &mc, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAT mount failed: %s", esp_err_to_name(err));
        return false;
    }
    write_version_file();
    esp_vfs_fat_spiflash_unmount_rw_wl(BASE_PATH, wl);

    /*
     * Pass two: hand the raw volume to TinyUSB. It stays UNMOUNTED locally -
     * a FAT volume with two writers is a corrupted FAT volume, and the dryer
     * is the one that needs to read it.
     */
    wl = WL_INVALID_HANDLE;
    err = wl_mount(part, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount failed: %s", esp_err_to_name(err));
        return false;
    }
    const tinyusb_msc_spiflash_config_t cfg = { .wl_handle = wl };
    err = tinyusb_msc_storage_init_spiflash(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc storage init failed: %s", esp_err_to_name(err));
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "mass storage ready: %u KB volume, offered to the dryer",
             (unsigned)(part->size / 1024));
    return true;
}

bool hr_msc_ready(void)
{
    return s_ready;
}

#endif /* CONFIG_TINYUSB_MSC_ENABLED */
