/*
 * Copyright (c) 2026 HOS_COW Project
 *
 * Portable cowMMC Overlay & Save-Reserve FAT32 Ballooning Core
 * Compiles cleanly both in Hekate/Nyx (ARM32 freestanding) and Linux/macOS host tests.
 */

#ifndef _COW_NYX_CORE_H_
#define _COW_NYX_CORE_H_

#ifdef __HOST_COW_TEST__
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef zalloc
#define zalloc(sz) calloc(1, (sz))
#endif
#else
#include <stdlib.h>
#include <string.h>
#include <bdk.h>
#endif

#define COW_NYX_SECTOR_SIZE           512U
#define COW_NYX_BLOCK_SECTORS         32U
#define COW_NYX_BLOCK_SHIFT           5U
#define COW_NYX_BLOCK_MASK            31U
#define COW_NYX_BLOCK_BYTES           16384U
#define COW_NYX_MIN_L1_ENTRIES        1024U
#define COW_NYX_V2_DEF_L1_ENTRIES     4096U
#define COW_NYX_L1_ENTRIES_MAX        8192U
#define COW_NYX_L2_SHIFT              12U
#define COW_NYX_L2_ENTRIES            4096U
#define COW_NYX_L2_MASK               4095U
#define COW_NYX_L1_SLOTS_FOR(n)       (((((u32)(n) * 4U) + COW_NYX_BLOCK_BYTES - 1U)) / COW_NYX_BLOCK_BYTES)
#define COW_NYX_SUPERBLOCK_SLOTS      2U
#define COW_NYX_FIRST_DATA_SLOT       (COW_NYX_SUPERBLOCK_SLOTS + 2U * COW_NYX_L1_SLOTS_FOR(COW_NYX_MIN_L1_ENTRIES))
#define COW_NYX_DEFAULT_PREFIX_SECTS     65536U
#define COW_NYX_MAX_SPLIT_PARTS          32U
#define COW_NYX_L2_CACHE_WAYS            8U
#define COW_NYX_SD_RESERVE_DEFAULT_MB    512U
#define COW_NYX_SYSMEM_RESERVE_DEFAULT_MB 1024U
#define COW_NYX_PART_MAX_DEFAULT_MB      4092U
#define COW_NYX_BALLOON_MAX_PART_BYTES (4092ULL * 1024ULL * 1024ULL)

#define COW_NYX_MAGIC                 0x30574F43U /* "COW0" */
#define COW_NYX_VERSION_V2            2U
#define COW_NYX_VERSION               COW_NYX_VERSION_V2

#define COW_NYX_FLAG_EPHEMERAL        (1U << 0)
#define COW_NYX_FLAG_THIN_PROVISION   (1U << 1)
#define COW_NYX_FLAG_NEEDS_REBASE     (1U << 2)
#define COW_NYX_FLAG_FW_MISMATCH      (1U << 3)
#define COW_NYX_FLAG_CONFLICT_LOCKED  (1U << 4)

#define COW_NYX_ALLOC_THICK           0U
#define COW_NYX_ALLOC_THIN            1U

#define COW_NYX_DRIFT_BOOT0_PKG1      (1U << 0)
#define COW_NYX_DRIFT_BOOT0_PATROL    (1U << 1)
#define COW_NYX_DRIFT_BOOT1           (1U << 2)
#define COW_NYX_DRIFT_GPT             (1U << 3)
#define COW_NYX_DRIFT_BCPKG2          (1U << 4)

typedef enum {
	COW_NYX_OK = 0,
	COW_NYX_ERR_ARG = -1,
	COW_NYX_ERR_IO = -2,
	COW_NYX_ERR_NOT_COW = -3,
	COW_NYX_ERR_CORRUPT = -4,
	COW_NYX_ERR_NO_MEM = -5,
	COW_NYX_ERR_POOL_FULL = -6,
	COW_NYX_ERR_NO_SD_SPACE = -7,
	COW_NYX_ERR_FS_INVALID = -8,
	COW_NYX_ERR_UNSUPPORTED = -9
} cow_nyx_err_t;

typedef enum {
	COW_BAL_UNCHANGED = 0,
	COW_BAL_CREATED = 1,
	COW_BAL_SHRUNK = 2,
	COW_BAL_GROWN = 3,
	COW_BAL_REMOVED = 4,
	COW_BAL_REPAIRED_FSINFO = 5
} cow_nyx_bal_action_t;

