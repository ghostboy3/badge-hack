/*
 * Bugdex -- step 4 (dex, met-log, 30-minute rule) per GAME.md section 8.
 *
 * The "storage" partition (partitions.csv, ~1.25MB after the dex/met
 * regions carved out below) is used as two independent append-only logs
 * rather than a filesystem:
 *
 *   [region header (8B)] [record][record]...[record][ 0xFF ... blank ]
 *
 * A record's first byte is a marker: 0xFF means "not written yet" (NOR
 * flash's erased state), RECORD_VALID means "fully committed". Each record
 * is written in two esp_partition_write() calls -- the payload first, the
 * marker byte last -- so a power loss between them leaves the marker at
 * 0xFF and the half-written record is simply treated as blank, never as
 * valid-but-corrupt. On boot, each region is scanned once for its first
 * blank slot to find the append point; already-initialized regions (valid
 * header) are never erased, so existing data survives a reboot.
 *
 * There's no wraparound/reclaim: once a region fills, further writes to it
 * are refused (logged, not fatal). At 64KB per region and roughly 30-byte
 * records, that's on the order of ~2000 entries -- far beyond what a single
 * event will produce, so reclaiming isn't worth the complexity here.
 *
 * No RTC caveat: this hardware has no battery-backed clock, so timestamps
 * are seconds-since-boot, not wall-clock time. storage_recently_caught()
 * only calls a match "within the window" when `now >= stored_timestamp`,
 * which is automatically false for any record left over from a previous
 * boot (a fresh boot's clock starts at 0, so an old session's larger
 * timestamp value can't satisfy that). That's a deliberate, safe default:
 * a reboot resets the anti-farming cooldown rather than risking a stuck
 * "always refused" state from an unrecoverable clock discontinuity.
 */
#include "storage.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "storage";

#define REGION_HEADER_SIZE 8

#define DEX_REGION_OFFSET 0
#define DEX_REGION_SIZE   (64 * 1024)
#define MET_REGION_OFFSET (DEX_REGION_OFFSET + DEX_REGION_SIZE)
#define MET_REGION_SIZE   (64 * 1024)
// Remaining ~1.13MB of the storage partition is reserved for selfies (steps 5-6).

#define DEX_RECORD_VALID 0xA5
#define DEX_RECORD_SIZE  32

typedef struct __attribute__((packed)) {
    uint8_t marker;
    uint8_t species;
    uint32_t person_id;
    char person_name[PERSON_NAME_LEN];
    uint32_t timestamp;
    uint8_t selfie_slot;
    uint8_t flags;
    uint8_t reserved[DEX_RECORD_SIZE - (1 + 1 + 4 + PERSON_NAME_LEN + 4 + 1 + 1)];
} dex_record_t;

#define MET_RECORD_VALID 0xA5
#define MET_RECORD_SIZE  16

typedef struct __attribute__((packed)) {
    uint8_t marker;
    uint32_t person_id;
    uint32_t last_catch_time;
    uint8_t reserved[MET_RECORD_SIZE - (1 + 4 + 4)];
} met_record_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t reserved[4];
} region_header_t;

static const esp_partition_t *s_partition;
static size_t s_dex_next_offset;
static size_t s_met_next_offset;

_Static_assert(sizeof(dex_record_t) == DEX_RECORD_SIZE, "dex_record_t size drifted");
_Static_assert(sizeof(met_record_t) == MET_RECORD_SIZE, "met_record_t size drifted");

/* Ensures the region has a valid header, erasing+reinitializing it if not.
 * Returns true if the region was (re)erased (so the caller knows the append
 * point is right after the header, without needing to scan). */
static bool ensure_region(size_t region_offset, size_t region_size, const char magic[4])
{
    region_header_t hdr;
    esp_partition_read(s_partition, region_offset, &hdr, sizeof(hdr));
    if (memcmp(hdr.magic, magic, 4) == 0) {
        return false; // already initialized -- leave existing data alone
    }

    ESP_LOGW(TAG, "region at 0x%x uninitialized/corrupt, erasing", (unsigned)region_offset);
    ESP_ERROR_CHECK(esp_partition_erase_range(s_partition, region_offset, region_size));
    memcpy(hdr.magic, magic, 4);
    memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));
    ESP_ERROR_CHECK(esp_partition_write(s_partition, region_offset, &hdr, sizeof(hdr)));
    return true;
}

static size_t scan_append_offset(size_t region_offset, size_t region_size, size_t record_size)
{
    size_t offset = region_offset + REGION_HEADER_SIZE;
    size_t end = region_offset + region_size;
    while (offset + record_size <= end) {
        uint8_t marker;
        esp_partition_read(s_partition, offset, &marker, 1);
        if (marker == 0xFF) {
            break;
        }
        offset += record_size;
    }
    return offset;
}

