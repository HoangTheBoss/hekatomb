/*
 * cowMMC DRAM Module (libsys_cow.bso) ABI for Hekate Bootloader
 */

#ifndef _COW_BSO_H_
#define _COW_BSO_H_

#include <utils/types.h>
#include <libs/fatfs/ff.h>
#include <storage/sdmmc.h>

#define COW_BSO_MAGIC 0x42574F43U /* "COWB" */

typedef struct _cow_bso_ctx_t
{
	u32 api_magic;
	u32 loaded;
	u32 active;
	u32 rc;
	u32 force_disable;
	u32 emu_id;
	const char *base_path;

	sdmmc_storage_t *emmc_storage;
	int (*fn_emmc_set_partition)(u32 partition);
	int (*fn_sdmmc_storage_read)(sdmmc_storage_t *storage, u32 sector, u32 num_sectors, void *buf);

	FRESULT (*fn_f_open)(FIL *fp, const TCHAR *path, BYTE mode);
	FRESULT (*fn_f_close)(FIL *fp);
	FRESULT (*fn_f_read)(FIL *fp, void *buff, UINT btr, UINT *br);
	FRESULT (*fn_f_write)(FIL *fp, const void *buff, UINT btw, UINT *bw);
	FRESULT (*fn_f_lseek)(FIL *fp, FSIZE_t ofs);
	FRESULT (*fn_f_sync)(FIL *fp);
	FRESULT (*fn_f_stat)(const TCHAR *path, FILINFO *fno);
	FRESULT (*fn_f_mkdir)(const TCHAR *path);
	FRESULT (*fn_f_unlink)(const TCHAR *path);
	FRESULT (*fn_f_rename)(const TCHAR *path_old, const TCHAR *path_new);
	FRESULT (*fn_f_getfree)(const TCHAR *path, DWORD *nclst, FATFS **fatfs);

	int (*fn_se_sha_hash_256_oneshot)(void *hash, const void *src, u32 size);
	int (*fn_se_sha_hash_256_partial_start)(void *hash, const void *src, u32 size, bool is_oneshot);
	int (*fn_se_sha_hash_256_partial_update)(void *hash, const void *src, u32 size, bool is_oneshot);
	int (*fn_se_sha_hash_256_partial_end)(void *hash, u64 total_size, const void *src, u32 src_size, bool is_oneshot);
	u32 (*fn_crc32_calc)(u32 crc, const u8 *buf, u32 len);
	void (*fn_gfx_printf)(const char *fmt, ...);

	void *(*fn_malloc)(u32 size);
	void *(*fn_zalloc)(u32 size);
	void (*fn_free)(void *ptr);

	int (*fn_run_init)(struct _cow_bso_ctx_t *ctx);
	int (*fn_read_gpp)(u32 sector, u32 num_sectors, void *buf);
} cow_bso_ctx_t;

#endif
