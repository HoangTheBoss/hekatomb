/*
 * Copyright (c) 2026 HOS_COW Project
 *
 * Portable cowMMC Overlay & Save-Reserve FAT32 Ballooning Core
 */

#include "cow_nyx_core.h"

#ifdef __HOST_COW_TEST__
#include <stdio.h>
#define _cow_fmt2(dst, idx) snprintf((dst), 4, "%02u", (unsigned)(idx))
#else
void s_printf(char *out_buf, const char *fmt, ...);
#define _cow_fmt2(dst, idx) s_printf((dst), "%02d", (int)(idx))
#endif

static inline u16 _rd_le16(const void *p)
{
	const u8 *b = (const u8 *)p;
	return (u16)b[0] | ((u16)b[1] << 8);
}

static inline u32 _rd_le32(const void *p)
{
	const u8 *b = (const u8 *)p;
	return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

static inline u64 _rd_le64(const void *p)
{
	return (u64)_rd_le32(p) | ((u64)_rd_le32((const u8 *)p + 4) << 32);
}

static inline void _wr_le16(void *p, u16 v)
{
	u8 *b = (u8 *)p;
	b[0] = (u8)(v & 0xFFU);
	b[1] = (u8)((v >> 8) & 0xFFU);
}

static inline void _wr_le32(void *p, u32 v)
{
	u8 *b = (u8 *)p;
	b[0] = (u8)(v & 0xFFU);
	b[1] = (u8)((v >> 8) & 0xFFU);
	b[2] = (u8)((v >> 16) & 0xFFU);
	b[3] = (u8)((v >> 24) & 0xFFU);
}

static u32 _sw_crc32(u32 init_crc, const void *buf, u32 len)
{
	const u8 *p = (const u8 *)buf;
	u32 crc = ~init_crc;
	for (u32 i = 0; i < len; i++)
	{
		crc ^= p[i];
		for (u32 b = 0; b < 8U; b++)
			crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
	}
	return ~crc;
}

u32 cow_nyx_crc32(const cow_nyx_hal_t *hal, u32 init_crc, const void *buf, u32 len)
{
	if (hal && hal->crc32_fn)
		return hal->crc32_fn(init_crc, buf, len);
	return _sw_crc32(init_crc, buf, len);
}

static void _build_part_path(char *out, size_t out_sz, const char *emmc_dir, u32 part_idx)
{
	size_t n = strlen(emmc_dir);
	if (n + 4U >= out_sz)
		n = (out_sz > 5U) ? (out_sz - 5U) : 0U;
	memcpy(out, emmc_dir, n);
	out[n++] = '/';
	char num[4] = { 0 };
	_cow_fmt2(num, part_idx);
	out[n++] = num[0];
	out[n++] = num[1];
	out[n] = 0;
}

static void _close_handles(const cow_nyx_hal_t *hal, void **handles, u32 num_parts)
{
	for (u32 i = 0; i < num_parts; i++)
	{
		if (handles[i])
		{
			hal->file_close(hal->user_ctx, handles[i]);
			handles[i] = NULL;
		}
	}
}

static int _split_rw(
	const cow_nyx_hal_t *hal,
	void *const *handles,
	const u64 *part_sizes,
	u32 num_parts,
	u64 abs_off,
	void *buf,
	u32 len,
	bool is_write)
{
	u64 cur = abs_off;
	u32 rem = len;
	u8 *ptr = (u8 *)buf;
	u64 part_start = 0ULL;

	for (u32 i = 0; i < num_parts && rem > 0U; i++)
	{
		u64 part_end = part_start + part_sizes[i];
		if (cur < part_end)
		{
			u64 in_off = cur - part_start;
			u64 avail = part_end - cur;
			u32 take = (avail < (u64)rem) ? (u32)avail : rem;
			int rc = is_write
				? hal->file_write(hal->user_ctx, handles[i], in_off, ptr, take)
				: hal->file_read(hal->user_ctx, handles[i], in_off, ptr, take);
			if (rc != 0)
				return COW_NYX_ERR_IO;
			ptr += take;
			cur += take;
			rem -= take;
		}
		part_start = part_end;
	}

	return (rem == 0U) ? COW_NYX_OK : COW_NYX_ERR_IO;
}

static bool _validate_sb_struct(
	const cow_nyx_hal_t *hal,
	const cow_nyx_sb_t *hdr,
	u64 archive_bytes,
	u32 sb_slot,
	u32 *out_l1_entries,
	u32 *out_first_data)
{
	if (hdr->magic != COW_NYX_MAGIC || hdr->prefix_sectors != COW_NYX_DEFAULT_PREFIX_SECTS)
		return false;

	u64 pfx_bytes = (u64)hdr->prefix_sectors * COW_NYX_SECTOR_SIZE;
	if (archive_bytes <= pfx_bytes)
		return false;
	u64 backing_slots = (archive_bytes - pfx_bytes) / COW_NYX_BLOCK_BYTES;

	if (cow_nyx_crc32(hal, 0, hdr, offsetof(cow_nyx_sb_t, header_crc32)) != hdr->header_crc32)
		return false;
	if (hdr->gpp_total_sectors == 0ULL)
		return false;

	if (hdr->version != COW_NYX_VERSION_V2)
		return false;

	if (hdr->header_ext_size != COW_NYX_SECTOR_SIZE ||
	    hdr->block_shift != COW_NYX_BLOCK_SHIFT ||
	    hdr->l2_shift != COW_NYX_L2_SHIFT)
		return false;
	if (hdr->alloc_policy != COW_NYX_ALLOC_THICK && hdr->alloc_policy != COW_NYX_ALLOC_THIN)
		return false;
	if (hdr->l1_entries < COW_NYX_MIN_L1_ENTRIES || hdr->l1_entries > COW_NYX_L1_ENTRIES_MAX ||
	    (hdr->l1_entries % 128U) != 0U)
		return false;
	if (hdr->physical_base_sectors == 0ULL || hdr->gpp_total_sectors != hdr->physical_base_sectors)
		return false;

	u32 l1_slots = COW_NYX_L1_SLOTS_FOR(hdr->l1_entries);
	if (hdr->l1_slot_a < COW_NYX_SUPERBLOCK_SLOTS || hdr->l1_slot_b < COW_NYX_SUPERBLOCK_SLOTS)
		return false;
	bool non_overlap =
		((u64)hdr->l1_slot_a + l1_slots <= (u64)hdr->l1_slot_b) ||
		((u64)hdr->l1_slot_b + l1_slots <= (u64)hdr->l1_slot_a);
	if (!non_overlap)
		return false;
	if ((u64)hdr->l1_slot_a + l1_slots > hdr->first_data_slot ||
	    (u64)hdr->l1_slot_b + l1_slots > hdr->first_data_slot)
		return false;
	if (hdr->total_slots <= hdr->first_data_slot)
		return false;
	if (hdr->max_overlay_slots != 0U && hdr->total_slots > hdr->max_overlay_slots)
		return false;
	u32 my_l1_slot = (sb_slot == 0U) ? hdr->l1_slot_a : hdr->l1_slot_b;
	if ((u64)my_l1_slot + l1_slots > backing_slots)
		return false;

	u32 l1_entries = hdr->l1_entries;
	u32 first_data = hdr->first_data_slot;

	u64 max_l1_sects = (u64)l1_entries * COW_NYX_L2_ENTRIES * COW_NYX_BLOCK_SECTORS;
	if (max_l1_sects < hdr->gpp_total_sectors)
		return false;
	if ((u64)hdr->total_slots > backing_slots)
		return false;
	if (hdr->next_free_slot < first_data || hdr->next_free_slot > hdr->total_slots)
		return false;

	*out_l1_entries = l1_entries;
	*out_first_data = first_data;
	return true;
}

static int _scan_and_open_parts(
	const cow_nyx_hal_t *hal,
	const char *emmc_dir,
	void **handles,
	u64 *part_sizes,
	u32 *out_num_parts,
	u64 *out_archive_bytes)
{
	char fpath[300];
	u32 count = 0U;
	u64 total = 0ULL;

	memset(handles, 0, sizeof(void *) * COW_NYX_MAX_SPLIT_PARTS);
	for (u32 i = 0U; i < COW_NYX_MAX_SPLIT_PARTS; i++)
	{
		u64 sz = 0ULL;
		void *h = NULL;
		_build_part_path(fpath, sizeof(fpath), emmc_dir, i);
		if (hal->file_open(hal->user_ctx, fpath, false, &sz, &h) != 0)
			break;
		handles[i] = h;
		part_sizes[i] = sz;
		total += sz;
		count++;
	}

	if (count == 0U || total <= (u64)COW_NYX_DEFAULT_PREFIX_SECTS * COW_NYX_SECTOR_SIZE + 2ULL * COW_NYX_BLOCK_BYTES)
	{
		_close_handles(hal, handles, count);
		return COW_NYX_ERR_NOT_COW;
	}

	*out_num_parts = count;
	*out_archive_bytes = total;
	return COW_NYX_OK;
}

static int _pick_active_sb(
	const cow_nyx_hal_t *hal,
	void *const *handles,
	const u64 *part_sizes,
	u32 num_parts,
	u64 archive_bytes,
	cow_nyx_sb_t *out_sb,
	u32 *out_sb_slot,
	u32 *out_l1_entries,
	u32 *out_first_data,
	u32 *opt_out_l1)
{
	u8 *slot_buf = (u8 *)malloc(COW_NYX_BLOCK_BYTES);
	u32 *tmp_l1 = (u32 *)malloc(COW_NYX_L1_ENTRIES_MAX * sizeof(u32));
	if (!slot_buf || !tmp_l1)
	{
		free(slot_buf);
		free(tmp_l1);
		return COW_NYX_ERR_NO_MEM;
	}

	bool found = false;
	cow_nyx_sb_t best_sb = { 0 };
	u32 best_slot = 0U;
	u32 best_l1_entries = 0U;
	u32 best_first_data = 0U;
	u64 pfx_bytes = (u64)COW_NYX_DEFAULT_PREFIX_SECTS * COW_NYX_SECTOR_SIZE;

	for (u32 slot = 0U; slot < COW_NYX_SUPERBLOCK_SLOTS; slot++)
	{
		if (_split_rw(hal, handles, part_sizes, num_parts,
			pfx_bytes + (u64)slot * COW_NYX_BLOCK_BYTES, slot_buf, COW_NYX_BLOCK_BYTES, false) != COW_NYX_OK)
			continue;

		const cow_nyx_sb_t *cand = (const cow_nyx_sb_t *)slot_buf;
		u32 cand_l1_ents = 0U, cand_first = 0U;
		if (!_validate_sb_struct(hal, cand, archive_bytes, slot, &cand_l1_ents, &cand_first))
			continue;

		u32 l1_bytes = cand_l1_ents * 4U;
		u32 l1_slot = (slot == 0U) ? cand->l1_slot_a : cand->l1_slot_b;
		if (_split_rw(hal, handles, part_sizes, num_parts,
			pfx_bytes + (u64)l1_slot * COW_NYX_BLOCK_BYTES, tmp_l1, l1_bytes, false) != COW_NYX_OK)
			continue;

		if (cow_nyx_crc32(hal, 0, tmp_l1, l1_bytes) != cand->l1_crc32)
			continue;

		if (!found || cand->seq_num > best_sb.seq_num)
		{
			found = true;
			best_sb = *cand;
			best_slot = slot;
			best_l1_entries = cand_l1_ents;
			best_first_data = cand_first;
			if (opt_out_l1)
				memcpy(opt_out_l1, tmp_l1, l1_bytes);
		}
	}

	free(slot_buf);
	free(tmp_l1);
	if (!found)
		return COW_NYX_ERR_NOT_COW;

	*out_sb = best_sb;
	*out_sb_slot = best_slot;
	*out_l1_entries = best_l1_entries;
	*out_first_data = best_first_data;
	return COW_NYX_OK;
}

int cow_nyx_probe(const cow_nyx_hal_t *hal, const char *emmc_dir, cow_nyx_probe_t *out_probe)
{
	if (!hal || !emmc_dir || !out_probe)
		return COW_NYX_ERR_ARG;

	memset(out_probe, 0, sizeof(*out_probe));
	void *handles[COW_NYX_MAX_SPLIT_PARTS];
	u64 part_sizes[COW_NYX_MAX_SPLIT_PARTS];
	u32 num_parts = 0U;
	u64 archive_bytes = 0ULL;

	int rc = _scan_and_open_parts(hal, emmc_dir, handles, part_sizes, &num_parts, &archive_bytes);
	if (rc != COW_NYX_OK)
		return rc;

	cow_nyx_sb_t sb;
	u32 sb_slot = 0U, l1_ents = 0U, first_data = 0U;
	rc = _pick_active_sb(hal, handles, part_sizes, num_parts, archive_bytes,
		&sb, &sb_slot, &l1_ents, &first_data, NULL);
	_close_handles(hal, handles, num_parts);
	if (rc != COW_NYX_OK)
		return rc;

	u64 backing_slots = (archive_bytes - (u64)sb.prefix_sectors * COW_NYX_SECTOR_SIZE) / COW_NYX_BLOCK_BYTES;
	u32 eff_slots = (u32)MIN(backing_slots, (u64)sb.total_slots);
	if (sb.max_overlay_slots > 0U)
		eff_slots = MIN(eff_slots, sb.max_overlay_slots);

	out_probe->is_cow = true;
	out_probe->version = sb.version;
	out_probe->seq_num = sb.seq_num;
	out_probe->gpp_total_sectors = sb.gpp_total_sectors;
	out_probe->phys_base_sectors = (sb.physical_base_sectors > 0ULL)
		? sb.physical_base_sectors
		: sb.gpp_total_sectors;
	out_probe->total_slots = sb.total_slots;
	out_probe->effective_total_slots = eff_slots;
	out_probe->max_overlay_slots = (sb.max_overlay_slots > 0U)
		? sb.max_overlay_slots
		: sb.total_slots;
	out_probe->next_free_slot = sb.next_free_slot;
	out_probe->first_data_slot = first_data;
	out_probe->dirty_data_blocks = sb.dirty_data_blocks;
	out_probe->dirty_l2_pages = sb.dirty_l2_pages;
	out_probe->flags = sb.flags;
	out_probe->alloc_policy = sb.alloc_policy;
	out_probe->drift_mask = sb.base_drift_part_mask;
	out_probe->part_max_sectors = (sb.part_max_sectors > 0U)
		? sb.part_max_sectors
		: (COW_NYX_PART_MAX_DEFAULT_MB * 2048U);
	out_probe->num_parts = num_parts;
	out_probe->archive_bytes = archive_bytes;
	out_probe->active_sb_slot = sb_slot;
	memcpy(out_probe->overlay_name, sb.overlay_name, sizeof(out_probe->overlay_name));
	out_probe->overlay_name[sizeof(out_probe->overlay_name) - 1U] = 0;

	return COW_NYX_OK;
}

int cow_nyx_open(const cow_nyx_hal_t *hal, const char *emmc_dir, cow_nyx_vol_t *out_vol)
{
	if (!hal || !emmc_dir || !out_vol)
		return COW_NYX_ERR_ARG;

	memset(out_vol, 0, sizeof(*out_vol));
	out_vol->hal = *hal;
	size_t dir_len = strlen(emmc_dir);
	if (dir_len >= sizeof(out_vol->emmc_dir))
		return COW_NYX_ERR_ARG;
	memcpy(out_vol->emmc_dir, emmc_dir, dir_len + 1U);

	u64 archive_bytes = 0ULL;
	int rc = _scan_and_open_parts(hal, emmc_dir, out_vol->part_handles, out_vol->part_sizes,
		&out_vol->num_parts, &archive_bytes);
	if (rc != COW_NYX_OK)
		return rc;

	out_vol->l1_table = (u32 *)zalloc(COW_NYX_L1_ENTRIES_MAX * sizeof(u32));
	out_vol->scratch_blk = (u8 *)malloc(COW_NYX_BLOCK_BYTES);
	out_vol->xts_scratch = (u8 *)malloc(COW_NYX_BLOCK_BYTES);
	if (!out_vol->l1_table || !out_vol->scratch_blk || !out_vol->xts_scratch)
	{
		cow_nyx_close(out_vol);
		return COW_NYX_ERR_NO_MEM;
	}

	for (u32 w = 0U; w < COW_NYX_L2_CACHE_WAYS; w++)
	{
		out_vol->l2_cache[w].entries = (u32 *)zalloc(COW_NYX_BLOCK_BYTES);
		if (!out_vol->l2_cache[w].entries)
		{
			cow_nyx_close(out_vol);
			return COW_NYX_ERR_NO_MEM;
		}
	}

	rc = _pick_active_sb(hal, out_vol->part_handles, out_vol->part_sizes, out_vol->num_parts, archive_bytes,
		&out_vol->hdr, &out_vol->active_sb_slot, &out_vol->l1_entries, &out_vol->first_data_slot, out_vol->l1_table);
	if (rc != COW_NYX_OK)
	{
		cow_nyx_close(out_vol);
		return rc;
	}

	u64 backing_slots = (archive_bytes - (u64)out_vol->hdr.prefix_sectors * COW_NYX_SECTOR_SIZE) / COW_NYX_BLOCK_BYTES;
	u32 eff = (u32)MIN(backing_slots, (u64)out_vol->hdr.total_slots);
	if (out_vol->hdr.version == COW_NYX_VERSION_V2 && out_vol->hdr.max_overlay_slots > 0U)
		eff = MIN(eff, out_vol->hdr.max_overlay_slots);
	out_vol->effective_total_slots = eff;
	out_vol->phys_base_sectors = (out_vol->hdr.version == COW_NYX_VERSION_V2 && out_vol->hdr.physical_base_sectors > 0ULL)
		? out_vol->hdr.physical_base_sectors
		: out_vol->hdr.gpp_total_sectors;
	out_vol->opened = true;
	return COW_NYX_OK;
}

void cow_nyx_close(cow_nyx_vol_t *vol)
{
	if (!vol)
		return;
	_close_handles(&vol->hal, vol->part_handles, vol->num_parts);
	free(vol->l1_table);
	vol->l1_table = NULL;
	free(vol->scratch_blk);
	vol->scratch_blk = NULL;
	free(vol->xts_scratch);
	vol->xts_scratch = NULL;
	for (u32 w = 0U; w < COW_NYX_L2_CACHE_WAYS; w++)
	{
		free(vol->l2_cache[w].entries);
		vol->l2_cache[w].entries = NULL;
	}
	vol->opened = false;
}

static int _flush_l2_way(cow_nyx_vol_t *vol, cow_nyx_l2_slot_t *way)
{
	if (!way->valid || !way->dirty)
		return COW_NYX_OK;
	u32 l2_slot = vol->l1_table[way->l1_idx];
	if (l2_slot < vol->first_data_slot || l2_slot >= vol->effective_total_slots)
		return COW_NYX_ERR_CORRUPT;
	u64 off = (u64)vol->hdr.prefix_sectors * COW_NYX_SECTOR_SIZE + (u64)l2_slot * COW_NYX_BLOCK_BYTES;
	int rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
		off, way->entries, COW_NYX_BLOCK_BYTES, true);
	if (rc == COW_NYX_OK)
		way->dirty = false;
	return rc;
}

