/*
 * Copyright (c) 2026 HOS_COW Project
 *
 * Non-replacing Experimental cowMMC Overlay Management & Setup UI for Nyx.
 * Leaves Hekate's Create/Change/Migrate emuMMC workflows untouched and works
 * over Hekate's own libsys_cow.bso + fatfs + Tegra X1 SE AES-XTS hardware.
 */

#include <stdlib.h>
#include <string.h>

#include <bdk.h>
#include <libs/fatfs/ff.h>

#include "gui.h"
#include "fe_emummc_tools.h"
#include "gui_cow_tools.h"
#include "cow_nyx_core.h"
#include "../hos/hos.h"
#include "../gfx/gfx.h"

#define COW_MAX_DISCOVERED       16U
#define COW_BOOT_PART_BYTES      0x400000U
#define COW_BOOT_PART_SECTORS    0x2000U
#define COW_BOOT0_BCTSAVE_OFF    0xFC000U
#define COW_BOOT0_BCTSAVE_LEN    0x4000U
#define COW_BOOT0_PATROL_OFF     0x184000U
#define COW_BOOT0_PATROL_LEN     0x200U
#define COW_HASH_BUF_BYTES       0x800000U
#define COW_PART_ANCHOR_BYTES    0x1000000U

typedef struct _cow_entry_item_t
{
	char            name[64];      /* e.g., "COW0" */
	char            base_path[96]; /* e.g., "emuMMC/COW0" */
	char            emmc_dir[112]; /* e.g., "emuMMC/COW0/eMMC" */
	bool            is_active;
	bool            has_lkg;
	bool            has_boot_ok;
	bool            has_migrate_pending;
	cow_nyx_probe_t probe;
} cow_entry_item_t;

typedef struct _cow_ui_ctx_t
{
	lv_obj_t        *win;
	lv_obj_t        *list_overlays;
	lv_obj_t        *lbl_detail;
	lv_obj_t        *btn_activate;
	lv_obj_t        *btn_guard;
	lv_obj_t        *btn_verify;
	lv_obj_t        *btn_maint;
	cow_entry_item_t items[COW_MAX_DISCOVERED];
	u32              item_count;
	int              selected_idx;
	u64              sd_free_bytes;
	bool             has_block_flag;
	bool             has_allow_flag;
	char             active_path[128];
} cow_ui_ctx_t;

typedef struct _cow_nyx_op_hal_ctx_t
{
	u8        *dma_buf; /* 16 KiB aligned buffer for SE XTS */
	lv_obj_t  *bar;
	lv_obj_t  *lbl_pct;
	lv_obj_t  *lbl_log;
} cow_nyx_op_hal_ctx_t;

static lv_obj_t   *s_parent_manage_win = NULL;
static lv_action_t s_parent_reopen_fn  = NULL;
static bool        s_cfg_changed       = false;
static cow_ui_ctx_t *s_ui              = NULL;
static int         s_pending_action_slot = 0;

void gui_cow_set_parent_window(lv_obj_t *manage_win, lv_action_t reopen_fn)
{
	s_parent_manage_win = manage_win;
	s_parent_reopen_fn  = reopen_fn;
}

static void _show_info_mbox(const char *title, const char *body)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\251", "OK", "\251", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);

	char *buf = (char *)malloc(SZ_4K);
	if (buf)
	{
		s_printf(buf, "%s\n\n%s", title, body);
		lv_mbox_set_text(mbox, buf);
		free(buf);
	}
	lv_mbox_add_btns(mbox, mbox_btn_map, nyx_mbox_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
}

/* --------------------------------------------------------------------------
 * FatFS / SD / Tegra SE HAL backend for cow_nyx_core
 * -------------------------------------------------------------------------- */

static int _hal_file_open(void *ctx, const char *path, bool create, u64 *out_size, void **out_handle)
{
	(void)ctx;
	FIL *fp = (FIL *)zalloc(sizeof(FIL));
	if (!fp)
		return -1;
	BYTE mode = FA_READ | FA_WRITE | (create ? FA_CREATE_ALWAYS : FA_OPEN_EXISTING);
	if (f_open(fp, path, mode) != FR_OK)
	{
		free(fp);
		return -1;
	}
	if (out_size)
		*out_size = (u64)f_size(fp);
	*out_handle = fp;
	return 0;
}

static void _hal_file_close(void *ctx, void *handle)
{
	(void)ctx;
	FIL *fp = (FIL *)handle;
	if (!fp)
		return;
	f_sync(fp);
	f_close(fp);
	free(fp);
}

static int _hal_file_read(void *ctx, void *handle, u64 off, void *buf, u32 len)
{
	(void)ctx;
	FIL *fp = (FIL *)handle;
	if (f_lseek(fp, (FSIZE_t)off) != FR_OK)
		return -1;
	u8 *ptr = (u8 *)buf;
	u32 rem = len;
	while (rem > 0U)
	{
		u32 chunk = MIN(rem, SZ_1M);
		UINT br = 0U;
		if (f_read(fp, ptr, chunk, &br) != FR_OK || br != chunk)
			return -1;
		ptr += chunk;
		rem -= chunk;
	}
	return 0;
}

static int _hal_file_write(void *ctx, void *handle, u64 off, const void *buf, u32 len)
{
	(void)ctx;
	FIL *fp = (FIL *)handle;
	if (f_lseek(fp, (FSIZE_t)off) != FR_OK)
		return -1;
	const u8 *ptr = (const u8 *)buf;
	u32 rem = len;
	while (rem > 0U)
	{
		u32 chunk = MIN(rem, SZ_1M);
		UINT bw = 0U;
		if (f_write(fp, ptr, chunk, &bw) != FR_OK || bw != chunk)
			return -1;
		ptr += chunk;
		rem -= chunk;
	}
	return 0;
}

static int _hal_file_grow(void *ctx, void *handle, u64 new_size)
{
	(void)ctx;
	FIL *fp = (FIL *)handle;
	if (new_size == 0ULL)
		return 0;
	u8 zero = 0U;
	UINT bw = 0U;
	if (f_lseek(fp, (FSIZE_t)(new_size - 1ULL)) != FR_OK)
		return -1;
	if (f_write(fp, &zero, 1U, &bw) != FR_OK || bw != 1U)
		return -1;
	if (f_sync(fp) != FR_OK)
		return -1;
	return 0;
}

static int _hal_base_gpp_read(void *ctx, u32 lba, u32 count, void *buf)
{
	(void)ctx;
	emmc_set_partition(EMMC_GPP);
	u8 *ptr = (u8 *)buf;
	u32 done = 0U;
	while (done < count)
	{
		u32 chunk = MIN(count - done, 1024U);
		if (sdmmc_storage_read(&emmc_storage, lba + done, chunk, ptr + done * COW_NYX_SECTOR_SIZE) != 0)
			return -1;
		done += chunk;
	}
	return 0;
}

static int _hal_xts_user_crypt(void *ctx, bool encrypt, u32 part_rel_lba, void *buf, u32 sector_count)
{
	cow_nyx_op_hal_ctx_t *hctx = (cow_nyx_op_hal_ctx_t *)ctx;
	if (!hctx || !hctx->dma_buf)
		return -1;

	u8 tweak[16] __attribute__((aligned(4)));
	u32 done = 0U;
	u8 *ptr = (u8 *)buf;

	while (done < sector_count)
	{
		u32 cur_lba = part_rel_lba + done;
		u32 sub = cur_lba & 0x1FU;
		u32 chunk = MIN(sector_count - done, 32U - sub);
		u64 cluster = (u64)(cur_lba >> 5);
		u32 bytes = chunk * COW_NYX_SECTOR_SIZE;

		memcpy(hctx->dma_buf, ptr + done * COW_NYX_SECTOR_SIZE, bytes);
		if (se_aes_crypt_xts_sec_nx(5U, 4U, encrypt ? ENCRYPT : DECRYPT, cluster,
			tweak, true, sub, hctx->dma_buf, hctx->dma_buf, bytes))
			return -1;
		memcpy(ptr + done * COW_NYX_SECTOR_SIZE, hctx->dma_buf, bytes);
		done += chunk;
	}
	return 0;
}

static u32 _hal_crc32(u32 init_crc, const void *buf, u32 len)
{
	return crc32_calc(init_crc, (const u8 *)buf, len);
}

static void _hal_progress(void *ctx, const char *msg, int pct)
{
	cow_nyx_op_hal_ctx_t *hctx = (cow_nyx_op_hal_ctx_t *)ctx;
	if (!hctx)
		return;
	if (hctx->bar && pct >= 0)
		lv_bar_set_value(hctx->bar, pct);
	if (hctx->lbl_pct && pct >= 0)
	{
		char pbuf[24];
		s_printf(pbuf, " "SYMBOL_DOT" %d%%", pct);
		lv_label_set_text(hctx->lbl_pct, pbuf);
	}
	if (hctx->lbl_log && msg)
	{
		lv_label_ins_text(hctx->lbl_log, LV_LABEL_POS_LAST, msg);
		lv_label_ins_text(hctx->lbl_log, LV_LABEL_POS_LAST, "\n");
	}
	manual_system_maintenance(true);
}

static void _build_hal(cow_nyx_hal_t *hal, cow_nyx_op_hal_ctx_t *hctx, bool with_xts)
{
	memset(hal, 0, sizeof(*hal));
	hal->user_ctx = hctx;
	hal->file_open = _hal_file_open;
	hal->file_close = _hal_file_close;
	hal->file_read = _hal_file_read;
	hal->file_write = _hal_file_write;
	hal->file_grow = _hal_file_grow;
	hal->base_gpp_read = _hal_base_gpp_read;
	hal->xts_user_crypt = with_xts ? _hal_xts_user_crypt : NULL;
	hal->crc32_fn = _hal_crc32;
	hal->progress_fn = (hctx && (hctx->bar || hctx->lbl_log)) ? _hal_progress : NULL;
}

static u64 _get_sd_free_bytes_mounted(void)
{
	DWORD free_clst = 0U;
	FATFS *fs = NULL;
	if (f_getfree("", &free_clst, &fs) != FR_OK || !fs)
		return 0ULL;
	return (u64)free_clst * (u64)fs->csize * COW_NYX_SECTOR_SIZE;
}

static int _derive_bis_keys_muted(void)
{
	bool prev_mute = gfx_con.mute;
	gfx_con.mute = true;
	int ok = emummc_raw_derive_bis_keys();
	gfx_con.mute = prev_mute;
	return ok;
}

static void _clear_bis_keys_safe(void)
{
	hos_bis_keys_clear();
	hos_eks_clear(HOS_MKEY_VER_MAX);
}