typedef struct __attribute__((packed)) _cow_nyx_sb_t
{
	/* 0x000..0x033 (52 bytes) */
	u32 magic;
	u32 version;
	u64 seq_num;
	u64 gpp_total_sectors;
	u32 prefix_sectors;
	u32 total_slots;
	u32 next_free_slot;
	u32 flags;
	u32 dirty_data_blocks;
	u32 dirty_l2_pages;
	u32 l1_crc32;

	/* 0x034..0x1FB (456 bytes) */
	u32 header_ext_size;
	u64 physical_base_sectors;
	u32 max_overlay_slots;
	u32 l1_entries;
	u32 l1_slot_a;
	u32 l1_slot_b;
	u32 first_data_slot;
	u32 thin_grow_step_slots;
	u16 thin_high_watermark_pct;
	u8  alloc_policy;
	u8  block_shift;
	u8  l2_shift;
	u8  reserved_geom[3];
	u32 part_max_sectors;
	u32 base_hos_version;
	u64 base_gen_id;
	u8  overlay_uuid[16];
	u8  parent_snapshot_uuid[16];
	u32 base_pkg1_ver;
	u32 ext_meta_slot;
	u8  emmc_cid_sha256_16[16];
	u8  boot0_pkg1_sha256[32];
	u8  boot1_sha256[32];
	u8  gpt_sha256[32];
	u8  bcpkg2_sha256[32];
	u8  system_digest_sha256[32];
	u8  user_digest_sha256[32];
	char overlay_name[32];
	u64 created_epoch;
	u64 last_rebase_seq;
	u64 last_verified_base_gen_id;
	u32 snapshot_count;
	u32 conflict_count;
	u32 base_drift_part_mask;
	u32 reserved_meta;
	u8  boot0_patrol_sha256_16[16];
	u8  last_snapshot_sha256_16[16];
	u8  reserved_v2[44];

	/* 0x1FC..0x1FF (4 bytes) */
	u32 header_crc32;
} cow_nyx_sb_t;

_Static_assert(sizeof(cow_nyx_sb_t) == COW_NYX_SECTOR_SIZE, "cow_nyx_sb_t must be 512 bytes");
_Static_assert(offsetof(cow_nyx_sb_t, header_ext_size) == 0x034, "cow_nyx_sb_t ext offset");
_Static_assert(offsetof(cow_nyx_sb_t, physical_base_sectors) == 0x038, "cow_nyx_sb_t phys offset");
_Static_assert(offsetof(cow_nyx_sb_t, header_crc32) == 0x1FC, "cow_nyx_sb_t crc offset");

typedef struct _cow_nyx_hal_t
{
	void *user_ctx;
	int (*file_open)(void *ctx, const char *path, bool create, u64 *out_size, void **out_handle);
	void (*file_close)(void *ctx, void *handle);
	int (*file_read)(void *ctx, void *handle, u64 off, void *buf, u32 len);
	int (*file_write)(void *ctx, void *handle, u64 off, const void *buf, u32 len);
	int (*file_grow)(void *ctx, void *handle, u64 new_size);
	int (*base_gpp_read)(void *ctx, u32 lba, u32 count, void *buf);
	int (*xts_user_crypt)(void *ctx, bool encrypt, u32 part_rel_lba, void *buf, u32 sector_count);
	u32 (*crc32_fn)(u32 crc, const void *buf, u32 len);
	void (*progress_fn)(void *ctx, const char *msg, int pct);
} cow_nyx_hal_t;

typedef struct _cow_nyx_probe_t
{
	bool is_cow;
	u32  version;
	u64  seq_num;
	u64  gpp_total_sectors;
	u64  phys_base_sectors;
	u32  total_slots;
	u32  effective_total_slots;
	u32  max_overlay_slots;
	u32  next_free_slot;
	u32  first_data_slot;
	u32  dirty_data_blocks;
	u32  dirty_l2_pages;
	u32  flags;
	u8   alloc_policy;
	u32  drift_mask;
	u32  part_max_sectors;
	u32  num_parts;
	u64  archive_bytes;
	u32  active_sb_slot;
	char overlay_name[32];
} cow_nyx_probe_t;

typedef struct _cow_nyx_l2_slot_t
{
	u32  l1_idx;
	u32  last_used;
	bool valid;
	bool dirty;
	u32 *entries; /* COW_NYX_L2_ENTRIES u32s */
} cow_nyx_l2_slot_t;

typedef struct _cow_nyx_vol_t
{
	cow_nyx_hal_t hal;
	char          emmc_dir[256];
	cow_nyx_sb_t  hdr;
	u32           active_sb_slot;
	u32           l1_entries;
	u32           first_data_slot;
	u32           effective_total_slots;
	u64           phys_base_sectors;
	u32           num_parts;
	u64           part_sizes[COW_NYX_MAX_SPLIT_PARTS];
	void         *part_handles[COW_NYX_MAX_SPLIT_PARTS];
	u32          *l1_table;
	u8           *scratch_blk; /* 16384B aligned scratch */
	u8           *xts_scratch; /* 16384B dedicated XTS bounce scratch */
	cow_nyx_l2_slot_t l2_cache[COW_NYX_L2_CACHE_WAYS];
	u32           lru_tick;
	bool          dirty_hdr;
	bool          opened;
} cow_nyx_vol_t;