void storage_init(void)
{
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_UNDEFINED,
                                            "storage");
    if (s_partition == NULL) {
        ESP_LOGE(TAG, "storage partition not found -- dex/met-log disabled");
        return;
    }

    bool dex_fresh = ensure_region(DEX_REGION_OFFSET, DEX_REGION_SIZE, "DEX1");
    s_dex_next_offset = dex_fresh ? (DEX_REGION_OFFSET + REGION_HEADER_SIZE)
                                   : scan_append_offset(DEX_REGION_OFFSET, DEX_REGION_SIZE, DEX_RECORD_SIZE);

    bool met_fresh = ensure_region(MET_REGION_OFFSET, MET_REGION_SIZE, "MET1");
    s_met_next_offset = met_fresh ? (MET_REGION_OFFSET + REGION_HEADER_SIZE)
                                    : scan_append_offset(MET_REGION_OFFSET, MET_REGION_SIZE, MET_RECORD_SIZE);

    ESP_LOGI(TAG, "storage init: dex_count=%d met_count=%d", storage_dex_count(),
             (int)((s_met_next_offset - MET_REGION_OFFSET - REGION_HEADER_SIZE) / MET_RECORD_SIZE));
}

int storage_dex_count(void)
{
    if (s_partition == NULL) {
        return 0;
    }
    return (int)((s_dex_next_offset - DEX_REGION_OFFSET - REGION_HEADER_SIZE) / DEX_RECORD_SIZE);
}

bool storage_dex_get(int index, dex_entry_t *out)
{
    if (s_partition == NULL || index < 0 || index >= storage_dex_count()) {
        return false;
    }
    size_t offset = DEX_REGION_OFFSET + REGION_HEADER_SIZE + (size_t)index * DEX_RECORD_SIZE;
    dex_record_t rec;
    esp_partition_read(s_partition, offset, &rec, sizeof(rec));
    if (rec.marker != DEX_RECORD_VALID) {
        return false; // shouldn't happen for an index within storage_dex_count(), but be safe
    }

    out->species = rec.species;
    out->person_id = rec.person_id;
    memcpy(out->person_name, rec.person_name, PERSON_NAME_LEN);
    out->timestamp = rec.timestamp;
    out->selfie_slot = rec.selfie_slot;
    out->flags = rec.flags;
    return true;
}

bool storage_dex_add(uint8_t species, uint32_t person_id, uint32_t timestamp, uint8_t selfie_slot)
{
    if (s_partition == NULL) {
        return false;
    }
    if (s_dex_next_offset + DEX_RECORD_SIZE > DEX_REGION_OFFSET + DEX_REGION_SIZE) {
        ESP_LOGE(TAG, "dex log full");
        return false;
    }

    dex_record_t rec;
    memset(&rec, 0xFF, sizeof(rec));
    rec.species = species;
    rec.person_id = person_id;
    memset(rec.person_name, 0, sizeof(rec.person_name));
    rec.timestamp = timestamp;
    rec.selfie_slot = selfie_slot;
    rec.flags = 0;

    esp_err_t err = esp_partition_write(s_partition, s_dex_next_offset + 1,
                                         (const uint8_t *)&rec + 1, sizeof(rec) - 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dex payload write failed: %d", err);
        return false;
    }
    uint8_t marker = DEX_RECORD_VALID;
    err = esp_partition_write(s_partition, s_dex_next_offset, &marker, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dex marker write failed: %d", err);
        return false;
    }

    s_dex_next_offset += DEX_RECORD_SIZE;
    return true;
}

bool storage_recently_caught(uint32_t person_id, uint32_t now, uint32_t window_seconds)
{
    if (s_partition == NULL) {
        return false;
    }
    int count = (int)((s_met_next_offset - MET_REGION_OFFSET - REGION_HEADER_SIZE) / MET_RECORD_SIZE);

    // Newest-first: the first record we find for this person is their most
    // recent catch, and if that one's outside the window, older ones can
    // only be further outside it.
    for (int i = count - 1; i >= 0; i--) {
        size_t offset = MET_REGION_OFFSET + REGION_HEADER_SIZE + (size_t)i * MET_RECORD_SIZE;
        met_record_t rec;
        esp_partition_read(s_partition, offset, &rec, sizeof(rec));
        if (rec.marker != MET_RECORD_VALID || rec.person_id != person_id) {
            continue;
        }
        return now >= rec.last_catch_time && (now - rec.last_catch_time) < window_seconds;
    }
    return false;
}

void storage_record_catch(uint32_t person_id, uint32_t now)
{
    if (s_partition == NULL) {
        return;
    }
    if (s_met_next_offset + MET_RECORD_SIZE > MET_REGION_OFFSET + MET_REGION_SIZE) {
        ESP_LOGE(TAG, "met log full");
        return;
    }

    met_record_t rec;
    memset(&rec, 0xFF, sizeof(rec));
    rec.person_id = person_id;
    rec.last_catch_time = now;

    esp_err_t err = esp_partition_write(s_partition, s_met_next_offset + 1,
                                         (const uint8_t *)&rec + 1, sizeof(rec) - 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "met payload write failed: %d", err);
        return;
    }
    uint8_t marker = MET_RECORD_VALID;
    err = esp_partition_write(s_partition, s_met_next_offset, &marker, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "met marker write failed: %d", err);
        return;
    }

    s_met_next_offset += MET_RECORD_SIZE;
}
