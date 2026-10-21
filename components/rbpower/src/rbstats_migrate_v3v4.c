/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstats_migrate_v3v4.c
 * @brief v3->v4 STATS region migrator.
 *
 * Runs inside the deferred-migration engine (rbstats_migrate) AFTER the running image is
 * confirmed valid. The v4 format changes record sizes AND the slot stride (208->320 KB), so the
 * whole region is re-laid-out:
 *
 *   for slot in OCCUPIED slots, DESCENDING index:          ← geometry-safe order: writing new
 *       read old slot's valid records (old 208K base,        slot k only overlaps old slots j>k
 *         self-describing sector markers) into PSRAM         (already migrated) or k itself
 *       convert v3→v4 + shelve TIME_UNSYNC → ts=0            (read before write)
 *       truncate-keep-newest to the v4 retention (logged)
 *       erase new slot range → write v4 sector images
 *       persist per-slot done-bit (NVS "rbmig")            ← idempotent resume; power-cut mid-slot
 *                                                            loses THAT slot only (accepted)
 *   restamp the format marker (atomic finale), clear rbmig.
 *
 * The NEW layout always comes from the LIVE tables (RBPOWER_TRANSFER_SIZES / RECORDS_COUNT /
 * BUFFER_SECTORS / slot base helper) — post-flip those ARE v4, so the migrator cannot drift
 * from what init expects. Only the OLD (v3) geometry is frozen here as private constants.
 * Written buffers carry standard sector markers (CLOSED chain + one ACTIVE tail), so normal
 * boot scan-recovery reconstructs cursors with no NVS dependency.
 */
#include "rbstats_migrate.h"
#include "rbstats_v4_types.h"
#include "rbextflash_operations.h"
#include "rbextflash_sector_metadata.h"
#include "rbstorage_media.h"
#include "partition_layout.h"
#include "littlefs_raw.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <nvs.h>

static const char *TAG = "RBSTATS_MIG34";

/* Diagnostic trace of the LAST migration run — readable via the `migdiag` diagnostic console verb
 * (ESP_LOG output is not visible on that console, and a silently failing migrator must be
 * diagnosable). Captures the decisions that matter: slot map, per-slot staging, offsets. */
/** @brief Diagnostic trace of the last migration run. */
typedef struct {
    bool     ran;                ///< A migration ran since boot
    bool     slots_file_exists;  ///< /flash_slots.json exists
    esp_err_t slot_map_err;      ///< Result of loading the slot map
    int      slots_used;         ///< Highest occupied slot index + 1
    int      total_occupied;     ///< Occupied slots
    int      new_fit;            ///< Slots that fit the STATS region under the v4 layout
    int      slots_done;         ///< Slots migrated so far
    /* trace of the LAST migrated slot's HOUR buffers, per dtype 0..2 (el/en/ec) */
    uint16_t t_uid;              ///< Unit ID of the traced slot
    uint32_t t_v3_base;          ///< v3 flash offset of the traced HOUR electrical buffer
    uint32_t t_v4_base;          ///< v4 flash offset of the traced HOUR electrical buffer
    uint16_t t_sectors_scanned;  ///< Sectors scanned in the traced buffer
    uint16_t t_sectors_magic_ok; ///< Sectors with a valid init magic in the traced buffer
    uint32_t t_staged;           ///< Records staged from the traced buffer
    uint32_t t_written;          ///< Records written to the traced buffer
    uint16_t d_scan[3];          ///< HOUR sectors scanned, per data type (el/en/ec)
    uint16_t d_magic[3];         ///< HOUR sectors with valid magic, per data type
    uint32_t d_staged[3];        ///< HOUR records staged, per data type
    uint32_t d_written[3];       ///< HOUR records written, per data type
    uint32_t d_v3_base[3];       ///< HOUR v3 buffer offsets, per data type
    uint32_t d_v4_base[3];       ///< HOUR v4 buffer offsets, per data type
    uint32_t total_staged;       ///< Records staged, all buffers, all slots
    uint32_t total_written;      ///< Records written, all buffers, all slots
    esp_err_t t_err;             ///< Last error recorded by the trace
} mig_diag_t;
static mig_diag_t s_diag;

