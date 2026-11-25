/*
 * Copyright (c) 2026 MooSwitch Contributors
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

#ifndef _MOOSWITCH_UPDATE_H_
#define _MOOSWITCH_UPDATE_H_

#define MSW_BSO_MAGIC                0x3157534DU /* "MSW1" */
#define MOOSWITCH_STAGING_DIR        "config/mooswitch/staging"
#define MOOSWITCH_STAGING_MANIFEST   "config/mooswitch/staging/pack-manifest.json"
#define MOOSWITCH_MANIFEST_DST       "pack-manifest.json"
#define MOOSWITCH_MAX_MANIFEST_BYTES (512U * 1024U)
#define MOOSWITCH_MAX_ENTRIES        2048U
#define MOOSWITCH_PATH_MAX           256U
#define MOOSWITCH_JOIN_MAX           512U
#define MOOSWITCH_DIR_STACK_MAX      64U

#ifdef MOOSWITCH_UPDATE_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t  BYTE;
typedef char     TCHAR;
typedef uint64_t FSIZE_t;

typedef enum {
	FR_OK = 0,
	FR_DISK_ERR,
	FR_INT_ERR,
	FR_NOT_READY,
	FR_NO_FILE,
	FR_NO_PATH,
	FR_INVALID_NAME,
	FR_DENIED,
	FR_EXIST,
	FR_INVALID_OBJECT,
	FR_WRITE_PROTECTED,
	FR_INVALID_DRIVE,
	FR_NOT_ENABLED,
	FR_NO_FILESYSTEM,
	FR_MKFS_ABORTED,
	FR_TIMEOUT,
	FR_LOCKED,
	FR_NOT_ENOUGH_CORE,
	FR_TOO_MANY_OPEN_FILES,
	FR_INVALID_PARAMETER
} FRESULT;

#define AM_RDO 0x01
#define AM_DIR 0x10

typedef struct {
	FSIZE_t fsize;
	BYTE    fattrib;
	TCHAR   fname[256];
} FILINFO;

typedef struct {
	void *host_dir;
} DIR;

#else
#include <utils/types.h>
#include <libs/fatfs/ff.h>
#endif

typedef struct _msw_fs_ops_t
{
	FRESULT (*stat)(const TCHAR *path, FILINFO *fno);
	FRESULT (*mkdir)(const TCHAR *path);
	FRESULT (*unlink)(const TCHAR *path);
	FRESULT (*rename)(const TCHAR *path_old, const TCHAR *path_new);
	FRESULT (*chmod)(const TCHAR *path, BYTE attr, BYTE mask);
	FRESULT (*opendir)(DIR *dp, const TCHAR *path);
	FRESULT (*readdir)(DIR *dp, FILINFO *fno);
	FRESULT (*closedir)(DIR *dp);
	void   *(*read_file)(const char *path, u32 *fsize);
	void   *(*malloc)(u32 size);
	void   *(*zalloc)(u32 size);
	void    (*free)(void *ptr);
	void    (*show_console)(void);
	void    (*printf)(const char *fmt, ...);
	void    (*msleep)(u32 ms);
	void    (*btn_wait)(void);
} msw_fs_ops_t;

typedef struct _msw_bso_ctx_t
{
	u32          api_magic;
	u32          applied;
	msw_fs_ops_t ops;
} msw_bso_ctx_t;

bool mooswitch_apply_staged_update_with_ops(const msw_fs_ops_t *ops);

#endif /* _MOOSWITCH_UPDATE_H_ */