static int _load_l2_way(cow_nyx_vol_t *vol, u32 l1_idx, cow_nyx_l2_slot_t **out_way)
{
	vol->lru_tick++;
	cow_nyx_l2_slot_t *victim = &vol->l2_cache[0];
	for (u32 w = 0U; w < COW_NYX_L2_CACHE_WAYS; w++)
	{
		cow_nyx_l2_slot_t *c = &vol->l2_cache[w];
		if (c->valid && c->l1_idx == l1_idx)
		{
			c->last_used = vol->lru_tick;
			*out_way = c;
			return COW_NYX_OK;
		}
		if (!c->valid)
		{
			victim = c;
			break;
		}
		if (c->last_used < victim->last_used)
			victim = c;
	}

	if (victim->valid && victim->dirty)
	{
		int rc = _flush_l2_way(vol, victim);
		if (rc != COW_NYX_OK)
			return rc;
	}

	u32 l2_slot = vol->l1_table[l1_idx];
	if (l2_slot == 0U)
		return COW_NYX_ERR_CORRUPT;
	u64 off = (u64)vol->hdr.prefix_sectors * COW_NYX_SECTOR_SIZE + (u64)l2_slot * COW_NYX_BLOCK_BYTES;
	int rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
		off, victim->entries, COW_NYX_BLOCK_BYTES, false);
	if (rc != COW_NYX_OK)
		return rc;

	victim->valid = true;
	victim->dirty = false;
	victim->l1_idx = l1_idx;
	victim->last_used = vol->lru_tick;
	*out_way = victim;
	return COW_NYX_OK;
}

static int _lookup_data_slot(cow_nyx_vol_t *vol, u64 block_idx, u32 *out_slot)
{
	u64 l1_idx64 = block_idx >> COW_NYX_L2_SHIFT;
	if (l1_idx64 >= vol->l1_entries)
	{
		*out_slot = 0U;
		return COW_NYX_OK;
	}
	u32 l1_idx = (u32)l1_idx64;
	if (vol->l1_table[l1_idx] == 0U)
	{
		*out_slot = 0U;
		return COW_NYX_OK;
	}
	cow_nyx_l2_slot_t *way = NULL;
	int rc = _load_l2_way(vol, l1_idx, &way);
	if (rc != COW_NYX_OK)
		return rc;
	*out_slot = way->entries[block_idx & COW_NYX_L2_MASK];
	return COW_NYX_OK;
}