static void _write_block_user_content_flag(bool enable)
{
	f_mkdir("atmosphere");
	f_mkdir("atmosphere/flags");

	if (enable)
	{
		f_unlink("atmosphere/flags/allow_user_content_install.flag");
		FIL fp;
		if (f_open(&fp, "atmosphere/flags/block_user_content_install.flag", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
			f_close(&fp);
	}
	else
	{
		f_unlink("atmosphere/flags/block_user_content_install.flag");
		FIL fp;
		if (f_open(&fp, "atmosphere/flags/allow_user_content_install.flag", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
			f_close(&fp);
	}
}

/* --------------------------------------------------------------------------
 * Active summary helper exposed to gui_emummc_tools.c
 * -------------------------------------------------------------------------- */

bool gui_cow_format_active_summary(const char *base_path, char *out_buf, u32 out_sz)
{
	if (!base_path || !*base_path || !out_buf || out_sz < 64U)
		return false;

	char emmc_dir[160];
	if (strlen(base_path) + 6U >= sizeof(emmc_dir))
		return false;
	s_printf(emmc_dir, "%s/eMMC", base_path);

	cow_nyx_hal_t hal;
	_build_hal(&hal, NULL, false);

	cow_nyx_probe_t probe;
	if (cow_nyx_probe(&hal, emmc_dir, &probe) != COW_NYX_OK || !probe.is_cow)
		return false;

	u32 phys_gib = (u32)(probe.phys_base_sectors / 2097152ULL);
	u32 used_mb  = (probe.next_free_slot > probe.first_data_slot)
		? ((probe.next_free_slot - probe.first_data_slot) / 64U) : 0U;
	u32 total_mb = probe.total_slots / 64U;
	u32 max_mb   = probe.max_overlay_slots / 64U;
	const char *pol = (probe.alloc_policy == COW_NYX_ALLOC_THIN) ? "Thin" : "Thick";
	const char *mode = (probe.flags & COW_NYX_FLAG_EPHEMERAL) ? "Ephemeral" : "Persistent";

	s_printf(out_buf,
		"#FFDD00 cowMMC (Exp):# v%d %s (%s) | %d GiB\n"
		"#00DDFF Pool:# %d/%d MiB (cap %d MiB)\n",
		(int)probe.version, pol, mode, (int)phys_gib,
		(int)used_mb, (int)total_mb, (int)max_mb);
	return true;
}

/* --------------------------------------------------------------------------
 * Physical Base Verification & Hashing Helpers (BOOT0/BOOT1/GPT/BCPKG2)
 * -------------------------------------------------------------------------- */

typedef struct _cow_phys_digests_t
{
	u8   emmc_cid_sha256_16[16];
	u8   boot0_pkg1_sha256[32];
	u8   boot0_patrol_sha256_16[16];
	u8   boot1_sha256[32];
	u8   gpt_sha256[32];
	u8   bcpkg2_sha256[32];
	u8   system_digest_sha256[32];
	u8   user_digest_sha256[32];
	u64  phys_gpp_sectors;
	bool gpt_valid;
	bool pkg2_valid;
	bool system_valid;
	bool user_valid;
} cow_phys_digests_t;

static bool _is_zero_bytes(const u8 *buf, u32 len)
{
	for (u32 i = 0U; i < len; i++)
	{
		if (buf[i] != 0U)
			return false;
	}
	return true;
}

static int _compute_unified_base_gen_id(
	const cow_phys_digests_t *d,
	u32 base_hos_version,
	u32 base_pkg1_ver,
	u64 *out_gen_id)
{
	if (!d || !out_gen_id)
		return -1;

	u8 msg[224];
	u32 pos = 0U;
	memcpy(msg + pos, d->boot0_pkg1_sha256, 32U);       pos += 32U;
	memcpy(msg + pos, d->boot0_patrol_sha256_16, 16U);  pos += 16U;
	memcpy(msg + pos, d->boot1_sha256, 32U);            pos += 32U;
	memcpy(msg + pos, d->gpt_sha256, 32U);              pos += 32U;
	memcpy(msg + pos, d->bcpkg2_sha256, 32U);           pos += 32U;
	memcpy(msg + pos, d->system_digest_sha256, 32U);    pos += 32U;
	memcpy(msg + pos, d->user_digest_sha256, 32U);      pos += 32U;

	u64 phys_s = d->phys_gpp_sectors;
	for (u32 i = 0U; i < 8U; i++)
		msg[pos++] = (u8)((phys_s >> (i * 8U)) & 0xFFU);
	for (u32 i = 0U; i < 4U; i++)
		msg[pos++] = (u8)((base_hos_version >> (i * 8U)) & 0xFFU);
	for (u32 i = 0U; i < 4U; i++)
		msg[pos++] = (u8)((base_pkg1_ver >> (i * 8U)) & 0xFFU);

	u8 sha[32];
	if (se_sha_hash_256_oneshot(sha, msg, sizeof(msg)) != 0)
		return -1;

	u64 gid = 0ULL;
	for (u32 i = 0U; i < 8U; i++)
		gid |= ((u64)sha[i]) << (i * 8U);
	*out_gen_id = gid ? gid : 1ULL;
	return 0;
}

static void _ensure_overlay_subdirs_and_config(const char *base_path)
{
	char p[160];
	s_printf(p, "%s/saves", base_path);             f_mkdir(p);
	s_printf(p, "%s/snapshots", base_path);         f_mkdir(p);
	s_printf(p, "%s/cow_state", base_path);         f_mkdir(p);
	s_printf(p, "%s/cow_state/tickets", base_path); f_mkdir(p);

	s_printf(p, "%s/cow_config", base_path);
	if (f_stat(p, NULL) != FR_OK)
	{
		FIL fp;
		if (f_open(&fp, p, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
		{
			static const char *cfg_txt =
				"auto_reset_on_drift=1\n"
				"auto_snapshot=1\n"
				"auto_rollback=1\n"
				"rollback_threshold=2\n";
			UINT bw = 0U;
			f_write(&fp, cfg_txt, (UINT)strlen(cfg_txt), &bw);
			f_sync(&fp);
			f_close(&fp);
		}
	}
}

static int _read_mmc_part_span(u32 part, u32 lba, u32 count, u8 *dst, const char *opt_file_path)
{
	emmc_set_partition(part);
	u32 done = 0U;
	while (done < count)
	{
		u32 chunk = MIN(count - done, 1024U);
		if (sdmmc_storage_read(&emmc_storage, lba + done, chunk, dst + done * COW_NYX_SECTOR_SIZE) != 0)
			return -1;
		done += chunk;
	}

	if (opt_file_path)
	{
		FIL fp;
		if (f_open(&fp, opt_file_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
			return -1;
		u32 rem = count * COW_NYX_SECTOR_SIZE;
		u32 off = 0U;
		while (rem > 0U)
		{
			u32 wchunk = MIN(rem, SZ_1M);
			UINT bw = 0U;
			if (f_write(&fp, dst + off, wchunk, &bw) != FR_OK || bw != wchunk)
			{
				f_close(&fp);
				return -1;
			}
			off += wchunk;
			rem -= wchunk;
		}
		f_sync(&fp);
		f_close(&fp);
	}
	return 0;
}

static bool _utf16_eq_ascii_u16(const u16 *name, const char *ascii)
{
	size_t n = strlen(ascii);
	if (n > 36U)
		return false;
	for (size_t i = 0; i < n; i++)
	{
		if (name[i] != (u16)(u8)ascii[i])
			return false;
	}
	return (n == 36U) || (name[n] == 0U);
}

static int _hash_sha256_streaming(const u8 *buf, u32 total_bytes, u8 *out_digest)
{
	if (total_bytes <= SZ_8M)
		return se_sha_hash_256_oneshot(out_digest, buf, total_bytes);
	u32 off = SZ_8M;
	if (se_sha_hash_256_partial_start(out_digest, buf, SZ_8M, true))
		return -1;
	while (total_bytes - off > SZ_8M)
	{
		if (se_sha_hash_256_partial_update(out_digest, buf + off, SZ_8M, true))
			return -1;
		off += SZ_8M;
	}
	return se_sha_hash_256_partial_end(out_digest, (u64)total_bytes, buf + off, total_bytes - off, true);
}

static int _hash_mmc_part_span(u32 part, u32 start_lba, u32 total_sects, u8 *buf, u8 *out_digest)
{
	if (total_sects == 0U)
		return -1;
	u64 total_bytes = (u64)total_sects * COW_NYX_SECTOR_SIZE;
	if (total_bytes <= COW_HASH_BUF_BYTES)
	{
		if (_read_mmc_part_span(part, start_lba, total_sects, buf, NULL) != 0)
			return -1;
		return _hash_sha256_streaming(buf, (u32)total_bytes, out_digest);
	}

	u32 rem = total_sects;
	u32 cur = start_lba;
	u32 b_secs = COW_HASH_BUF_BYTES / COW_NYX_SECTOR_SIZE;
	u32 first = MIN(rem, b_secs);
	if (_read_mmc_part_span(part, cur, first, buf, NULL) != 0 ||
	    se_sha_hash_256_partial_start(out_digest, buf, first * COW_NYX_SECTOR_SIZE, true))
		return -1;
	cur += first;
	rem -= first;
	while (rem > b_secs)
	{
		if (_read_mmc_part_span(part, cur, b_secs, buf, NULL) != 0 ||
		    se_sha_hash_256_partial_update(out_digest, buf, b_secs * COW_NYX_SECTOR_SIZE, true))
			return -1;
		cur += b_secs;
		rem -= b_secs;
	}
	if (_read_mmc_part_span(part, cur, rem, buf, NULL) != 0 ||
	    se_sha_hash_256_partial_end(out_digest, total_bytes, buf, rem * COW_NYX_SECTOR_SIZE, true))
		return -1;
	return 0;
}

static int _collect_physical_base_digests(
	cow_phys_digests_t *out,
	const char *opt_emmc_dir_for_boot_copy)
{
	memset(out, 0, sizeof(*out));
	u8 *buf = (u8 *)malloc(COW_HASH_BUF_BYTES);
	if (!buf)
		return COW_NYX_ERR_NO_MEM;

	u8 cid_full[32];
	if (se_sha_hash_256_oneshot(cid_full, emmc_storage.raw_cid, sizeof(emmc_storage.raw_cid)) == 0)
		memcpy(out->emmc_cid_sha256_16, cid_full, sizeof(out->emmc_cid_sha256_16));

	char boot_path[180];
	const char *b0_path = NULL;
	const char *b1_path = NULL;
	if (opt_emmc_dir_for_boot_copy)
	{
		s_printf(boot_path, "%s/boot0", opt_emmc_dir_for_boot_copy);
		b0_path = boot_path;
	}

	if (_read_mmc_part_span(1U, 0U, COW_BOOT_PART_SECTORS, buf, b0_path) != 0)
		goto fail;

	u8 patrol_full[32];
	if (se_sha_hash_256_oneshot(patrol_full, buf + COW_BOOT0_PATROL_OFF, COW_BOOT0_PATROL_LEN) != 0)
		goto fail;
	memcpy(out->boot0_patrol_sha256_16, patrol_full, sizeof(out->boot0_patrol_sha256_16));

	memset(buf + COW_BOOT0_BCTSAVE_OFF, 0, COW_BOOT0_BCTSAVE_LEN);
	memset(buf + COW_BOOT0_PATROL_OFF, 0, COW_BOOT0_PATROL_LEN);
	if (se_sha_hash_256_oneshot(out->boot0_pkg1_sha256, buf, COW_BOOT_PART_BYTES) != 0)
		goto fail;

	if (opt_emmc_dir_for_boot_copy)
	{
		s_printf(boot_path, "%s/boot1", opt_emmc_dir_for_boot_copy);
		b1_path = boot_path;
	}
	if (_read_mmc_part_span(2U, 0U, COW_BOOT_PART_SECTORS, buf, b1_path) != 0)
		goto fail;
	if (se_sha_hash_256_oneshot(out->boot1_sha256, buf, COW_BOOT_PART_BYTES) != 0)
		goto fail;

	/* GPP LBA 0..33 (MBR + GPT) */
	if (_read_mmc_part_span(0U, 0U, 34U, buf, NULL) != 0)
		goto fail;
	if (se_sha_hash_256_oneshot(out->gpt_sha256, buf, 34U * COW_NYX_SECTOR_SIZE) != 0)
		goto fail;

	gpt_t *gpt = (gpt_t *)(buf + COW_NYX_SECTOR_SIZE);
	if (!memcmp(&gpt->header.signature, "EFI PART", 8) && gpt->header.num_part_ents <= 128U)
	{
		out->gpt_valid = true;
		out->phys_gpp_sectors = gpt->header.alt_lba ? (gpt->header.alt_lba + 1ULL) : (u64)emmc_storage.sec_cnt;

		const u32 anchor_max_sects = COW_PART_ANCHOR_BYTES / COW_NYX_SECTOR_SIZE;
		u32 pkg2_start = 0U, pkg2_sects = 0U;
		u32 sys_start  = 0U, sys_sects  = 0U;
		u32 usr_start  = 0U, usr_sects  = 0U;
		for (u32 i = 0U; i < gpt->header.num_part_ents; i++)
		{
			u64 ls = gpt->entries[i].lba_start;
			u64 le = gpt->entries[i].lba_end;
			if (le < ls || le >= out->phys_gpp_sectors)
				continue;

			if (_utf16_eq_ascii_u16(gpt->entries[i].name, "BCPKG2-1-Normal-Main"))
			{
				if (le < COW_NYX_DEFAULT_PREFIX_SECTS)
				{
					out->pkg2_valid = true;
					pkg2_start = (u32)ls;
					pkg2_sects = (u32)(le - ls + 1ULL);
				}
			}
			else if (_utf16_eq_ascii_u16(gpt->entries[i].name, "SYSTEM"))
			{
				u64 cnt = le - ls + 1ULL;
				out->system_valid = true;
				sys_start = (u32)ls;
				sys_sects = (cnt > anchor_max_sects) ? anchor_max_sects : (u32)cnt;
			}
			else if (_utf16_eq_ascii_u16(gpt->entries[i].name, "USER"))
			{
				u64 cnt = le - ls + 1ULL;
				out->user_valid = true;
				usr_start = (u32)ls;
				usr_sects = (cnt > anchor_max_sects) ? anchor_max_sects : (u32)cnt;
			}
		}

		if (out->pkg2_valid && _hash_mmc_part_span(0U, pkg2_start, pkg2_sects, buf, out->bcpkg2_sha256) != 0)
			goto fail;
		if (out->system_valid && _hash_mmc_part_span(0U, sys_start, sys_sects, buf, out->system_digest_sha256) != 0)
			goto fail;
		if (out->user_valid && _hash_mmc_part_span(0U, usr_start, usr_sects, buf, out->user_digest_sha256) != 0)
			goto fail;
	}

	emmc_set_partition(0U);
	free(buf);
	return COW_NYX_OK;

fail:
	emmc_set_partition(0U);
	free(buf);
	return COW_NYX_ERR_IO;
}

/* --------------------------------------------------------------------------
 * UI Refresh & Overlay Discovery
 * -------------------------------------------------------------------------- */

static void _refresh_cow_overlay_list(void);

static void _update_detail_panel(void)
{
	if (!s_ui || !s_ui->lbl_detail)
		return;

	if (s_ui->item_count == 0U || s_ui->selected_idx < 0 || (u32)s_ui->selected_idx >= s_ui->item_count)
	{
		lv_label_set_text(s_ui->lbl_detail,
			"#FFDD00 No cowMMC (v2) overlays found yet.#\n\n"
			"Use #C7EA46 Create New cowMMC Overlay# on the right\n"
			"to initialize a sparse experimental overlay\n"
			"alongside your existing emuMMC setups.\n\n"
			"#00DDFF SD Free:# Ready");
		lv_obj_set_click(s_ui->btn_activate, false);
		lv_btn_set_state(s_ui->btn_activate, LV_BTN_STATE_INA);
		lv_obj_set_click(s_ui->btn_guard, false);
		lv_btn_set_state(s_ui->btn_guard, LV_BTN_STATE_INA);
		lv_obj_set_click(s_ui->btn_verify, false);
		lv_btn_set_state(s_ui->btn_verify, LV_BTN_STATE_INA);
		lv_obj_set_click(s_ui->btn_maint, false);
		lv_btn_set_state(s_ui->btn_maint, LV_BTN_STATE_INA);
		return;
	}

	const cow_entry_item_t *it = &s_ui->items[s_ui->selected_idx];
	const cow_nyx_probe_t *p = &it->probe;
	u32 phys_gib = (u32)(p->phys_base_sectors / 2097152ULL);
	u32 used_mb  = (p->next_free_slot > p->first_data_slot) ? ((p->next_free_slot - p->first_data_slot) / 64U) : 0U;
	u32 tot_mb   = p->total_slots / 64U;
	u32 max_mb   = p->max_overlay_slots / 64U;
	u32 sd_free_mb = (u32)(s_ui->sd_free_bytes / (1024ULL * 1024ULL));

	const char *pol = (p->alloc_policy == COW_NYX_ALLOC_THIN) ? "Thin" : "Thick";
	const char *mode = (p->flags & COW_NYX_FLAG_EPHEMERAL) ? "#FFDD00 Ephemeral#" : "#96FF00 Persistent#";
	const char *guard = s_ui->has_allow_flag
		? "#FF8000 ALLOWED (Flag Override)#"
		: (s_ui->has_block_flag ? "#96FF00 BLOCKED (Flag + Auto)#" : "#96FF00 BLOCKED (Auto COW)#");
	const char *health = "#96FF00 Clean#";
	if (p->flags & COW_NYX_FLAG_FW_MISMATCH)
		health = "#FF4444 FW_MISMATCH#";
	else if (p->flags & COW_NYX_FLAG_CONFLICT_LOCKED)
		health = "#FF8000 CONFLICT_LOCKED#";
	else if (p->flags & COW_NYX_FLAG_NEEDS_REBASE)
		health = "#FFDD00 NEEDS_REBASE#";

	u32 used_slots = (p->next_free_slot > p->first_data_slot) ? (p->next_free_slot - p->first_data_slot) : 0U;
	u32 cap_slots  = (p->total_slots > p->first_data_slot) ? (p->total_slots - p->first_data_slot) : 0U;
	bool pool_high = (cap_slots > 0U && ((u64)used_slots * 100ULL >= (u64)cap_slots * 85ULL));
	const char *pool_warn = pool_high ? " #FF4444 [>=85% FULL]#" : "";
	const char *lkg_str = it->has_lkg
		? (it->has_boot_ok ? "#96FF00 LKG Ready (Boot Ack)#" : "#00DDFF LKG Ready#")
		: "#888888 No LKG#";
	const char *mig_str = it->has_migrate_pending
		? " | #FFDD00 Migrate Pending#"
		: "";

	char *buf = (char *)malloc(SZ_2K);
	if (buf)
	{
		s_printf(buf,
			"#00DDFF Selected:# %s  %s\n"
			"#00DDFF Format:# v%d %s | #00DDFF Mode:# %s\n"
			"#00DDFF Base NAND:# %d GiB (Native Geometry)\n"
			"#00DDFF COW Pool:# %d / %d MiB (Max: %d MiB)%s\n"
			"#00DDFF Dirty Blocks:# %d | #00DDFF Seq:# %d\n"
			"#00DDFF SysMem Guard:# %s\n"
			"#00DDFF Status:# %s (Drift: 0x%03X)\n"
			"#00DDFF Recovery:# %s%s\n"
			"#00DDFF SD Free:# %d MiB (512 MiB reserve)",
			it->base_path, it->is_active ? "#96FF00 [ACTIVE]#" : "#888888 [Inactive]#",
			(int)p->version, pol, mode,
			(int)phys_gib,
			(int)used_mb, (int)tot_mb, (int)max_mb, pool_warn,
			(int)p->dirty_data_blocks, (int)p->seq_num,
			guard,
			health, (unsigned)p->drift_mask,
			lkg_str, mig_str,
			(int)sd_free_mb);
		lv_label_set_text(s_ui->lbl_detail, buf);
		free(buf);
	}

	lv_obj_set_click(s_ui->btn_activate, true);
	lv_btn_set_state(s_ui->btn_activate, LV_BTN_STATE_REL);
	lv_obj_set_click(s_ui->btn_guard, true);
	lv_btn_set_state(s_ui->btn_guard, LV_BTN_STATE_REL);
	lv_obj_set_click(s_ui->btn_verify, true);
	lv_btn_set_state(s_ui->btn_verify, LV_BTN_STATE_REL);
	lv_obj_set_click(s_ui->btn_maint, true);
	lv_btn_set_state(s_ui->btn_maint, LV_BTN_STATE_REL);
}

static lv_res_t _on_select_overlay(lv_obj_t *btn)
{
	if (!s_ui)
		return LV_RES_OK;
	u32 idx = (u32)(uintptr_t)lv_obj_get_free_num(btn);
	if (idx < s_ui->item_count)
	{
		s_ui->selected_idx = (int)idx;
		_update_detail_panel();
	}
	return LV_RES_OK;
}

static void _refresh_cow_overlay_list(void)
{
	if (!s_ui)
		return;

	s_ui->item_count = 0U;
	s_ui->selected_idx = -1;
	s_ui->active_path[0] = 0;

	if (sd_mount())
	{
		_update_detail_panel();
		return;
	}

	emummc_cfg_t cfg;
	load_emummc_cfg(&cfg);
	if (cfg.enabled && !cfg.sector && cfg.path)
	{
		size_t plen = strlen(cfg.path);
		if (plen < sizeof(s_ui->active_path))
			strcpy(s_ui->active_path, cfg.path);
	}
	if (cfg.path)
		free(cfg.path);
	if (cfg.nintendo_path)
		free(cfg.nintendo_path);

	s_ui->sd_free_bytes = _get_sd_free_bytes_mounted();
	s_ui->has_block_flag = (f_stat("atmosphere/flags/block_user_content_install.flag", NULL) == FR_OK);
	s_ui->has_allow_flag = (f_stat("atmosphere/flags/allow_user_content_install.flag", NULL) == FR_OK);

	cow_nyx_hal_t hal;
	_build_hal(&hal, NULL, false);

	dirlist_t *dl = dirlist("emuMMC", NULL, DIR_SHOW_DIRS | DIR_ASCII_ORDER);
	if (dl)
	{
		u32 idx = 0U;
		while (dl->name[idx] && s_ui->item_count < COW_MAX_DISCOVERED)
		{
			cow_entry_item_t *it = &s_ui->items[s_ui->item_count];
			memset(it, 0, sizeof(*it));
			s_printf(it->name, "%s", dl->name[idx]);
			s_printf(it->base_path, "emuMMC/%s", dl->name[idx]);
			s_printf(it->emmc_dir, "emuMMC/%s/eMMC", dl->name[idx]);

			if (cow_nyx_probe(&hal, it->emmc_dir, &it->probe) == COW_NYX_OK && it->probe.is_cow)
			{
				char spath[160];
				s_printf(spath, "%s/snapshots/boot_lkg.cow", it->base_path);
				it->has_lkg = (f_stat(spath, NULL) == FR_OK);
				s_printf(spath, "%s/boot_ok", it->base_path);
				it->has_boot_ok = (f_stat(spath, NULL) == FR_OK);
				s_printf(spath, "%s/post_reset_migrate.flag", it->base_path);
				it->has_migrate_pending = (f_stat(spath, NULL) == FR_OK);

				it->is_active = (strcmp(it->base_path, s_ui->active_path) == 0);
				if (it->is_active || s_ui->selected_idx < 0)
					s_ui->selected_idx = (int)s_ui->item_count;
				s_ui->item_count++;
			}
			idx++;
		}
		free(dl);
	}

	sd_unmount();

	if (s_ui->list_overlays)
	{
		lv_list_clean(s_ui->list_overlays);
		char lbl[128];
		for (u32 i = 0U; i < s_ui->item_count; i++)
		{
			cow_entry_item_t *it = &s_ui->items[i];
			u32 bg = (u32)(it->probe.phys_base_sectors / 2097152ULL);
			s_printf(lbl, "%s%s (v%d %s %dG)",
				it->is_active ? SYMBOL_OK" " : "",
				it->name,
				(int)it->probe.version,
				(it->probe.alloc_policy == COW_NYX_ALLOC_THIN) ? "Thin" : "Thick",
				(int)bg);
			lv_obj_t *b = lv_list_add(s_ui->list_overlays, NULL, lbl, _on_select_overlay);
			lv_obj_set_free_num(b, i);
		}
	}

	_update_detail_panel();
}

/* --------------------------------------------------------------------------
 * Progress Window Wrapper for Long Operations
 * -------------------------------------------------------------------------- */

typedef struct _cow_progress_win_t
{
	lv_obj_t *win;
	lv_obj_t *bar;
	lv_obj_t *lbl_pct;
	lv_obj_t *lbl_log;
	lv_obj_t *lbl_finish;
} cow_progress_win_t;

static lv_res_t _close_progress_win(lv_obj_t *btn)
{
	nyx_win_close_action(btn);
	_refresh_cow_overlay_list();
	return LV_RES_INV;
}

static void _open_progress_win(const char *title, cow_progress_win_t *pw)
{
	memset(pw, 0, sizeof(*pw));
	pw->win = nyx_create_standard_window(title, _close_progress_win);
	nyx_window_toggle_buttons(pw->win, true);

	pw->bar = lv_bar_create(pw->win, NULL);
	lv_obj_set_size(pw->bar, (LV_HOR_RES / 10) * 6, LV_DPI / 4);
	lv_bar_set_range(pw->bar, 0, 100);
	lv_bar_set_value(pw->bar, 0);
	lv_obj_align(pw->bar, NULL, LV_ALIGN_IN_TOP_LEFT, LV_DPI / 2, LV_DPI * 2 / 3);

	pw->lbl_pct = lv_label_create(pw->win, NULL);
	lv_label_set_text(pw->lbl_pct, " "SYMBOL_DOT" 0%");
	lv_obj_align(pw->lbl_pct, pw->bar, LV_ALIGN_OUT_RIGHT_MID, LV_DPI / 6, 0);

	pw->lbl_log = lv_label_create(pw->win, NULL);
	lv_label_set_recolor(pw->lbl_log, true);
	lv_label_set_long_mode(pw->lbl_log, LV_LABEL_LONG_BREAK);
	lv_obj_set_width(pw->lbl_log, (LV_HOR_RES / 10) * 8);
	lv_label_set_text(pw->lbl_log, "");
	lv_obj_align(pw->lbl_log, pw->bar, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	pw->lbl_finish = lv_label_create(pw->win, NULL);
	lv_label_set_recolor(pw->lbl_finish, true);
	lv_label_set_text(pw->lbl_finish, "");
	lv_obj_align(pw->lbl_finish, pw->bar, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI * 3);

	manual_system_maintenance(true);
}

/* --------------------------------------------------------------------------
 * Action: Create New Experimental cowMMC Overlay
 * -------------------------------------------------------------------------- */

static void _find_next_cow_name(char *out_name, size_t out_sz)
{
	(void)out_sz;
	char pbuf[128];
	for (u32 i = 0U; i < 100U; i++)
	{
		s_printf(pbuf, "emuMMC/COW%d", (int)i);
		if (f_stat(pbuf, NULL) != FR_OK)
		{
			s_printf(out_name, "COW%d", (int)i);
			return;
		}
	}
	s_printf(out_name, "COW99");
}

static int _write_v2_sb_slot(cow_nyx_sb_t *hdr, const u32 *l1_table, FIL *fp, u64 slot_off)
{
	u8 *blk = (u8 *)zalloc(COW_NYX_BLOCK_BYTES);
	if (!blk)
		return -1;
	u32 l1_bytes = hdr->l1_entries * 4U;
	hdr->l1_crc32 = crc32_calc(0, (const u8 *)l1_table, l1_bytes);
	hdr->header_crc32 = crc32_calc(0, (const u8 *)hdr, offsetof(cow_nyx_sb_t, header_crc32));
	memcpy(blk, hdr, sizeof(*hdr));
	int rc = 0;
	UINT bw = 0U;
	if (f_lseek(fp, (FSIZE_t)slot_off) != FR_OK ||
	    f_write(fp, blk, COW_NYX_BLOCK_BYTES, &bw) != FR_OK ||
	    bw != COW_NYX_BLOCK_BYTES)
		rc = -1;
	free(blk);
	return rc;
}

static void _execute_create_preset(bool ephemeral, bool apply_balloon)
{
	if (!nyx_emmc_check_battery_enough())
		return;

	cow_progress_win_t pw;
	_open_progress_win(SYMBOL_DRIVE"  Create Experimental cowMMC", &pw);

	cow_nyx_op_hal_ctx_t hctx = { 0 };
	hctx.dma_buf = (u8 *)malloc(COW_NYX_BLOCK_BYTES);
	hctx.bar = pw.bar;
	hctx.lbl_pct = pw.lbl_pct;
	hctx.lbl_log = pw.lbl_log;

	u8 *buf = (u8 *)malloc(SZ_512K);
	bool sd_ok = false, emmc_ok = false, bis_loaded = false;
	char new_name[32] = { 0 };
	char base_path[96] = { 0 };
	char emmc_dir[112] = { 0 };

	if (!hctx.dma_buf || !buf)
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Out of memory!#\n");
		goto finish;
	}
	if (sd_mount())
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed to mount SD card!#\n");
		goto finish;
	}
	sd_ok = true;
	if (emmc_initialize(false))
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed to init eMMC!#\n");
		goto finish;
	}
	emmc_ok = true;

	f_mkdir("emuMMC");
	_find_next_cow_name(new_name, sizeof(new_name));
	s_printf(base_path, "emuMMC/%s", new_name);
	s_printf(emmc_dir, "%s/eMMC", base_path);
	f_mkdir(base_path);
	f_mkdir(emmc_dir);

	char nin_dir[112];
	s_printf(nin_dir, "%s/Nintendo", base_path);
	f_mkdir(nin_dir);

	u64 sd_free = _get_sd_free_bytes_mounted();
	u64 reserve = (u64)COW_NYX_SD_RESERVE_DEFAULT_MB * 1024ULL * 1024ULL;
	u64 overhead = (COW_BOOT_PART_BYTES * 2ULL) + (u64)COW_NYX_DEFAULT_PREFIX_SECTS * COW_NYX_SECTOR_SIZE;
	if (sd_free <= reserve + overhead + 128ULL * 1024ULL * 1024ULL)
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST,
			"#FF0000 Insufficient SD free space (512 MiB safety reserve required)!#\n");
		goto finish;
	}
	u64 usable_bytes = sd_free - reserve - overhead;
	u32 max_safe_slots = (u32)MIN(usable_bytes / COW_NYX_BLOCK_BYTES, 0xFFFFFFFFULL);
	u32 init_target_slots = 256U * 64U; /* 256 MiB initial thin pool */
	u32 total_slots = MIN(init_target_slots, max_safe_slots);

	_hal_progress(&hctx, "Hashing physical BOOT0/BOOT1/GPT/BCPKG2...", 10);
	cow_phys_digests_t dig;
	if (_collect_physical_base_digests(&dig, emmc_dir) != COW_NYX_OK || !dig.gpt_valid || !dig.pkg2_valid)
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Physical eMMC digest collection failed!#\n");
		goto finish;
	}

	u64 phys_secs = dig.phys_gpp_sectors;
	u64 gpp_secs = phys_secs;
	u32 l1_ents = COW_NYX_V2_DEF_L1_ENTRIES;
	u32 l1_slots = COW_NYX_L1_SLOTS_FOR(l1_ents);
	u32 l1_a = COW_NYX_SUPERBLOCK_SLOTS;
	u32 l1_b = l1_a + l1_slots;
	u32 first_data = l1_b + l1_slots;
	if (total_slots <= first_data)
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Pool too small for L1 metadata!#\n");
		goto finish;
	}

	_hal_progress(&hctx, "Copying 32 MiB physical prefix & writing v2 header...", 25);
	char p0[140];
	s_printf(p0, "%s/00", emmc_dir);
	FIL fp;
	if (f_open(&fp, p0, FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK)
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed creating eMMC/00!#\n");
		goto finish;
	}
	u64 pfx_bytes = (u64)COW_NYX_DEFAULT_PREFIX_SECTS * COW_NYX_SECTOR_SIZE;
	u64 archive_sz = pfx_bytes + (u64)total_slots * COW_NYX_BLOCK_BYTES;
	u8 zero = 0U;
	UINT bw = 0U;
	if (f_lseek(&fp, (FSIZE_t)(archive_sz - 1ULL)) != FR_OK ||
	    f_write(&fp, &zero, 1U, &bw) != FR_OK || bw != 1U ||
	    f_lseek(&fp, 0) != FR_OK)
	{
		f_close(&fp);
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed preallocating eMMC/00!#\n");
		goto finish;
	}

	emmc_set_partition(EMMC_GPP);
	for (u32 lba = 0U; lba < COW_NYX_DEFAULT_PREFIX_SECTS; lba += 1024U)
	{
		u32 cur = MIN(COW_NYX_DEFAULT_PREFIX_SECTS - lba, 1024U);
		if (sdmmc_storage_read(&emmc_storage, lba, cur, buf) != 0 ||
		    f_write(&fp, buf, cur * COW_NYX_SECTOR_SIZE, &bw) != FR_OK || bw != cur * COW_NYX_SECTOR_SIZE)
		{
			f_close(&fp);
			lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed writing 32 MiB prefix!#\n");
			goto finish;
		}
	}

	u32 *empty_l1 = (u32 *)zalloc(l1_ents * sizeof(u32));
	if (!empty_l1)
	{
		f_close(&fp);
		goto finish;
	}

	cow_nyx_sb_t sb;
	memset(&sb, 0, sizeof(sb));
	sb.magic = COW_NYX_MAGIC;
	sb.version = COW_NYX_VERSION_V2;
	sb.seq_num = 1ULL;
	sb.gpp_total_sectors = gpp_secs;
	sb.prefix_sectors = COW_NYX_DEFAULT_PREFIX_SECTS;
	sb.total_slots = total_slots;
	sb.next_free_slot = first_data;
	sb.flags = COW_NYX_FLAG_THIN_PROVISION | (ephemeral ? COW_NYX_FLAG_EPHEMERAL : 0U);
	sb.header_ext_size = COW_NYX_SECTOR_SIZE;
	sb.physical_base_sectors = phys_secs;
	sb.max_overlay_slots = max_safe_slots;
	sb.l1_entries = l1_ents;
	sb.l1_slot_a = l1_a;
	sb.l1_slot_b = l1_b;
	sb.first_data_slot = first_data;
	sb.thin_grow_step_slots = 256U * 64U;
	sb.thin_high_watermark_pct = 80U;
	sb.alloc_policy = COW_NYX_ALLOC_THIN;
	sb.block_shift = COW_NYX_BLOCK_SHIFT;
	sb.l2_shift = COW_NYX_L2_SHIFT;
	sb.part_max_sectors = COW_NYX_PART_MAX_DEFAULT_MB * 2048U;
	memcpy(sb.emmc_cid_sha256_16, dig.emmc_cid_sha256_16, 16U);
	memcpy(sb.boot0_pkg1_sha256, dig.boot0_pkg1_sha256, 32U);
	memcpy(sb.boot0_patrol_sha256_16, dig.boot0_patrol_sha256_16, 16U);
	memcpy(sb.boot1_sha256, dig.boot1_sha256, 32U);
	memcpy(sb.gpt_sha256, dig.gpt_sha256, 32U);
	memcpy(sb.bcpkg2_sha256, dig.bcpkg2_sha256, 32U);
	memcpy(sb.system_digest_sha256, dig.system_digest_sha256, 32U);
	memcpy(sb.user_digest_sha256, dig.user_digest_sha256, 32U);
	memcpy(sb.overlay_name, new_name, MIN(strlen(new_name), sizeof(sb.overlay_name) - 1U));
	u64 unified_gen = 0ULL;
	if (_compute_unified_base_gen_id(&dig, 0U, 0U, &unified_gen) == 0)
		sb.base_gen_id = unified_gen;
	else
		sb.base_gen_id = phys_secs ? phys_secs : 1ULL;
	sb.last_verified_base_gen_id = sb.base_gen_id;

	for (u32 slot = 0U; slot < COW_NYX_SUPERBLOCK_SLOTS; slot++)
	{
		sb.seq_num = (u64)(slot + 1U);
		if (_write_v2_sb_slot(&sb, empty_l1, &fp, pfx_bytes + (u64)slot * COW_NYX_BLOCK_BYTES) != 0)
		{
			free(empty_l1);
			f_close(&fp);
			lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed writing COW v2 superblock!#\n");
			goto finish;
		}
	}
	free(empty_l1);

	/* Explicitly zero L1_A and L1_B slots (FatFS f_lseek past EOF does not zero clusters). */
	memset(hctx.dma_buf, 0, COW_NYX_BLOCK_BYTES);
	if (f_lseek(&fp, (FSIZE_t)(pfx_bytes + (u64)l1_a * COW_NYX_BLOCK_BYTES)) != FR_OK)
	{
		f_close(&fp);
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed seeking L1 metadata region!#\n");
		goto finish;
	}
	for (u32 s = 0U; s < 2U * l1_slots; s++)
	{
		bw = 0U;
		if (f_write(&fp, hctx.dma_buf, COW_NYX_BLOCK_BYTES, &bw) != FR_OK || bw != COW_NYX_BLOCK_BYTES)
		{
			f_close(&fp);
			lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed zeroing L1 metadata region!#\n");
			goto finish;
		}
	}
	f_sync(&fp);
	f_close(&fp);

	/* file_based marker + lifecycle directories + cow_config + enable SysMem install block flag */
	char mpath[140];
	s_printf(mpath, "%s/file_based", base_path);
	if (f_open(&fp, mpath, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
		f_close(&fp);

	_ensure_overlay_subdirs_and_config(base_path);
	_write_block_user_content_flag(true);

	if (apply_balloon)
	{
		_hal_progress(&hctx, "Deriving SE BIS keys (muted) for 1G Save-Reserve...", 55);
		if (!_derive_bis_keys_muted())
		{
			lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST,
				"#FF8000 BIS key derivation failed! Skipping FAT32 save-reserve balloon.#\n");
			goto done_ok;
		}
		bis_loaded = true;

		cow_nyx_hal_t ohal;
		_build_hal(&ohal, &hctx, true);
		cow_nyx_vol_t vol;
		if (cow_nyx_open(&ohal, emmc_dir, &vol) == COW_NYX_OK)
		{
			cow_nyx_balloon_res_t bres;
			int br = cow_nyx_sync_headroom(&vol, _get_sd_free_bytes_mounted(),
				COW_NYX_SD_RESERVE_DEFAULT_MB, COW_NYX_SYSMEM_RESERVE_DEFAULT_MB, &bres);
			if (br == COW_NYX_OK)
			{
				char msg[160];
				s_printf(msg, "#96FF00 Applied 1G Save-Reserve balloon (%d bal clusters, %d MiB avail).#\n",
					(int)bres.new_balloon_clusters,
					(int)(bres.effective_free_bytes / (1024ULL * 1024ULL)));
				_hal_progress(&hctx, msg, 95);
			}
			else
			{
				lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF8000 Save-reserve ballooning aborted cleanly.#\n");
			}
			cow_nyx_close(&vol);
		}
	}

done_ok:
	save_emummc_cfg(0U, 0U, base_path);
	s_cfg_changed = true;
	_hal_progress(&hctx, "#96FF00 Created & activated in emummc.ini!#", 100);
	lv_label_set_text(pw.lbl_finish, "#96FF00 Finished! You can close this window.#");

finish:
	if (bis_loaded)
		_clear_bis_keys_safe();
	if (emmc_ok)
		emmc_end();
	if (sd_ok)
		sd_unmount();
	free(buf);
	free(hctx.dma_buf);
	nyx_window_toggle_buttons(pw.win, false);
}

static lv_res_t _mbox_create_preset_action(lv_obj_t *btns, const char *txt)
{
	u32 pressed = lv_btnm_get_pressed(btns);
	nyx_mbox_action(btns, txt);
	switch (pressed)
	{
	case 0: _execute_create_preset(false, true); break;
	case 1: _execute_create_preset(true, true); break;
	case 2: _execute_create_preset(false, false); break;
	default: break;
	}
	return LV_RES_INV;
}

static lv_res_t _on_btn_create_new(lv_obj_t *btn)
{
	(void)btn;
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *btn_map[] = {
		"\222Thin Persistent",
		"\222Ephemeral",
		"\222Thin (No Bal)",
		"\222Cancel",
		""
	};
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES * 8 / 10);
	lv_mbox_set_text(mbox,
		"#FFDD00 Create Experimental cowMMC Overlay#\n\n"
		"Auto-selects next free #C7EA46 emuMMC/COWn# at native NAND geometry.\n"
		"Default presets enable #00DDFF fs_mitm SysMem Guard# + #00DDFF 1G Save-Reserve# FAT32\n"
		"ballooning (Option A+B) so game installs route cleanly to SD Card.");
	lv_mbox_add_btns(mbox, btn_map, _mbox_create_preset_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
	return LV_RES_OK;
}

/* --------------------------------------------------------------------------
 * Action: Activate Overlay in /emuMMC/emummc.ini
 * -------------------------------------------------------------------------- */

static lv_res_t _on_btn_activate(lv_obj_t *btn)
{
	(void)btn;
	if (!s_ui || s_ui->selected_idx < 0 || (u32)s_ui->selected_idx >= s_ui->item_count)
		return LV_RES_OK;

	char path_copy[96];
	strcpy(path_copy, s_ui->items[s_ui->selected_idx].base_path);
	save_emummc_cfg(0U, 0U, path_copy);
	sd_unmount();
	s_cfg_changed = true;
	_refresh_cow_overlay_list();
	_show_info_mbox("#96FF00 Overlay Activated#", path_copy);
	return LV_RES_OK;
}

/* --------------------------------------------------------------------------
 * Action: On-Device SysMem Guard (fs_mitm flag + Save-Reserve Balloon Sync)
 * -------------------------------------------------------------------------- */

static void _execute_sysmem_guard(u32 cap_mb)
{
	if (!s_ui || s_ui->selected_idx < 0 || (u32)s_ui->selected_idx >= s_ui->item_count)
		return;
	if (!nyx_emmc_check_battery_enough())
		return;

	char emmc_dir[112];
	strcpy(emmc_dir, s_ui->items[s_ui->selected_idx].emmc_dir);

	cow_progress_win_t pw;
	_open_progress_win(SYMBOL_SD"  SysMem Save-Reserve Balloon", &pw);

	cow_nyx_op_hal_ctx_t hctx = { 0 };
	hctx.dma_buf = (u8 *)malloc(COW_NYX_BLOCK_BYTES);
	hctx.bar = pw.bar;
	hctx.lbl_pct = pw.lbl_pct;
	hctx.lbl_log = pw.lbl_log;

	bool sd_ok = false, emmc_ok = false, bis_loaded = false;
	if (!hctx.dma_buf)
		goto finish;
	if (sd_mount())
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed to mount SD!#\n");
		goto finish;
	}
	sd_ok = true;
	if (emmc_initialize(false))
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed to init eMMC!#\n");
		goto finish;
	}
	emmc_ok = true;

	if (cap_mb > 0U)
		_write_block_user_content_flag(true);

	_hal_progress(&hctx, "Deriving SE BIS keys silently (hardware SE slots)...", 15);
	if (!_derive_bis_keys_muted())
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed BIS key derivation / CAL0 verification!#\n");
		goto finish;
	}
	bis_loaded = true;

	cow_nyx_hal_t hal;
	_build_hal(&hal, &hctx, true);
	cow_nyx_vol_t vol;
	if (cow_nyx_open(&hal, emmc_dir, &vol) != COW_NYX_OK)
	{
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, "#FF0000 Failed opening COW overlay!#\n");
		goto finish;
	}

	u64 sd_free = _get_sd_free_bytes_mounted();
	cow_nyx_balloon_res_t bres;
	int br = cow_nyx_sync_headroom(&vol, sd_free, COW_NYX_SD_RESERVE_DEFAULT_MB, cap_mb, &bres);
	cow_nyx_close(&vol);

	if (br == COW_NYX_OK)
	{
		char msg[192];
		s_printf(msg,
			"#96FF00 Save-Reserve synced!#\n"
			"  /__COW__ balloon: %d -> %d clusters (%d part(s))\n"
			"  HOS System Memory free: %d MiB (cap: %s)",
			(int)bres.old_balloon_clusters,
			(int)bres.new_balloon_clusters,
			(int)bres.num_balloon_parts,
			(int)(bres.effective_free_bytes / (1024ULL * 1024ULL)),
			(cap_mb > 0U) ? "save-reserve" : "uncapped");
		_hal_progress(&hctx, msg, 100);
		lv_label_set_text(pw.lbl_finish, "#96FF00 Finished! You can close this window.#");
	}
	else
	{
		char msg[96];
		s_printf(msg, "#FF0000 Save-reserve sync error (%d)!#\n", br);
		lv_label_ins_text(pw.lbl_log, LV_LABEL_POS_LAST, msg);
	}