typedef struct _cow_nyx_headroom_t
{
	u64 sd_free_bytes;
	u64 sd_reserve_bytes;
	u64 sd_safe_headroom_bytes;
	u64 unused_pool_bytes;
	u64 max_remaining_pool_bytes;
	u64 sysmem_cap_bytes;
	u64 safe_sysmem_bytes;
} cow_nyx_headroom_t;

typedef struct _cow_nyx_balloon_res_t
{
	cow_nyx_bal_action_t action;
	u32 cluster_bytes;
	u32 true_free_clusters;
	u32 old_balloon_clusters;
	u32 new_balloon_clusters;
	u32 target_free_clusters;
	u64 effective_free_bytes;
	u32 num_balloon_parts;
	cow_nyx_headroom_t headroom;
} cow_nyx_balloon_res_t;

#define COW_NYX_DRIFT_SYSTEM          (1U << 7)
#define COW_NYX_DRIFT_USER            (1U << 8)

#define COW_NYX_RAW_SNAP_MAGIC        0x52574F43U /* "COWR" */
#define COW_NYX_RAW_SNAP_VERSION      1U

typedef struct __attribute__((packed)) _cow_nyx_raw_snap_hdr_t
{
	u32 magic;
	u32 snap_version;
	u32 active_sb_slot;
	u32 cow_version;
	u64 seq_num;
	u64 gpp_total_sectors;
	u64 physical_base_sectors;
	u32 prefix_sectors;
	u32 total_slots;
	u32 first_data_slot;
	u32 next_free_slot;
	u32 l1_entries;
	u32 l1_slot_a;
	u32 l1_slot_b;
	u32 flags;
	u32 dirty_data_blocks;
	u32 dirty_l2_pages;
	u64 base_gen_id;
	u8  overlay_uuid[16];
	u32 payload_crc32;
	u8  reserved[400];
	u32 header_crc32;
} cow_nyx_raw_snap_hdr_t;

_Static_assert(sizeof(cow_nyx_raw_snap_hdr_t) == COW_NYX_SECTOR_SIZE, "cow_nyx_raw_snap_hdr_t must be 512 bytes");
_Static_assert(offsetof(cow_nyx_raw_snap_hdr_t, header_crc32) == 0x1FC, "cow_nyx_raw_snap_hdr_t crc offset");

u32 cow_nyx_crc32(const cow_nyx_hal_t *hal, u32 init_crc, const void *buf, u32 len);

int cow_nyx_probe(const cow_nyx_hal_t *hal, const char *emmc_dir, cow_nyx_probe_t *out_probe);
int cow_nyx_open(const cow_nyx_hal_t *hal, const char *emmc_dir, cow_nyx_vol_t *out_vol);
void cow_nyx_close(cow_nyx_vol_t *vol);

int cow_nyx_read_sectors(cow_nyx_vol_t *vol, u64 gpp_lba, u32 count, void *buf);
int cow_nyx_write_sectors(cow_nyx_vol_t *vol, u64 gpp_lba, u32 count, const void *buf);
int cow_nyx_commit(cow_nyx_vol_t *vol);

int cow_nyx_set_flags(cow_nyx_vol_t *vol, u32 flags);
int cow_nyx_reset_overlay(cow_nyx_vol_t *vol);
int cow_nyx_grow_pool(cow_nyx_vol_t *vol, u32 add_slots, u64 sd_free_bytes, u32 sd_reserve_mb);

int cow_nyx_probe_raw_snapshot(
	const cow_nyx_hal_t *hal,
	const char *snap_path,
	cow_nyx_raw_snap_hdr_t *out_hdr);

int cow_nyx_save_raw_snapshot(
	cow_nyx_vol_t *vol,
	const char *snap_path,
	cow_nyx_raw_snap_hdr_t *opt_out_hdr);

int cow_nyx_restore_raw_snapshot(
	cow_nyx_vol_t *vol,
	const char *snap_path,
	cow_nyx_raw_snap_hdr_t *opt_out_hdr);

void cow_nyx_compute_headroom(
	const cow_nyx_vol_t *vol,
	u64 sd_free_bytes,
	u32 sd_reserve_mb,
	u32 sysmem_cap_mb,
	cow_nyx_headroom_t *out_headroom);

int cow_nyx_find_user_gpt(
	cow_nyx_vol_t *vol,
	u32 *out_user_idx,
	u64 *out_user_start,
	u64 *out_user_end);

int cow_nyx_sync_headroom(
	cow_nyx_vol_t *vol,
	u64 sd_free_bytes,
	u32 sd_reserve_mb,
	u32 sysmem_cap_mb,
	cow_nyx_balloon_res_t *out_res);

#endif /* _COW_NYX_CORE_H_ */