static int _read_clean_span(cow_nyx_vol_t *vol, u64 gpp_lba, u32 cnt, u8 *dst)
{
	u64 cur = gpp_lba;
	u32 rem = cnt;
	u8 *ptr = dst;
	u64 pfx_end = (u64)vol->hdr.prefix_sectors;
	u64 phys_end = vol->phys_base_sectors;

	while (rem > 0U)
	{
		u32 take = 0U;
		if (cur < pfx_end)
		{
			u64 av = pfx_end - cur;
			take = (av < (u64)rem) ? (u32)av : rem;
			int rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
				cur * COW_NYX_SECTOR_SIZE, ptr, take * COW_NYX_SECTOR_SIZE, false);
			if (rc != COW_NYX_OK)
				return rc;
		}
		else if (cur < phys_end)
		{
			u64 av = phys_end - cur;
			take = (av < (u64)rem) ? (u32)av : rem;
			if (!vol->hal.base_gpp_read)
			{
				memset(ptr, 0, take * COW_NYX_SECTOR_SIZE);
			}
			else
			{
				u32 done = 0U;
				while (done < take)
				{
					u32 c = MIN(take - done, 1024U);
					if (vol->hal.base_gpp_read(vol->hal.user_ctx, (u32)(cur + done), c, ptr + done * COW_NYX_SECTOR_SIZE) != 0)
						return COW_NYX_ERR_IO;
					done += c;
				}
			}
		}
		else
		{
			return COW_NYX_ERR_ARG;
		}
		cur += take;
		rem -= take;
		ptr += take * COW_NYX_SECTOR_SIZE;
	}
	return COW_NYX_OK;
}

int cow_nyx_read_sectors(cow_nyx_vol_t *vol, u64 gpp_lba, u32 count, void *buf)
{
	if (!vol || !vol->opened || (!buf && count > 0U))
		return COW_NYX_ERR_ARG;
	if (count == 0U)
		return COW_NYX_OK;
	if (gpp_lba + count > vol->hdr.gpp_total_sectors)
		return COW_NYX_ERR_ARG;

	u64 cur = gpp_lba;
	u32 rem = count;
	u8 *ptr = (u8 *)buf;
	u64 pfx_bytes = (u64)vol->hdr.prefix_sectors * COW_NYX_SECTOR_SIZE;

	u64 clean_start = 0ULL;
	u32 clean_cnt = 0U;
	u8 *clean_ptr = NULL;

	while (rem > 0U)
	{
		u64 blk = cur >> COW_NYX_BLOCK_SHIFT;
		u32 sec_in_blk = (u32)(cur & COW_NYX_BLOCK_MASK);
		u32 chunk = MIN(COW_NYX_BLOCK_SECTORS - sec_in_blk, rem);

		u32 data_slot = 0U;
		int rc = _lookup_data_slot(vol, blk, &data_slot);
		if (rc != COW_NYX_OK)
			return rc;

		if (data_slot == 0U)
		{
			if (clean_cnt == 0U)
			{
				clean_start = cur;
				clean_ptr = ptr;
			}
			clean_cnt += chunk;
		}
		else
		{
			if (clean_cnt > 0U)
			{
				rc = _read_clean_span(vol, clean_start, clean_cnt, clean_ptr);
				if (rc != COW_NYX_OK)
					return rc;
				clean_cnt = 0U;
			}
			u64 off = pfx_bytes + (u64)data_slot * COW_NYX_BLOCK_BYTES + (u64)sec_in_blk * COW_NYX_SECTOR_SIZE;
			rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
				off, ptr, chunk * COW_NYX_SECTOR_SIZE, false);
			if (rc != COW_NYX_OK)
				return rc;
		}
		cur += chunk;
		rem -= chunk;
		ptr += chunk * COW_NYX_SECTOR_SIZE;
	}

	if (clean_cnt > 0U)
	{
		int rc = _read_clean_span(vol, clean_start, clean_cnt, clean_ptr);
		if (rc != COW_NYX_OK)
			return rc;
	}
	return COW_NYX_OK;
}

static int _estimate_slots_needed(cow_nyx_vol_t *vol, u64 gpp_lba, u32 count, u32 *out_slots)
{
	u32 needed = 0U;
	u32 touched_l1[64];
	u32 num_touched = 0U;
	u64 prev_blk = (u64)-1;

	u64 cur = gpp_lba;
	u32 rem = count;
	while (rem > 0U)
	{
		u64 blk = cur >> COW_NYX_BLOCK_SHIFT;
		u32 sec_in_blk = (u32)(cur & COW_NYX_BLOCK_MASK);
		u32 chunk = MIN(COW_NYX_BLOCK_SECTORS - sec_in_blk, rem);

		if (blk != prev_blk)
		{
			prev_blk = blk;
			u32 l1_idx = (u32)(blk >> COW_NYX_L2_SHIFT);
			if (l1_idx >= vol->l1_entries)
				return COW_NYX_ERR_ARG;

			u32 data_slot = 0U;
			int rc = _lookup_data_slot(vol, blk, &data_slot);
			if (rc != COW_NYX_OK)
				return rc;
			if (data_slot == 0U)
			{
				needed++;
				if (vol->l1_table[l1_idx] == 0U)
				{
					bool seen = false;
					for (u32 i = 0U; i < num_touched; i++)
					{
						if (touched_l1[i] == l1_idx)
						{
							seen = true;
							break;
						}
					}
					if (!seen)
					{
						if (num_touched < 64U)
							touched_l1[num_touched++] = l1_idx;
						needed++;
					}
				}
			}
		}
		cur += chunk;
		rem -= chunk;
	}
	*out_slots = needed;
	return COW_NYX_OK;
}

int cow_nyx_write_sectors(cow_nyx_vol_t *vol, u64 gpp_lba, u32 count, const void *buf)
{
	if (!vol || !vol->opened || (!buf && count > 0U))
		return COW_NYX_ERR_ARG;
	if (count == 0U)
		return COW_NYX_OK;
	if (gpp_lba + count > vol->hdr.gpp_total_sectors)
		return COW_NYX_ERR_ARG;

	u32 slots_needed = 0U;
	int rc = _estimate_slots_needed(vol, gpp_lba, count, &slots_needed);
	if (rc != COW_NYX_OK)
		return rc;
	if ((u64)vol->hdr.next_free_slot + slots_needed > vol->effective_total_slots)
		return COW_NYX_ERR_POOL_FULL;

	u64 cur = gpp_lba;
	u32 rem = count;
	const u8 *ptr = (const u8 *)buf;
	u64 pfx_bytes = (u64)vol->hdr.prefix_sectors * COW_NYX_SECTOR_SIZE;

	while (rem > 0U)
	{
		u64 blk = cur >> COW_NYX_BLOCK_SHIFT;
		u32 sec_in_blk = (u32)(cur & COW_NYX_BLOCK_MASK);
		u32 chunk = MIN(COW_NYX_BLOCK_SECTORS - sec_in_blk, rem);
		u32 l1_idx = (u32)(blk >> COW_NYX_L2_SHIFT);
		u32 l2_idx = (u32)(blk & COW_NYX_L2_MASK);

		if (vol->l1_table[l1_idx] == 0U)
		{
			u32 new_l2_slot = vol->hdr.next_free_slot++;
			vol->hdr.dirty_l2_pages++;
			vol->l1_table[l1_idx] = new_l2_slot;
			memset(vol->scratch_blk, 0, COW_NYX_BLOCK_BYTES);
			rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
				pfx_bytes + (u64)new_l2_slot * COW_NYX_BLOCK_BYTES, vol->scratch_blk, COW_NYX_BLOCK_BYTES, true);
			if (rc != COW_NYX_OK)
				return rc;
			vol->dirty_hdr = true;
		}

		cow_nyx_l2_slot_t *way = NULL;
		rc = _load_l2_way(vol, l1_idx, &way);
		if (rc != COW_NYX_OK)
			return rc;

		u32 data_slot = way->entries[l2_idx];
		if (data_slot == 0U)
		{
			data_slot = vol->hdr.next_free_slot++;
			vol->hdr.dirty_data_blocks++;
			way->entries[l2_idx] = data_slot;
			way->dirty = true;
			vol->dirty_hdr = true;

			u64 blk_base_lba = blk << COW_NYX_BLOCK_SHIFT;
			if (chunk == COW_NYX_BLOCK_SECTORS && sec_in_blk == 0U)
			{
				rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
					pfx_bytes + (u64)data_slot * COW_NYX_BLOCK_BYTES, (void *)ptr, COW_NYX_BLOCK_BYTES, true);
			}
			else
			{
				rc = _read_clean_span(vol, blk_base_lba, COW_NYX_BLOCK_SECTORS, vol->scratch_blk);
				if (rc == COW_NYX_OK)
				{
					memcpy(vol->scratch_blk + sec_in_blk * COW_NYX_SECTOR_SIZE, ptr, chunk * COW_NYX_SECTOR_SIZE);
					rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
						pfx_bytes + (u64)data_slot * COW_NYX_BLOCK_BYTES, vol->scratch_blk, COW_NYX_BLOCK_BYTES, true);
				}
			}
			if (rc != COW_NYX_OK)
				return rc;
		}
		else
		{
			u64 off = pfx_bytes + (u64)data_slot * COW_NYX_BLOCK_BYTES + (u64)sec_in_blk * COW_NYX_SECTOR_SIZE;
			rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
				off, (void *)ptr, chunk * COW_NYX_SECTOR_SIZE, true);
			if (rc != COW_NYX_OK)
				return rc;
		}

		cur += chunk;
		rem -= chunk;
		ptr += chunk * COW_NYX_SECTOR_SIZE;
	}

	return COW_NYX_OK;
}