finish:
	if (bis_loaded)
		_clear_bis_keys_safe();
	if (emmc_ok)
		emmc_end();
	if (sd_ok)
		sd_unmount();
	free(hctx.dma_buf);
	nyx_window_toggle_buttons(pw.win, false);
}

static lv_res_t _mbox_guard_action(lv_obj_t *btns, const char *txt)
{
	u32 pressed = lv_btnm_get_pressed(btns);
	nyx_mbox_action(btns, txt);
	switch (pressed)
	{
	case 0: _execute_sysmem_guard(512U); break;
	case 1: _execute_sysmem_guard(1024U); break;
	case 2: _execute_sysmem_guard(2048U); break;
	case 3: _execute_sysmem_guard(0U); break;
	default: break;
	}
	return LV_RES_INV;
}

static lv_res_t _on_btn_sysmem_guard(lv_obj_t *btn)
{
	(void)btn;
	if (!s_ui || s_ui->selected_idx < 0 || (u32)s_ui->selected_idx >= s_ui->item_count)
		return LV_RES_OK;

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *btn_map[] = {
		"\222512M Saves",
		"\2221G Saves",
		"\2222G Saves",
		"\222Unballoon",
		"\222Cancel",
		""
	};
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES * 8 / 10);
	lv_mbox_set_text(mbox,
		"#FFDD00 SysMem Install Guard & Save-Reserve Balloon#\n\n"
		"Keeps #00DDFF USER:/save/# healthy while preventing System Memory\n"
		"game installs even under stock Atmosphère package3:\n"
		"  #00DDFF 512M / 1G / 2G Saves:# balloon /__COW__/Rxx.BIN + enable block flag\n"
		"  #00DDFF Unballoon:# remove /__COW__ balloon (uncapped by reserve)");
	lv_mbox_add_btns(mbox, btn_map, _mbox_guard_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
	return LV_RES_OK;
}