/**
 * @brief Format the first line of the migration diagnostic trace.
 * @param[out] buf Destination string buffer.
 * @param[in]  cap Size of @p buf, bytes.
 */
void rbstats_migrate_v3v4_diag(char *buf, size_t cap)
{
    snprintf(buf, cap,
             "mig34: ran=%d file=%d map_err=%d slots_used=%d occupied=%d new_fit=%d done=%d | "
             "trace uid=%04X v3_base=0x%06lX v4_base=0x%06lX scan=%u magic_ok=%u staged=%lu written=%lu | TOTAL staged=%lu written=%lu err=%d",
             (int)s_diag.ran, (int)s_diag.slots_file_exists, (int)s_diag.slot_map_err, s_diag.slots_used, s_diag.total_occupied,
             s_diag.new_fit, s_diag.slots_done,
             (unsigned)s_diag.t_uid, (unsigned long)s_diag.t_v3_base, (unsigned long)s_diag.t_v4_base,
             (unsigned)s_diag.t_sectors_scanned, (unsigned)s_diag.t_sectors_magic_ok,
             (unsigned long)s_diag.t_staged, (unsigned long)s_diag.t_written,
             (unsigned long)s_diag.total_staged, (unsigned long)s_diag.total_written, (int)s_diag.t_err);
}

/**
 * @brief Format the second line of the migration diagnostic trace (per-data-type HOUR details).
 *
 * A diagnostic console reply is capped at ~160 characters, so one line cannot hold both.
 *
 * @param[out] buf Destination string buffer.
 * @param[in]  cap Size of @p buf, bytes.
 */
void rbstats_migrate_v3v4_diag2(char *buf, size_t cap)
{
    snprintf(buf, cap,
             "mig34 HOUR el/en/ec: scan=%u/%u/%u magic=%u/%u/%u staged=%lu/%lu/%lu written=%lu/%lu/%lu "
             "v3base=0x%lX/0x%lX/0x%lX v4base=0x%lX/0x%lX/0x%lX",
             s_diag.d_scan[0], s_diag.d_scan[1], s_diag.d_scan[2],
             s_diag.d_magic[0], s_diag.d_magic[1], s_diag.d_magic[2],
             (unsigned long)s_diag.d_staged[0], (unsigned long)s_diag.d_staged[1], (unsigned long)s_diag.d_staged[2],
             (unsigned long)s_diag.d_written[0], (unsigned long)s_diag.d_written[1], (unsigned long)s_diag.d_written[2],
             (unsigned long)s_diag.d_v3_base[0], (unsigned long)s_diag.d_v3_base[1], (unsigned long)s_diag.d_v3_base[2],
             (unsigned long)s_diag.d_v4_base[0], (unsigned long)s_diag.d_v4_base[1], (unsigned long)s_diag.d_v4_base[2]);
}

/* ---- FROZEN v3 geometry (the layout this migrator reads; never changes again) ---- */
#define V3_SLOT_BYTES   (208u * 1024u)           ///< v3 slot stride, bytes
#define V3_STATS_OFFSET PARTITION_STATS_OFFSET   ///< STATS region base (same in v3 and v4)
static const uint16_t V3_REC_SIZE[3] = {
    [RBPOWER_DATA_TYPE_ELECTRICAL] = 32,
    [RBPOWER_DATA_TYPE_ENERGY]     = 16,
    [RBPOWER_DATA_TYPE_ECONOMICS]  = 16,
};
static const uint8_t V3_SECTORS[RBPOWER_PERIOD_MAX][3] = {
    [RBPOWER_PERIOD_HOUR]  = { 10, 6, 6 },
    [RBPOWER_PERIOD_DAY]   = {  2, 2, 2 },
    [RBPOWER_PERIOD_WEEK]  = {  2, 2, 2 },
    [RBPOWER_PERIOD_MONTH] = {  2, 2, 2 },
    [RBPOWER_PERIOD_YEAR]  = {  2, 2, 0 },
};

/** @brief Local CRC16-CCITT copy (matches calculate_sector_crc in rbextflash_operations.c). */
static uint16_t mig_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/** @brief Staged v3 record (raw payload <= 32 B), ordered by (sector seq, index in sector). */
typedef struct {
    uint32_t seq;      ///< Sequence number of the source sector
    uint16_t idx;      ///< Record index inside the source sector
    uint8_t  raw[32];  ///< Raw v3 record bytes
} stage_rec_t;