int cow_nyx_commit(cow_nyx_vol_t *vol)
{
	if (!vol || !vol->opened)
		return COW_NYX_ERR_ARG;

	for (u32 w = 0U; w < COW_NYX_L2_CACHE_WAYS; w++)
	{
		if (vol->l2_cache[w].valid && vol->l2_cache[w].dirty)
		{
			int rc = _flush_l2_way(vol, &vol->l2_cache[w]);
			if (rc != COW_NYX_OK)
				return rc;
			vol->dirty_hdr = true;
		}
	}

	if (!vol->dirty_hdr)
		return COW_NYX_OK;

	u32 next_sb = vol->active_sb_slot ^ 1U;
	vol->hdr.seq_num++;
	u32 l1_bytes = vol->l1_entries * 4U;
	vol->hdr.l1_crc32 = cow_nyx_crc32(&vol->hal, 0, vol->l1_table, l1_bytes);
	vol->hdr.header_crc32 = cow_nyx_crc32(&vol->hal, 0, &vol->hdr, offsetof(cow_nyx_sb_t, header_crc32));

	u64 pfx_bytes = (u64)vol->hdr.prefix_sectors * COW_NYX_SECTOR_SIZE;
	u32 target_l1_slot = (next_sb == 0U) ? vol->hdr.l1_slot_a : vol->hdr.l1_slot_b;
	u32 l1_slots = COW_NYX_L1_SLOTS_FOR(vol->l1_entries);
	u32 pad_bytes = l1_slots * COW_NYX_BLOCK_BYTES;
	u8 *l1_buf = (u8 *)zalloc(pad_bytes);
	if (!l1_buf)
		return COW_NYX_ERR_NO_MEM;
	memcpy(l1_buf, vol->l1_table, l1_bytes);
	int rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
		pfx_bytes + (u64)target_l1_slot * COW_NYX_BLOCK_BYTES, l1_buf, pad_bytes, true);
	free(l1_buf);
	if (rc != COW_NYX_OK)
		return rc;

	memset(vol->scratch_blk, 0, COW_NYX_BLOCK_BYTES);
	memcpy(vol->scratch_blk, &vol->hdr, sizeof(cow_nyx_sb_t));
	rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
		pfx_bytes + (u64)next_sb * COW_NYX_BLOCK_BYTES, vol->scratch_blk, COW_NYX_BLOCK_BYTES, true);
	if (rc != COW_NYX_OK)
		return rc;

	vol->active_sb_slot = next_sb;
	vol->dirty_hdr = false;
	return COW_NYX_OK;
}

int cow_nyx_set_flags(cow_nyx_vol_t *vol, u32 flags)
{
	if (!vol || !vol->opened)
		return COW_NYX_ERR_ARG;
	vol->hdr.flags = flags;
	vol->dirty_hdr = true;
	return cow_nyx_commit(vol);
}

int cow_nyx_reset_overlay(cow_nyx_vol_t *vol)
{
	if (!vol || !vol->opened)
		return COW_NYX_ERR_ARG;

	for (u32 w = 0U; w < COW_NYX_L2_CACHE_WAYS; w++)
	{
		vol->l2_cache[w].valid = false;
		vol->l2_cache[w].dirty = false;
	}
	memset(vol->l1_table, 0, vol->l1_entries * sizeof(u32));
	vol->hdr.next_free_slot = vol->first_data_slot;
	vol->hdr.dirty_data_blocks = 0U;
	vol->hdr.dirty_l2_pages = 0U;
	vol->hdr.conflict_count = 0U;
	vol->dirty_hdr = true;
	return cow_nyx_commit(vol);
}

int cow_nyx_grow_pool(cow_nyx_vol_t *vol, u32 add_slots, u64 sd_free_bytes, u32 sd_reserve_mb)
{
	if (!vol || !vol->opened || add_slots == 0U)
		return COW_NYX_ERR_ARG;
	if (vol->num_parts == 0U || !vol->part_handles[0])
		return COW_NYX_ERR_IO;

	u64 res_bytes = (u64)sd_reserve_mb * 1024ULL * 1024ULL;
	u64 usable_free = (sd_free_bytes > res_bytes) ? (sd_free_bytes - res_bytes) : 0ULL;
	u64 add_bytes = (u64)add_slots * COW_NYX_BLOCK_BYTES;
	if (add_bytes > usable_free)
		return COW_NYX_ERR_NO_SD_SPACE;

	if (vol->hdr.max_overlay_slots > 0U &&
	    (u64)vol->hdr.total_slots + add_slots > vol->hdr.max_overlay_slots)
		return COW_NYX_ERR_POOL_FULL;

	u32 part_max_secs = (vol->hdr.part_max_sectors > 0U)
		? vol->hdr.part_max_sectors
		: (COW_NYX_PART_MAX_DEFAULT_MB * 2048U);
	u64 part_max_bytes = (u64)part_max_secs * COW_NYX_SECTOR_SIZE;
	u64 rem = add_bytes;

	while (rem > 0ULL)
	{
		u32 last_idx = vol->num_parts - 1U;
		if (vol->part_sizes[last_idx] < part_max_bytes)
		{
			u64 room = part_max_bytes - vol->part_sizes[last_idx];
			u64 extend = MIN(rem, room);
			u64 new_sz = vol->part_sizes[last_idx] + extend;
			if (vol->hal.file_grow(vol->hal.user_ctx, vol->part_handles[last_idx], new_sz) != 0)
				return COW_NYX_ERR_IO;
			vol->part_sizes[last_idx] = new_sz;
			rem -= extend;
		}
		else
		{
			if (vol->num_parts >= COW_NYX_MAX_SPLIT_PARTS)
				return COW_NYX_ERR_POOL_FULL;
			char npath[300];
			u32 ni = vol->num_parts;
			_build_part_path(npath, sizeof(npath), vol->emmc_dir, ni);
			u64 chunk = MIN(rem, part_max_bytes);
			u64 dummy = 0ULL;
			void *nh = NULL;
			if (vol->hal.file_open(vol->hal.user_ctx, npath, true, &dummy, &nh) != 0)
				return COW_NYX_ERR_IO;
			if (vol->hal.file_grow(vol->hal.user_ctx, nh, chunk) != 0)
			{
				vol->hal.file_close(vol->hal.user_ctx, nh);
				return COW_NYX_ERR_IO;
			}
			vol->part_handles[ni] = nh;
			vol->part_sizes[ni] = chunk;
			vol->num_parts++;
			rem -= chunk;
		}
	}

	vol->hdr.total_slots += add_slots;
	vol->effective_total_slots += add_slots;
	vol->dirty_hdr = true;
	return cow_nyx_commit(vol);
}

void cow_nyx_compute_headroom(
	const cow_nyx_vol_t *vol,
	u64 sd_free_bytes,
	u32 sd_reserve_mb,
	u32 sysmem_cap_mb,
	cow_nyx_headroom_t *out_headroom)
{
	memset(out_headroom, 0, sizeof(*out_headroom));
	u64 res_bytes = (u64)sd_reserve_mb * 1024ULL * 1024ULL;
	u64 sd_usable = (sd_free_bytes > res_bytes) ? (sd_free_bytes - res_bytes) : 0ULL;

	u32 unused_slots = (vol->effective_total_slots > vol->hdr.next_free_slot)
		? (vol->effective_total_slots - vol->hdr.next_free_slot)
		: 0U;
	u64 unused_bytes = (u64)unused_slots * COW_NYX_BLOCK_BYTES;

	u32 max_slots = (vol->hdr.max_overlay_slots > 0U)
		? vol->hdr.max_overlay_slots
		: vol->effective_total_slots;
	u32 max_rem_slots = (max_slots > vol->hdr.next_free_slot)
		? (max_slots - vol->hdr.next_free_slot)
		: 0U;
	u64 max_rem_bytes = (u64)max_rem_slots * COW_NYX_BLOCK_BYTES;

	u64 pool_safe = (vol->hdr.alloc_policy == COW_NYX_ALLOC_THIN)
		? MIN(unused_bytes + sd_usable, max_rem_bytes)
		: unused_bytes;
	u64 cap_bytes = (sysmem_cap_mb > 0U) ? ((u64)sysmem_cap_mb * 1024ULL * 1024ULL) : 0ULL;

	out_headroom->sd_free_bytes = sd_free_bytes;
	out_headroom->sd_reserve_bytes = res_bytes;
	out_headroom->sd_safe_headroom_bytes = sd_usable;
	out_headroom->unused_pool_bytes = unused_bytes;
	out_headroom->max_remaining_pool_bytes = max_rem_bytes;
	out_headroom->sysmem_cap_bytes = cap_bytes;
	out_headroom->safe_sysmem_bytes = (cap_bytes > 0ULL) ? MIN(pool_safe, cap_bytes) : pool_safe;
}

static bool _utf16_eq_ascii_nz(const u8 *name_bytes, const char *ascii)
{
	size_t n = strlen(ascii);
	if (n > 36U)
		return false;
	for (size_t i = 0; i < n; i++)
	{
		if (_rd_le16(name_bytes + i * 2U) != (u16)(u8)ascii[i])
			return false;
	}
	return (n == 36U) || (_rd_le16(name_bytes + n * 2U) == 0U);
}

int cow_nyx_find_user_gpt(
	cow_nyx_vol_t *vol,
	u32 *out_user_idx,
	u64 *out_user_start,
	u64 *out_user_end)
{
	u8 lba0_33[34U * COW_NYX_SECTOR_SIZE];
	int rc = cow_nyx_read_sectors(vol, 0ULL, 34U, lba0_33);
	if (rc != COW_NYX_OK)
		return rc;

	const u8 *hdr = lba0_33 + COW_NYX_SECTOR_SIZE;
	if (memcmp(hdr, "EFI PART", 8) != 0)
		return COW_NYX_ERR_CORRUPT;

	u32 hdr_sz = _rd_le32(hdr + 0x0C);
	u32 num_ents = _rd_le32(hdr + 0x50);
	u32 ent_sz = _rd_le32(hdr + 0x54);
	if (hdr_sz < 92U || hdr_sz > 512U || ent_sz != 128U || num_ents == 0U || num_ents > 128U)
		return COW_NYX_ERR_CORRUPT;

	for (u32 i = 0U; i < num_ents; i++)
	{
		const u8 *ent = lba0_33 + 1024U + i * ent_sz;
		if (_utf16_eq_ascii_nz(ent + 0x38, "USER"))
		{
			if (out_user_idx)
				*out_user_idx = i;
			if (out_user_start)
				*out_user_start = _rd_le64(ent + 0x20);
			if (out_user_end)
				*out_user_end = _rd_le64(ent + 0x28);
			return COW_NYX_OK;
		}
	}
	return COW_NYX_ERR_CORRUPT;
}

static int _user_rw_dec(
	cow_nyx_vol_t *vol,
	u64 user_start_lba,
	u32 part_rel_lba,
	void *buf,
	u32 sector_count,
	bool is_write)
{
	if (sector_count == 0U)
		return COW_NYX_OK;

	if (!is_write)
	{
		int rc = cow_nyx_read_sectors(vol, user_start_lba + part_rel_lba, sector_count, buf);
		if (rc != COW_NYX_OK)
			return rc;
		if (vol->hal.xts_user_crypt)
		{
			if (vol->hal.xts_user_crypt(vol->hal.user_ctx, false, part_rel_lba, buf, sector_count) != 0)
				return COW_NYX_ERR_IO;
		}
		return COW_NYX_OK;
	}
	else
	{
		if (!vol->hal.xts_user_crypt)
			return cow_nyx_write_sectors(vol, user_start_lba + part_rel_lba, sector_count, buf);

		u32 done = 0U;
		while (done < sector_count)
		{
			u32 take = MIN(sector_count - done, COW_NYX_BLOCK_SECTORS);
			memcpy(vol->xts_scratch, (const u8 *)buf + done * COW_NYX_SECTOR_SIZE, take * COW_NYX_SECTOR_SIZE);
			if (vol->hal.xts_user_crypt(vol->hal.user_ctx, true, part_rel_lba + done, vol->xts_scratch, take) != 0)
				return COW_NYX_ERR_IO;
			int rc = cow_nyx_write_sectors(vol, user_start_lba + part_rel_lba + done, take, vol->xts_scratch);
			if (rc != COW_NYX_OK)
				return rc;
			done += take;
		}
		return COW_NYX_OK;
	}
}

