/*
 * Copyright (c) 2019-2022 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <stdlib.h>

#include <bdk.h>

#include "emummc.h"
#include "../config.h"
#include <ianos/ianos.h>
#include <libs/fatfs/ff.h>
#include <storage/cow_bso.h>

static cow_bso_ctx_t s_cow_ctx = { 0 };
static char s_cow_base_path[0x200];

static bool _cow_overlay_requires_bso(const char *base_path)
{
	char fpath[0x240];
	strcpy(fpath, base_path);
	strcat(fpath, "/eMMC/00");

	FIL fp;
	if (f_open(&fp, fpath, FA_READ) != FR_OK)
	{
		strcpy(fpath, base_path);
		strcat(fpath, "/cow_autoinit");
		return (f_stat(fpath, NULL) == FR_OK);
	}

	bool found = false;
	for (u32 slot = 0U; slot < 2U; slot++)
	{
		u32 magic = 0U;
		UINT br = 0U;
		FSIZE_t off = (FSIZE_t)(0x2000000ULL + ((u64)slot * 0x4000ULL));
		if (f_lseek(&fp, off) == FR_OK && f_read(&fp, &magic, sizeof(magic), &br) == FR_OK &&
		    br == sizeof(magic) && magic == 0x30574F43U)
		{
			found = true;
			break;
		}
	}
	f_close(&fp);
	return found;
}

static void _cow_gfx_show_console(bool clear_screen)
{
	bool was_muted = gfx_con.mute;
	gfx_con.mute = false;
	gfx_con.fntsz = 16;
	gfx_con_setcol(TXT_CLR_DEFAULT, 1, TXT_CLR_BG);
	if (clear_screen || was_muted)
	{
		gfx_clear_grey(0x1B);
		gfx_con_setpos(0, 0);
	}
	display_backlight_brightness(h_cfg.backlight ? h_cfg.backlight : 100, 1000);
}

static void _cow_bind_ops(const char *base_path)
{
	s_cow_ctx.api_magic                        = COW_BSO_MAGIC_V2;
	s_cow_ctx.active                           = 0U;
	s_cow_ctx.rc                               = 0U;
	s_cow_ctx.force_disable                    = (u32)h_cfg.emummc_force_disable;
	s_cow_ctx.emu_id                           = emu_cfg.id;
	strncpy(s_cow_base_path, base_path ? base_path : "", sizeof(s_cow_base_path) - 1U);
	s_cow_base_path[sizeof(s_cow_base_path) - 1U] = 0;
	s_cow_ctx.base_path                        = s_cow_base_path;
	s_cow_ctx.emmc_storage                     = &emmc_storage;
	s_cow_ctx.fn_emmc_set_partition            = emmc_set_partition;
	s_cow_ctx.fn_sdmmc_storage_read            = sdmmc_storage_read;
	s_cow_ctx.fn_f_open                        = f_open;
	s_cow_ctx.fn_f_close                       = f_close;
	s_cow_ctx.fn_f_read                        = f_read;
	s_cow_ctx.fn_f_write                       = f_write;
	s_cow_ctx.fn_f_lseek                       = f_lseek;
	s_cow_ctx.fn_f_sync                        = f_sync;
	s_cow_ctx.fn_f_stat                        = f_stat;
	s_cow_ctx.fn_f_mkdir                       = f_mkdir;
	s_cow_ctx.fn_f_unlink                      = f_unlink;
	s_cow_ctx.fn_f_rename                      = f_rename;
	s_cow_ctx.fn_f_getfree                     = f_getfree;
	s_cow_ctx.fn_se_sha_hash_256_oneshot       = se_sha_hash_256_oneshot;
	s_cow_ctx.fn_se_sha_hash_256_partial_start = se_sha_hash_256_partial_start;
	s_cow_ctx.fn_se_sha_hash_256_partial_update = se_sha_hash_256_partial_update;
	s_cow_ctx.fn_se_sha_hash_256_partial_end   = se_sha_hash_256_partial_end;
	s_cow_ctx.fn_crc32_calc                    = crc32_calc;
	s_cow_ctx.fn_gfx_printf                    = gfx_printf;
	s_cow_ctx.fn_malloc                        = malloc;
	s_cow_ctx.fn_zalloc                        = zalloc;
	s_cow_ctx.fn_free                          = free;
	s_cow_ctx.fn_gfx_show_console              = _cow_gfx_show_console;
	s_cow_ctx.fn_gfx_con_getpos                = gfx_con_getpos;
	s_cow_ctx.fn_gfx_con_setpos                = gfx_con_setpos;
}

emummc_cfg_t emu_cfg = { 0 };

void emummc_load_cfg()
{
	emu_cfg.enabled = 0;
	emu_cfg.path = NULL;
	emu_cfg.sector = 0;
	emu_cfg.id = 0;
	emu_cfg.file_based_part_size = 0;
	emu_cfg.active_part = 0;
	emu_cfg.fs_ver = 0;
	if (!emu_cfg.nintendo_path)
		emu_cfg.nintendo_path = (char *)malloc(0x200);
	if (!emu_cfg.emummc_file_based_path)
		emu_cfg.emummc_file_based_path = (char *)malloc(0x200);

	emu_cfg.nintendo_path[0] = 0;
	emu_cfg.emummc_file_based_path[0] = 0;

	LIST_INIT(ini_sections);
	if (!ini_parse(&ini_sections, "emuMMC/emummc.ini", false))
	{
		LIST_FOREACH_ENTRY(ini_sec_t, ini_sec, &ini_sections, link)
		{
			if (ini_sec->type == INI_CHOICE)
			{
				if (strcmp(ini_sec->name, "emummc"))
					continue;

				LIST_FOREACH_ENTRY(ini_kv_t, kv, &ini_sec->kvs, link)
				{
					if (!strcmp("enabled",            kv->key))
						emu_cfg.enabled = atoi(kv->val);
					else if (!strcmp("sector",        kv->key))
						emu_cfg.sector  = strtol(kv->val, NULL, 16);
					else if (!strcmp("id",            kv->key))
						emu_cfg.id      = strtol(kv->val, NULL, 16);
					else if (!strcmp("path",          kv->key))
						emu_cfg.path   = kv->val;
					else if (!strcmp("nintendo_path", kv->key))
						strcpy(emu_cfg.nintendo_path, kv->val);
				}
				break;
			}
		}
	}
	else if (!f_stat("emuMMC/COW0/cow_autoinit", NULL))
	{
		// First-boot zero-PC auto-init fallback when emuMMC/emummc.ini does not exist yet.
		static char s_cow_default_path[] = "emuMMC/COW0";
		emummc_set_path(s_cow_default_path);
	}
}

bool emummc_set_path(char *path)
{
	FIL fp;
	bool found = false;

	strcpy(emu_cfg.emummc_file_based_path, path);
	strcat(emu_cfg.emummc_file_based_path, "/raw_based");

	if (!f_open(&fp, emu_cfg.emummc_file_based_path, FA_READ))
	{
		if (!f_read(&fp, &emu_cfg.sector, 4, NULL))
			if (emu_cfg.sector)
				found = true;
	}
	else
	{
		strcpy(emu_cfg.emummc_file_based_path, path);
		strcat(emu_cfg.emummc_file_based_path, "/file_based");

		if (!f_stat(emu_cfg.emummc_file_based_path, NULL))
		{
			emu_cfg.sector = 0;
			emu_cfg.path = path;

			found = true;
		}
		else
		{
			strcpy(emu_cfg.emummc_file_based_path, path);
			strcat(emu_cfg.emummc_file_based_path, "/cow_autoinit");
			if (!f_stat(emu_cfg.emummc_file_based_path, NULL))
			{
				emu_cfg.sector = 0;
				emu_cfg.path = path;

				found = true;
			}
		}
	}

	if (found)
	{
		emu_cfg.enabled = 1;

		// Get ID from path.
		u32 id_from_path = 0;
		u32 path_size = strlen(path);
		if (path_size >= 4)
			memcpy(&id_from_path, path + path_size - 4, 4);
		emu_cfg.id = id_from_path;

		strcpy(emu_cfg.nintendo_path, path);
		strcat(emu_cfg.nintendo_path, "/Nintendo");
	}

	return found;
}

static int emummc_raw_get_part_off(int part_idx)
{
	switch (part_idx)
	{
	case 0:
		return 2;
	case 1:
		return 0;
	case 2:
		return 1;
	}
	return 2;
}

int emummc_storage_init_mmc()
{
	FILINFO fno;
	emu_cfg.active_part = 0;
	s_cow_ctx.active = 0U;

	// Always init eMMC even when in emuMMC. eMMC is needed from the emuMMC driver anyway.
	if (emmc_initialize(false))
		return 2;

	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		return 0;

	if (sd_mount())
		goto out;

	if (!emu_cfg.sector)
	{
		_cow_bind_ops(emu_cfg.path);
		if (s_cow_ctx.loaded && s_cow_ctx.fn_run_init)
			s_cow_ctx.fn_run_init(&s_cow_ctx);
		else
			ianos_static_module("bootloader/sys/libsys_cow.bso", &s_cow_ctx);

		if (!s_cow_ctx.loaded)
		{
			if (_cow_overlay_requires_bso(emu_cfg.path))
			{
				_cow_gfx_show_console(false);
				EPRINTF("Missing/old bootloader/sys/libsys_cow.bso!");
				goto out;
			}
		}
		else if (s_cow_ctx.rc)
		{
			goto out;
		}

		strcpy(emu_cfg.emummc_file_based_path, emu_cfg.path);
		strcat(emu_cfg.emummc_file_based_path, "/eMMC");

		if (f_stat(emu_cfg.emummc_file_based_path, &fno))
		{
			EPRINTF("Failed to open eMMC folder.");
			goto out;
		}
		f_chmod(emu_cfg.emummc_file_based_path, AM_ARC, AM_ARC);

		strcat(emu_cfg.emummc_file_based_path, "/00");
		if (f_stat(emu_cfg.emummc_file_based_path, &fno))
		{
			EPRINTF("Failed to open emuMMC rawnand.");
			goto out;
		}
		emu_cfg.file_based_part_size = fno.fsize >> 9;
	}

	return 0;

out:
	return 1;
}

int emummc_storage_end()
{
	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		emmc_end();
	else
		sd_end();

	return 0;
}

int emummc_storage_read(u32 sector, u32 num_sectors, void *buf)
{
	FIL fp;
	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		return sdmmc_storage_read(&emmc_storage, sector, num_sectors, buf);
	else if (emu_cfg.sector)
	{
		sector += emu_cfg.sector;
		sector += emummc_raw_get_part_off(emu_cfg.active_part) * 0x2000;
		return sdmmc_storage_read(&sd_storage, sector, num_sectors, buf);
	}
	else
	{
		if (!emu_cfg.active_part && s_cow_ctx.active && s_cow_ctx.fn_read_gpp)
			return s_cow_ctx.fn_read_gpp(sector, num_sectors, buf);

		if (!emu_cfg.active_part)
		{
			u32 file_part = sector / emu_cfg.file_based_part_size;
			sector = sector % emu_cfg.file_based_part_size;
			if (file_part >= 10)
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 2, 10);
			else
			{
				emu_cfg.emummc_file_based_path[strlen(emu_cfg.emummc_file_based_path) - 2] = '0';
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 1, 10);
			}
		}
		if (f_open(&fp, emu_cfg.emummc_file_based_path, FA_READ))
		{
			EPRINTF("Failed to open emuMMC image.");
			return 1;
		}
		f_lseek(&fp, (u64)sector << 9);
		if (f_read(&fp, buf, (u64)num_sectors << 9, NULL))
		{
			EPRINTF("Failed to read emuMMC image.");
			f_close(&fp);
			return 1;
		}

		f_close(&fp);
		return 0;
	}
}

int emummc_storage_write(u32 sector, u32 num_sectors, void *buf)
{
	FIL fp;
	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		return sdmmc_storage_write(&emmc_storage, sector, num_sectors, buf);
	else if (emu_cfg.sector)
	{
		sector += emu_cfg.sector;
		sector += emummc_raw_get_part_off(emu_cfg.active_part) * 0x2000;
		return sdmmc_storage_write(&sd_storage, sector, num_sectors, buf);
	}
	else
	{
		if (!emu_cfg.active_part && s_cow_ctx.active)
			return 1;

		if (!emu_cfg.active_part)
		{
			u32 file_part = sector / emu_cfg.file_based_part_size;
			sector = sector % emu_cfg.file_based_part_size;
			if (file_part >= 10)
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 2, 10);
			else
			{
				emu_cfg.emummc_file_based_path[strlen(emu_cfg.emummc_file_based_path) - 2] = '0';
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 1, 10);
			}
		}

		if (f_open(&fp, emu_cfg.emummc_file_based_path, FA_WRITE))
			return 1;

		f_lseek(&fp, (u64)sector << 9);
		if (f_write(&fp, buf, (u64)num_sectors << 9, NULL))
		{
			f_close(&fp);
			return 1;
		}

		f_close(&fp);
		return 0;
	}
}

int emummc_storage_set_mmc_partition(u32 partition)
{
	emu_cfg.active_part = partition;
	emmc_set_partition(partition);

	if (!emu_cfg.enabled || h_cfg.emummc_force_disable || emu_cfg.sector)
		return 0;
	else
	{
		strcpy(emu_cfg.emummc_file_based_path, emu_cfg.path);
		strcat(emu_cfg.emummc_file_based_path, "/eMMC");

		switch (partition)
		{
		case 0:
			strcat(emu_cfg.emummc_file_based_path, "/00");
			break;
		case 1:
			strcat(emu_cfg.emummc_file_based_path, "/BOOT0");
			break;
		case 2:
			strcat(emu_cfg.emummc_file_based_path, "/BOOT1");
			break;
		}

		return 0;
	}
}