/** @brief qsort comparator: chronological order by (seq, idx). */
static int stage_cmp(const void *a, const void *b)
{
    const stage_rec_t *x = (const stage_rec_t *)a, *y = (const stage_rec_t *)b;
    if (x->seq != y->seq) return (x->seq < y->seq) ? -1 : 1;
    if (x->idx != y->idx) return (x->idx < y->idx) ? -1 : 1;
    return 0;
}

/** Old-layout offset of (period,dtype) inside a v3 slot: canonical packing order (period asc,
 *  dtype asc, skip zero-sector combos) — the same order init_buffers_from_network packs with. */
static uint32_t v3_buffer_offset(int slot, rbpower_period_t period, rbpower_data_type_t dtype)
{
    uint32_t off = V3_STATS_OFFSET + (uint32_t)slot * V3_SLOT_BYTES;
    for (int p = RBPOWER_PERIOD_HOUR; p < RBPOWER_PERIOD_MAX; p++) {
        for (int t = 0; t < RBPOWER_DATA_TYPE_MAX; t++) {
            uint8_t sect = (t <= RBPOWER_DATA_TYPE_ECONOMICS) ? V3_SECTORS[p][t] : 0;
            if (sect == 0) continue;
            if (p == (int)period && t == (int)dtype) return off;
            off += (uint32_t)sect * RBPOWER_FLASH_SECTOR_SIZE;
        }
    }
    return 0;
}

/** New-layout offset of (period,dtype) inside the v4 slot — LIVE tables, same packing order. */
static uint32_t v4_buffer_offset(int slot, rbpower_period_t period, rbpower_data_type_t dtype,
                                 uint8_t *sectors_out)
{
    uint32_t off = rbpower_flash_slot_base_offset(slot);
    for (int p = RBPOWER_PERIOD_HOUR; p < RBPOWER_PERIOD_MAX; p++) {
        for (int t = 0; t < RBPOWER_DATA_TYPE_MAX; t++) {
            uint8_t sect = rbpower_get_buffer_sectors((rbpower_period_t)p, (rbpower_data_type_t)t);
            if (sect == 0) continue;
            if (p == (int)period && t == (int)dtype) {
                if (sectors_out) *sectors_out = sect;
                return off;
            }
            off += (uint32_t)sect * RBPOWER_FLASH_SECTOR_SIZE;
        }
    }
    if (sectors_out) *sectors_out = 0;
    return 0;
}