typedef struct _fat32_vinfo_t
{
	u32 byts_per_sec;
	u32 sec_per_clus;
	u32 rsvd_sec_cnt;
	u32 num_fats;
	u32 tot_sec32;
	u32 fatsz32;
	u32 root_clus;
	u32 fsinfo_sec;
	u32 bk_boot_sec;
	u32 data_start_sec;
	u32 total_clusters;
	u32 fsi_free_count;
	u32 fsi_nxt_free;
	bool has_fsinfo;
} fat32_vinfo_t;

static int _parse_fat32(
	cow_nyx_vol_t *vol,
	u64 user_start,
	fat32_vinfo_t *out,
	u8 *out_vbr,
	u8 *out_fsinfo)
{
	u8 vbr[512];
	int rc = _user_rw_dec(vol, user_start, 0U, vbr, 1U, false);
	if (rc != COW_NYX_OK)
		return rc;
	if (vbr[510] != 0x55 || vbr[511] != 0xAA)
		return COW_NYX_ERR_FS_INVALID;

	memset(out, 0, sizeof(*out));
	out->byts_per_sec = _rd_le16(vbr + 0x0B);
	out->sec_per_clus = vbr[0x0D];
	out->rsvd_sec_cnt = _rd_le16(vbr + 0x0E);
	out->num_fats     = vbr[0x10];
	out->tot_sec32    = _rd_le32(vbr + 0x20);
	out->fatsz32      = _rd_le32(vbr + 0x24);
	out->root_clus    = _rd_le32(vbr + 0x2C);
	out->fsinfo_sec   = _rd_le16(vbr + 0x30);
	out->bk_boot_sec  = _rd_le16(vbr + 0x32);

	if (out->byts_per_sec != 512U || out->sec_per_clus == 0U ||
	    out->num_fats == 0U || out->fatsz32 == 0U || out->tot_sec32 == 0U ||
	    _rd_le16(vbr + 0x11) != 0U || _rd_le16(vbr + 0x16) != 0U)
		return COW_NYX_ERR_FS_INVALID;

	out->data_start_sec = out->rsvd_sec_cnt + out->num_fats * out->fatsz32;
	if (out->tot_sec32 <= out->data_start_sec)
		return COW_NYX_ERR_FS_INVALID;
	out->total_clusters = (out->tot_sec32 - out->data_start_sec) / out->sec_per_clus;

	if (out_vbr)
		memcpy(out_vbr, vbr, 512U);

	u8 fsi[512];
	if (out->fsinfo_sec > 0U && out->fsinfo_sec < out->rsvd_sec_cnt)
	{
		rc = _user_rw_dec(vol, user_start, out->fsinfo_sec, fsi, 1U, false);
		if (rc != COW_NYX_OK)
			return rc;
		if (_rd_le32(fsi + 0) == 0x41615252U &&
		    _rd_le32(fsi + 0x1E4) == 0x61417272U &&
		    _rd_le32(fsi + 0x1FC) == 0xAA550000U)
		{
			out->has_fsinfo = true;
			out->fsi_free_count = _rd_le32(fsi + 0x1E8);
			out->fsi_nxt_free = _rd_le32(fsi + 0x1EC);
			if (out_fsinfo)
				memcpy(out_fsinfo, fsi, 512U);
		}
	}
	return COW_NYX_OK;
}

static bool _bitset_get(const u8 *bits, u32 idx)
{
	return (bits[idx >> 3] & (1U << (idx & 7U))) != 0;
}

static void _bitset_set(u8 *bits, u32 idx)
{
	bits[idx >> 3] |= (u8)(1U << (idx & 7U));
}

static const u8 COW_BAL_DIR_DOS[11] = { '_', '_', 'C', 'O', 'W', '_', '_', ' ', ' ', ' ', ' ' };

static void _format_balloon_dos(u32 idx, u8 *out11)
{
	static const char HEX[] = "0123456789ABCDEF";
	memcpy(out11, "R00     BIN", 11);
	out11[1] = (u8)HEX[(idx >> 4) & 0x0FU];
	out11[2] = (u8)HEX[idx & 0x0FU];
}

static void _pack_sfn_entry(u8 *dst, const u8 *dos11, u8 attr, u32 start_clus, u32 file_sz)
{
	memset(dst, 0, 32U);
	memcpy(dst, dos11, 11U);
	dst[11] = attr;
	_wr_le16(dst + 0x10U, 0x5421U); /* CrtDate */
	_wr_le16(dst + 0x12U, 0x5421U); /* LstAccDate */
	_wr_le16(dst + 0x14U, (u16)((start_clus >> 16) & 0xFFFFU));
	_wr_le16(dst + 0x18U, 0x5421U); /* WrtDate */
	_wr_le16(dst + 0x1AU, (u16)(start_clus & 0xFFFFU));
	_wr_le32(dst + 0x1CU, file_sz);
}

static int _parse_hex2(u8 c0, u8 c1, u32 *out)
{
	u8 chars[2] = { c0, c1 };
	u32 val = 0U;
	for (int i = 0; i < 2; i++)
	{
		u8 c = chars[i];
		u32 d = 0U;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'A' && c <= 'F') d = 10U + (c - 'A');
		else if (c >= 'a' && c <= 'f') d = 10U + (c - 'a');
		else return -1;
		val = (val << 4) | d;
	}
	*out = val;
	return 0;
}

static void _mark_fat_entry(u32 *fat_u32, u8 *dirty_bits, u32 clus, u32 val28)
{
	u32 old = fat_u32[clus];
	u32 nv = (old & 0xF0000000U) | (val28 & 0x0FFFFFFFU);
	if (nv != old)
	{
		fat_u32[clus] = nv;
		_bitset_set(dirty_bits, clus >> 7);
	}
}

