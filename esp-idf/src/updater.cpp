/**
 * updater — app-side staging + trigger.
 *
 * Single main-app + tiny updater-partition update model. This file is the
 * app-side half: it stages-and-triggers, the updater partition does the
 * actual flashing. Behavior:
 *
 *   updaterInit()         post-update cleanup (delete leftover flashme.bin),
 *                         subscribe to updater.cmd.*, register `updater` CLI.
 *   updaterTrigger(force) check the staged /state/flashme.bin, guard against
 *                         overwriting `state` (unless forced), set boot to the
 *                         updater app slot, reboot.
 *
 * NOTE: signature/crypto verification is intentionally absent here. The staged
 * file is trusted: the updater partition flashes whatever is at
 * /state/flashme.bin and never verifies, so this component pulls in no mbedtls.
 */
#include "updater.h"

#include "storage.h"
#include "log.h"
#include "cli.h"
#include "fs.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

namespace {

/* ---- ESP partition table format (at flash/image offset 0x8000) ----
 * 32-byte little-endian entries; table ends at the first entry whose magic
 * is not 0xAA50 (the MD5 sentinel uses magic 0xEBEB and is not a real entry). */
constexpr uint32_t PART_TABLE_OFFSET = 0x8000;
constexpr uint16_t PART_MAGIC        = 0xAA50;
constexpr size_t   PART_ENTRY_SIZE   = 32;
constexpr int      PART_MAX_ENTRIES  = 95;   /* 0x8000..0xC000 is 0x1000 bytes; cap defensively */

struct part_entry_t {
    uint16_t magic;
    uint8_t  type;
    uint8_t  subtype;
    uint32_t offset;
    uint32_t size;
    char     label[16];
    uint32_t flags;
} __attribute__((packed));
static_assert(sizeof(part_entry_t) == PART_ENTRY_SIZE, "partition entry must be 32 bytes");

/* Parse the partition table embedded in flashme.bin and return the highest
 * (offset + size) over the firmware-carrying entries (app + the `fixed` data
 * partitions). The image only carries app + fixed (bootloader/itself skipped,
 * nvs/otadata never present), but we conservatively take the max end over ALL
 * table entries — anything the image declares could land on flash. Returns
 * false if the table can't be read or has no valid first entry. */
bool stagedImageEnd(const char* path, uint32_t* endOut) {
    int f = fs_open(path, "rb");
    if (f < 0) return false;
    if (fs_seek(f, (long)PART_TABLE_OFFSET, SEEK_SET) != 0) {
        fs_close(f);
        return false;
    }
    uint32_t maxEnd = 0;
    bool any = false;
    for (int i = 0; i < PART_MAX_ENTRIES; i++) {
        part_entry_t e;
        if (fs_read(&e, sizeof(e), 1, f) != 1) break;
        if (e.magic != PART_MAGIC) break;   /* end of table (or MD5 sentinel) */
        uint32_t end = e.offset + e.size;
        if (end > maxEnd) maxEnd = end;
        any = true;
    }
    fs_close(f);
    if (!any) return false;
    *endOut = maxEnd;
    return true;
}

/* Find the updater app slot: by label "updater" first, else by subtype ota_1.
 * Returns null if the layout has no updater partition. */
const esp_partition_t* findUpdaterPartition() {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "updater");
    if (p) return p;
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
}

/* The `state` data partition; may be null until the partition-layout change lands. */
const esp_partition_t* findStatePartition() {
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "state");
}

/* Shared by the CLI and the storage subscription. Returns false on any guard
 * failure (with err + updater.error set); on success it reboots and does not
 * return. */
bool updaterTrigger(bool force, std::string& err) {
    storageSet("updater.error", "");

    std::string path = fsStatePath("/flashme.bin");

    struct stat st;
    if (fs_stat(path.c_str(), &st) != 0) {
        err = "no image staged";
        storageSet("updater.error", err);
        return false;
    }

    if (!force) {
        uint32_t imgEnd = 0;
        if (!stagedImageEnd(path.c_str(), &imgEnd)) {
            err = "bad image (no partition table)";
            storageSet("updater.error", err);
            return false;
        }
        const esp_partition_t* state = findStatePartition();
        if (!state) {
            err = "no state partition";
            storageSet("updater.error", err);
            return false;
        }
        if (imgEnd > state->address) {
            err = "would overwrite state";
            storageSet("updater.error", err);
            return false;
        }
    }

    const esp_partition_t* upd = findUpdaterPartition();
    if (!upd) {
        err = "no updater partition";
        storageSet("updater.error", err);
        return false;
    }

    info("updater: booting into '%s' to apply staged image\n", upd->label);
    esp_err_t e = esp_ota_set_boot_partition(upd);
    if (e != ESP_OK) {
        err = std::string("set_boot_partition failed: ") + esp_err_to_name(e);
        storageSet("updater.error", err);
        warn("updater: %s\n", err.c_str());
        return false;
    }

    esp_restart();   /* does not return */
    return true;
}

void cliCmd(const char* args) {
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s apply a staged firmware update (-f: bypass state guard)\n",
                  CLI_HELP_COL, "updater [-f]");
        return;
    }
    bool force = (strstr(args, "-f") != nullptr);
    std::string err;
    if (!updaterTrigger(force, err)) {
        cliPrintf("%s\n", err.c_str());
    }
    /* On success updaterTrigger reboots — the session just drops. */
}

}  /* namespace */

void UpdaterService::onInit() {
    /* Post-update cleanup: we're the main app, so any prior staged image is
     * spent. Delete a leftover /flashme.bin if present. */
    std::string path = fsStatePath("/flashme.bin");
    struct stat st;
    if (fs_stat(path.c_str(), &st) == 0) {
        info("updater: clearing spent /flashme.bin\n");
        fs_remove(path.c_str());
    }

    /* Command keys are bare/ephemeral; edge-trigger on truthy is natural. */
    storageSubscribeChanges("updater.cmd.", ON_CHANGE {
        std::string e;
        if (strcmp(key, "updater.cmd.update") == 0 && atoi(val)) {
            updaterTrigger(false, e);
        } else if (strcmp(key, "updater.cmd.update_force") == 0 && atoi(val)) {
            updaterTrigger(true, e);
        }
    });

    cliRegisterCmd("updater", cliCmd);
}