/** Read all valid v3 records of one old buffer into a chronologically sorted staging array. */
static esp_err_t read_v3_buffer(int slot, rbpower_period_t period, rbpower_data_type_t dtype,
                                stage_rec_t **out, size_t *count_out)
{
    *out = NULL;
    *count_out = 0;
    uint8_t sectors = V3_SECTORS[period][dtype];
    uint16_t rec_size = V3_REC_SIZE[dtype];
    if (sectors == 0 || rec_size == 0) return ESP_OK;

    uint16_t per_sector = SECTOR_DATA_SIZE / rec_size;
    size_t cap = (size_t)sectors * per_sector;
    stage_rec_t *stage = heap_caps_malloc(cap * sizeof(stage_rec_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stage) stage = malloc(cap * sizeof(stage_rec_t));
    if (!stage) return ESP_ERR_NO_MEM;

    uint8_t *sector_buf = malloc(RBPOWER_FLASH_SECTOR_SIZE);
    if (!sector_buf) { free(stage); return ESP_ERR_NO_MEM; }

    uint32_t base = v3_buffer_offset(slot, period, dtype);
    bool trace = (period == RBPOWER_PERIOD_HOUR && dtype == RBPOWER_DATA_TYPE_ELECTRICAL);
    bool dtrace = (period == RBPOWER_PERIOD_HOUR && (int)dtype <= 2);
    if (trace) { s_diag.t_v3_base = base; s_diag.t_sectors_scanned = 0; s_diag.t_sectors_magic_ok = 0; }
    if (dtrace) { s_diag.d_v3_base[dtype] = base; s_diag.d_scan[dtype] = 0; s_diag.d_magic[dtype] = 0; }
    size_t n = 0;
    for (uint8_t s = 0; s < sectors; s++) {
        uint32_t s_off = base + (uint32_t)s * RBPOWER_FLASH_SECTOR_SIZE;
        esp_err_t e = rbmedia_stats_read(s_off, sector_buf, RBPOWER_FLASH_SECTOR_SIZE);
        if (trace) s_diag.t_sectors_scanned++;
        if (dtrace) s_diag.d_scan[dtype]++;
        if (e != ESP_OK) { if (trace) s_diag.t_err = e; free(sector_buf); free(stage); return e; }
        const sector_metadata_t *meta =
            (const sector_metadata_t *)(sector_buf + SECTOR_METADATA_OFFSET);
        if (meta->init_magic != SECTOR_INIT_MAGIC) continue;   /* virgin/alien sector */
        if (trace) s_diag.t_sectors_magic_ok++;
        if (dtrace) s_diag.d_magic[dtype]++;
        for (uint16_t r = 0; r < per_sector; r++) {
            const uint8_t *rec = sector_buf + (size_t)r * rec_size;
            uint32_t ts;
            memcpy(&ts, rec, sizeof(ts));
            if (ts == 0xFFFFFFFFu) break;      /* records are appended — first gap ends the sector */
            if (ts == 0) continue;             /* defensive: never-valid slot */
            stage[n].seq = meta->sequence_number;
            stage[n].idx = r;
            memcpy(stage[n].raw, rec, rec_size);
            n++;
        }
        vTaskDelay(1);
    }
    free(sector_buf);
    if (n > 1) qsort(stage, n, sizeof(stage_rec_t), stage_cmp);
    if (trace) s_diag.t_staged = (uint32_t)n;
    if (dtrace) s_diag.d_staged[dtype] = (uint32_t)n;
    s_diag.total_staged += (uint32_t)n;
    *out = stage;
    *count_out = n;
    return ESP_OK;
}

/** Convert one staged v3 record to v4 bytes (into dst, stride = live v4 record size).
 *  Migrator-only rule on top of the pure converters: TIME_UNSYNC → ts = 0 (unsynced shelf). */
static void convert_record(rbpower_data_type_t dtype, rbpower_period_t period,
                           const uint8_t *raw, uint8_t *dst)
{
    switch (dtype) {
        case RBPOWER_DATA_TYPE_ELECTRICAL: {
            rbstats_electrical_v4_t v4;
            rbstats_v4_conv_electrical((const rbpower_electrical_transfer_t *)raw, period, &v4);
            if (v4.h.q & RBSTATS_Q_TIME_UNSYNC) v4.h.ts = 0;
            memcpy(dst, &v4, sizeof(v4));
            break;
        }
        case RBPOWER_DATA_TYPE_ENERGY: {
            rbstats_energy_v4_t v4;
            rbstats_v4_conv_energy((const rbpower_energy_transfer_t *)raw, period, &v4);
            if (v4.h.q & RBSTATS_Q_TIME_UNSYNC) v4.h.ts = 0;
            memcpy(dst, &v4, sizeof(v4));
            break;
        }
        default: {
            rbstats_econ_v4_t v4;
            rbstats_v4_conv_econ((const rbpower_economics_transfer_t *)raw, period, &v4);
            if (v4.h.q & RBSTATS_Q_TIME_UNSYNC) v4.h.ts = 0;
            memcpy(dst, &v4, sizeof(v4));
            break;
        }
    }
}

/** Write a chronological record list as a fresh v4 ring: erased range, sectors filled in order,
 *  every full sector CLOSED (crc over the data area, 0xFF padding included), the LAST used sector
 *  left ACTIVE so the normal writer continues it; untouched sectors stay virgin. */
static esp_err_t write_v4_buffer(int slot, rbpower_period_t period, rbpower_data_type_t dtype,
                                 const stage_rec_t *stage, size_t count)
{
    uint8_t sectors = 0;
    uint32_t base = v4_buffer_offset(slot, period, dtype, &sectors);
    bool trace = (period == RBPOWER_PERIOD_HOUR && dtype == RBPOWER_DATA_TYPE_ELECTRICAL);
    bool dtrace = (period == RBPOWER_PERIOD_HOUR && (int)dtype <= 2);
    if (trace) { s_diag.t_v4_base = base; s_diag.t_written = (uint32_t)count; }
    if (dtrace) { s_diag.d_v4_base[dtype] = base; s_diag.d_written[dtype] = (uint32_t)count; }
    s_diag.total_written += (uint32_t)count;
    if (sectors == 0) { if (trace) s_diag.t_written = 0; return ESP_OK; }                      /* combo absent in v4 — nothing to write */

    uint16_t rec_size = rbpower_get_transfer_size(dtype);
    uint16_t per_sector = SECTOR_DATA_SIZE / rec_size;

    esp_err_t e = rbmedia_stats_erase(base, (size_t)sectors * RBPOWER_FLASH_SECTOR_SIZE);
    if (e != ESP_OK) return e;
    if (count == 0) return ESP_OK;                        /* fresh virgin ring */

    uint8_t *img = malloc(RBPOWER_FLASH_SECTOR_SIZE);
    if (!img) return ESP_ERR_NO_MEM;

    size_t written = 0;
    uint32_t seq = 0;
    while (written < count) {
        memset(img, 0xFF, RBPOWER_FLASH_SECTOR_SIZE);
        size_t in_this = count - written;
        if (in_this > per_sector) in_this = per_sector;
        uint32_t newest_ts = 0;
        for (size_t r = 0; r < in_this; r++) {
            uint8_t *dst = img + r * rec_size;
            convert_record(dtype, period, stage[written + r].raw, dst);
            uint32_t ts;
            memcpy(&ts, dst, sizeof(ts));
            if (ts > newest_ts) newest_ts = ts;
        }
        written += in_this;
        seq++;

        sector_metadata_t *meta = (sector_metadata_t *)(img + SECTOR_METADATA_OFFSET);
        bool last_used = (written >= count);
        if (last_used) {
            sector_metadata_init_active(meta, seq);       /* writer continues here */
        } else {
            meta->last_write_timestamp = newest_ts;
            meta->crc16 = 0;                              /* computed below over the final image */
            meta->flags = SECTOR_FLAG_CLOSED;
            meta->init_magic = SECTOR_INIT_MAGIC;
            meta->sequence_number = seq;
            memset(meta->reserved, 0, sizeof(meta->reserved));
            meta->crc16 = mig_crc16(img, SECTOR_DATA_SIZE);
        }

        uint32_t s_off = base + (uint32_t)(seq - 1) * RBPOWER_FLASH_SECTOR_SIZE;
        e = rbmedia_stats_write(s_off, img, RBPOWER_FLASH_SECTOR_SIZE);
        if (e != ESP_OK) { free(img); return e; }
        vTaskDelay(1);
    }
    free(img);
    return ESP_OK;
}

/* ---- per-slot done-bits (NVS) for idempotent resume ---- */
/** @brief Load the per-slot done bits (0 when absent). */
static uint64_t mig_done_bits_load(void)
{
    uint64_t bits = 0;
    nvs_handle_t h;
    if (nvs_open("rbmig", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u64(h, "done", &bits);
        nvs_close(h);
    }
    return bits;
}

/** @brief Persist the per-slot done bits. */
static void mig_done_bits_store(uint64_t bits)
{
    nvs_handle_t h;
    if (nvs_open("rbmig", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u64(h, "done", bits);
        nvs_commit(h);
        nvs_close(h);
    }
}

/** @brief Erase the done bits after a completed migration. */
static void mig_done_bits_clear(void)
{
    nvs_handle_t h;
    if (nvs_open("rbmig", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

/**
 * @brief Migrate the whole STATS region from v3 to v4 (rbstats_migrator_fn).
 *
 * Idempotent per slot; resumes from the NVS done bits after a power loss.
 *
 * @param[in] from_version Stored format version (must be 3).
 * @param[in] progress     Progress callback, percent 0..100 (may be NULL).
 * @return ESP_OK when the region is at v4 and the marker is restamped.
 * @retval ESP_ERR_NOT_SUPPORTED     @p from_version is not 3, or the build is not v4.
 * @retval ESP_ERR_INVALID_STATE     the slot map is empty but v3 data exists (aborted, data kept).
 * @retval ESP_ERR_NO_MEM            staging memory could not be allocated.
 * @return Other esp_err_t codes from the slot map, flash read/erase/write or the marker restamp.
 */
esp_err_t rbstats_migrate_v3_to_v4(uint32_t from_version, void (*progress)(uint8_t pct))
{
    if (from_version != 3 || RBPOWER_RECORD_FORMAT_VERSION < 4) {
        ESP_LOGE(TAG, "migrator covers v3 -> v4 only (asked v%lu -> v%d)",
                 (unsigned long)from_version, RBPOWER_RECORD_FORMAT_VERSION);
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.ran = true;
    s_diag.slots_file_exists = littlefs_raw_exists("/flash_slots.json");
    uint16_t unit_by_slot[RBPOWER_MAX_UNITS];
    int slots_used = 0;
    esp_err_t e = rbpower_flash_slot_map_get(unit_by_slot, RBPOWER_MAX_UNITS, &slots_used);
    s_diag.slot_map_err = e;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "slot map unavailable: %s", esp_err_to_name(e));
        return e;
    }
    int new_fit = rbpower_flash_max_usable_slots();
    s_diag.slots_used = slots_used;
    s_diag.new_fit = new_fit;

    uint64_t done = mig_done_bits_load();
    int total = 0, finished = 0;
    for (int i = 0; i < slots_used; i++) {
        if (unit_by_slot[i] != 0xFFFF) total++;
    }
    s_diag.total_occupied = total;
    if (total == 0) {
        /* Safety net: an EMPTY slot map over a region that still holds v3
         * sectors means the map is wrong (e.g. the loader truncated the slots JSON), not that
         * there is nothing to migrate. Restamping here orphans real data under a v4 marker.
         * Probe each possible v3 slot's first sector for the init magic; any hit → ABORT, the
         * engine keeps the region INCOMPATIBLE with the data preserved. */
        uint8_t *probe = malloc(RBPOWER_FLASH_SECTOR_SIZE);
        if (probe) {
            int v3_fit = (int)(rbmedia_stats_region_size() / V3_SLOT_BYTES);
            for (int sl = 0; sl < v3_fit && sl < RBPOWER_MAX_UNITS; sl++) {
                if (rbmedia_stats_read(V3_STATS_OFFSET + (uint32_t)sl * V3_SLOT_BYTES,
                                       probe, RBPOWER_FLASH_SECTOR_SIZE) != ESP_OK) continue;
                const sector_metadata_t *m =
                    (const sector_metadata_t *)(probe + SECTOR_METADATA_OFFSET);
                if (m->init_magic == SECTOR_INIT_MAGIC) {
                    ESP_LOGE(TAG, "slot map EMPTY but v3 data found at slot %d — ABORT "
                                  "(region stays INCOMPATIBLE, data preserved; fix the slot map)", sl);
                    free(probe);
                    s_diag.t_err = ESP_ERR_INVALID_STATE;
                    return ESP_ERR_INVALID_STATE;
                }
                vTaskDelay(1);
            }
            free(probe);
        }
        ESP_LOGW(TAG, "no occupied slots — nothing to migrate, restamping marker");
        e = rbpower_flash_format_marker_restamp();
        mig_done_bits_clear();
        return e;
    }

    ESP_LOGW(TAG, "v3 -> v4 migration: %d occupied slots, new fit %d, resume bits 0x%016llx",
             total, new_fit, (unsigned long long)done);

    /* DESCENDING slot order: the v4 stride is larger, so writing new slot k only overlaps old
     * slots j > k (already migrated) or k itself (read before write). */
    for (int slot = slots_used - 1; slot >= 0; slot--) {
        if (unit_by_slot[slot] == 0xFFFF) continue;
        if (done & (1ULL << slot)) { finished++; continue; }

        rbpower_unit_id_t uid = unit_by_slot[slot];
        if (slot >= new_fit) {
            ESP_LOGE(TAG, "MIGRATION_TRUNCATED: unit 0x%04X (slot %d) exceeds the v4 slot fit (%d) "
                          "— its history is NOT migrated (unit continues RAM-only)",
                     (unsigned)uid, slot, new_fit);
            done |= (1ULL << slot);
            mig_done_bits_store(done);
            finished++;
            continue;
        }

        ESP_LOGW(TAG, "migrating slot %d (unit 0x%04X)...", slot, (unsigned)uid);
        s_diag.t_uid = (uint16_t)uid;
        /* TWO PHASES per slot: the v4 buffers
         * are LARGER than the v3 ones (HOUR el 10→16 sectors…), so writing v4 buffer N erases the v3
         * bytes of buffer N+1 in the same slot before it is read. Slot 0 has identical v3/v4 bases, so
         * it always hit; slots ≥1 only for later periods. Read EVERYTHING of the slot into PSRAM first,
         * write everything second. */
        stage_rec_t *stg[RBPOWER_PERIOD_MAX][3] = {{0}};
        size_t       stn[RBPOWER_PERIOD_MAX][3] = {{0}};
        bool read_failed = false;
        for (int p = RBPOWER_PERIOD_HOUR; p < RBPOWER_PERIOD_MAX && !read_failed; p++) {
            for (int t = 0; t <= RBPOWER_DATA_TYPE_ECONOMICS; t++) {
                e = read_v3_buffer(slot, (rbpower_period_t)p, (rbpower_data_type_t)t, &stg[p][t], &stn[p][t]);
                if (e != ESP_OK) {
                    ESP_LOGE(TAG, "slot %d p=%d t=%d: read failed (%s) — slot skipped, data kept",
                             slot, p, t, esp_err_to_name(e));
                    read_failed = true;
                    break;
                }
            }
        }
        if (read_failed) {
            for (int p = 0; p < RBPOWER_PERIOD_MAX; p++) for (int t = 0; t < 3; t++) free(stg[p][t]);
            return e;   /* abort: engine keeps INCOMPATIBLE, nothing destroyed yet (no writes happened) */
        }
        for (int p = RBPOWER_PERIOD_HOUR; p < RBPOWER_PERIOD_MAX; p++) {
            for (int t = 0; t <= RBPOWER_DATA_TYPE_ECONOMICS; t++) {
                size_t n = stn[p][t];
                /* truncate-keep-newest to the v4 retention (newest wins, logged) */
                uint32_t keep = rbpower_get_records_count((rbpower_period_type_t)p);
                const stage_rec_t *first = stg[p][t];
                size_t n_keep = n;
                if (n > keep) {
                    first = stg[p][t] + (n - keep);
                    n_keep = keep;
                    ESP_LOGW(TAG, "MIGRATION_TRUNCATED: unit 0x%04X p=%d t=%d: %u -> %lu records "
                                  "(oldest dropped)",
                             (unsigned)uid, p, t, (unsigned)n, (unsigned long)n_keep);
                }
                e = write_v4_buffer(slot, (rbpower_period_t)p, (rbpower_data_type_t)t, first, n_keep);
                if (e != ESP_OK) {
                    ESP_LOGE(TAG, "slot %d p=%d t=%d: v4 write failed (%s)", slot, p, t,
                             esp_err_to_name(e));
                    for (int pp = 0; pp < RBPOWER_PERIOD_MAX; pp++) for (int tt = 0; tt < 3; tt++) free(stg[pp][tt]);
                    return e;   /* this slot is lost on retry (erased) — the accepted worst case */
                }
            }
        }
        for (int p = 0; p < RBPOWER_PERIOD_MAX; p++) for (int t = 0; t < 3; t++) free(stg[p][t]);
        done |= (1ULL << slot);
        mig_done_bits_store(done);
        finished++;
        s_diag.slots_done = finished;
        if (progress) progress((uint8_t)(finished * 100 / total));
        ESP_LOGW(TAG, "slot %d (unit 0x%04X) migrated (%d/%d)", slot, (unsigned)uid, finished, total);
    }

    /* Atomic finale: only a fully migrated region gets the v4 marker. */
    e = rbpower_flash_format_marker_restamp();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "marker restamp failed (%s) — migration will no-op-resume next boot",
                 esp_err_to_name(e));
        return e;
    }
    mig_done_bits_clear();
    ESP_LOGW(TAG, "v3 -> v4 migration COMPLETE (%d slots)", finished);
    return ESP_OK;
}