int cow_nyx_sync_headroom(
	cow_nyx_vol_t *vol,
	u64 sd_free_bytes,
	u32 sd_reserve_mb,
	u32 sysmem_cap_mb,
	cow_nyx_balloon_res_t *out_res)
{
	if (!vol || !vol->opened || !out_res)
		return COW_NYX_ERR_ARG;

	memset(out_res, 0, sizeof(*out_res));
	u64 user_start = 0ULL, user_end = 0ULL;
	int rc = cow_nyx_find_user_gpt(vol, NULL, &user_start, &user_end);
	if (rc != COW_NYX_OK)
		return rc;

	fat32_vinfo_t vi;
	u8 vbr[512], fsinfo[512];
	rc = _parse_fat32(vol, user_start, &vi, vbr, fsinfo);
	if (rc != COW_NYX_OK)
		return rc;
	if (vi.tot_sec32 > (user_end - user_start + 1ULL))
		return COW_NYX_ERR_FS_INVALID;

	u32 max_clus = vi.total_clusters + 1U;
	if (max_clus + 1U > vi.fatsz32 * 128U)
		return COW_NYX_ERR_FS_INVALID;

	u32 clus_secs = vi.sec_per_clus;
	u32 clus_bytes = clus_secs * COW_NYX_SECTOR_SIZE;
	u32 max_part_clus = (u32)MIN(COW_NYX_BALLOON_MAX_PART_BYTES / clus_bytes, (u64)0xFFFFFFFFU / clus_bytes);
	if (max_part_clus == 0U)
		return COW_NYX_ERR_FS_INVALID;

	/* Ensure pool room BEFORE mutating FAT32 (all FAT copies + FSInfo + root + dir) */
	u32 est_fat_slots = ((vi.fatsz32 * vi.num_fats) / COW_NYX_BLOCK_SECTORS) + 32U;
	u32 cur_free_slots = (vol->effective_total_slots > vol->hdr.next_free_slot)
		? (vol->effective_total_slots - vol->hdr.next_free_slot) : 0U;
	if (cur_free_slots < est_fat_slots && vol->hdr.alloc_policy == COW_NYX_ALLOC_THIN)
	{
		u32 grow_s = est_fat_slots - cur_free_slots;
		if (grow_s > 0U)
		{
			rc = cow_nyx_grow_pool(vol, grow_s, sd_free_bytes, sd_reserve_mb);
			if (rc == COW_NYX_OK)
				sd_free_bytes = (sd_free_bytes > (u64)grow_s * COW_NYX_BLOCK_BYTES)
					? (sd_free_bytes - (u64)grow_s * COW_NYX_BLOCK_BYTES) : 0ULL;
			else if (rc != COW_NYX_ERR_POOL_FULL && rc != COW_NYX_ERR_NO_SD_SPACE)
				return rc;
		}
	}

	u32 fat_words = vi.fatsz32 * 128U;
	u32 *fat_u32 = (u32 *)malloc((size_t)fat_words * sizeof(u32));
	u8  *dirty_secs = (u8 *)zalloc((vi.fatsz32 + 7U) >> 3);
	u8  *visited_bits = (u8 *)zalloc((max_clus + 8U) >> 3);
	u8  *alloc_bits = (u8 *)zalloc((max_clus + 8U) >> 3);
	u8  *clus_buf = (u8 *)malloc(clus_bytes);
	if (!fat_u32 || !dirty_secs || !visited_bits || !alloc_bits || !clus_buf)
	{
		rc = COW_NYX_ERR_NO_MEM;
		goto cleanup;
	}

	if (vol->hal.progress_fn)
		vol->hal.progress_fn(vol->hal.user_ctx, "Scanning FAT1 for free space...", 15);

	u32 f_done = 0U;
	while (f_done < vi.fatsz32)
	{
		u32 take = MIN(vi.fatsz32 - f_done, 1024U);
		rc = _user_rw_dec(vol, user_start, vi.rsvd_sec_cnt + f_done,
			(u8 *)fat_u32 + f_done * 512U, take, false);
		if (rc != COW_NYX_OK)
			goto cleanup;
		f_done += take;
	}

	/* Locate root directory chain + existing /__COW__ dir + R00..Rnn chains */
	u32 root_chain[64];
	u32 root_chain_len = 0U;
	{
		u32 c = vi.root_clus;
		while (c >= 2U && c <= max_clus && root_chain_len < 64U)
		{
			if (_bitset_get(visited_bits, c)) { rc = COW_NYX_ERR_FS_INVALID; goto cleanup; }
			_bitset_set(visited_bits, c);
			root_chain[root_chain_len++] = c;
			u32 nxt = fat_u32[c] & 0x0FFFFFFFU;
			if (nxt >= 0x0FFFFFF8U) break;
			c = nxt;
		}
		if (root_chain_len == 0U) { rc = COW_NYX_ERR_FS_INVALID; goto cleanup; }
	}

	bool found_dir_entry = false;
	u32 dir_entry_rc = 0U, dir_entry_off = 0U;
	u32 balloon_dir_clus = 0U;
	bool found_free_root = false;
	u32 free_root_c = 0U, free_root_off = 0U;

	for (u32 ri = 0U; ri < root_chain_len; ri++)
	{
		u32 r_c = root_chain[ri];
		rc = _user_rw_dec(vol, user_start, vi.data_start_sec + (r_c - 2U) * clus_secs, clus_buf, clus_secs, false);
		if (rc != COW_NYX_OK) goto cleanup;

		bool eod = false;
		for (u32 off = 0U; off < clus_bytes; off += 32U)
		{
			u8 fb = clus_buf[off];
			u8 attr = clus_buf[off + 11U];
			if (fb == 0x00U)
			{
				if (!found_free_root) { found_free_root = true; free_root_c = r_c; free_root_off = off; }
				eod = true;
				break;
			}
			if (fb == 0xE5U)
			{
				if (!found_free_root) { found_free_root = true; free_root_c = r_c; free_root_off = off; }
				continue;
			}
			if (attr == 0x0FU) continue;
			if ((attr & 0x10U) && memcmp(clus_buf + off, COW_BAL_DIR_DOS, 11U) == 0)
			{
				found_dir_entry = true;
				dir_entry_rc = r_c;
				dir_entry_off = off;
				balloon_dir_clus = ((u32)_rd_le16(clus_buf + off + 0x14U) << 16) | _rd_le16(clus_buf + off + 0x1AU);
			}
		}
		if (eod) break;
	}

	u32 existing_dir_chain[8];
	u32 existing_dir_len = 0U;
	if (balloon_dir_clus >= 2U)
	{
		u32 c = balloon_dir_clus;
		while (c >= 2U && c <= max_clus && existing_dir_len < 8U)
		{
			if (_bitset_get(visited_bits, c)) { rc = COW_NYX_ERR_FS_INVALID; goto cleanup; }
			_bitset_set(visited_bits, c);
			existing_dir_chain[existing_dir_len++] = c;
			u32 nxt = fat_u32[c] & 0x0FFFFFFFU;
			if (nxt >= 0x0FFFFFF8U) break;
			c = nxt;
		}
	}

	u32 exist_starts[256] = { 0 };
	u32 exist_counts[256] = { 0 };
	bool exist_present[256] = { 0 };
	u32 exist_part_cnt = 0U;
	u32 current_balloon_clus = 0U;

	if (existing_dir_len > 0U)
	{
		for (u32 di = 0U; di < existing_dir_len; di++)
		{
			rc = _user_rw_dec(vol, user_start, vi.data_start_sec + (existing_dir_chain[di] - 2U) * clus_secs, clus_buf, clus_secs, false);
			if (rc != COW_NYX_OK) goto cleanup;

			for (u32 off = 0U; off < clus_bytes; off += 32U)
			{
				u8 fb = clus_buf[off];
				u8 attr = clus_buf[off + 11U];
				if (fb == 0x00U) break;
				if (fb == 0xE5U || attr == 0x0FU || (attr & 0x18U)) continue;
				if (clus_buf[off] == 'R' && memcmp(clus_buf + off + 3U, "     BIN", 8U) == 0)
				{
					u32 pi = 0U;
					if (_parse_hex2(clus_buf[off + 1U], clus_buf[off + 2U], &pi) != 0 || pi >= 256U)
						continue;
					u32 f_start = ((u32)_rd_le16(clus_buf + off + 0x14U) << 16) | _rd_le16(clus_buf + off + 0x1AU);
					if (f_start < 2U || f_start > max_clus) continue;
					u32 len_c = 0U, cur = f_start;
					while (cur >= 2U && cur <= max_clus)
					{
						if (_bitset_get(visited_bits, cur)) { rc = COW_NYX_ERR_FS_INVALID; goto cleanup; }
						_bitset_set(visited_bits, cur);
						len_c++;
						u32 nxt = fat_u32[cur] & 0x0FFFFFFFU;
						if (nxt >= 0x0FFFFFF8U) break;
						cur = nxt;
					}
					if (len_c > 0U)
					{
						exist_present[pi] = true;
						exist_starts[pi] = f_start;
						exist_counts[pi] = len_c;
						current_balloon_clus += len_c;
						exist_part_cnt++;
					}
				}
			}
		}
	}

	u32 fat_free_cnt = 0U;
	u32 first_free_clus = 0xFFFFFFFFU;
	for (u32 c = 2U; c <= max_clus; c++)
	{
		if ((fat_u32[c] & 0x0FFFFFFFU) == 0U)
		{
			fat_free_cnt++;
			if (first_free_clus == 0xFFFFFFFFU)
				first_free_clus = c;
		}
	}

	cow_nyx_compute_headroom(vol, sd_free_bytes, sd_reserve_mb, sysmem_cap_mb, &out_res->headroom);
	u64 safe_bytes = out_res->headroom.safe_sysmem_bytes;

	u32 true_free = fat_free_cnt + current_balloon_clus;
	u32 desired_free = (u32)MIN((u64)true_free, safe_bytes / clus_bytes);
	u32 prelim_target = true_free - desired_free;

	bool need_new_dir = (prelim_target > 0U && existing_dir_len == 0U);
	u32 extra_dir_clus = need_new_dir ? 1U : 0U;
	if (extra_dir_clus > 0U && !found_free_root)
		extra_dir_clus++;

	u32 avail_after_dir = (true_free > extra_dir_clus) ? (true_free - extra_dir_clus) : 0U;
	desired_free = (u32)MIN((u64)avail_after_dir, safe_bytes / clus_bytes);
	u32 target_balloon = avail_after_dir - desired_free;
	u32 max_balloon = 256U * max_part_clus;
	if (target_balloon > max_balloon)
	{
		target_balloon = max_balloon;
		desired_free = avail_after_dir - target_balloon;
	}

	out_res->cluster_bytes = clus_bytes;
	out_res->true_free_clusters = true_free;
	out_res->old_balloon_clusters = current_balloon_clus;

	if (target_balloon == current_balloon_clus)
	{
		if (vi.has_fsinfo && vi.fsi_free_count != fat_free_cnt)
		{
			out_res->action = COW_BAL_REPAIRED_FSINFO;
			_wr_le32(fsinfo + 0x1E8U, fat_free_cnt);
			_wr_le32(fsinfo + 0x1ECU, first_free_clus);
			rc = _user_rw_dec(vol, user_start, vi.fsinfo_sec, fsinfo, 1U, true);
			if (rc == COW_NYX_OK && vi.bk_boot_sec > 0U && (vi.bk_boot_sec + vi.fsinfo_sec) < vi.rsvd_sec_cnt)
				rc = _user_rw_dec(vol, user_start, vi.bk_boot_sec + vi.fsinfo_sec, fsinfo, 1U, true);
			if (rc == COW_NYX_OK)
				rc = cow_nyx_commit(vol);
		}
		else
		{
			out_res->action = COW_BAL_UNCHANGED;
		}
		out_res->new_balloon_clusters = current_balloon_clus;
		out_res->target_free_clusters = fat_free_cnt;
		out_res->effective_free_bytes = (u64)fat_free_cnt * clus_bytes;
		out_res->num_balloon_parts = exist_part_cnt;
		goto cleanup;
	}

	if (vol->hal.progress_fn)
		vol->hal.progress_fn(vol->hal.user_ctx, "Updating /__COW__ balloon chain...", 55);

	u32 new_part_starts[256] = { 0 };
	u32 new_part_counts[256] = { 0 };
	u32 new_num_parts = 0U;
	u32 new_free_clus = desired_free;

	if (target_balloon == 0U)
	{
		for (u32 pi = 0U; pi < 256U; pi++)
		{
			if (!exist_present[pi]) continue;
			u32 cur = exist_starts[pi];
			while (cur >= 2U && cur <= max_clus)
			{
				u32 nxt = fat_u32[cur] & 0x0FFFFFFFU;
				_mark_fat_entry(fat_u32, dirty_secs, cur, 0U);
				if (nxt >= 0x0FFFFFF8U) break;
				cur = nxt;
			}
		}
		for (u32 di = 0U; di < existing_dir_len; di++)
			_mark_fat_entry(fat_u32, dirty_secs, existing_dir_chain[di], 0U);

		if (found_dir_entry)
		{
			rc = _user_rw_dec(vol, user_start, vi.data_start_sec + (dir_entry_rc - 2U) * clus_secs, clus_buf, clus_secs, false);
			if (rc != COW_NYX_OK) goto cleanup;
			clus_buf[dir_entry_off] = 0xE5U;
			rc = _user_rw_dec(vol, user_start, vi.data_start_sec + (dir_entry_rc - 2U) * clus_secs, clus_buf, clus_secs, true);
			if (rc != COW_NYX_OK) goto cleanup;
		}
		new_free_clus = true_free + existing_dir_len;
		out_res->action = (existing_dir_len > 0U || current_balloon_clus > 0U) ? COW_BAL_REMOVED : COW_BAL_REPAIRED_FSINFO;
	}
	else if (target_balloon < current_balloon_clus)
	{
		/* Shrink existing balloon chains in place, keeping first target_balloon clusters */
		u32 kept_so_far = 0U;
		u32 active_p_start = 0U, active_p_tail = 0U, active_p_cnt = 0U;

		for (u32 pi = 0U; pi < 256U; pi++)
		{
			if (!exist_present[pi]) continue;
			u32 cur = exist_starts[pi];
			while (cur >= 2U && cur <= max_clus)
			{
				u32 nxt = fat_u32[cur] & 0x0FFFFFFFU;
				if (kept_so_far < target_balloon)
				{
					if (active_p_cnt == max_part_clus)
					{
						_mark_fat_entry(fat_u32, dirty_secs, active_p_tail, 0x0FFFFFFFU);
						new_part_starts[new_num_parts] = active_p_start;
						new_part_counts[new_num_parts] = active_p_cnt;
						new_num_parts++;
						active_p_cnt = 0U;
					}
					if (active_p_cnt == 0U)
					{
						active_p_start = cur;
					}
					else if ((fat_u32[active_p_tail] & 0x0FFFFFFFU) != cur)
					{
						_mark_fat_entry(fat_u32, dirty_secs, active_p_tail, cur);
					}
					active_p_tail = cur;
					active_p_cnt++;
					kept_so_far++;
				}
				else
				{
					_mark_fat_entry(fat_u32, dirty_secs, cur, 0U);
				}
				if (nxt >= 0x0FFFFFF8U) break;
				cur = nxt;
			}
		}
		if (active_p_cnt > 0U)
		{
			_mark_fat_entry(fat_u32, dirty_secs, active_p_tail, 0x0FFFFFFFU);
			new_part_starts[new_num_parts] = active_p_start;
			new_part_counts[new_num_parts] = active_p_cnt;
			new_num_parts++;
		}
		out_res->action = COW_BAL_SHRUNK;
	}
	else
	{
		/* Create or grow from FAT tail (descending pick + ascending link via alloc_bits) */
		u32 add_data_clus = target_balloon - current_balloon_clus;
		u32 total_pick = add_data_clus + extra_dir_clus;

		/* Allocate root extension / dir cluster first at very top tail */
		u32 tail_c = max_clus;
		if (existing_dir_len == 0U)
		{
			u32 dir_picked[2] = { 0U, 0U };
			u32 dp = 0U;
			while (tail_c >= 2U && dp < extra_dir_clus)
			{
				if ((fat_u32[tail_c] & 0x0FFFFFFFU) == 0U)
					dir_picked[dp++] = tail_c;
				tail_c--;
			}
			if (dp < extra_dir_clus) { rc = COW_NYX_ERR_FS_INVALID; goto cleanup; }

			u32 d_idx = 0U;
			if (!found_free_root)
			{
				u32 new_r = dir_picked[d_idx++];
				_mark_fat_entry(fat_u32, dirty_secs, root_chain[root_chain_len - 1U], new_r);
				_mark_fat_entry(fat_u32, dirty_secs, new_r, 0x0FFFFFFFU);
				memset(clus_buf, 0, clus_bytes);
				rc = _user_rw_dec(vol, user_start, vi.data_start_sec + (new_r - 2U) * clus_secs, clus_buf, clus_secs, true);
				if (rc != COW_NYX_OK) goto cleanup;
				free_root_c = new_r;
				free_root_off = 0U;
			}
			balloon_dir_clus = dir_picked[d_idx++];
			_mark_fat_entry(fat_u32, dirty_secs, balloon_dir_clus, 0x0FFFFFFFU);
			existing_dir_chain[0] = balloon_dir_clus;
			existing_dir_len = 1U;

			rc = _user_rw_dec(vol, user_start, vi.data_start_sec + (free_root_c - 2U) * clus_secs, clus_buf, clus_secs, false);
			if (rc != COW_NYX_OK) goto cleanup;
			_pack_sfn_entry(clus_buf + free_root_off, COW_BAL_DIR_DOS, 0x16U, balloon_dir_clus, 0U);
			rc = _user_rw_dec(vol, user_start, vi.data_start_sec + (free_root_c - 2U) * clus_secs, clus_buf, clus_secs, true);
			if (rc != COW_NYX_OK) goto cleanup;
		}
		(void)total_pick;

		/* Pick add_data_clus tail clusters descending and mark in alloc_bits */
		u32 picked = 0U;
		u32 low_picked = max_clus;
		while (tail_c >= 2U && picked < add_data_clus)
		{
			if ((fat_u32[tail_c] & 0x0FFFFFFFU) == 0U)
			{
				_bitset_set(alloc_bits, tail_c);
				low_picked = tail_c;
				picked++;
			}
			if (tail_c == 2U) break;
			tail_c--;
		}
		if (picked < add_data_clus)
		{
			rc = COW_NYX_ERR_FS_INVALID;
			goto cleanup;
		}

		/* Preserve existing parts and append newly picked clusters ascending into <= max_part_clus chunks */
		u32 active_p_start = 0U, active_p_tail = 0U, active_p_cnt = 0U;
		for (u32 pi = 0U; pi < 256U; pi++)
		{
			if (!exist_present[pi]) continue;
			if (exist_counts[pi] == max_part_clus && active_p_cnt == 0U)
			{
				new_part_starts[new_num_parts] = exist_starts[pi];
				new_part_counts[new_num_parts] = exist_counts[pi];
				new_num_parts++;
			}
			else
			{
				u32 cur = exist_starts[pi];
				while (cur >= 2U && cur <= max_clus)
				{
					u32 nxt = fat_u32[cur] & 0x0FFFFFFFU;
					if (active_p_cnt == max_part_clus)
					{
						_mark_fat_entry(fat_u32, dirty_secs, active_p_tail, 0x0FFFFFFFU);
						new_part_starts[new_num_parts] = active_p_start;
						new_part_counts[new_num_parts] = active_p_cnt;
						new_num_parts++;
						active_p_cnt = 0U;
					}
					if (active_p_cnt == 0U) active_p_start = cur;
					else if ((fat_u32[active_p_tail] & 0x0FFFFFFFU) != cur)
						_mark_fat_entry(fat_u32, dirty_secs, active_p_tail, cur);
					active_p_tail = cur;
					active_p_cnt++;
					if (nxt >= 0x0FFFFFF8U) break;
					cur = nxt;
				}
			}
		}

		for (u32 c = low_picked; c <= max_clus; c++)
		{
			if (!_bitset_get(alloc_bits, c)) continue;
			if (active_p_cnt == max_part_clus)
			{
				_mark_fat_entry(fat_u32, dirty_secs, active_p_tail, 0x0FFFFFFFU);
				new_part_starts[new_num_parts] = active_p_start;
				new_part_counts[new_num_parts] = active_p_cnt;
				new_num_parts++;
				active_p_cnt = 0U;
			}
			if (active_p_cnt == 0U) active_p_start = c;
			else _mark_fat_entry(fat_u32, dirty_secs, active_p_tail, c);
			active_p_tail = c;
			active_p_cnt++;
		}
		if (active_p_cnt > 0U)
		{
			_mark_fat_entry(fat_u32, dirty_secs, active_p_tail, 0x0FFFFFFFU);
			new_part_starts[new_num_parts] = active_p_start;
			new_part_counts[new_num_parts] = active_p_cnt;
			new_num_parts++;
		}

		out_res->action = (current_balloon_clus > 0U) ? COW_BAL_GROWN : COW_BAL_CREATED;
	}

	if (target_balloon > 0U)
	{
		if (2U + new_num_parts > clus_bytes / 32U)
		{
			rc = COW_NYX_ERR_UNSUPPORTED;
			goto cleanup;
		}
		memset(clus_buf, 0, clus_bytes);
		_pack_sfn_entry(clus_buf + 0U, (const u8 *)".          ", 0x10U, balloon_dir_clus, 0U);
		_pack_sfn_entry(clus_buf + 32U, (const u8 *)"..         ", 0x10U, 0U, 0U);
		for (u32 pi = 0U; pi < new_num_parts; pi++)
		{
			u8 dos11[11];
			_format_balloon_dos(pi, dos11);
			_pack_sfn_entry(clus_buf + (2U + pi) * 32U, dos11, 0x07U, new_part_starts[pi], new_part_counts[pi] * clus_bytes);
		}
		rc = _user_rw_dec(vol, user_start, vi.data_start_sec + (existing_dir_chain[0] - 2U) * clus_secs,
			clus_buf, clus_secs, true);
		if (rc != COW_NYX_OK) goto cleanup;
	}

	/* Flush contiguous dirty FAT sectors across all num_fats */
	if (vol->hal.progress_fn)
		vol->hal.progress_fn(vol->hal.user_ctx, "Encrypting & flushing FAT...", 80);

	u32 sec_i = 0U;
	while (sec_i < vi.fatsz32)
	{
		if (!_bitset_get(dirty_secs, sec_i)) { sec_i++; continue; }
		u32 r_start = sec_i;
		while (sec_i < vi.fatsz32 && _bitset_get(dirty_secs, sec_i) && (sec_i - r_start) < 512U)
			sec_i++;
		u32 r_cnt = sec_i - r_start;
		for (u32 fat_i = 0U; fat_i < vi.num_fats; fat_i++)
		{
			rc = _user_rw_dec(vol, user_start, vi.rsvd_sec_cnt + fat_i * vi.fatsz32 + r_start,
				(u8 *)fat_u32 + r_start * 512U, r_cnt, true);
			if (rc != COW_NYX_OK) goto cleanup;
		}
	}

	if (vi.has_fsinfo)
	{
		u32 nxt_hint = first_free_clus;
		if (out_res->action == COW_BAL_SHRUNK || out_res->action == COW_BAL_REMOVED)
		{
			for (u32 c = 2U; c <= max_clus; c++)
			{
				if ((fat_u32[c] & 0x0FFFFFFFU) == 0U)
				{
					nxt_hint = c;
					break;
				}
			}
		}
		else if (nxt_hint <= max_clus && (fat_u32[nxt_hint] & 0x0FFFFFFFU) != 0U)
		{
			nxt_hint = 0xFFFFFFFFU;
			for (u32 c = 2U; c <= max_clus; c++)
			{
				if ((fat_u32[c] & 0x0FFFFFFFU) == 0U) { nxt_hint = c; break; }
			}
		}
		if (nxt_hint > max_clus || nxt_hint < 2U)
			nxt_hint = 0xFFFFFFFFU;
		_wr_le32(fsinfo + 0x1E8U, new_free_clus);
		_wr_le32(fsinfo + 0x1ECU, nxt_hint);
		rc = _user_rw_dec(vol, user_start, vi.fsinfo_sec, fsinfo, 1U, true);
		if (rc == COW_NYX_OK && vi.bk_boot_sec > 0U && (vi.bk_boot_sec + vi.fsinfo_sec) < vi.rsvd_sec_cnt)
			rc = _user_rw_dec(vol, user_start, vi.bk_boot_sec + vi.fsinfo_sec, fsinfo, 1U, true);
		if (rc != COW_NYX_OK) goto cleanup;
	}

	rc = cow_nyx_commit(vol);
	if (rc == COW_NYX_OK)
	{
		out_res->new_balloon_clusters = target_balloon;
		out_res->target_free_clusters = new_free_clus;
		out_res->effective_free_bytes = (u64)new_free_clus * clus_bytes;
		out_res->num_balloon_parts = new_num_parts;
	}

cleanup:
	free(clus_buf);
	free(alloc_bits);
	free(visited_bits);
	free(dirty_secs);
	free(fat_u32);
	return rc;
}