/* --------------------------------------------------------------------------
 * Action: Verify Physical Base Digests (BOOT0/BOOT1/GPT/BCPKG2)
 * -------------------------------------------------------------------------- */

static lv_res_t _on_btn_verify_base(lv_obj_t *btn)
{
	(void)btn;
	if (!s_ui || s_ui->selected_idx < 0 || (u32)s_ui->selected_idx >= s_ui->item_count)
		return LV_RES_OK;

	char emmc_dir[112];
	strcpy(emmc_dir, s_ui->items[s_ui->selected_idx].emmc_dir);

	if (sd_mount())
		return LV_RES_OK;
	if (emmc_initialize(false))
	{
		sd_unmount();
		return LV_RES_OK;
	}

	cow_phys_digests_t live;
	int drc = _collect_physical_base_digests(&live, NULL);
	emmc_end();

	cow_nyx_hal_t hal;
	_build_hal(&hal, NULL, false);
	cow_nyx_vol_t vol;
	int orc = cow_nyx_open(&hal, emmc_dir, &vol);

	if (drc != COW_NYX_OK || orc != COW_NYX_OK)
	{
		if (orc == COW_NYX_OK) cow_nyx_close(&vol);
		sd_unmount();
		_show_info_mbox("#FF0000 Verification Error#", "Failed reading live physical eMMC or COW header.");
		return LV_RES_OK;
	}

	u32 drift = 0U;
	if (!_is_zero_bytes(vol.hdr.boot0_pkg1_sha256, 32U) &&
	    memcmp(live.boot0_pkg1_sha256, vol.hdr.boot0_pkg1_sha256, 32U) != 0)
		drift |= COW_NYX_DRIFT_BOOT0_PKG1;
	if (!_is_zero_bytes(vol.hdr.boot0_patrol_sha256_16, 16U) &&
	    memcmp(live.boot0_patrol_sha256_16, vol.hdr.boot0_patrol_sha256_16, 16U) != 0)
		drift |= COW_NYX_DRIFT_BOOT0_PATROL;
	if (!_is_zero_bytes(vol.hdr.boot1_sha256, 32U) &&
	    memcmp(live.boot1_sha256, vol.hdr.boot1_sha256, 32U) != 0)
		drift |= COW_NYX_DRIFT_BOOT1;
	if (!_is_zero_bytes(vol.hdr.gpt_sha256, 32U) &&
	    (!live.gpt_valid || memcmp(live.gpt_sha256, vol.hdr.gpt_sha256, 32U) != 0))
		drift |= COW_NYX_DRIFT_GPT;
	if (!_is_zero_bytes(vol.hdr.bcpkg2_sha256, 32U) &&
	    (!live.pkg2_valid || memcmp(live.bcpkg2_sha256, vol.hdr.bcpkg2_sha256, 32U) != 0))
		drift |= COW_NYX_DRIFT_BCPKG2;
	if (!_is_zero_bytes(vol.hdr.system_digest_sha256, 32U) &&
	    (!live.system_valid || memcmp(live.system_digest_sha256, vol.hdr.system_digest_sha256, 32U) != 0))
		drift |= COW_NYX_DRIFT_SYSTEM;
	if (!_is_zero_bytes(vol.hdr.user_digest_sha256, 32U) &&
	    (!live.user_valid || memcmp(live.user_digest_sha256, vol.hdr.user_digest_sha256, 32U) != 0))
		drift |= COW_NYX_DRIFT_USER;

	u32 hard_drift = drift & ~COW_NYX_DRIFT_BOOT0_PATROL;
	vol.hdr.base_drift_part_mask = drift;
	if (drift == 0U)
	{
		vol.hdr.flags &= ~(COW_NYX_FLAG_NEEDS_REBASE | COW_NYX_FLAG_FW_MISMATCH);
		vol.hdr.last_verified_base_gen_id = vol.hdr.base_gen_id;
	}
	else if (hard_drift != 0U)
	{
		vol.hdr.flags |= (COW_NYX_FLAG_FW_MISMATCH | COW_NYX_FLAG_NEEDS_REBASE);
	}
	else
	{
		vol.hdr.flags = (vol.hdr.flags & ~COW_NYX_FLAG_FW_MISMATCH) | COW_NYX_FLAG_NEEDS_REBASE;
	}
	vol.dirty_hdr = true;
	cow_nyx_commit(&vol);
	cow_nyx_close(&vol);
	sd_unmount();

	char report[640];
	s_printf(report,
		"#00DDFF BOOT0 Pkg1:# %s\n"
		"#00DDFF BOOT0 Patrol:# %s\n"
		"#00DDFF BOOT1:# %s\n"
		"#00DDFF GPT:# %s\n"
		"#00DDFF BCPKG2:# %s\n"
		"#00DDFF SYSTEM (16M):# %s\n"
		"#00DDFF USER (16M):# %s\n\n"
		"%s",
		(drift & COW_NYX_DRIFT_BOOT0_PKG1) ? "#FF4444 DRIFT#" : "#96FF00 MATCH#",
		(drift & COW_NYX_DRIFT_BOOT0_PATROL) ? "#FFDD00 CHANGED#" : "#96FF00 MATCH#",
		(drift & COW_NYX_DRIFT_BOOT1) ? "#FF4444 DRIFT#" : "#96FF00 MATCH#",
		(drift & COW_NYX_DRIFT_GPT) ? "#FF4444 DRIFT#" : "#96FF00 MATCH#",
		(drift & COW_NYX_DRIFT_BCPKG2) ? "#FF4444 DRIFT#" : "#96FF00 MATCH#",
		(drift & COW_NYX_DRIFT_SYSTEM) ? "#FF4444 DRIFT#" : "#96FF00 MATCH#",
		(drift & COW_NYX_DRIFT_USER) ? "#FF4444 DRIFT#" : "#96FF00 MATCH#",
		(drift == 0U)
			? "#96FF00 Base matches physical sysMMC cleanly!#"
			: ((hard_drift != 0U)
				? "#FF4444 Hard base drift detected! Re-baseline overlay before booting.#"
				: "#FF8000 BOOT0 Patrol sector changed (non-fatal).#"));

	_refresh_cow_overlay_list();
	_show_info_mbox("#00DDFF Base Digest Verification#", report);
	return LV_RES_OK;
}