int cow_nyx_probe_raw_snapshot(
	const cow_nyx_hal_t *hal,
	const char *snap_path,
	cow_nyx_raw_snap_hdr_t *out_hdr)
{
	if (!hal || !hal->file_open || !hal->file_read || !hal->file_close || !snap_path || !*snap_path)
		return COW_NYX_ERR_ARG;

	void *sh = NULL;
	u64 fsize = 0ULL;
	if (hal->file_open(hal->user_ctx, snap_path, false, &fsize, &sh) != 0 || !sh)
		return COW_NYX_ERR_IO;

	cow_nyx_raw_snap_hdr_t shdr;
	memset(&shdr, 0, sizeof(shdr));
	if (fsize < sizeof(shdr) || hal->file_read(hal->user_ctx, sh, 0ULL, &shdr, sizeof(shdr)) != 0)
	{
		hal->file_close(hal->user_ctx, sh);
		return COW_NYX_ERR_CORRUPT;
	}

	if (shdr.magic != COW_NYX_RAW_SNAP_MAGIC ||
	    shdr.snap_version != COW_NYX_RAW_SNAP_VERSION ||
	    shdr.cow_version != COW_NYX_VERSION_V2 ||
	    shdr.prefix_sectors != COW_NYX_DEFAULT_PREFIX_SECTS ||
	    shdr.active_sb_slot >= COW_NYX_SUPERBLOCK_SLOTS ||
	    shdr.next_free_slot < shdr.first_data_slot ||
	    shdr.next_free_slot > shdr.total_slots)
	{
		hal->file_close(hal->user_ctx, sh);
		return COW_NYX_ERR_CORRUPT;
	}

	u32 hcrc = cow_nyx_crc32(hal, 0U, &shdr, offsetof(cow_nyx_raw_snap_hdr_t, header_crc32));
	if (hcrc != shdr.header_crc32)
	{
		hal->file_close(hal->user_ctx, sh);
		return COW_NYX_ERR_CORRUPT;
	}

	u64 payload_bytes = (u64)shdr.next_free_slot * COW_NYX_BLOCK_BYTES;
	if (fsize != (u64)sizeof(shdr) + payload_bytes)
	{
		hal->file_close(hal->user_ctx, sh);
		return COW_NYX_ERR_CORRUPT;
	}

	const u32 buf_sz = 64U * 1024U;
	u8 *buf = (u8 *)malloc(buf_sz);
	if (!buf)
	{
		hal->file_close(hal->user_ctx, sh);
		return COW_NYX_ERR_NO_MEM;
	}

	u64 off = 0ULL;
	u32 pcrc = 0U;
	int rc = COW_NYX_OK;
	while (off < payload_bytes)
	{
		u64 rem = payload_bytes - off;
		u32 cur = (rem > (u64)buf_sz) ? buf_sz : (u32)rem;
		if (hal->file_read(hal->user_ctx, sh, (u64)sizeof(shdr) + off, buf, cur) != 0)
		{
			rc = COW_NYX_ERR_IO;
			break;
		}
		pcrc = cow_nyx_crc32(hal, pcrc, buf, cur);
		off += cur;
	}

	free(buf);
	hal->file_close(hal->user_ctx, sh);
	if (rc != COW_NYX_OK)
		return rc;
	if (pcrc != shdr.payload_crc32)
		return COW_NYX_ERR_CORRUPT;

	if (out_hdr)
		*out_hdr = shdr;
	return COW_NYX_OK;
}