/* --------------------------------------------------------------------------
 * Action: Maintenance (Toggle Ephemeral, Grow Pool, Reset Dirty Blocks)
 * -------------------------------------------------------------------------- */

static void _do_toggle_mode(void)
{
	if (!s_ui || s_ui->selected_idx < 0)
		return;
	if (sd_mount())
		return;
	cow_nyx_hal_t hal;
	_build_hal(&hal, NULL, false);
	cow_nyx_vol_t vol;
	int rc = cow_nyx_open(&hal, s_ui->items[s_ui->selected_idx].emmc_dir, &vol);
	if (rc == COW_NYX_OK)
	{
		u32 nf = vol.hdr.flags ^ COW_NYX_FLAG_EPHEMERAL;
		cow_nyx_set_flags(&vol, nf);
		cow_nyx_close(&vol);
	}
	sd_unmount();
	_refresh_cow_overlay_list();
}

static void _do_grow_pool(u32 grow_mb)
{
	if (!s_ui || s_ui->selected_idx < 0)
		return;
	if (sd_mount())
		return;
	cow_nyx_hal_t hal;
	_build_hal(&hal, NULL, false);
	cow_nyx_vol_t vol;
	int rc = cow_nyx_open(&hal, s_ui->items[s_ui->selected_idx].emmc_dir, &vol);
	if (rc == COW_NYX_OK)
	{
		u64 sd_free = _get_sd_free_bytes_mounted();
		rc = cow_nyx_grow_pool(&vol, grow_mb * 64U, sd_free, COW_NYX_SD_RESERVE_DEFAULT_MB);
		cow_nyx_close(&vol);
	}
	sd_unmount();
	_refresh_cow_overlay_list();
	if (rc != COW_NYX_OK)
	{
		char msg[128];
		s_printf(msg, "Pool growth refused or failed (code %d). Check 512 MiB SD reserve or max cap.", rc);
		_show_info_mbox("#FF8000 Grow Pool#", msg);
	}
}