int cow_nyx_save_raw_snapshot(
	cow_nyx_vol_t *vol,
	const char *snap_path,
	cow_nyx_raw_snap_hdr_t *opt_out_hdr)
{
	if (!vol || !vol->opened || !snap_path || !*snap_path)
		return COW_NYX_ERR_ARG;

	int rc = cow_nyx_commit(vol);
	if (rc != COW_NYX_OK)
		return rc;

	const u32 buf_sz = 64U * 1024U;
	u8 *buf = (u8 *)malloc(buf_sz);
	if (!buf)
		return COW_NYX_ERR_NO_MEM;

	void *sh = NULL;
	if (vol->hal.file_open(vol->hal.user_ctx, snap_path, true, NULL, &sh) != 0 || !sh)
	{
		free(buf);
		return COW_NYX_ERR_IO;
	}

	cow_nyx_raw_snap_hdr_t shdr;
	memset(&shdr, 0, sizeof(shdr));
	if (vol->hal.file_write(vol->hal.user_ctx, sh, 0ULL, &shdr, sizeof(shdr)) != 0)
	{
		vol->hal.file_close(vol->hal.user_ctx, sh);
		free(buf);
		return COW_NYX_ERR_IO;
	}

	u64 pfx_bytes = (u64)vol->hdr.prefix_sectors * COW_NYX_SECTOR_SIZE;
	u64 payload_bytes = (u64)vol->hdr.next_free_slot * COW_NYX_BLOCK_BYTES;
	u64 off = 0ULL;
	u32 pcrc = 0U;
	while (off < payload_bytes)
	{
		u64 rem = payload_bytes - off;
		u32 cur = (rem > (u64)buf_sz) ? buf_sz : (u32)rem;
		rc = _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
			pfx_bytes + off, buf, cur, false);
		if (rc != COW_NYX_OK ||
		    vol->hal.file_write(vol->hal.user_ctx, sh, (u64)sizeof(shdr) + off, buf, cur) != 0)
		{
			vol->hal.file_close(vol->hal.user_ctx, sh);
			free(buf);
			return COW_NYX_ERR_IO;
		}
		pcrc = cow_nyx_crc32(&vol->hal, pcrc, buf, cur);
		off += cur;
	}

	shdr.magic                 = COW_NYX_RAW_SNAP_MAGIC;
	shdr.snap_version          = COW_NYX_RAW_SNAP_VERSION;
	shdr.active_sb_slot        = vol->active_sb_slot;
	shdr.cow_version           = vol->hdr.version;
	shdr.seq_num               = vol->hdr.seq_num;
	shdr.gpp_total_sectors     = vol->hdr.gpp_total_sectors;
	shdr.physical_base_sectors = vol->hdr.physical_base_sectors;
	shdr.prefix_sectors        = vol->hdr.prefix_sectors;
	shdr.total_slots           = vol->hdr.total_slots;
	shdr.first_data_slot       = vol->first_data_slot;
	shdr.next_free_slot        = vol->hdr.next_free_slot;
	shdr.l1_entries            = vol->l1_entries;
	shdr.l1_slot_a             = vol->hdr.l1_slot_a;
	shdr.l1_slot_b             = vol->hdr.l1_slot_b;
	shdr.flags                 = vol->hdr.flags;
	shdr.dirty_data_blocks     = vol->hdr.dirty_data_blocks;
	shdr.dirty_l2_pages        = vol->hdr.dirty_l2_pages;
	shdr.base_gen_id           = vol->hdr.base_gen_id;
	memcpy(shdr.overlay_uuid, vol->hdr.overlay_uuid, 16U);
	shdr.payload_crc32         = pcrc;
	shdr.header_crc32          = cow_nyx_crc32(&vol->hal, 0U, &shdr, offsetof(cow_nyx_raw_snap_hdr_t, header_crc32));

	int wrc = vol->hal.file_write(vol->hal.user_ctx, sh, 0ULL, &shdr, sizeof(shdr));
	vol->hal.file_close(vol->hal.user_ctx, sh);
	free(buf);
	if (wrc != 0)
		return COW_NYX_ERR_IO;

	if (opt_out_hdr)
		*opt_out_hdr = shdr;
	return COW_NYX_OK;
}

int cow_nyx_restore_raw_snapshot(
	cow_nyx_vol_t *vol,
	const char *snap_path,
	cow_nyx_raw_snap_hdr_t *opt_out_hdr)
{
	if (!vol || !vol->opened || !snap_path || !*snap_path)
		return COW_NYX_ERR_ARG;

	cow_nyx_raw_snap_hdr_t shdr;
	int rc = cow_nyx_probe_raw_snapshot(&vol->hal, snap_path, &shdr);
	if (rc != COW_NYX_OK)
		return rc;

	if (vol->hdr.base_gen_id != 0ULL && shdr.base_gen_id != 0ULL && shdr.base_gen_id != vol->hdr.base_gen_id)
		return COW_NYX_ERR_UNSUPPORTED;

	u64 archive_bytes = 0ULL;
	for (u32 i = 0U; i < vol->num_parts; i++)
		archive_bytes += vol->part_sizes[i];

	u64 pfx_bytes = (u64)shdr.prefix_sectors * COW_NYX_SECTOR_SIZE;
	u64 payload_bytes = (u64)shdr.next_free_slot * COW_NYX_BLOCK_BYTES;
	if (archive_bytes < pfx_bytes + payload_bytes)
		return COW_NYX_ERR_POOL_FULL;

	const u32 buf_sz = 64U * 1024U;
	u8 *buf = (u8 *)malloc(buf_sz);
	if (!buf)
		return COW_NYX_ERR_NO_MEM;

	void *sh = NULL;
	u64 fsize = 0ULL;
	if (vol->hal.file_open(vol->hal.user_ctx, snap_path, false, &fsize, &sh) != 0 || !sh)
	{
		free(buf);
		return COW_NYX_ERR_IO;
	}

	for (u32 w = 0U; w < COW_NYX_L2_CACHE_WAYS; w++)
	{
		vol->l2_cache[w].valid = false;
		vol->l2_cache[w].dirty = false;
	}
	vol->dirty_hdr = false;

	u64 off = 0ULL;
	while (off < payload_bytes)
	{
		u64 rem = payload_bytes - off;
		u32 cur = (rem > (u64)buf_sz) ? buf_sz : (u32)rem;
		if (vol->hal.file_read(vol->hal.user_ctx, sh, (u64)sizeof(shdr) + off, buf, cur) != 0 ||
		    _split_rw(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts,
		              pfx_bytes + off, buf, cur, true) != COW_NYX_OK)
		{
			vol->hal.file_close(vol->hal.user_ctx, sh);
			free(buf);
			return COW_NYX_ERR_IO;
		}
		off += cur;
	}

	vol->hal.file_close(vol->hal.user_ctx, sh);
	free(buf);

	rc = _pick_active_sb(&vol->hal, vol->part_handles, vol->part_sizes, vol->num_parts, archive_bytes,
		&vol->hdr, &vol->active_sb_slot, &vol->l1_entries, &vol->first_data_slot, vol->l1_table);
	if (rc != COW_NYX_OK)
		return rc;

	u64 backing_slots = (archive_bytes - (u64)vol->hdr.prefix_sectors * COW_NYX_SECTOR_SIZE) / COW_NYX_BLOCK_BYTES;
	u32 eff = (u32)MIN(backing_slots, (u64)vol->hdr.total_slots);
	if (vol->hdr.max_overlay_slots > 0U)
		eff = MIN(eff, vol->hdr.max_overlay_slots);
	vol->effective_total_slots = eff;
	vol->phys_base_sectors = (vol->hdr.physical_base_sectors > 0ULL)
		? vol->hdr.physical_base_sectors
		: vol->hdr.gpp_total_sectors;

	if (opt_out_hdr)
		*opt_out_hdr = shdr;
	return COW_NYX_OK;
}