static void _do_restore_lkg(void)
{
	if (!s_ui || s_ui->selected_idx < 0 || (u32)s_ui->selected_idx >= s_ui->item_count)
		return;
	if (sd_mount())
		return;

	char base_path[96];
	char emmc_dir[112];
	strcpy(base_path, s_ui->items[s_ui->selected_idx].base_path);
	strcpy(emmc_dir, s_ui->items[s_ui->selected_idx].emmc_dir);

	cow_nyx_hal_t hal;
	_build_hal(&hal, NULL, false);
	cow_nyx_vol_t vol;
	int rc = cow_nyx_open(&hal, emmc_dir, &vol);
	if (rc == COW_NYX_OK)
	{
		char lkg_path[160];
		char prev_path[160];
		s_printf(lkg_path, "%s/snapshots/boot_lkg.cow", base_path);
		s_printf(prev_path, "%s/snapshots/boot_prev.cow", base_path);

		rc = cow_nyx_restore_raw_snapshot(&vol, lkg_path, NULL);
		if (rc != COW_NYX_OK)
			rc = cow_nyx_restore_raw_snapshot(&vol, prev_path, NULL);

		if (rc == COW_NYX_OK)
		{
			char pend_path[160];
			s_printf(pend_path, "%s/boot_pending", base_path);
			f_unlink(pend_path);
		}
		cow_nyx_close(&vol);
	}
	sd_unmount();
	_refresh_cow_overlay_list();
	_show_info_mbox(
		(rc == COW_NYX_OK) ? "#96FF00 LKG Snapshot Restored#" : "#FF4444 Restore LKG Failed#",
		(rc == COW_NYX_OK)
			? "Overlay restored to Last-Known-Good boot snapshot and boot_pending cleared."
			: "No valid or matching LKG snapshot found in snapshots/boot_lkg.cow.");
}

static lv_res_t _mbox_confirm_reset_action(lv_obj_t *btns, const char *txt)
{
	u32 pressed = lv_btnm_get_pressed(btns);
	nyx_mbox_action(btns, txt);
	if (pressed != 0 || !s_ui || s_pending_action_slot < 0 || (u32)s_pending_action_slot >= s_ui->item_count)
		return LV_RES_INV;

	char base_path[96];
	char emmc_dir[112];
	strcpy(base_path, s_ui->items[s_pending_action_slot].base_path);
	strcpy(emmc_dir, s_ui->items[s_pending_action_slot].emmc_dir);

	if (sd_mount())
		return LV_RES_INV;

	bool emmc_ok = (emmc_initialize(false) == 0);
	cow_phys_digests_t dig;
	memset(&dig, 0, sizeof(dig));
	bool dig_ok = false;
	if (emmc_ok)
	{
		if (_collect_physical_base_digests(&dig, emmc_dir) == COW_NYX_OK && dig.gpt_valid && dig.pkg2_valid)
			dig_ok = true;
	}

	/* Re-copy 32 MiB physical prefix into eMMC/00 without truncating COW pool */
	if (dig_ok)
	{
		u8 *pbuf = (u8 *)malloc(SZ_512K);
		if (pbuf)
		{
			char p0[140];
			s_printf(p0, "%s/00", emmc_dir);
			FIL fp;
			if (f_open(&fp, p0, FA_OPEN_EXISTING | FA_READ | FA_WRITE) == FR_OK)
			{
				emmc_set_partition(EMMC_GPP);
				for (u32 lba = 0U; lba < COW_NYX_DEFAULT_PREFIX_SECTS; lba += 1024U)
				{
					u32 cur = MIN(COW_NYX_DEFAULT_PREFIX_SECTS - lba, 1024U);
					UINT bw = 0U;
					if (sdmmc_storage_read(&emmc_storage, lba, cur, pbuf) != 0 ||
					    f_write(&fp, pbuf, cur * COW_NYX_SECTOR_SIZE, &bw) != FR_OK ||
					    bw != cur * COW_NYX_SECTOR_SIZE)
					{
						dig_ok = false;
						break;
					}
				}
				f_sync(&fp);
				f_close(&fp);
			}
			free(pbuf);
		}
	}

	cow_nyx_op_hal_ctx_t hctx = { 0 };
	hctx.dma_buf = (u8 *)malloc(COW_NYX_BLOCK_BYTES);
	cow_nyx_hal_t hal;
	_build_hal(&hal, &hctx, false);
	cow_nyx_vol_t vol;
	int rc = cow_nyx_open(&hal, emmc_dir, &vol);
	if (rc == COW_NYX_OK)
	{
		rc = cow_nyx_reset_overlay(&vol);
		if (rc == COW_NYX_OK)
		{
			if (dig_ok)
			{
				vol.hdr.gpp_total_sectors = dig.phys_gpp_sectors;
				vol.hdr.physical_base_sectors = dig.phys_gpp_sectors;
				memcpy(vol.hdr.emmc_cid_sha256_16, dig.emmc_cid_sha256_16, 16U);
				memcpy(vol.hdr.boot0_pkg1_sha256, dig.boot0_pkg1_sha256, 32U);
				memcpy(vol.hdr.boot0_patrol_sha256_16, dig.boot0_patrol_sha256_16, 16U);
				memcpy(vol.hdr.boot1_sha256, dig.boot1_sha256, 32U);
				memcpy(vol.hdr.gpt_sha256, dig.gpt_sha256, 32U);
				memcpy(vol.hdr.bcpkg2_sha256, dig.bcpkg2_sha256, 32U);
				memcpy(vol.hdr.system_digest_sha256, dig.system_digest_sha256, 32U);
				memcpy(vol.hdr.user_digest_sha256, dig.user_digest_sha256, 32U);
				u64 new_gen = 0ULL;
				if (_compute_unified_base_gen_id(&dig, vol.hdr.base_hos_version, vol.hdr.base_pkg1_ver, &new_gen) == 0)
					vol.hdr.base_gen_id = new_gen;
				vol.hdr.last_verified_base_gen_id = vol.hdr.base_gen_id;
			}
			vol.hdr.base_drift_part_mask = 0U;
			vol.hdr.last_rebase_seq = vol.hdr.seq_num + 1ULL;
			u32 clear_flags = vol.hdr.flags & ~(COW_NYX_FLAG_FW_MISMATCH | COW_NYX_FLAG_CONFLICT_LOCKED | COW_NYX_FLAG_NEEDS_REBASE);
			rc = cow_nyx_set_flags(&vol, clear_flags);
		}
		cow_nyx_close(&vol);
	}

	/* Re-apply 1G Save-Reserve balloon if BIS keys derive cleanly */
	if (rc == COW_NYX_OK && emmc_ok && hctx.dma_buf && _derive_bis_keys_muted())
	{
		cow_nyx_hal_t xhal;
		_build_hal(&xhal, &hctx, true);
		if (cow_nyx_open(&xhal, emmc_dir, &vol) == COW_NYX_OK)
		{
			cow_nyx_balloon_res_t bres;
			cow_nyx_sync_headroom(&vol, _get_sd_free_bytes_mounted(),
				COW_NYX_SD_RESERVE_DEFAULT_MB, COW_NYX_SYSMEM_RESERVE_DEFAULT_MB, &bres);
			cow_nyx_close(&vol);
		}
		_clear_bis_keys_safe();
	}

	if (rc == COW_NYX_OK)
	{
		_ensure_overlay_subdirs_and_config(base_path);
		char p[160];
		s_printf(p, "%s/boot_pending", base_path);            f_unlink(p);
		s_printf(p, "%s/boot_ok", base_path);                 f_unlink(p);
		s_printf(p, "%s/snapshots/boot_lkg.cow", base_path);  f_unlink(p);
		s_printf(p, "%s/snapshots/boot_prev.cow", base_path); f_unlink(p);
		s_printf(p, "%s/post_reset_migrate.flag", base_path);
		FIL fp;
		if (f_open(&fp, p, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
		{
			UINT bw = 0U;
			f_write(&fp, "1\n", 2U, &bw);
			f_close(&fp);
		}
	}

	free(hctx.dma_buf);
	if (emmc_ok)
		emmc_end();
	sd_unmount();
	_refresh_cow_overlay_list();
	_show_info_mbox("#96FF00 Overlay Re-baselined#",
		(rc == COW_NYX_OK)
			? "Dirty COW blocks cleared, BOOT0/BOOT1/32M prefix + all 7 digests refreshed, and 1G Save-Reserve synced."
			: "Re-baseline failed!");
	return LV_RES_INV;
}

static void _prompt_reset_overlay(void)
{
	if (!s_ui || s_ui->selected_idx < 0)
		return;
	s_pending_action_slot = s_ui->selected_idx;

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *btn_map[] = { "\222Re-baseline", "\222Cancel", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);

	const cow_nyx_probe_t *p = &s_ui->items[s_ui->selected_idx].probe;
	lv_mbox_set_text(mbox,
		(p->flags & COW_NYX_FLAG_FW_MISMATCH)
			? "#FF4444 WARNING: FW_MISMATCH Detected!#\n\nRe-baselining discards overlay blocks, refreshes BOOT0/BOOT1/prefix/digests from physical sysMMC, and preserves <overlay>/saves & Nintendo SD content."
			: "#FF8000 Confirm Overlay Re-baseline#\n\nDiscard dirty COW blocks and re-sync base to current physical sysMMC?");
	lv_mbox_add_btns(mbox, btn_map, _mbox_confirm_reset_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
}

static lv_res_t _mbox_maint_action(lv_obj_t *btns, const char *txt)
{
	u32 pressed = lv_btnm_get_pressed(btns);
	nyx_mbox_action(btns, txt);
	switch (pressed)
	{
	case 0: _do_toggle_mode(); break;
	case 1: _do_grow_pool(512U); break;
	case 2: _do_grow_pool(1024U); break;
	case 3: _do_restore_lkg(); break;
	case 4: _prompt_reset_overlay(); break;
	default: break;
	}
	return LV_RES_INV;
}

static lv_res_t _on_btn_maint(lv_obj_t *btn)
{
	(void)btn;
	if (!s_ui || s_ui->selected_idx < 0 || (u32)s_ui->selected_idx >= s_ui->item_count)
		return LV_RES_OK;

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *btn_map[] = {
		"\222Mode",
		"\222+512M Pool",
		"\222+1G Pool",
		"\222Restore LKG",
		"\222Re-baseline",
		"\222Cancel",
		""
	};
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES * 8 / 10);
	lv_mbox_set_text(mbox,
		"#FFDD00 Overlay Maintenance#\n\n"
		"Toggle Persistent/Ephemeral, expand pool slots on SD,\n"
		"restore Last-Known-Good snapshot, or re-baseline to physical sysMMC.");
	lv_mbox_add_btns(mbox, btn_map, _mbox_maint_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
	return LV_RES_OK;
}

/* --------------------------------------------------------------------------
 * Main Window Entry Point
 * -------------------------------------------------------------------------- */

static lv_res_t _action_cow_win_close(lv_obj_t *btn)
{
	bool need_relaunch = s_cfg_changed;
	s_cfg_changed = false;
	nyx_win_close_action(btn);

	if (s_ui)
	{
		free(s_ui);
		s_ui = NULL;
	}
	if (need_relaunch && s_parent_manage_win && s_parent_reopen_fn)
	{
		lv_obj_del(s_parent_manage_win);
		s_parent_manage_win = NULL;
		(*s_parent_reopen_fn)(NULL);
	}
	return LV_RES_INV;
}

lv_res_t create_win_cow_tools(lv_obj_t *btn)
{
	(void)btn;
	s_cfg_changed = false;
	s_ui = (cow_ui_ctx_t *)zalloc(sizeof(cow_ui_ctx_t));
	if (!s_ui)
		return LV_RES_OK;

	lv_obj_t *win = nyx_create_standard_window(SYMBOL_MODULES"  Experimental: cowMMC", _action_cow_win_close);
	s_ui->win = win;

	static lv_style_t h_style;
	lv_style_copy(&h_style, &lv_style_transp);
	h_style.body.padding.inner = 0;
	h_style.body.padding.hor = LV_DPI - (LV_DPI / 4);
	h_style.body.padding.ver = LV_DPI / 9;

	/* Left container: Overlay Selector & Telemetry */
	lv_obj_t *h1 = lv_cont_create(win, NULL);
	lv_cont_set_style(h1, &h_style);
	lv_cont_set_fit(h1, false, true);
	lv_obj_set_width(h1, (LV_HOR_RES / 9) * 4);
	lv_obj_set_click(h1, false);
	lv_cont_set_layout(h1, LV_LAYOUT_OFF);

	lv_obj_t *lbl_h1 = lv_label_create(h1, NULL);
	lv_label_set_static_text(lbl_h1, "cowMMC Overlays (/emuMMC/*)");
	lv_obj_set_style(lbl_h1, lv_theme_get_current()->label.prim);
	lv_obj_align(lbl_h1, h1, LV_ALIGN_IN_TOP_LEFT, LV_DPI / 4, LV_DPI / 10);

	lv_obj_t *line1 = lv_line_create(h1, NULL);
	static const lv_point_t line_pp[] = { {0, 0}, { LV_HOR_RES - (LV_DPI - (LV_DPI / 4)) * 2, 0} };
	lv_line_set_points(line1, line_pp, 2);
	lv_line_set_style(line1, lv_theme_get_current()->line.decor);
	lv_obj_align(line1, lbl_h1, LV_ALIGN_OUT_BOTTOM_LEFT, -(LV_DPI / 4), LV_DPI / 8);

	s_ui->list_overlays = lv_list_create(h1, NULL);
	lv_obj_set_size(s_ui->list_overlays, LV_HOR_RES * 38 / 100, LV_DPI * 22 / 10);
	lv_list_set_single_mode(s_ui->list_overlays, true);
	lv_obj_align(s_ui->list_overlays, line1, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, LV_DPI / 5);

	s_ui->lbl_detail = lv_label_create(h1, NULL);
	lv_label_set_recolor(s_ui->lbl_detail, true);
	lv_obj_set_style(s_ui->lbl_detail, &hint_small_style);
	lv_label_set_text(s_ui->lbl_detail, "");
	lv_obj_align(s_ui->lbl_detail, s_ui->list_overlays, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);

	/* Right container: Experimental Setup & Tools */
	lv_obj_t *h2 = lv_cont_create(win, NULL);
	lv_cont_set_style(h2, &h_style);
	lv_cont_set_fit(h2, false, true);
	lv_obj_set_width(h2, (LV_HOR_RES / 9) * 4);
	lv_obj_set_click(h2, false);
	lv_cont_set_layout(h2, LV_LAYOUT_OFF);
	lv_obj_align(h2, h1, LV_ALIGN_OUT_RIGHT_TOP, LV_DPI * 17 / 29, 0);

	lv_obj_t *lbl_h2 = lv_label_create(h2, NULL);
	lv_label_set_static_text(lbl_h2, "Experimental Actions");
	lv_obj_set_style(lbl_h2, lv_theme_get_current()->label.prim);
	lv_obj_align(lbl_h2, h2, LV_ALIGN_IN_TOP_LEFT, LV_DPI / 4, LV_DPI / 10);

	lv_obj_t *line2 = lv_line_create(h2, line1);
	lv_obj_align(line2, lbl_h2, LV_ALIGN_OUT_BOTTOM_LEFT, -(LV_DPI / 4), LV_DPI / 8);

	lv_obj_t *btn_create = lv_btn_create(h2, NULL);
	lv_btn_set_fit(btn_create, true, true);
	lv_label_set_static_text(lv_label_create(btn_create, NULL), SYMBOL_DRIVE"  Create New cowMMC Overlay...");
	lv_obj_align(btn_create, line2, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, LV_DPI / 5);
	lv_btn_set_action(btn_create, LV_BTN_ACTION_CLICK, _on_btn_create_new);

	s_ui->btn_activate = lv_btn_create(h2, NULL);
	lv_btn_set_fit(s_ui->btn_activate, true, true);
	lv_label_set_static_text(lv_label_create(s_ui->btn_activate, NULL), SYMBOL_OK"  Activate in emummc.ini");
	lv_obj_align(s_ui->btn_activate, btn_create, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);
	lv_btn_set_action(s_ui->btn_activate, LV_BTN_ACTION_CLICK, _on_btn_activate);

	s_ui->btn_guard = lv_btn_create(h2, NULL);
	lv_btn_set_fit(s_ui->btn_guard, true, true);
	lv_label_set_static_text(lv_label_create(s_ui->btn_guard, NULL), SYMBOL_SD"  SysMem Guard (Balloon)...");
	lv_obj_align(s_ui->btn_guard, s_ui->btn_activate, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);
	lv_btn_set_action(s_ui->btn_guard, LV_BTN_ACTION_CLICK, _on_btn_sysmem_guard);

	s_ui->btn_verify = lv_btn_create(h2, NULL);
	lv_btn_set_fit(s_ui->btn_verify, true, true);
	lv_label_set_static_text(lv_label_create(s_ui->btn_verify, NULL), SYMBOL_REFRESH"  Verify Physical Base");
	lv_obj_align(s_ui->btn_verify, s_ui->btn_guard, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);
	lv_btn_set_action(s_ui->btn_verify, LV_BTN_ACTION_CLICK, _on_btn_verify_base);

	s_ui->btn_maint = lv_btn_create(h2, NULL);
	lv_btn_set_fit(s_ui->btn_maint, true, true);
	lv_label_set_static_text(lv_label_create(s_ui->btn_maint, NULL), SYMBOL_TOOLS"  Maintenance...");
	lv_obj_align(s_ui->btn_maint, s_ui->btn_verify, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);
	lv_btn_set_action(s_ui->btn_maint, LV_BTN_ACTION_CLICK, _on_btn_maint);

	_refresh_cow_overlay_list();
	return LV_RES_OK;
}
