/*
 * cowMMC DRAM Module (libsys_cow.bso) for Hekate
 * Implements zero-PC v2 auto-init, pre-boot physical sysMMC digest
 * verification, SD-safe thin pool growth, and COW-routed virtual GPP reads.
 */

#include <stddef.h>
#include <utils/types.h>
#include <module.h>
#include <storage/mbr_gpt.h>
#include <storage/sdmmc.h>
#include <sec/se_t210.h>
#include <libs/fatfs/ff.h>
#include <storage/cow_bso.h>

#ifdef COW_BSO_MODULE

#define COW_CLR_DEF                  0xFFCCCCCCU
#define COW_CLR_CYAN                 0xFF00CCFFU
#define COW_CLR_GREEN                0xFF96FF00U
#define COW_CLR_WARN                 0xFFFFDD00U
#define COW_CLR_ERR                  0xFFFF0000U

static cow_bso_ctx_t *g_ops = NULL;
static bdk_params_t *g_bdk = NULL;

typedef struct __cow_bdk_gfx_ctxt_t
{
	u32 *fb;
	u32 width;
	u32 height;
	u32 stride;
} _cow_bdk_gfx_ctxt_t;

typedef struct __cow_bdk_gfx_con_t
{
	_cow_bdk_gfx_ctxt_t *gfx_ctxt;
	u32 fntsz;
	u32 x;
	u32 y;
	u32 savedx;
	u32 savedy;
	u32 fgcol;
	int fillbg;
	u32 bgcol;
	bool mute;
} _cow_bdk_gfx_con_t;

void *memcpy(void *dst, const void *src, size_t n)
{
	volatile u8 *d = (volatile u8 *)dst;
	const volatile u8 *s = (const volatile u8 *)src;
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	volatile u8 *d = (volatile u8 *)dst;
	u8 v = (u8)c;
	while (n--)
		*d++ = v;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const volatile u8 *pa = (const volatile u8 *)a;
	const volatile u8 *pb = (const volatile u8 *)b;
	while (n--)
	{
		u8 va = *pa++;
		u8 vb = *pb++;
		if (va != vb)
			return (int)va - (int)vb;
	}
	return 0;
}

static size_t _cow_strlen(const char *s)
{
	size_t n = 0;
	while (s && s[n])
		n++;
	return n;
}

static char *_cow_strcpy(char *dst, const char *src)
{
	char *d = dst;
	while ((*d++ = *src++))
		;
	return dst;
}

static char *_cow_strcat(char *dst, const char *src)
{
	char *d = dst + _cow_strlen(dst);
	while ((*d++ = *src++))
		;
	return dst;
}

static char *_cow_strncpy(char *dst, const char *src, size_t n)
{
	size_t i = 0;
	for (; i < n && src[i]; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = 0;
	return dst;
}

static int _cow_strcmp(const char *a, const char *b)
{
	while (*a && (*a == *b))
	{
		a++;
		b++;
	}
	return (int)(u8)*a - (int)(u8)*b;
}

static char *_cow_strchr(const char *s, int c)
{
	while (*s)
	{
		if (*s == (char)c)
			return (char *)s;
		s++;
	}
	return (c == 0) ? (char *)s : NULL;
}

static char *_cow_strrchr(const char *s, int c)
{
	const char *last = NULL;
	while (*s)
	{
		if (*s == (char)c)
			last = s;
		s++;
	}
	if (c == 0)
		return (char *)s;
	return (char *)last;
}

#define strlen  _cow_strlen
#define strcpy  _cow_strcpy
#define strcat  _cow_strcat
#define strncpy _cow_strncpy
#define strcmp  _cow_strcmp
#define strchr  _cow_strchr
#define strrchr _cow_strrchr

#define f_open(a,b,c)                        g_ops->fn_f_open((a),(b),(c))
#define f_close(a)                           g_ops->fn_f_close((a))
#define f_read(a,b,c,d)                      g_ops->fn_f_read((a),(b),(c),(d))
#define f_write(a,b,c,d)                     g_ops->fn_f_write((a),(b),(c),(d))
#define f_lseek(a,b)                         g_ops->fn_f_lseek((a),(b))
#define f_sync(a)                            g_ops->fn_f_sync((a))
#define f_stat(a,b)                          g_ops->fn_f_stat((a),(b))
#define f_mkdir(a)                           g_ops->fn_f_mkdir((a))
#define f_unlink(a)                          g_ops->fn_f_unlink((a))
#define f_rename(a,b)                        g_ops->fn_f_rename((a),(b))
#define f_getfree(a,b,c)                     g_ops->fn_f_getfree((a),(b),(c))
#define emmc_storage                         (*(g_ops->emmc_storage))
#define emmc_set_partition(a)                g_ops->fn_emmc_set_partition((a))
#define sdmmc_storage_read(a,b,c,d)          g_ops->fn_sdmmc_storage_read((a),(b),(c),(d))
#define se_sha_hash_256_oneshot(a,b,c)       g_ops->fn_se_sha_hash_256_oneshot((a),(b),(c))
#define se_sha_hash_256_partial_start(a,b,c,d)  g_ops->fn_se_sha_hash_256_partial_start((a),(b),(c),(d))
#define se_sha_hash_256_partial_update(a,b,c,d) g_ops->fn_se_sha_hash_256_partial_update((a),(b),(c),(d))
#define se_sha_hash_256_partial_end(a,b,c,d,e)  g_ops->fn_se_sha_hash_256_partial_end((a),(b),(c),(d),(e))
#define crc32_calc(a,b,c)                    g_ops->fn_crc32_calc((a),(b),(c))
#define gfx_printf(...)                      g_ops->fn_gfx_printf(__VA_ARGS__)
#define malloc(a)                            g_ops->fn_malloc((a))
#define zalloc(a)                            g_ops->fn_zalloc((a))
#define free(a)                              g_ops->fn_free((a))
static void _cow_bso_ui_show(bool clr)
{
	if (g_ops && g_ops->api_magic == COW_BSO_MAGIC_V2 && g_ops->fn_gfx_show_console)
	{
		g_ops->fn_gfx_show_console(clr);
		return;
	}
	if (g_bdk && g_bdk->gfx_con)
	{
		_cow_bdk_gfx_con_t *con = (_cow_bdk_gfx_con_t *)g_bdk->gfx_con;
		_cow_bdk_gfx_ctxt_t *ctx = (_cow_bdk_gfx_ctxt_t *)g_bdk->gfx_ctx;
		bool was_muted = con->mute;
		con->mute = false;
		con->fntsz = 16U;
		con->fgcol = COW_CLR_DEF;
		con->fillbg = 1;
		con->bgcol = 0xFF1B1B1BU;
		if ((clr || was_muted) && ctx && ctx->fb)
		{
			memset(ctx->fb, 0x1B, (size_t)ctx->width * (size_t)ctx->height * 4U);
			con->x = 0U;
			con->y = 0U;
		}
	}
}

static void _cow_bso_ui_getpos(u32 *px, u32 *py)
{
	if (g_ops && g_ops->api_magic == COW_BSO_MAGIC_V2 && g_ops->fn_gfx_con_getpos)
	{
		g_ops->fn_gfx_con_getpos(px, py);
		return;
	}
	if (g_bdk && g_bdk->gfx_con)
	{
		_cow_bdk_gfx_con_t *con = (_cow_bdk_gfx_con_t *)g_bdk->gfx_con;
		*px = con->x;
		*py = con->y;
		return;
	}
	*px = 0U;
	*py = 0U;
}

static void _cow_bso_ui_setpos(u32 x, u32 y)
{
	if (g_ops && g_ops->api_magic == COW_BSO_MAGIC_V2 && g_ops->fn_gfx_con_setpos)
	{
		g_ops->fn_gfx_con_setpos(x, y);
		return;
	}
	if (g_bdk && g_bdk->gfx_con)
	{
		_cow_bdk_gfx_con_t *con = (_cow_bdk_gfx_con_t *)g_bdk->gfx_con;
		con->x = x;
		con->y = y;
	}
}

#define cow_ui_show(clr)                     _cow_bso_ui_show((clr))
#define cow_ui_getpos(px, py)                _cow_bso_ui_getpos((px), (py))
#define cow_ui_setpos(x, y)                  _cow_bso_ui_setpos((x), (y))
#define EPRINTF(text) do { \
	cow_ui_show(false); \
	gfx_printf("%kERR: " text "%k\n", COW_CLR_ERR, COW_CLR_DEF); \
} while (0)
#define EPRINTFARGS(text, ...) do { \
	cow_ui_show(false); \
	gfx_printf("%kERR: " text "%k\n", COW_CLR_ERR, __VA_ARGS__, COW_CLR_DEF); \
} while (0)
#define WPRINTF(text) \
	gfx_printf("%kWARN: " text "%k\n", COW_CLR_WARN, COW_CLR_DEF)

#endif /* COW_BSO_MODULE */

#define COW_MAGIC                    0x30574F43U /* "COW0" */
#define COW_VERSION_V2               2U
#define COW_VERSION                  COW_VERSION_V2
#define COW_SECTOR_SIZE              512U
#define COW_BLOCK_SECTORS            32U
#define COW_BLOCK_SHIFT              5U
#define COW_BLOCK_BYTES              16384U
#define COW_MIN_L1_ENTRIES           1024U
#define COW_L1_ENTRIES               8192U
#define COW_V2_DEFAULT_L1_ENTRIES    4096U
#define COW_L2_SHIFT                 12U
#define COW_L2_ENTRIES               (1U << COW_L2_SHIFT)
#define COW_L1_SLOTS_FOR(n)          (((((u32)(n) * (u32)sizeof(u32)) + COW_BLOCK_BYTES - 1U)) / COW_BLOCK_BYTES)
#define COW_SUPERBLOCK_SLOTS         2U
#define COW_FIRST_DATA_SLOT          (COW_SUPERBLOCK_SLOTS + 2U * COW_L1_SLOTS_FOR(COW_MIN_L1_ENTRIES))
#define COW_DEFAULT_PREFIX_SECTS     65536U /* 32 MiB */
#define COW_DEFAULT_PREFIX_BYTES     ((u64)COW_DEFAULT_PREFIX_SECTS * COW_SECTOR_SIZE)
#define COW_FLAG_EPHEMERAL           (1U << 0)
#define COW_FLAG_THIN_PROVISION      (1U << 1)
#define COW_FLAG_NEEDS_REBASE        (1U << 2)
#define COW_FLAG_FW_MISMATCH         (1U << 3)
#define COW_FLAG_CONFLICT_LOCKED     (1U << 4)
#define COW_ALLOC_THICK              0U
#define COW_ALLOC_THIN               1U
#define COW_DRIFT_BOOT0_PKG1         (1U << 0)
#define COW_DRIFT_BOOT0_PATROL       (1U << 1)
#define COW_DRIFT_BOOT1              (1U << 2)
#define COW_DRIFT_GPT                (1U << 3)
#define COW_DRIFT_BCPKG2             (1U << 4)
#define COW_DRIFT_SYSTEM             (1U << 7)
#define COW_DRIFT_USER               (1U << 8)
#define COW_DEFAULT_SD_RESERVE_MB    512U
#define COW_DEFAULT_WATERMARK_PCT    80U
#define COW_DEFAULT_GROW_STEP_MB     256U
#define COW_DEFAULT_PART_MAX_MB      4092U
#define COW_MAX_SPLIT_PARTS          32U
#define COW_BOOT_PART_BYTES          0x400000U
#define COW_BOOT_PART_SECTORS        0x2000U
#define COW_BOOT0_BCTSAVE_OFF        0xFC000U
#define COW_BOOT0_BCTSAVE_LEN        0x4000U
#define COW_BOOT0_PATROL_OFF         0x184000U
#define COW_BOOT0_PATROL_LEN         0x200U
#define COW_HASH_BUF_BYTES           0x800000U /* 8 MiB reusable buffer */
#define COW_PART_ANCHOR_BYTES        0x1000000U /* 16 MiB SYSTEM/USER head anchor */
#define COW_RAW_SNAP_MAGIC           0x52574F43U /* "COWR" */
#define COW_RAW_SNAP_VERSION         1U
#define COW_DEFAULT_ROLLBACK_THRESH  2U
#define COW_SNAP_IO_BUF_BYTES        (512U * 1024U)
#define COW_SNAP_UI_THRESH_BYTES     (8ULL * 1024ULL * 1024ULL)

#ifndef COW_BSO_MODULE
#define COW_CLR_DEF                  0xFFCCCCCCU
#define COW_CLR_CYAN                 0xFF00CCFFU
#define COW_CLR_GREEN                0xFF96FF00U
#define COW_CLR_WARN                 0xFFFFDD00U
#define COW_CLR_ERR                  0xFFFF0000U
#endif

typedef struct __attribute__((packed)) _cow_header_t
{
	/* 0x000..0x033 (52 bytes): Common v1/v2 header */
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

	/* 0x034..0x1FB (456 bytes): v2 Extended Geometry, Capacity & Generation Metadata */
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
} cow_header_t;

_Static_assert(sizeof(cow_header_t) == COW_SECTOR_SIZE, "cow_header_t must be 512 bytes");
_Static_assert(offsetof(cow_header_t, header_ext_size) == 0x034, "cow_header_t ext offset");
_Static_assert(offsetof(cow_header_t, physical_base_sectors) == 0x038, "cow_header_t phys offset");
_Static_assert(offsetof(cow_header_t, header_crc32) == 0x1FC, "cow_header_t crc offset");

typedef struct __attribute__((packed)) _cow_raw_snap_hdr_t
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
} cow_raw_snap_hdr_t;

_Static_assert(sizeof(cow_raw_snap_hdr_t) == COW_SECTOR_SIZE, "cow_raw_snap_hdr_t must be 512 bytes");
_Static_assert(offsetof(cow_raw_snap_hdr_t, header_crc32) == 0x1FC, "cow_raw_snap_hdr_t crc offset");

typedef struct _cow_autoinit_cfg_t
{
	u32  version;
	u32  size_mb;
	bool size_auto;
	u32  max_size_mb;
	bool max_size_auto;
	u32  sd_reserve_mb;
	u32  thin;
	u32  grow_step_mb;
	u32  watermark_pct;
	u32  part_max_mb;
	u32  ephemeral;
	u32  strict_patrol;
	u32  auto_reset_on_drift;
	u32  auto_snapshot;
	u32  auto_rollback;
	u32  rollback_threshold;
	bool invalid;
} cow_autoinit_cfg_t;

typedef struct _cow_base_digests_t
{
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
} cow_base_digests_t;

typedef struct _cow_rt_mount_t
{
	bool         active;
	cow_header_t hdr;
	u32          l1_entries;
	u32          first_data_slot;
	u32          num_parts;
	u64          part_sizes[COW_MAX_SPLIT_PARTS];
	char         emmc_dir[0x200];
	u32         *l1_table;
	u32         *l2_buf;
	u32          cached_l1_idx;
} cow_rt_mount_t;

static cow_rt_mount_t g_rt = { 0 };

static bool _cow_utf16_eq_ascii(const u16 *name, const char *ascii)
{
	u32 len = (u32)strlen(ascii);
	if (len > 36U)
		return false;
	for (u32 i = 0; i < len; i++)
	{
		if (name[i] != (u16)(u8)ascii[i])
			return false;
	}
	return (len == 36U) || (name[len] == 0U);
}

static bool _cow_is_zero(const u8 *buf, u32 len)
{
	for (u32 i = 0; i < len; i++)
	{
		if (buf[i] != 0U)
			return false;
	}
	return true;
}

static char *_cow_trim_ws(char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	size_t n = strlen(s);
	while (n > 0U && (s[n - 1U] == ' ' || s[n - 1U] == '\t' || s[n - 1U] == '\r' || s[n - 1U] == '\n'))
	{
		s[n - 1U] = 0;
		n--;
	}
	return s;
}

static bool _cow_streq_ci(const char *a, const char *b)
{
	while (*a && *b)
	{
		char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
		if (ca != cb)
			return false;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

static bool _cow_parse_u32(const char *s, u32 *out)
{
	if (!s || !*s)
		return false;
	u64 acc = 0ULL;
	for (const char *p = s; *p; p++)
	{
		if (*p < '0' || *p > '9')
			return false;
		acc = acc * 10ULL + (u64)(*p - '0');
		if (acc > 0xFFFFFFFFULL)
			return false;
	}
	*out = (u32)acc;
	return true;
}

static bool _cow_parse_bool(const char *s)
{
	u32 v = 0U;
	if (_cow_parse_u32(s, &v))
		return v != 0U;
	return _cow_streq_ci(s, "true") || _cow_streq_ci(s, "yes") || _cow_streq_ci(s, "on");
}

static void _cow_append_hex32(char *dst, u32 v)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t len = strlen(dst);
	char *p = dst + len;
	for (int i = 7; i >= 0; i--)
		*p++ = hex[(v >> (i * 4)) & 0xFU];
	*p = 0;
}

static void _cow_extract_overlay_name(const char *base_path, char *out, u32 out_sz)
{
	if (!out || out_sz == 0U)
		return;
	memset(out, 0, out_sz);
	if (!base_path || !*base_path)
	{
		strncpy(out, "COW0", out_sz - 1U);
		return;
	}
	const char *last = strrchr(base_path, '/');
	const char *name = (last && last[1]) ? (last + 1) : base_path;
	strncpy(out, name, out_sz - 1U);
}

__attribute__((noinline))
static void _cow_apply_cfg_file(
	const char *cfg_path,
	cow_autoinit_cfg_t *cfg,
	bool *has_size_mb,
	bool *has_max_size_mb,
	bool *has_grow_step_mb)
{
	FIL fp;
	if (f_open(&fp, cfg_path, FA_READ) != FR_OK)
		return;

	char buf[512];
	UINT br = 0;

	if (f_read(&fp, buf, sizeof(buf) - 1U, &br) == FR_OK)
	{
		buf[br] = 0;
		char *p = buf;
		while (*p)
		{
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
				p++;
			if (!*p)
				break;
			char *eol = p;
			while (*eol && *eol != '\r' && *eol != '\n')
				eol++;
			if (*eol)
				*eol++ = 0;
			if (*p == '#' || *p == ';')
			{
				p = eol;
				continue;
			}
			char *eq = strchr(p, '=');
			if (eq)
			{
				*eq++ = 0;
				char *key = _cow_trim_ws(p);
				char *val = _cow_trim_ws(eq);
				u32 v = 0U;

				if (!strcmp(key, "version"))
				{
					if (_cow_parse_u32(val, &v) && v == COW_VERSION_V2)
						cfg->version = v;
					else
						cfg->invalid = true;
				}
				else if (!strcmp(key, "size_mb"))
				{
					if (_cow_streq_ci(val, "auto"))
					{
						cfg->size_auto = true;
						*has_size_mb = true;
					}
					else if (_cow_parse_u32(val, &v) && v >= 16U)
					{
						cfg->size_mb = v;
						cfg->size_auto = false;
						*has_size_mb = true;
					}
					else
					{
						cfg->invalid = true;
					}
				}
				else if (!strcmp(key, "max_size_mb"))
				{
					if (_cow_streq_ci(val, "auto"))
					{
						cfg->max_size_auto = true;
						*has_max_size_mb = true;
					}
					else if (_cow_parse_u32(val, &v) && v >= 16U)
					{
						cfg->max_size_mb = v;
						cfg->max_size_auto = false;
						*has_max_size_mb = true;
					}
					else
					{
						cfg->invalid = true;
					}
				}
				else if (!strcmp(key, "sd_reserve_mb"))
				{
					if (_cow_parse_u32(val, &v))
						cfg->sd_reserve_mb = v;
					else
						cfg->invalid = true;
				}
				else if (!strcmp(key, "virtual_gb"))
				{
					if (!_cow_parse_u32(val, &v) || v != 0U)
						cfg->invalid = true;
				}
				else if (!strcmp(key, "thin"))
				{
					if (_cow_parse_u32(val, &v))
						cfg->thin = v ? 1U : 0U;
					else
						cfg->invalid = true;
				}
				else if (!strcmp(key, "grow_step_mb"))
				{
					if (_cow_parse_u32(val, &v) && v >= 1U)
					{
						cfg->grow_step_mb = v;
						*has_grow_step_mb = true;
					}
					else
					{
						cfg->invalid = true;
					}
				}
				else if (!strcmp(key, "watermark_pct"))
				{
					if (_cow_parse_u32(val, &v) && v >= 1U && v <= 99U)
						cfg->watermark_pct = v;
					else
						cfg->invalid = true;
				}
				else if (!strcmp(key, "part_max_mb"))
				{
					if (_cow_parse_u32(val, &v) && v >= 64U && v <= 4092U)
						cfg->part_max_mb = v;
					else
						cfg->invalid = true;
				}
				else if (!strcmp(key, "ephemeral"))
				{
					cfg->ephemeral = _cow_parse_bool(val) ? 1U : 0U;
				}
				else if (!strcmp(key, "strict_patrol"))
				{
					cfg->strict_patrol = _cow_parse_bool(val) ? 1U : 0U;
				}
				else if (!strcmp(key, "auto_reset_on_drift"))
				{
					cfg->auto_reset_on_drift = _cow_parse_bool(val) ? 1U : 0U;
				}
				else if (!strcmp(key, "auto_snapshot"))
				{
					cfg->auto_snapshot = _cow_parse_bool(val) ? 1U : 0U;
				}
				else if (!strcmp(key, "auto_rollback"))
				{
					cfg->auto_rollback = _cow_parse_bool(val) ? 1U : 0U;
				}
				else if (!strcmp(key, "rollback_threshold"))
				{
					if (_cow_parse_u32(val, &v) && v >= 1U)
						cfg->rollback_threshold = v;
					else
						cfg->invalid = true;
				}
			}
			p = eol;
		}
	}
	f_close(&fp);
}

__attribute__((noinline))
static void _cow_parse_autoinit_cfg(const char *cfg_path, cow_autoinit_cfg_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->version            = COW_VERSION_V2;
	cfg->size_mb            = 2048U;
	cfg->sd_reserve_mb      = COW_DEFAULT_SD_RESERVE_MB;
	cfg->watermark_pct      = COW_DEFAULT_WATERMARK_PCT;
	cfg->part_max_mb        = COW_DEFAULT_PART_MAX_MB;
	cfg->auto_reset_on_drift = 0U;
	cfg->auto_snapshot      = 1U;
	cfg->auto_rollback      = 1U;
	cfg->rollback_threshold = COW_DEFAULT_ROLLBACK_THRESH;

	bool has_size_mb      = false;
	bool has_max_size_mb  = false;
	bool has_grow_step_mb = false;
	_cow_apply_cfg_file(cfg_path, cfg, &has_size_mb, &has_max_size_mb, &has_grow_step_mb);

	if (!has_size_mb)
		cfg->size_mb = cfg->thin ? 512U : 2048U;
	if (cfg->thin && !has_max_size_mb)
		cfg->max_size_auto = true;
	if (cfg->thin && !has_grow_step_mb)
		cfg->grow_step_mb = COW_DEFAULT_GROW_STEP_MB;
}

static void _cow_mkdir_p(const char *path)
{
	char tmp[0x200];
	strcpy(tmp, path);
	for (char *p = tmp + 1; *p; p++)
	{
		if (*p == '/')
		{
			*p = 0;
			f_mkdir(tmp);
			*p = '/';
		}
	}
	f_mkdir(tmp);
}

static int _cow_get_sd_free_bytes(u64 *out_free_bytes)
{
	DWORD fre_clust = 0;
	FATFS *fs = NULL;
	if (f_getfree("0:", &fre_clust, &fs) != FR_OK || !fs || fs->csize == 0U)
		return 1;
	*out_free_bytes = (u64)fre_clust * (u64)fs->csize * (u64)COW_SECTOR_SIZE;
	return 0;
}

static u32 _cow_zero_crc32(u32 byte_len)
{
	static const u8 zero_sec[COW_SECTOR_SIZE] = { 0 };
	u32 crc = 0U;
	while (byte_len > 0U)
	{
		u32 cur = MIN(byte_len, COW_SECTOR_SIZE);
		crc = crc32_calc(crc, zero_sec, cur);
		byte_len -= cur;
	}
	return crc;
}

static void _cow_fill_superblock_v2(u8 *slot_buf, const cow_header_t *tmpl, const u32 *l1_table, u64 seq_num)
{
	memset(slot_buf, 0, COW_BLOCK_BYTES);
	cow_header_t *hdr = (cow_header_t *)slot_buf;
	memcpy(hdr, tmpl, sizeof(*hdr));

	hdr->magic           = COW_MAGIC;
	hdr->version         = COW_VERSION_V2;
	hdr->seq_num         = seq_num;
	hdr->header_ext_size = COW_SECTOR_SIZE;
	hdr->block_shift     = COW_BLOCK_SHIFT;
	hdr->l2_shift        = COW_L2_SHIFT;
	if (hdr->prefix_sectors == 0U)
		hdr->prefix_sectors = COW_DEFAULT_PREFIX_SECTS;
	if (hdr->physical_base_sectors == 0ULL)
		hdr->physical_base_sectors = hdr->gpp_total_sectors;

	if (hdr->l1_entries == 0U)
	{
		u64 sectors_per_l1 = (u64)COW_L2_ENTRIES * COW_BLOCK_SECTORS;
		u64 needed_l1 = (hdr->gpp_total_sectors + sectors_per_l1 - 1ULL) / sectors_per_l1;
		hdr->l1_entries = (needed_l1 <= COW_V2_DEFAULT_L1_ENTRIES) ? COW_V2_DEFAULT_L1_ENTRIES : COW_L1_ENTRIES;
	}

	u32 l1_slots = COW_L1_SLOTS_FOR(hdr->l1_entries);
	if (hdr->l1_slot_a == 0U && hdr->l1_slot_b == 0U && hdr->first_data_slot == 0U)
	{
		hdr->l1_slot_a       = COW_SUPERBLOCK_SLOTS;
		hdr->l1_slot_b       = COW_SUPERBLOCK_SLOTS + l1_slots;
		hdr->first_data_slot = COW_SUPERBLOCK_SLOTS + 2U * l1_slots;
	}

	if (hdr->next_free_slot == 0U)
		hdr->next_free_slot = hdr->first_data_slot;
	if (hdr->max_overlay_slots == 0U)
		hdr->max_overlay_slots = hdr->total_slots;
	if (hdr->part_max_sectors == 0U)
		hdr->part_max_sectors = COW_DEFAULT_PART_MAX_MB * 2048U;
	if (hdr->alloc_policy == COW_ALLOC_THIN)
		hdr->flags |= COW_FLAG_THIN_PROVISION;

	u32 l1_bytes = hdr->l1_entries * (u32)sizeof(u32);
	hdr->l1_crc32 = l1_table ? crc32_calc(0, (const u8 *)l1_table, l1_bytes) : _cow_zero_crc32(l1_bytes);
	hdr->header_crc32 = crc32_calc(0, (const u8 *)hdr, offsetof(cow_header_t, header_crc32));
}

static int _cow_hash_sha256_buf(const u8 *buf, u32 len, u8 *out_digest)
{
	if (len <= SE_LL_MAX_SIZE)
		return se_sha_hash_256_oneshot(out_digest, buf, len);

	u32 off = 0U;
	u32 chunk = MIN(len, SE_LL_MAX_SIZE);
	if (se_sha_hash_256_partial_start(out_digest, buf, chunk, true))
		return 1;
	off += chunk;
	while (off + SE_LL_MAX_SIZE < len)
	{
		if (se_sha_hash_256_partial_update(out_digest, buf + off, SE_LL_MAX_SIZE, true))
			return 1;
		off += SE_LL_MAX_SIZE;
	}
	return se_sha_hash_256_partial_end(out_digest, len, buf + off, len - off, true);
}

static int _cow_read_mmc_span(u32 mmc_part, u32 start_lba, u32 num_sectors, u8 *dst, const char *opt_dst_path)
{
	FIL fp;
	bool write_file = (opt_dst_path != NULL);
	if (write_file && f_open(&fp, opt_dst_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
		return 1;

	emmc_set_partition(mmc_part);
	const u32 chunk_sects = 1024U;
	u32 done = 0U;
	while (done < num_sectors)
	{
		u32 cur = MIN(num_sectors - done, chunk_sects);
		u8 *p = dst + (done * COW_SECTOR_SIZE);
		if (sdmmc_storage_read(&emmc_storage, start_lba + done, cur, p) != 0)
		{
			if (write_file)
				f_close(&fp);
			return 1;
		}
		if (write_file)
		{
			UINT bw = 0;
			if (f_write(&fp, p, cur * COW_SECTOR_SIZE, &bw) != FR_OK || bw != cur * COW_SECTOR_SIZE)
			{
				f_close(&fp);
				return 1;
			}
		}
		done += cur;
	}
	if (write_file)
		f_close(&fp);
	return 0;
}

static int _cow_hash_mmc_span(u32 mmc_part, u32 start_lba, u32 num_sectors, u8 *buf, u8 *out_digest)
{
	if (num_sectors == 0U || !buf || !out_digest)
		return 1;

	u32 buf_sects = COW_HASH_BUF_BYTES / COW_SECTOR_SIZE;
	u64 total_bytes = (u64)num_sectors * COW_SECTOR_SIZE;

	if (num_sectors <= buf_sects)
	{
		if (_cow_read_mmc_span(mmc_part, start_lba, num_sectors, buf, NULL))
			return 1;
		return _cow_hash_sha256_buf(buf, (u32)total_bytes, out_digest);
	}

	u32 rem_sects = num_sectors;
	u32 cur_lba = start_lba;
	u32 first_sects = buf_sects;
	if (_cow_read_mmc_span(mmc_part, cur_lba, first_sects, buf, NULL))
		return 1;
	if (se_sha_hash_256_partial_start(out_digest, buf, first_sects * COW_SECTOR_SIZE, true))
		return 1;
	cur_lba += first_sects;
	rem_sects -= first_sects;

	while (rem_sects > buf_sects)
	{
		if (_cow_read_mmc_span(mmc_part, cur_lba, buf_sects, buf, NULL))
			return 1;
		if (se_sha_hash_256_partial_update(out_digest, buf, buf_sects * COW_SECTOR_SIZE, true))
			return 1;
		cur_lba += buf_sects;
		rem_sects -= buf_sects;
	}

	if (_cow_read_mmc_span(mmc_part, cur_lba, rem_sects, buf, NULL))
		return 1;
	return se_sha_hash_256_partial_end(out_digest, total_bytes, buf, rem_sects * COW_SECTOR_SIZE, true);
}

static int _cow_compute_base_gen_id(
	const cow_base_digests_t *d,
	u32 base_hos_version,
	u32 base_pkg1_ver,
	u64 *out_gen_id)
{
	if (!d || !out_gen_id)
		return 1;

	u8 msg[224];
	u32 pos = 0U;
	memcpy(msg + pos, d->boot0_pkg1_sha256, 32U);    pos += 32U;
	memcpy(msg + pos, d->boot0_patrol_sha256_16, 16U); pos += 16U;
	memcpy(msg + pos, d->boot1_sha256, 32U);         pos += 32U;
	memcpy(msg + pos, d->gpt_sha256, 32U);           pos += 32U;
	memcpy(msg + pos, d->bcpkg2_sha256, 32U);        pos += 32U;
	memcpy(msg + pos, d->system_digest_sha256, 32U); pos += 32U;
	memcpy(msg + pos, d->user_digest_sha256, 32U);   pos += 32U;

	u64 phys_s = d->phys_gpp_sectors;
	for (u32 i = 0U; i < 8U; i++)
		msg[pos++] = (u8)((phys_s >> (i * 8U)) & 0xFFU);
	for (u32 i = 0U; i < 4U; i++)
		msg[pos++] = (u8)((base_hos_version >> (i * 8U)) & 0xFFU);
	for (u32 i = 0U; i < 4U; i++)
		msg[pos++] = (u8)((base_pkg1_ver >> (i * 8U)) & 0xFFU);

	u8 sha[32];
	if (se_sha_hash_256_oneshot(sha, msg, sizeof(msg)))
		return 1;

	u64 gid = 0ULL;
	for (u32 i = 0U; i < 8U; i++)
		gid |= ((u64)sha[i]) << (i * 8U);
	*out_gen_id = gid ? gid : 1ULL;
	return 0;
}

__attribute__((noinline))
static int _cow_read_and_hash_physical_base(cow_base_digests_t *out, bool copy_boot_files, const char *emmc_dir, bool compute_hashes)
{
	memset(out, 0, sizeof(*out));
	u8 *buf = (u8 *)malloc(COW_HASH_BUF_BYTES);
	if (!buf)
		return 1;

	bool log_steps = copy_boot_files;
	char fpath[0x240];
	const char *boot0_path = NULL;
	const char *boot1_path = NULL;
	if (copy_boot_files && emmc_dir)
	{
		strcpy(fpath, emmc_dir);
		strcat(fpath, "/boot0");
		boot0_path = fpath;
	}

	if (copy_boot_files || compute_hashes)
	{
		if (log_steps)
			gfx_printf("      BOOT0 (4 MiB)...");
		if (_cow_read_mmc_span(1U, 0U, COW_BOOT_PART_SECTORS, buf, boot0_path))
		{
			if (log_steps)
				gfx_printf(" fail\n");
			goto fail;
		}
		if (compute_hashes)
		{
			u8 patrol_full[32];
			if (se_sha_hash_256_oneshot(patrol_full, buf + COW_BOOT0_PATROL_OFF, COW_BOOT0_PATROL_LEN))
			{
				if (log_steps)
					gfx_printf(" fail\n");
				goto fail;
			}
			memcpy(out->boot0_patrol_sha256_16, patrol_full, sizeof(out->boot0_patrol_sha256_16));

			memset(buf + COW_BOOT0_BCTSAVE_OFF, 0, COW_BOOT0_BCTSAVE_LEN);
			memset(buf + COW_BOOT0_PATROL_OFF, 0, COW_BOOT0_PATROL_LEN);
			if (se_sha_hash_256_oneshot(out->boot0_pkg1_sha256, buf, COW_BOOT_PART_BYTES))
			{
				if (log_steps)
					gfx_printf(" fail\n");
				goto fail;
			}
		}
		if (log_steps)
			gfx_printf(" ok\n");

		if (copy_boot_files && emmc_dir)
		{
			strcpy(fpath, emmc_dir);
			strcat(fpath, "/boot1");
			boot1_path = fpath;
		}
		if (log_steps)
			gfx_printf("      BOOT1 (4 MiB)...");
		if (_cow_read_mmc_span(2U, 0U, COW_BOOT_PART_SECTORS, buf, boot1_path))
		{
			if (log_steps)
				gfx_printf(" fail\n");
			goto fail;
		}
		if (compute_hashes)
		{
			if (se_sha_hash_256_oneshot(out->boot1_sha256, buf, COW_BOOT_PART_BYTES))
			{
				if (log_steps)
					gfx_printf(" fail\n");
				goto fail;
			}
		}
		if (log_steps)
			gfx_printf(" ok\n");
	}

	/* Read and parse GPT (LBA 0..33 = 34 sectors) from physical GPP */
	if (log_steps)
		gfx_printf("      GPT + BCPKG2...");
	if (_cow_read_mmc_span(0U, 0U, 34U, buf, NULL))
	{
		if (log_steps)
			gfx_printf(" fail\n");
		goto fail;
	}

	if (compute_hashes && se_sha_hash_256_oneshot(out->gpt_sha256, buf, 34U * COW_SECTOR_SIZE))
	{
		if (log_steps)
			gfx_printf(" fail\n");
		goto fail;
	}

	gpt_t *gpt = (gpt_t *)(buf + COW_SECTOR_SIZE);
	if (!memcmp(&gpt->header.signature, "EFI PART", 8) && gpt->header.num_part_ents <= 128U)
	{
		out->gpt_valid = true;
		out->phys_gpp_sectors = gpt->header.alt_lba ? (gpt->header.alt_lba + 1ULL) : (u64)emmc_storage.sec_cnt;

		u32 pkg2_start = 0U, pkg2_sects = 0U;
		u32 sys_start = 0U, sys_sects = 0U;
		u32 usr_start = 0U, usr_sects = 0U;
		const u32 anchor_max_sects = COW_PART_ANCHOR_BYTES / COW_SECTOR_SIZE;

		for (u32 i = 0; i < gpt->header.num_part_ents; i++)
		{
			u64 ls = gpt->entries[i].lba_start;
			u64 le = gpt->entries[i].lba_end;
			if (le < ls || ls > 0xFFFFFFFFULL)
				continue;

			if (_cow_utf16_eq_ascii(gpt->entries[i].name, "BCPKG2-1-Normal-Main"))
			{
				if (le < COW_DEFAULT_PREFIX_SECTS)
				{
					out->pkg2_valid = true;
					pkg2_start = (u32)ls;
					pkg2_sects = (u32)(le - ls + 1ULL);
				}
			}
			else if (_cow_utf16_eq_ascii(gpt->entries[i].name, "SYSTEM"))
			{
				u64 cnt = le - ls + 1ULL;
				out->system_valid = true;
				sys_start = (u32)ls;
				sys_sects = (cnt > anchor_max_sects) ? anchor_max_sects : (u32)cnt;
			}
			else if (_cow_utf16_eq_ascii(gpt->entries[i].name, "USER"))
			{
				u64 cnt = le - ls + 1ULL;
				out->user_valid = true;
				usr_start = (u32)ls;
				usr_sects = (cnt > anchor_max_sects) ? anchor_max_sects : (u32)cnt;
			}
		}

		if (compute_hashes)
		{
			if (out->pkg2_valid && _cow_hash_mmc_span(0U, pkg2_start, pkg2_sects, buf, out->bcpkg2_sha256))
			{
				if (log_steps)
					gfx_printf(" fail\n");
				goto fail;
			}
			if (log_steps)
				gfx_printf(" ok\n");

			if (out->system_valid)
			{
				if (log_steps)
					gfx_printf("      SYSTEM (16 MiB)...");
				if (_cow_hash_mmc_span(0U, sys_start, sys_sects, buf, out->system_digest_sha256))
				{
					if (log_steps)
						gfx_printf(" fail\n");
					goto fail;
				}
				if (log_steps)
					gfx_printf(" ok\n");
			}
			if (out->user_valid)
			{
				if (log_steps)
					gfx_printf("      USER (16 MiB)...");
				if (_cow_hash_mmc_span(0U, usr_start, usr_sects, buf, out->user_digest_sha256))
				{
					if (log_steps)
						gfx_printf(" fail\n");
					goto fail;
				}
				if (log_steps)
					gfx_printf(" ok\n");
			}
		}
		else if (log_steps)
		{
			gfx_printf(" ok\n");
		}
	}
	else if (log_steps)
	{
		gfx_printf(" fail\n");
	}

	emmc_set_partition(0U);
	free(buf);
	return 0;

fail:
	emmc_set_partition(0U);
	free(buf);
	return 1;
}

static void _cow_part_path(char *out, const char *emmc_dir, u32 part_idx)
{
	strcpy(out, emmc_dir);
	size_t n = strlen(out);
	out[n++] = '/';
	out[n++] = (char)('0' + ((part_idx / 10U) % 10U));
	out[n++] = (char)('0' + (part_idx % 10U));
	out[n] = 0;
}

__attribute__((noinline))
static int _cow_scan_parts(const char *emmc_dir, u64 *part_sizes, u32 max_parts, u32 *out_num_parts, u64 *out_total_bytes)
{
	char fpath[0x240];
	FILINFO fno;
	u32 count = 0U;
	u64 total = 0ULL;

	for (u32 i = 0U; i < max_parts; i++)
	{
		_cow_part_path(fpath, emmc_dir, i);
		if (f_stat(fpath, &fno) != FR_OK)
			break;
		if (fno.fsize == 0ULL)
			return 1;
		part_sizes[i] = (u64)fno.fsize;
		total += (u64)fno.fsize;
		count++;
	}

	*out_num_parts = count;
	*out_total_bytes = total;
	return (count > 0U) ? 0 : 1;
}

__attribute__((noinline))
static int _cow_archive_rw(const char *emmc_dir, const u64 *part_sizes, u32 num_parts, u64 abs_off, void *buf, u32 len, bool is_write)
{
	u8 *ptr = (u8 *)buf;
	u32 rem = len;
	u64 cur_off = abs_off;
	char fpath[0x240];

	for (u32 i = 0U; i < num_parts && rem > 0U; i++)
	{
		u64 psz = part_sizes[i];
		if (cur_off >= psz)
		{
			cur_off -= psz;
			continue;
		}
		u64 avail = psz - cur_off;
		u32 chunk = (rem < avail) ? rem : (u32)avail;

		_cow_part_path(fpath, emmc_dir, i);
		FIL fp;
		if (f_open(&fp, fpath, is_write ? (FA_OPEN_EXISTING | FA_READ | FA_WRITE) : FA_READ) != FR_OK)
			return 1;
		if (f_lseek(&fp, (FSIZE_t)cur_off) != FR_OK)
		{
			f_close(&fp);
			return 1;
		}
		UINT done = 0U;
		FRESULT fr = is_write ? f_write(&fp, ptr, chunk, &done) : f_read(&fp, ptr, chunk, &done);
		if (fr != FR_OK || done != chunk)
		{
			f_close(&fp);
			return 1;
		}
		if (is_write && f_sync(&fp) != FR_OK)
		{
			f_close(&fp);
			return 1;
		}
		f_close(&fp);

		ptr += chunk;
		rem -= chunk;
		cur_off = 0ULL;
	}

	return (rem == 0U) ? 0 : 1;
}

static int _cow_prealloc_file_stepped(
	FIL *fp,
	u64 start_size,
	u64 target_size,
	const char *prefix_label,
	int part_idx)
{
	if (!fp || target_size == 0ULL)
		return 1;

	u32 px = 0U, py = 0U;
	int total_mb = (int)((target_size + (1024ULL * 1024ULL - 1ULL)) / (1024ULL * 1024ULL));
	if (total_mb < 1)
		total_mb = 1;

	if (prefix_label)
	{
		cow_ui_getpos(&px, &py);
		int init_mb = (int)(start_size >> 20);
		int init_pct = (int)((start_size * 100ULL) / target_size);
		if (init_pct > 100)
			init_pct = 100;
		gfx_printf("%s %02d: %4d / %d MiB (%3d%%)",
			prefix_label, part_idx, init_mb, total_mb, init_pct);
	}

	const u64 step_bytes = 32ULL * 1024ULL * 1024ULL;
	u64 end_off = target_size - 1ULL;
	if (start_size < end_off)
	{
		u64 pos = start_size + step_bytes;
		if (pos > end_off || pos < start_size)
			pos = end_off;
		while (pos < end_off)
		{
			if (f_lseek(fp, (FSIZE_t)pos) != FR_OK)
			{
				if (prefix_label)
					gfx_printf("\n");
				return 1;
			}
			if (prefix_label)
			{
				int cur_mb = (int)(pos >> 20);
				int pct = (int)((pos * 100ULL) / target_size);
				if (pct > 99)
					pct = 99;
				cow_ui_setpos(px, py);
				gfx_printf("%s %02d: %4d / %d MiB (%3d%%)",
					prefix_label, part_idx, cur_mb, total_mb, pct);
			}
			u64 next = pos + step_bytes;
			pos = (next > end_off || next < pos) ? end_off : next;
		}
	}

	u8 zero = 0U;
	UINT bw = 0U;
	if (f_lseek(fp, (FSIZE_t)end_off) != FR_OK ||
	    f_tell(fp) != (FSIZE_t)end_off ||
	    f_write(fp, &zero, 1U, &bw) != FR_OK || bw != 1U)
	{
		if (prefix_label)
			gfx_printf("\n");
		return 1;
	}

	if (prefix_label)
	{
		cow_ui_setpos(px, py);
		gfx_printf("%s %02d: %4d / %d MiB (100%%)\n",
			prefix_label, part_idx, total_mb, total_mb);
	}
	return 0;
}

static int _cow_extend_or_create_part(const char *fpath, u64 new_size, bool create_new)
{
	if (new_size == 0ULL || new_size > 0xFFFFFFFFULL)
		return 1;

	FIL fp;
	BYTE mode = create_new ? (FA_CREATE_ALWAYS | FA_READ | FA_WRITE) : (FA_OPEN_EXISTING | FA_READ | FA_WRITE);
	if (f_open(&fp, fpath, mode) != FR_OK)
		return 1;

	u64 start_size = create_new ? 0ULL : (u64)f_size(&fp);
	int pnum = 0;
	const char *base = strrchr(fpath, '/');
	if (base && base[1] >= '0' && base[1] <= '9' && base[2] >= '0' && base[2] <= '9')
		pnum = (base[1] - '0') * 10 + (base[2] - '0');

	bool ok = false;
	if (_cow_prealloc_file_stepped(&fp, start_size, new_size, "      Part", pnum) == 0 &&
	    f_sync(&fp) == FR_OK &&
	    f_size(&fp) == (FSIZE_t)new_size)
	{
		ok = true;
	}

	f_close(&fp);
	return ok ? 0 : 1;
}

static bool _cow_validate_header(const cow_header_t *hdr, u64 total_archive_bytes, u32 sb_slot, u32 *out_l1_entries, u32 *out_first_data)
{
	if (hdr->magic != COW_MAGIC)
		return false;
	if (hdr->prefix_sectors != COW_DEFAULT_PREFIX_SECTS)
		return false;

	u64 prefix_bytes = (u64)hdr->prefix_sectors * COW_SECTOR_SIZE;
	if (total_archive_bytes <= prefix_bytes)
		return false;
	u64 backing_slots = (total_archive_bytes - prefix_bytes) / COW_BLOCK_BYTES;

	u32 calc_crc = crc32_calc(0, (const u8 *)hdr, offsetof(cow_header_t, header_crc32));
	if (calc_crc != hdr->header_crc32)
		return false;
	if (hdr->gpp_total_sectors == 0ULL)
		return false;

	if (hdr->version != COW_VERSION_V2)
		return false;

	if (hdr->header_ext_size != COW_SECTOR_SIZE ||
	    hdr->block_shift != COW_BLOCK_SHIFT ||
	    hdr->l2_shift != COW_L2_SHIFT)
		return false;
	if (hdr->alloc_policy != COW_ALLOC_THICK && hdr->alloc_policy != COW_ALLOC_THIN)
		return false;
	if (hdr->l1_entries < COW_MIN_L1_ENTRIES || hdr->l1_entries > COW_L1_ENTRIES)
		return false;
	if ((hdr->l1_entries % (COW_SECTOR_SIZE / (u32)sizeof(u32))) != 0U)
		return false;
	if (hdr->physical_base_sectors == 0ULL || hdr->gpp_total_sectors != hdr->physical_base_sectors)
		return false;

	u32 l1_slots = COW_L1_SLOTS_FOR(hdr->l1_entries);
	if (hdr->l1_slot_a < COW_SUPERBLOCK_SLOTS || hdr->l1_slot_b < COW_SUPERBLOCK_SLOTS)
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

	u64 max_l1_sectors = (u64)l1_entries * COW_L2_ENTRIES * COW_BLOCK_SECTORS;
	if (max_l1_sectors < hdr->gpp_total_sectors)
		return false;
	if ((u64)hdr->total_slots > backing_slots)
		return false;
	if (hdr->next_free_slot < first_data || hdr->next_free_slot > hdr->total_slots)
		return false;

	*out_l1_entries = l1_entries;
	*out_first_data = first_data;
	return true;
}

static int _cow_commit_superblock(const char *emmc_dir, const u64 *part_sizes, u32 num_parts, u32 target_sb_slot, cow_header_t *hdr, const u32 *l1_table)
{
	u8 *slot_buf = (u8 *)zalloc(COW_BLOCK_BYTES);
	if (!slot_buf)
		return 1;

	u64 prefix_bytes = (u64)hdr->prefix_sectors * COW_SECTOR_SIZE;
	u64 sb_off = prefix_bytes + ((u64)target_sb_slot * COW_BLOCK_BYTES);
	int rc = 1;

	u32 l1_bytes = hdr->l1_entries * (u32)sizeof(u32);
	u32 l1_slots = COW_L1_SLOTS_FOR(hdr->l1_entries);
	u32 target_l1_slot = (target_sb_slot == 0U) ? hdr->l1_slot_a : hdr->l1_slot_b;
	u64 l1_off = prefix_bytes + ((u64)target_l1_slot * COW_BLOCK_BYTES);

	for (u32 s = 0U; s < l1_slots; s++)
	{
		memset(slot_buf, 0, COW_BLOCK_BYTES);
		u32 byte_off = s * COW_BLOCK_BYTES;
		if (byte_off < l1_bytes)
		{
			u32 take = MIN(l1_bytes - byte_off, COW_BLOCK_BYTES);
			memcpy(slot_buf, ((const u8 *)l1_table) + byte_off, take);
		}
		if (_cow_archive_rw(emmc_dir, part_sizes, num_parts, l1_off + ((u64)s * COW_BLOCK_BYTES), slot_buf, COW_BLOCK_BYTES, true))
			goto out;
	}

	_cow_fill_superblock_v2(slot_buf, hdr, l1_table, hdr->seq_num);
	memcpy(hdr, slot_buf, sizeof(*hdr));
	rc = _cow_archive_rw(emmc_dir, part_sizes, num_parts, sb_off, slot_buf, COW_BLOCK_BYTES, true);

out:
	free(slot_buf);
	return rc;
}

static void _cow_join_path(char *out, const char *base_path, const char *suffix)
{
	strcpy(out, base_path);
	strcat(out, suffix);
}

static void _cow_format_u32(char *dst, u32 v)
{
	char rev[16];
	u32 n = 0U;
	do
	{
		rev[n++] = (char)('0' + (v % 10U));
		v /= 10U;
	}
	while (v > 0U && n < sizeof(rev));

	for (u32 i = 0U; i < n; i++)
		dst[i] = rev[n - 1U - i];
	dst[n] = 0;
}

static int _cow_write_text_file(const char *path, const char *text)
{
	FIL fp;
	if (f_open(&fp, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
		return 1;
	UINT len = (UINT)strlen(text);
	UINT bw = 0U;
	FRESULT fr = f_write(&fp, text, len, &bw);
	f_sync(&fp);
	f_close(&fp);
	return (fr == FR_OK && bw == len) ? 0 : 1;
}

static int _cow_read_u32_file(const char *path, u32 fallback_val)
{
	FIL fp;
	if (f_open(&fp, path, FA_READ) != FR_OK)
		return (int)fallback_val;
	char buf[32];
	UINT br = 0U;
	FRESULT fr = f_read(&fp, buf, sizeof(buf) - 1U, &br);
	f_close(&fp);
	if (fr != FR_OK || br == 0U)
		return (int)fallback_val;
	buf[br] = 0;
	u32 v = 0U;
	if (!_cow_parse_u32(_cow_trim_ws(buf), &v))
		return (int)fallback_val;
	return (int)v;
}

static int _cow_write_u32_file(const char *path, u32 v)
{
	char buf[24];
	_cow_format_u32(buf, v);
	strcat(buf, "\n");
	return _cow_write_text_file(path, buf);
}

static int _cow_load_active_state(
	const char *emmc_dir,
	const u64 *part_sizes,
	u32 num_parts,
	u64 total_archive_bytes,
	u32 *out_active_slot,
	u32 *out_active_l1_entries,
	u32 *out_active_first_data,
	cow_header_t *out_hdr,
	u32 *l1_table)
{
	/* Plain file-based emuMMC shorter than 32 MiB + 2 slots cannot be COW0 */
	if (total_archive_bytes < COW_DEFAULT_PREFIX_BYTES + (u64)COW_SUPERBLOCK_SLOTS * COW_BLOCK_BYTES)
		return 2;

	cow_header_t probe[COW_SUPERBLOCK_SLOTS];
	bool has_magic = false;
	for (u32 s = 0U; s < COW_SUPERBLOCK_SLOTS; s++)
	{
		memset(&probe[s], 0, sizeof(probe[s]));
		if (_cow_archive_rw(emmc_dir, part_sizes, num_parts,
		                    COW_DEFAULT_PREFIX_BYTES + ((u64)s * COW_BLOCK_BYTES),
		                    &probe[s], sizeof(probe[s]), false))
		{
			return 1;
		}
		if (probe[s].magic == COW_MAGIC)
			has_magic = true;
	}

	/* Standard non-COW file-based emuMMC -> caller skips COW handling */
	if (!has_magic)
		return 2;

	u32 hdr_l1_ents[COW_SUPERBLOCK_SLOTS] = { 0U, 0U };
	u32 hdr_first_data[COW_SUPERBLOCK_SLOTS] = { 0U, 0U };
	bool hdr_ok[COW_SUPERBLOCK_SLOTS] = { false, false };
	for (u32 s = 0U; s < COW_SUPERBLOCK_SLOTS; s++)
		hdr_ok[s] = _cow_validate_header(&probe[s], total_archive_bytes, s, &hdr_l1_ents[s], &hdr_first_data[s]);

	if (!hdr_ok[0] && !hdr_ok[1])
	{
		EPRINTF("cowMMC: Both superblocks invalid!");
		return 1;
	}

	u32 order[COW_SUPERBLOCK_SLOTS];
	if (hdr_ok[0] && (!hdr_ok[1] || probe[0].seq_num >= probe[1].seq_num))
	{
		order[0] = 0U;
		order[1] = 1U;
	}
	else
	{
		order[0] = 1U;
		order[1] = 0U;
	}

	for (u32 i = 0U; i < COW_SUPERBLOCK_SLOTS; i++)
	{
		u32 s = order[i];
		if (!hdr_ok[s])
			continue;

		u32 l1_bytes = hdr_l1_ents[s] * (u32)sizeof(u32);
		u64 prefix_bytes = (u64)probe[s].prefix_sectors * COW_SECTOR_SIZE;
		u64 l1_off = prefix_bytes + ((u64)((s == 0U) ? probe[s].l1_slot_a : probe[s].l1_slot_b) * COW_BLOCK_BYTES);

		memset(l1_table, 0, COW_L1_ENTRIES * sizeof(u32));
		if (_cow_archive_rw(emmc_dir, part_sizes, num_parts, l1_off, l1_table, l1_bytes, false))
			continue;

		if (crc32_calc(0, (const u8 *)l1_table, l1_bytes) == probe[s].l1_crc32)
		{
			*out_active_slot = s;
			*out_active_l1_entries = hdr_l1_ents[s];
			*out_active_first_data = hdr_first_data[s];
			*out_hdr = probe[s];
			return 0;
		}
	}

	EPRINTF("cowMMC: L1 CRC32 verification failed!");
	return 1;
}

__attribute__((noinline))
static int _cow_probe_raw_snapshot(const char *snap_path, cow_raw_snap_hdr_t *out_hdr, u64 total_archive_bytes, bool show_progress)
{
	FILINFO fno;
	if (f_stat(snap_path, &fno) != FR_OK)
		return 1;
	if (fno.fsize < sizeof(cow_raw_snap_hdr_t))
		return 1;

	FIL fp;
	if (f_open(&fp, snap_path, FA_READ) != FR_OK)
		return 1;

	cow_raw_snap_hdr_t shdr;
	memset(&shdr, 0, sizeof(shdr));
	UINT br = 0U;
	if (f_read(&fp, &shdr, sizeof(shdr), &br) != FR_OK || br != sizeof(shdr))
	{
		f_close(&fp);
		return 1;
	}

	if (shdr.magic != COW_RAW_SNAP_MAGIC ||
	    shdr.snap_version != COW_RAW_SNAP_VERSION ||
	    shdr.cow_version != COW_VERSION_V2 ||
	    shdr.prefix_sectors != COW_DEFAULT_PREFIX_SECTS ||
	    shdr.active_sb_slot >= COW_SUPERBLOCK_SLOTS ||
	    shdr.next_free_slot < shdr.first_data_slot ||
	    shdr.next_free_slot > shdr.total_slots)
	{
		f_close(&fp);
		return 1;
	}

	u32 calc_hcrc = crc32_calc(0, (const u8 *)&shdr, offsetof(cow_raw_snap_hdr_t, header_crc32));
	if (calc_hcrc != shdr.header_crc32)
	{
		f_close(&fp);
		return 1;
	}

	u64 prefix_bytes = (u64)shdr.prefix_sectors * COW_SECTOR_SIZE;
	u64 payload_bytes = (u64)shdr.next_free_slot * COW_BLOCK_BYTES;
	u64 expected_file_sz = (u64)sizeof(cow_raw_snap_hdr_t) + payload_bytes;
	if ((u64)fno.fsize != expected_file_sz || total_archive_bytes < prefix_bytes + payload_bytes)
	{
		f_close(&fp);
		return 1;
	}

	u32 buf_sz = COW_SNAP_IO_BUF_BYTES;
	u8 *buf = (u8 *)malloc(buf_sz);
	if (!buf)
	{
		buf_sz = 64U * 1024U;
		buf = (u8 *)malloc(buf_sz);
	}
	if (!buf)
	{
		f_close(&fp);
		return 1;
	}

	bool ui = show_progress && (payload_bytes >= COW_SNAP_UI_THRESH_BYTES);
	int total_mb = (int)(payload_bytes >> 20);
	if (total_mb < 1)
		total_mb = 1;
	u32 px = 0U, py = 0U;
	u64 next_log = 0ULL;
	if (ui)
		cow_ui_getpos(&px, &py);

	u64 rem = payload_bytes;
	u64 done = 0ULL;
	u32 pcrc = 0U;
	int rc = 0;
	while (rem > 0ULL)
	{
		if (ui && done >= next_log)
		{
			int cur_mb = (int)(done >> 20);
			int pct = (int)((done * 100ULL) / payload_bytes);
			cow_ui_setpos(px, py);
			gfx_printf("      Verify: %4d / %d MiB (%3d%%)", cur_mb, total_mb, pct);
			next_log += 4ULL * 1024ULL * 1024ULL;
		}
		u32 cur = (rem > buf_sz) ? buf_sz : (u32)rem;
		br = 0U;
		if (f_read(&fp, buf, cur, &br) != FR_OK || br != cur)
		{
			rc = 1;
			break;
		}
		pcrc = crc32_calc(pcrc, buf, cur);
		rem -= cur;
		done += cur;
	}

	if (ui)
	{
		if (rc == 0)
		{
			cow_ui_setpos(px, py);
			gfx_printf("      Verify: %4d / %d MiB (100%%)\n", total_mb, total_mb);
		}
		else
		{
			gfx_printf("\n");
		}
	}

	free(buf);
	f_close(&fp);
	if (rc != 0 || pcrc != shdr.payload_crc32)
		return 1;

	if (out_hdr)
		*out_hdr = shdr;
	return 0;
}

static int _cow_snapshot_transfer(
	FIL *snap_fp,
	const char *emmc_dir,
	const u64 *part_sizes,
	u32 num_parts,
	u64 abs_off,
	u64 payload_bytes,
	bool to_archive,
	u32 *out_crc32,
	const char *progress_label)
{
	u32 buf_sz = COW_SNAP_IO_BUF_BYTES;
	u8 *buf = (u8 *)malloc(buf_sz);
	if (!buf)
	{
		buf_sz = 64U * 1024U;
		buf = (u8 *)malloc(buf_sz);
	}
	if (!buf)
		return 1;

	bool ui = (progress_label != NULL) && (payload_bytes >= COW_SNAP_UI_THRESH_BYTES);
	int total_mb = (int)(payload_bytes >> 20);
	if (total_mb < 1)
		total_mb = 1;
	u32 px = 0U, py = 0U;
	u64 next_log = 0ULL;
	if (ui)
		cow_ui_getpos(&px, &py);

	u64 part_off = abs_off;
	u64 done = 0ULL;
	u32 pcrc = 0U;
	int rc = 0;
	char ppath[0x240];

	for (u32 i = 0U; i < num_parts && done < payload_bytes && rc == 0; i++)
	{
		u64 psz = part_sizes[i];
		if (part_off >= psz)
		{
			part_off -= psz;
			continue;
		}

		_cow_part_path(ppath, emmc_dir, i);
		FIL part_fp;
		BYTE pmode = to_archive ? (FA_OPEN_EXISTING | FA_READ | FA_WRITE) : FA_READ;
		if (f_open(&part_fp, ppath, pmode) != FR_OK)
		{
			rc = 1;
			break;
		}
		if (f_lseek(&part_fp, (FSIZE_t)part_off) != FR_OK)
		{
			f_close(&part_fp);
			rc = 1;
			break;
		}

		u64 part_rem = psz - part_off;
		while (done < payload_bytes && part_rem > 0ULL)
		{
			if (ui && done >= next_log)
			{
				int cur_mb = (int)(done >> 20);
				int pct = (int)((done * 100ULL) / payload_bytes);
				cow_ui_setpos(px, py);
				gfx_printf("%s%4d / %d MiB (%3d%%)", progress_label, cur_mb, total_mb, pct);
				next_log += 4ULL * 1024ULL * 1024ULL;
			}

			u64 rem = payload_bytes - done;
			u64 take64 = (rem < part_rem) ? rem : part_rem;
			u32 cur = (take64 > buf_sz) ? buf_sz : (u32)take64;
			UINT io1 = 0U, io2 = 0U;
			if (to_archive)
			{
				if (f_read(snap_fp, buf, cur, &io1) != FR_OK || io1 != cur ||
				    f_write(&part_fp, buf, cur, &io2) != FR_OK || io2 != cur)
				{
					rc = 1;
					break;
				}
			}
			else
			{
				if (f_read(&part_fp, buf, cur, &io1) != FR_OK || io1 != cur ||
				    f_write(snap_fp, buf, cur, &io2) != FR_OK || io2 != cur)
				{
					rc = 1;
					break;
				}
				pcrc = crc32_calc(pcrc, buf, cur);
			}
			done += cur;
			part_rem -= cur;
		}

		if (to_archive && rc == 0 && f_sync(&part_fp) != FR_OK)
			rc = 1;
		f_close(&part_fp);
		part_off = 0ULL;
	}

	if (done < payload_bytes)
		rc = 1;

	if (ui)
	{
		if (rc == 0)
		{
			cow_ui_setpos(px, py);
			gfx_printf("%s%4d / %d MiB (100%%)\n", progress_label, total_mb, total_mb);
		}
		else
		{
			gfx_printf("\n");
		}
	}

	free(buf);
	if (rc == 0 && out_crc32)
		*out_crc32 = pcrc;
	return rc;
}

__attribute__((noinline))
static int _cow_save_raw_snapshot(
	const char *base_path,
	const char *emmc_dir,
	const u64 *part_sizes,
	u32 num_parts,
	u32 active_sb_slot,
	const cow_header_t *hdr,
	const char *snap_path,
	const char *opt_prev_path,
	u32 min_free_margin_mb)
{
	char snap_dir[0x240];
	_cow_join_path(snap_dir, base_path, "/snapshots");
	_cow_mkdir_p(snap_dir);

	u64 payload_bytes = (u64)hdr->next_free_slot * COW_BLOCK_BYTES;

	FILINFO cur_fno;
	if (f_stat(snap_path, &cur_fno) == FR_OK &&
	    (u64)cur_fno.fsize == (u64)sizeof(cow_raw_snap_hdr_t) + payload_bytes)
	{
		FIL cur_fp;
		if (f_open(&cur_fp, snap_path, FA_READ) == FR_OK)
		{
			cow_raw_snap_hdr_t cur_shdr;
			UINT rbr = 0U;
			bool same = (f_read(&cur_fp, &cur_shdr, sizeof(cur_shdr), &rbr) == FR_OK &&
			             rbr == sizeof(cur_shdr) &&
			             cur_shdr.magic == COW_RAW_SNAP_MAGIC &&
			             cur_shdr.snap_version == COW_RAW_SNAP_VERSION &&
			             crc32_calc(0, (const u8 *)&cur_shdr, offsetof(cow_raw_snap_hdr_t, header_crc32)) == cur_shdr.header_crc32 &&
			             cur_shdr.cow_version == hdr->version &&
			             cur_shdr.active_sb_slot == active_sb_slot &&
			             cur_shdr.seq_num == hdr->seq_num &&
			             cur_shdr.next_free_slot == hdr->next_free_slot &&
			             cur_shdr.flags == hdr->flags &&
			             cur_shdr.dirty_data_blocks == hdr->dirty_data_blocks &&
			             cur_shdr.dirty_l2_pages == hdr->dirty_l2_pages &&
			             cur_shdr.base_gen_id == hdr->base_gen_id &&
			             memcmp(cur_shdr.overlay_uuid, hdr->overlay_uuid, 16U) == 0);
			f_close(&cur_fp);
			if (same)
				return 0;
		}
	}

	u64 needed = (u64)sizeof(cow_raw_snap_hdr_t) + payload_bytes + ((u64)min_free_margin_mb * 1024ULL * 1024ULL);
	u64 sd_free = 0ULL;
	if (_cow_get_sd_free_bytes(&sd_free) == 0 && sd_free < needed)
		return 1;

	char tmp_path[0x260];
	strcpy(tmp_path, snap_path);
	strcat(tmp_path, ".tmp");

	FIL fp;
	if (f_open(&fp, tmp_path, FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK)
		return 1;

	cow_raw_snap_hdr_t shdr;
	memset(&shdr, 0, sizeof(shdr));
	UINT bw = 0U;
	if (f_write(&fp, &shdr, sizeof(shdr), &bw) != FR_OK || bw != sizeof(shdr))
		goto fail;

	if (payload_bytes >= COW_SNAP_UI_THRESH_BYTES)
	{
		cow_ui_show(false);
		gfx_printf("%kcowMMC: saving LKG snapshot...%k\n", COW_CLR_CYAN, COW_CLR_DEF);
	}

	u64 prefix_bytes = (u64)hdr->prefix_sectors * COW_SECTOR_SIZE;
	u32 pcrc = 0U;
	if (_cow_snapshot_transfer(&fp, emmc_dir, part_sizes, num_parts, prefix_bytes,
	                           payload_bytes, false, &pcrc, "      Save:    "))
		goto fail;

	shdr.magic                 = COW_RAW_SNAP_MAGIC;
	shdr.snap_version          = COW_RAW_SNAP_VERSION;
	shdr.active_sb_slot        = active_sb_slot;
	shdr.cow_version           = hdr->version;
	shdr.seq_num               = hdr->seq_num;
	shdr.gpp_total_sectors     = hdr->gpp_total_sectors;
	shdr.physical_base_sectors = hdr->physical_base_sectors;
	shdr.prefix_sectors        = hdr->prefix_sectors;
	shdr.total_slots           = hdr->total_slots;
	shdr.first_data_slot       = hdr->first_data_slot;
	shdr.next_free_slot        = hdr->next_free_slot;
	shdr.l1_entries            = hdr->l1_entries;
	shdr.l1_slot_a             = hdr->l1_slot_a;
	shdr.l1_slot_b             = hdr->l1_slot_b;
	shdr.flags                 = hdr->flags;
	shdr.dirty_data_blocks     = hdr->dirty_data_blocks;
	shdr.dirty_l2_pages        = hdr->dirty_l2_pages;
	shdr.base_gen_id           = hdr->base_gen_id;
	memcpy(shdr.overlay_uuid, hdr->overlay_uuid, 16U);
	shdr.payload_crc32         = pcrc;
	shdr.header_crc32          = crc32_calc(0, (const u8 *)&shdr, offsetof(cow_raw_snap_hdr_t, header_crc32));

	bw = 0U;
	if (f_lseek(&fp, 0) != FR_OK || f_write(&fp, &shdr, sizeof(shdr), &bw) != FR_OK || bw != sizeof(shdr))
		goto fail;
	f_sync(&fp);
	f_close(&fp);

	if (opt_prev_path && f_stat(snap_path, NULL) == FR_OK)
	{
		f_unlink(opt_prev_path);
		if (f_rename(snap_path, opt_prev_path) != FR_OK)
			f_unlink(snap_path);
	}
	else
	{
		f_unlink(snap_path);
	}
	if (f_rename(tmp_path, snap_path) != FR_OK)
	{
		f_unlink(tmp_path);
		return 1;
	}
	return 0;

fail:
	f_close(&fp);
	f_unlink(tmp_path);
	return 1;
}

__attribute__((noinline))
static int _cow_restore_raw_snapshot(
	const char *snap_path,
	const char *emmc_dir,
	const u64 *part_sizes,
	u32 num_parts,
	u64 total_archive_bytes,
	u64 expected_base_gen_id)
{
	cow_raw_snap_hdr_t shdr;
	if (_cow_probe_raw_snapshot(snap_path, &shdr, total_archive_bytes, true))
		return 1;
	if (expected_base_gen_id != 0ULL && shdr.base_gen_id != 0ULL && shdr.base_gen_id != expected_base_gen_id)
		return 1;

	FIL fp;
	if (f_open(&fp, snap_path, FA_READ) != FR_OK)
		return 1;
	if (f_lseek(&fp, sizeof(cow_raw_snap_hdr_t)) != FR_OK)
	{
		f_close(&fp);
		return 1;
	}

	u64 prefix_bytes = (u64)shdr.prefix_sectors * COW_SECTOR_SIZE;
	u64 payload_bytes = (u64)shdr.next_free_slot * COW_BLOCK_BYTES;
	int rc = _cow_snapshot_transfer(&fp, emmc_dir, part_sizes, num_parts, prefix_bytes,
	                                payload_bytes, true, NULL, "      Restore: ");
	f_close(&fp);
	return rc;
}

__attribute__((noinline))
static int _cow_rebaseline_overlay(
	const char *base_path,
	const char *emmc_dir,
	const u64 *part_sizes,
	u32 num_parts,
	u32 *io_active_slot,
	cow_header_t *io_hdr,
	u32 *l1_table,
	u32 active_l1_entries)
{
	const u32 chunk_sectors = 1024U; /* 512 KiB buffer */
	u8 *buf = (u8 *)malloc(chunk_sectors * COW_SECTOR_SIZE);
	if (!buf)
		return 1;

	cow_base_digests_t cur;
	if (_cow_read_and_hash_physical_base(&cur, true, emmc_dir, true))
	{
		free(buf);
		return 1;
	}
	if (!cur.gpt_valid || !cur.pkg2_valid)
	{
		free(buf);
		return 1;
	}

	u64 sectors_per_l1 = (u64)COW_L2_ENTRIES * COW_BLOCK_SECTORS;
	u64 needed_l1 = (cur.phys_gpp_sectors + sectors_per_l1 - 1ULL) / sectors_per_l1;
	if (needed_l1 > active_l1_entries)
	{
		free(buf);
		return 1;
	}

	u32 prefix_sects = io_hdr->prefix_sectors ? io_hdr->prefix_sectors : COW_DEFAULT_PREFIX_SECTS;
	emmc_set_partition(0U);
	u32 lba = 0U;
	u32 p_total_mb = prefix_sects >> 11;
	u32 px = 0U, py = 0U;
	cow_ui_getpos(&px, &py);
	u32 next_log_lba = 0U;
	FIL p0_fp;
	bool stream_p0 = (num_parts > 0U && part_sizes[0] >= (u64)prefix_sects * COW_SECTOR_SIZE);
	if (stream_p0)
	{
		char p0_path[0x240];
		_cow_part_path(p0_path, emmc_dir, 0U);
		if (f_open(&p0_fp, p0_path, FA_OPEN_EXISTING | FA_READ | FA_WRITE) != FR_OK || f_lseek(&p0_fp, 0) != FR_OK)
		{
			f_close(&p0_fp);
			gfx_printf("\n");
			free(buf);
			return 1;
		}
	}
	while (lba < prefix_sects)
	{
		if (lba >= next_log_lba)
		{
			u32 cur_mb = lba >> 11;
			u32 pct = prefix_sects ? (u32)(((u64)lba * 100ULL) / prefix_sects) : 100U;
			cow_ui_setpos(px, py);
			gfx_printf("      Prefix: %2d / %d MiB (%3d%%)  ",
				(int)cur_mb, (int)p_total_mb, (int)pct);
			next_log_lba += 4096U;
		}
		u32 take = MIN(prefix_sects - lba, chunk_sectors);
		UINT bw = 0U;
		bool wfail = false;
		if (sdmmc_storage_read(&emmc_storage, lba, take, buf) != 0)
			wfail = true;
		else if (stream_p0)
			wfail = (f_write(&p0_fp, buf, take * COW_SECTOR_SIZE, &bw) != FR_OK || bw != take * COW_SECTOR_SIZE);
		else
			wfail = (_cow_archive_rw(emmc_dir, part_sizes, num_parts, (u64)lba * COW_SECTOR_SIZE, buf, take * COW_SECTOR_SIZE, true) != 0);
		if (wfail)
		{
			if (stream_p0)
				f_close(&p0_fp);
			gfx_printf("\n");
			free(buf);
			return 1;
		}
		lba += take;
	}
	if (stream_p0)
	{
		if (f_sync(&p0_fp) != FR_OK)
		{
			f_close(&p0_fp);
			gfx_printf("\n");
			free(buf);
			return 1;
		}
		f_close(&p0_fp);
	}
	cow_ui_setpos(px, py);
	gfx_printf("      Prefix: %2d / %d MiB (100%%)\n",
		(int)p_total_mb, (int)p_total_mb);
	free(buf);

	memset(l1_table, 0, active_l1_entries * sizeof(u32));
	io_hdr->gpp_total_sectors     = cur.phys_gpp_sectors;
	io_hdr->physical_base_sectors = cur.phys_gpp_sectors;
	io_hdr->next_free_slot        = io_hdr->first_data_slot;
	io_hdr->dirty_data_blocks     = 0U;
	io_hdr->dirty_l2_pages        = 0U;
	io_hdr->flags                &= ~(COW_FLAG_FW_MISMATCH | COW_FLAG_NEEDS_REBASE | COW_FLAG_CONFLICT_LOCKED);
	io_hdr->base_drift_part_mask  = 0U;

	memcpy(io_hdr->boot0_pkg1_sha256, cur.boot0_pkg1_sha256, 32U);
	memcpy(io_hdr->boot0_patrol_sha256_16, cur.boot0_patrol_sha256_16, 16U);
	memcpy(io_hdr->boot1_sha256, cur.boot1_sha256, 32U);
	memcpy(io_hdr->gpt_sha256, cur.gpt_sha256, 32U);
	memcpy(io_hdr->bcpkg2_sha256, cur.bcpkg2_sha256, 32U);
	memcpy(io_hdr->system_digest_sha256, cur.system_digest_sha256, 32U);
	memcpy(io_hdr->user_digest_sha256, cur.user_digest_sha256, 32U);

	u64 new_gen = 0ULL;
	if (_cow_compute_base_gen_id(&cur, io_hdr->base_hos_version, io_hdr->base_pkg1_ver, &new_gen) == 0)
		io_hdr->base_gen_id = new_gen;
	io_hdr->last_verified_base_gen_id = io_hdr->base_gen_id;

	u32 slot_a = (*io_active_slot) ^ 1U;
	io_hdr->seq_num++;
	io_hdr->last_rebase_seq = io_hdr->seq_num;
	if (_cow_commit_superblock(emmc_dir, part_sizes, num_parts, slot_a, io_hdr, l1_table))
		return 1;

	u32 slot_b = slot_a ^ 1U;
	io_hdr->seq_num++;
	io_hdr->last_rebase_seq = io_hdr->seq_num;
	if (_cow_commit_superblock(emmc_dir, part_sizes, num_parts, slot_b, io_hdr, l1_table))
		return 1;
	*io_active_slot = slot_b;

	char p[0x240];
	_cow_join_path(p, base_path, "/boot_pending");              f_unlink(p);
	_cow_join_path(p, base_path, "/boot_ok");                   f_unlink(p);
	_cow_join_path(p, base_path, "/snapshots/boot_lkg.cow");    f_unlink(p);
	_cow_join_path(p, base_path, "/snapshots/boot_prev.cow");   f_unlink(p);
	_cow_join_path(p, base_path, "/post_reset_migrate.flag");   _cow_write_text_file(p, "1\n");
	return 0;
}

__attribute__((noinline))
static int _cow_preboot_check_and_grow_impl(const char *base_path, cow_rt_mount_t *opt_rt, bool skip_base_rehash)
{
	char emmc_dir[0x200];
	strcpy(emmc_dir, base_path);
	strcat(emmc_dir, "/eMMC");

	u64 part_sizes[COW_MAX_SPLIT_PARTS];
	u32 num_parts = 0U;
	u64 total_archive_bytes = 0ULL;
	if (_cow_scan_parts(emmc_dir, part_sizes, COW_MAX_SPLIT_PARTS, &num_parts, &total_archive_bytes))
		return 1;

	u32 *l1_table = (u32 *)zalloc(COW_L1_ENTRIES * sizeof(u32));
	if (!l1_table)
		return 1;

	u32 active_slot = 0U;
	u32 active_l1_entries = 0U;
	u32 active_first_data = 0U;
	cow_header_t hdr;
	memset(&hdr, 0, sizeof(hdr));

	int load_rc = _cow_load_active_state(
		emmc_dir, part_sizes, num_parts, total_archive_bytes,
		&active_slot, &active_l1_entries, &active_first_data, &hdr, l1_table);
	if (load_rc == 2)
	{
		free(l1_table);
		return 0;
	}
	if (load_rc != 0)
	{
		free(l1_table);
		return 1;
	}

	if (hdr.flags & COW_FLAG_CONFLICT_LOCKED)
	{
		free(l1_table);
		EPRINTF("cowMMC: CONFLICT_LOCKED flag!\nReconcile overlay first.");
		return 1;
	}

	/* Read optional cow_autoinit and per-overlay cow_config overrides */
	cow_autoinit_cfg_t cfg;
	char cfg_path[0x240];
	_cow_join_path(cfg_path, base_path, "/cow_autoinit");
	_cow_parse_autoinit_cfg(cfg_path, &cfg);
	bool dummy_sz = false, dummy_max = false, dummy_step = false;
	_cow_join_path(cfg_path, base_path, "/cow_config");
	_cow_apply_cfg_file(cfg_path, &cfg, &dummy_sz, &dummy_max, &dummy_step);
	(void)dummy_sz;
	(void)dummy_max;
	(void)dummy_step;

	if ((hdr.flags & COW_FLAG_FW_MISMATCH) && !cfg.auto_reset_on_drift)
	{
		free(l1_table);
		EPRINTF("cowMMC: FW_MISMATCH flag set!\nRebase or new overlay required.");
		return 1;
	}

	bool need_sb_commit = false;
	bool rebaseline_now = (hdr.flags & COW_FLAG_FW_MISMATCH) != 0U;

	/* Phase 2 physical sysMMC SHA-256 verification */
	bool chk_pkg1   = !_cow_is_zero(hdr.boot0_pkg1_sha256, 32U);
	bool chk_patrol = !_cow_is_zero(hdr.boot0_patrol_sha256_16, 16U);
	bool chk_boot1  = !_cow_is_zero(hdr.boot1_sha256, 32U);
	bool chk_gpt    = !_cow_is_zero(hdr.gpt_sha256, 32U);
	bool chk_pkg2   = !_cow_is_zero(hdr.bcpkg2_sha256, 32U);
	bool chk_sys    = !_cow_is_zero(hdr.system_digest_sha256, 32U);
	bool chk_usr    = !_cow_is_zero(hdr.user_digest_sha256, 32U);

	if (!skip_base_rehash && !rebaseline_now && (chk_pkg1 || chk_patrol || chk_boot1 || chk_gpt || chk_pkg2 || chk_sys || chk_usr))
	{
		cow_base_digests_t cur;
		if (_cow_read_and_hash_physical_base(&cur, false, NULL, true))
		{
			free(l1_table);
			EPRINTF("cowMMC: Failed to hash\nphysical eMMC base.");
			return 1;
		}

		u32 hard_drift = 0U;
		if (chk_pkg1 && memcmp(cur.boot0_pkg1_sha256, hdr.boot0_pkg1_sha256, 32U))
			hard_drift |= COW_DRIFT_BOOT0_PKG1;
		if (chk_boot1 && memcmp(cur.boot1_sha256, hdr.boot1_sha256, 32U))
			hard_drift |= COW_DRIFT_BOOT1;
		if (chk_gpt && (!cur.gpt_valid || memcmp(cur.gpt_sha256, hdr.gpt_sha256, 32U)))
			hard_drift |= COW_DRIFT_GPT;
		if (chk_pkg2 && (!cur.pkg2_valid || memcmp(cur.bcpkg2_sha256, hdr.bcpkg2_sha256, 32U)))
			hard_drift |= COW_DRIFT_BCPKG2;
		if (chk_sys && (!cur.system_valid || memcmp(cur.system_digest_sha256, hdr.system_digest_sha256, 32U)))
			hard_drift |= COW_DRIFT_SYSTEM;
		if (chk_usr && (!cur.user_valid || memcmp(cur.user_digest_sha256, hdr.user_digest_sha256, 32U)))
			hard_drift |= COW_DRIFT_USER;

		bool patrol_changed = chk_patrol && memcmp(cur.boot0_patrol_sha256_16, hdr.boot0_patrol_sha256_16, 16U);

		if (hard_drift != 0U)
		{
			if (cfg.auto_reset_on_drift)
			{
				rebaseline_now = true;
			}
			else
			{
				hdr.flags |= COW_FLAG_FW_MISMATCH;
				hdr.base_drift_part_mask |= hard_drift | (patrol_changed ? COW_DRIFT_BOOT0_PATROL : 0U);
				hdr.seq_num++;
				u32 next_slot = active_slot ^ 1U;
				_cow_commit_superblock(emmc_dir, part_sizes, num_parts, next_slot, &hdr, l1_table);
				free(l1_table);
				EPRINTFARGS("sysNAND changed (drift=0x%08X)!\nMount blocked.", hdr.base_drift_part_mask);
				return 1;
			}
		}
		else if (patrol_changed)
		{
			u32 new_mask = hdr.base_drift_part_mask | COW_DRIFT_BOOT0_PATROL;
			u32 new_flags = hdr.flags | (cfg.strict_patrol ? COW_FLAG_NEEDS_REBASE : 0U);
			if (new_mask != hdr.base_drift_part_mask || new_flags != hdr.flags)
			{
				hdr.base_drift_part_mask = new_mask;
				hdr.flags = new_flags;
				need_sb_commit = true;
			}
			WPRINTF("cowMMC: BOOT0 Patrol changed\n(non-fatal).");
		}
	}

	if (rebaseline_now)
	{
		cow_ui_show(true);
		gfx_printf("%kcowMMC: sysNAND drift detected!%k\nAuto-rebaselining overlay...\n",
			COW_CLR_CYAN, COW_CLR_DEF);
		if (_cow_rebaseline_overlay(base_path, emmc_dir, part_sizes, num_parts,
		                            &active_slot, &hdr, l1_table, active_l1_entries))
		{
			free(l1_table);
			EPRINTF("cowMMC: Auto-rebaseline failed!");
			return 1;
		}
		gfx_printf("%kcowMMC: re-baseline complete!%k\n\n", COW_CLR_GREEN, COW_CLR_DEF);
		need_sb_commit = false;
	}

	/* Phase 2 SD-safe pre-boot thin pool auto-grow */
	if (hdr.alloc_policy == COW_ALLOC_THIN && hdr.max_overlay_slots > hdr.total_slots)
	{
		u32 data_start = hdr.first_data_slot;
		u32 used_slots = (hdr.next_free_slot >= data_start) ? (hdr.next_free_slot - data_start) : 0U;
		u32 backed_slots = (hdr.total_slots >= data_start) ? (hdr.total_slots - data_start) : 0U;
		u32 pct = hdr.thin_high_watermark_pct ? (u32)hdr.thin_high_watermark_pct : COW_DEFAULT_WATERMARK_PCT;
		if (pct > 100U)
			pct = 100U;

		if (backed_slots > 0U && ((u64)used_slots * 100ULL >= (u64)backed_slots * (u64)pct))
		{
			u64 free_bytes = 0ULL;
			if (_cow_get_sd_free_bytes(&free_bytes) == 0)
			{
				u64 reserve_bytes = (u64)cfg.sd_reserve_mb * 1024ULL * 1024ULL;
				u64 safe_free_bytes = (free_bytes > reserve_bytes) ? (free_bytes - reserve_bytes) : 0ULL;
				u64 safe_free_slots64 = safe_free_bytes / COW_BLOCK_BYTES;
				u32 safe_free_slots = (safe_free_slots64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (u32)safe_free_slots64;

				u32 step_slots = hdr.thin_grow_step_slots ? hdr.thin_grow_step_slots : (COW_DEFAULT_GROW_STEP_MB * 64U);
				u32 cap_slots = hdr.max_overlay_slots - hdr.total_slots;
				u32 grow_slots = MIN(step_slots, cap_slots);
				grow_slots = MIN(grow_slots, safe_free_slots);

				if (grow_slots > 0U)
				{
					u64 cfg_part_max = hdr.part_max_sectors
						? ((u64)hdr.part_max_sectors * COW_SECTOR_SIZE)
						: ((u64)COW_DEFAULT_PART_MAX_MB * 1024ULL * 1024ULL);
					if (cfg_part_max > (u64)COW_DEFAULT_PART_MAX_MB * 1024ULL * 1024ULL)
						cfg_part_max = (u64)COW_DEFAULT_PART_MAX_MB * 1024ULL * 1024ULL;
					if (cfg_part_max < part_sizes[0])
						cfg_part_max = part_sizes[0];

					u64 uniform_part_bytes = (num_parts == 1U) ? cfg_part_max : part_sizes[0];
					u64 max_arch_bytes = (u64)COW_MAX_SPLIT_PARTS * uniform_part_bytes;
					u64 prefix_bytes = (u64)hdr.prefix_sectors * COW_SECTOR_SIZE;
					u64 target_total_bytes = prefix_bytes + ((u64)(hdr.total_slots + grow_slots) * COW_BLOCK_BYTES);
					if (target_total_bytes > max_arch_bytes)
						target_total_bytes = max_arch_bytes;

					if (target_total_bytes > total_archive_bytes)
					{
						u64 bytes_to_add = target_total_bytes - total_archive_bytes;
						u32 add_mb = (u32)((bytes_to_add + (1024ULL * 1024ULL - 1ULL)) >> 20);
						cow_ui_show(false);
						gfx_printf("%kcowMMC: growing pool (+%d MiB)...%k\n",
							COW_CLR_CYAN, (int)add_mb, COW_CLR_DEF);
						u32 last_idx = num_parts - 1U;
						char ppath[0x240];

						if (part_sizes[last_idx] < uniform_part_bytes)
						{
							u64 room = uniform_part_bytes - part_sizes[last_idx];
							u64 ext = MIN(bytes_to_add, room);
							u64 new_sz = part_sizes[last_idx] + ext;
							_cow_part_path(ppath, emmc_dir, last_idx);
							if (_cow_extend_or_create_part(ppath, new_sz, false) == 0)
							{
								part_sizes[last_idx] = new_sz;
								total_archive_bytes += ext;
								bytes_to_add -= ext;
							}
							else
							{
								bytes_to_add = 0ULL;
							}
						}

						while (bytes_to_add > 0ULL && num_parts < COW_MAX_SPLIT_PARTS)
						{
							u64 new_sz = MIN(bytes_to_add, uniform_part_bytes);
							_cow_part_path(ppath, emmc_dir, num_parts);
							if (_cow_extend_or_create_part(ppath, new_sz, true) != 0)
							{
								f_unlink(ppath);
								break;
							}
							part_sizes[num_parts] = new_sz;
							num_parts++;
							total_archive_bytes += new_sz;
							bytes_to_add -= new_sz;
						}
					}

					u64 new_backing_slots64 = (total_archive_bytes - prefix_bytes) / COW_BLOCK_BYTES;
					u32 new_total_slots = (u32)MIN((u64)hdr.total_slots + grow_slots, new_backing_slots64);
					if (new_total_slots > hdr.max_overlay_slots)
						new_total_slots = hdr.max_overlay_slots;
					if (new_total_slots > hdr.total_slots)
					{
						gfx_printf("%kcowMMC: pool grew %d -> %d MiB.%k\n\n",
							COW_CLR_GREEN, (int)(hdr.total_slots / 64U), (int)(new_total_slots / 64U), COW_CLR_DEF);
						hdr.total_slots = new_total_slots;
						need_sb_commit = true;
					}
				}
			}
		}
	}

	if (need_sb_commit)
	{
		hdr.seq_num++;
		u32 next_slot = active_slot ^ 1U;
		if (_cow_commit_superblock(emmc_dir, part_sizes, num_parts, next_slot, &hdr, l1_table))
		{
			free(l1_table);
			EPRINTF("cowMMC: Failed to commit\nsuperblock update.");
			return 1;
		}
		active_slot = next_slot;
	}

	/* Warn when COW pool is >= 85% full so user can snapshot/prune safely */
	u32 pool_used = (hdr.next_free_slot >= hdr.first_data_slot) ? (hdr.next_free_slot - hdr.first_data_slot) : 0U;
	u32 pool_cap  = (hdr.total_slots > hdr.first_data_slot) ? (hdr.total_slots - hdr.first_data_slot) : 0U;
	if (pool_cap > 0U && ((u64)pool_used * 100ULL >= (u64)pool_cap * 85ULL))
	{
		gfx_printf("%kcowMMC: COW pool %d%% full\n(%d / %d slots)!%k\n",
			COW_CLR_WARN,
			(int)(((u64)pool_used * 100ULL) / (u64)pool_cap),
			(int)pool_used, (int)pool_cap, COW_CLR_DEF);
	}

	/* Persistent-mode LKG snapshot capture and A/B bootloop auto-rollback */
	if ((hdr.flags & COW_FLAG_EPHEMERAL) == 0U)
	{
		char ok_path[0x240];
		char pend_path[0x240];
		char lkg_path[0x240];
		char prev_path[0x240];
		_cow_join_path(ok_path, base_path, "/boot_ok");
		_cow_join_path(pend_path, base_path, "/boot_pending");
		_cow_join_path(lkg_path, base_path, "/snapshots/boot_lkg.cow");
		_cow_join_path(prev_path, base_path, "/snapshots/boot_prev.cow");

		bool has_ok      = (f_stat(ok_path, NULL) == FR_OK);
		bool has_pending = (f_stat(pend_path, NULL) == FR_OK);
		bool rolled_back = false;

		if (has_ok && has_pending)
		{
			u32 attempts = (u32)_cow_read_u32_file(pend_path, 1U);
			if (cfg.auto_rollback && attempts >= cfg.rollback_threshold)
			{
				cow_ui_show(true);
				gfx_printf("%kcowMMC: bootloop (%d fails)%k\nRestoring LKG snapshot...\n",
					COW_CLR_WARN, (int)attempts, COW_CLR_DEF);
				const char *cand = NULL;
				if (_cow_restore_raw_snapshot(lkg_path, emmc_dir, part_sizes, num_parts, total_archive_bytes, hdr.base_gen_id) == 0)
					cand = lkg_path;
				else if (_cow_restore_raw_snapshot(prev_path, emmc_dir, part_sizes, num_parts, total_archive_bytes, hdr.base_gen_id) == 0)
					cand = prev_path;

				if (cand)
				{
					if (_cow_load_active_state(emmc_dir, part_sizes, num_parts, total_archive_bytes,
					                           &active_slot, &active_l1_entries, &active_first_data, &hdr, l1_table))
					{
						free(l1_table);
						EPRINTF("cowMMC: Reload failed after\nLKG rollback!");
						return 1;
					}
					f_unlink(pend_path);
					has_pending = false;
					rolled_back = true;
					gfx_printf("%kcowMMC: LKG snapshot restored!%k\n\n", COW_CLR_GREEN, COW_CLR_DEF);
				}
				else
				{
					WPRINTF("Bootloop detected, no valid\nLKG snapshot available.");
				}
			}
			if (has_pending)
				_cow_write_u32_file(pend_path, attempts + 1U);
		}

		if (!has_pending && !rolled_back)
		{
			if (cfg.auto_snapshot)
			{
				if (_cow_save_raw_snapshot(base_path, emmc_dir, part_sizes, num_parts,
				                           active_slot, &hdr, lkg_path, prev_path, 16U))
				{
					WPRINTF("Skipped auto LKG snapshot\n(low SD space or I/O error).");
				}
			}
			if (has_ok && cfg.auto_rollback)
				_cow_write_u32_file(pend_path, 1U);
		}
	}

	if (opt_rt)
	{
		u32 *l2_buf = (u32 *)malloc(COW_BLOCK_BYTES);
		if (!l2_buf)
		{
			free(l1_table);
			return 1;
		}
		memset(opt_rt, 0, sizeof(*opt_rt));
		opt_rt->active = true;
		opt_rt->hdr = hdr;
		opt_rt->l1_entries = active_l1_entries;
		opt_rt->first_data_slot = active_first_data;
		opt_rt->num_parts = num_parts;
		memcpy(opt_rt->part_sizes, part_sizes, sizeof(part_sizes));
		strcpy(opt_rt->emmc_dir, emmc_dir);
		opt_rt->l1_table = l1_table;
		opt_rt->l2_buf = l2_buf;
		opt_rt->cached_l1_idx = 0xFFFFFFFFU;
		return 0;
	}

	free(l1_table);
	return 0;
}

#ifdef COW_BSO_MODULE
__attribute__((unused))
#endif
static int _cow_preboot_check_and_grow_ex(const char *base_path, cow_rt_mount_t *opt_rt)
{
	return _cow_preboot_check_and_grow_impl(base_path, opt_rt, false);
}

#ifdef COW_BSO_MODULE
__attribute__((unused))
#endif
static int _cow_preboot_check_and_grow(const char *base_path)
{
	return _cow_preboot_check_and_grow_ex(base_path, NULL);
}

__attribute__((noinline))
static int _cow_autoinit(const char *base_path, const char *cfg_path, u32 emu_id)
{
	cow_autoinit_cfg_t cfg;
	_cow_parse_autoinit_cfg(cfg_path, &cfg);
	if (cfg.invalid)
	{
		EPRINTF("Invalid cow_autoinit cfg.");
		return 1;
	}

	cow_ui_show(true);
	gfx_printf("%kcowMMC Auto-Init (v%d)%k\n",
		COW_CLR_CYAN, (int)COW_VERSION_V2, COW_CLR_DEF);
	gfx_printf("Path: %s\n", base_path);
	gfx_printf("Mode: %s, %s\n\n",
		cfg.thin ? "thin" : "thick",
		cfg.ephemeral ? "ephemeral" : "persistent");
	gfx_printf("[1/5] Checking SD free space...\n");

	const u32 chunk_sectors = 1024U; /* 512 KiB buffer */
	u8 *buf = (u8 *)malloc(chunk_sectors * COW_SECTOR_SIZE);
	if (!buf)
	{
		EPRINTF("Out of memory.");
		return 1;
	}

	char emmc_dir[0x200];
	char fpath[0x240];
	strcpy(emmc_dir, base_path);
	strcat(emmc_dir, "/eMMC");
	_cow_mkdir_p(emmc_dir);

	strcpy(fpath, base_path);
	strcat(fpath, "/Nintendo");
	f_mkdir(fpath);

	/* 1. Check SD free headroom before allocating pool */
	u64 sd_free_bytes = 0ULL;
	if (_cow_get_sd_free_bytes(&sd_free_bytes))
	{
		EPRINTF("Failed to query SD free space.");
		goto fail;
	}

	u64 reserve_bytes = (u64)cfg.sd_reserve_mb * 1024ULL * 1024ULL;
	u64 overhead_bytes = ((u64)COW_BOOT_PART_BYTES * 2ULL) + COW_DEFAULT_PREFIX_BYTES;
	if (sd_free_bytes <= reserve_bytes + overhead_bytes)
	{
		EPRINTF("Insufficient SD free space\n(respecting sd_reserve_mb).");
		goto fail;
	}
	u64 max_safe_pool_bytes = ((sd_free_bytes - reserve_bytes - overhead_bytes) / COW_BLOCK_BYTES) * COW_BLOCK_BYTES;
	u64 max_safe_pool_slots64 = max_safe_pool_bytes / COW_BLOCK_BYTES;
	u32 max_safe_pool_slots = (max_safe_pool_slots64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (u32)max_safe_pool_slots64;

	u32 total_slots = 0U;
	if (cfg.size_auto)
	{
		u32 target_mb = cfg.thin ? 512U : 2048U;
		u64 target_slots64 = ((u64)target_mb * 1024ULL * 1024ULL) / COW_BLOCK_BYTES;
		total_slots = (u32)MIN(target_slots64, (u64)max_safe_pool_slots);
	}
	else
	{
		u64 req_bytes = (((u64)cfg.size_mb * 1024ULL * 1024ULL) / COW_BLOCK_BYTES) * COW_BLOCK_BYTES;
		u64 req_slots64 = req_bytes / COW_BLOCK_BYTES;
		if (req_slots64 > max_safe_pool_slots)
		{
			EPRINTF("Requested size_mb exceeds\nsafe SD free space.");
			goto fail;
		}
		total_slots = (u32)req_slots64;
	}

	if (total_slots <= COW_FIRST_DATA_SLOT)
	{
		EPRINTF("Pool too small for metadata.");
		goto fail;
	}

	gfx_printf("      Free: %d MiB, Pool: %d MiB\n",
		(int)(sd_free_bytes >> 20), (int)(total_slots / 64U));
	gfx_printf("[2/5] Copying & hashing sysMMC...\n");

	/* 2. Copy physical boot0 & boot1 and hash physical regions */
	cow_base_digests_t digests;
	if (_cow_read_and_hash_physical_base(&digests, true, emmc_dir, true))
	{
		EPRINTF("Failed to read/hash sysMMC.");
		goto fail;
	}

	if (!digests.gpt_valid || !digests.pkg2_valid)
	{
		EPRINTF("Invalid physical GPT or pkg2.");
		goto fail;
	}

	u64 phys_gpp_sectors = digests.phys_gpp_sectors;
	u64 gpp_total_sectors = phys_gpp_sectors;

	u64 sectors_per_l1 = (u64)COW_L2_ENTRIES * COW_BLOCK_SECTORS;
	u64 needed_l1 = (gpp_total_sectors + sectors_per_l1 - 1ULL) / sectors_per_l1;
	if (needed_l1 > COW_L1_ENTRIES)
	{
		EPRINTF("Physical GPP exceeds L1 map.");
		goto fail;
	}
	u32 l1_entries = (needed_l1 <= COW_V2_DEFAULT_L1_ENTRIES) ? COW_V2_DEFAULT_L1_ENTRIES : COW_L1_ENTRIES;
	u32 l1_slots = COW_L1_SLOTS_FOR(l1_entries);
	u32 l1_slot_a = COW_SUPERBLOCK_SLOTS;
	u32 l1_slot_b = l1_slot_a + l1_slots;
	u32 first_data_slot = l1_slot_b + l1_slots;
	u32 max_overlay_slots = total_slots;

	if (total_slots <= first_data_slot)
	{
		EPRINTF("Pool smaller than L1 metadata.");
		goto fail;
	}

	if (cfg.thin)
	{
		if (cfg.max_size_auto)
		{
			max_overlay_slots = max_safe_pool_slots;
		}
		else if (cfg.max_size_mb > 0U)
		{
			u64 max64 = (((u64)cfg.max_size_mb * 1024ULL * 1024ULL) / COW_BLOCK_BYTES);
			max_overlay_slots = (max64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (u32)max64;
		}
	}
	else if (cfg.max_size_mb > 0U && !cfg.max_size_auto)
	{
		u64 max64 = (((u64)cfg.max_size_mb * 1024ULL * 1024ULL) / COW_BLOCK_BYTES);
		max_overlay_slots = (max64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (u32)max64;
	}
	if (max_overlay_slots < total_slots)
	{
		EPRINTF("max_size_mb below initial pool.");
		goto fail;
	}

	/* 3. Create /eMMC/00..NN with 32 MiB prefix + ping-pong superblocks + external L1 + COW pool */
	u64 cow_pool_bytes = (u64)total_slots * COW_BLOCK_BYTES;
	gfx_printf("[3/5] Allocating pool (%d MiB)...\n",
		(int)(cow_pool_bytes >> 20));
	u64 remaining = COW_DEFAULT_PREFIX_BYTES + cow_pool_bytes;
	u64 part_max_bytes = (u64)cfg.part_max_mb * 1024ULL * 1024ULL;
	u32 flags = cfg.ephemeral ? COW_FLAG_EPHEMERAL : 0U;
	if (cfg.thin)
		flags |= COW_FLAG_THIN_PROVISION;

	cow_header_t v2_tmpl;
	memset(&v2_tmpl, 0, sizeof(v2_tmpl));
	v2_tmpl.magic                   = COW_MAGIC;
	v2_tmpl.version                 = COW_VERSION_V2;
	v2_tmpl.gpp_total_sectors       = gpp_total_sectors;
	v2_tmpl.prefix_sectors          = COW_DEFAULT_PREFIX_SECTS;
	v2_tmpl.total_slots             = total_slots;
	v2_tmpl.next_free_slot          = first_data_slot;
	v2_tmpl.flags                   = flags;
	v2_tmpl.header_ext_size         = COW_SECTOR_SIZE;
	v2_tmpl.physical_base_sectors   = phys_gpp_sectors;
	v2_tmpl.max_overlay_slots       = max_overlay_slots;
	v2_tmpl.l1_entries              = l1_entries;
	v2_tmpl.l1_slot_a               = l1_slot_a;
	v2_tmpl.l1_slot_b               = l1_slot_b;
	v2_tmpl.first_data_slot         = first_data_slot;
	v2_tmpl.thin_grow_step_slots    = cfg.thin ? (u32)(((u64)cfg.grow_step_mb * 1024ULL * 1024ULL) / COW_BLOCK_BYTES) : 0U;
	v2_tmpl.thin_high_watermark_pct = (u16)cfg.watermark_pct;
	v2_tmpl.alloc_policy            = cfg.thin ? COW_ALLOC_THIN : COW_ALLOC_THICK;
	v2_tmpl.block_shift             = COW_BLOCK_SHIFT;
	v2_tmpl.l2_shift                = COW_L2_SHIFT;
	v2_tmpl.part_max_sectors        = cfg.part_max_mb * 2048U;
	memcpy(v2_tmpl.boot0_pkg1_sha256, digests.boot0_pkg1_sha256, 32U);
	memcpy(v2_tmpl.boot0_patrol_sha256_16, digests.boot0_patrol_sha256_16, 16U);
	memcpy(v2_tmpl.boot1_sha256, digests.boot1_sha256, 32U);
	memcpy(v2_tmpl.gpt_sha256, digests.gpt_sha256, 32U);
	memcpy(v2_tmpl.bcpkg2_sha256, digests.bcpkg2_sha256, 32U);
	memcpy(v2_tmpl.system_digest_sha256, digests.system_digest_sha256, 32U);
	memcpy(v2_tmpl.user_digest_sha256, digests.user_digest_sha256, 32U);
	_cow_extract_overlay_name(base_path, v2_tmpl.overlay_name, sizeof(v2_tmpl.overlay_name));

	u64 unified_gen_id = 0ULL;
	if (_cow_compute_base_gen_id(&digests, 0U, 0U, &unified_gen_id) == 0)
		v2_tmpl.base_gen_id = unified_gen_id;
	else
		v2_tmpl.base_gen_id = phys_gpp_sectors ? phys_gpp_sectors : 1ULL;
	v2_tmpl.last_verified_base_gen_id = v2_tmpl.base_gen_id;

	u8 *sb_buf = (u8 *)malloc(COW_BLOCK_BYTES);
	if (!sb_buf)
	{
		EPRINTF("Out of memory.");
		goto fail;
	}

	u32 part_idx = 0U;
	while (remaining > 0ULL && part_idx < COW_MAX_SPLIT_PARTS)
	{
		u64 cur_size = (remaining > part_max_bytes) ? part_max_bytes : remaining;
		_cow_part_path(fpath, emmc_dir, part_idx);

		FIL fp;
		if (f_open(&fp, fpath, FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK)
		{
			free(sb_buf);
			EPRINTF("Failed to create pool part.");
			goto fail;
		}

		if (_cow_prealloc_file_stepped(&fp, 0ULL, cur_size, "      Part", (int)part_idx) != 0 ||
		    f_lseek(&fp, 0) != FR_OK)
		{
			f_close(&fp);
			free(sb_buf);
			EPRINTF("Failed to prealloc pool file.");
			goto fail;
		}

		if (part_idx == 0U)
		{
			gfx_printf("[4/5] Copying 32 MiB GPP prefix...\n");
			emmc_set_partition(0U);
			u32 lba = 0U;
			u32 p_total_mb = COW_DEFAULT_PREFIX_SECTS >> 11;
			u32 px = 0U, py = 0U;
			cow_ui_getpos(&px, &py);
			u32 next_log_lba = 0U;
			UINT bw = 0U;
			while (lba < COW_DEFAULT_PREFIX_SECTS)
			{
				if (lba >= next_log_lba)
				{
					u32 cur_mb = lba >> 11;
					u32 pct = (u32)(((u64)lba * 100ULL) / COW_DEFAULT_PREFIX_SECTS);
					cow_ui_setpos(px, py);
					gfx_printf("      %2d / %d MiB (%3d%%)  ",
						(int)cur_mb, (int)p_total_mb, (int)pct);
					next_log_lba += 4096U;
				}
				u32 cur = MIN(COW_DEFAULT_PREFIX_SECTS - lba, chunk_sectors);
				if (sdmmc_storage_read(&emmc_storage, lba, cur, buf) != 0)
				{
					gfx_printf("\n");
					f_close(&fp);
					free(sb_buf);
					EPRINTF("Failed to read sysMMC prefix.");
					goto fail;
				}
				bw = 0U;
				if (f_write(&fp, buf, cur * COW_SECTOR_SIZE, &bw) != FR_OK || bw != cur * COW_SECTOR_SIZE)
				{
					gfx_printf("\n");
					f_close(&fp);
					free(sb_buf);
					EPRINTF("Failed to write SD prefix.");
					goto fail;
				}
				lba += cur;
			}
			cow_ui_setpos(px, py);
			gfx_printf("      %2d / %d MiB (100%%)\n",
				(int)p_total_mb, (int)p_total_mb);

			gfx_printf("      Writing superblocks & L1...");
			for (u32 slot = 0U; slot < COW_SUPERBLOCK_SLOTS; slot++)
			{
				bw = 0U;
				_cow_fill_superblock_v2(sb_buf, &v2_tmpl, NULL, (u64)(slot + 1U));

				if (f_write(&fp, sb_buf, COW_BLOCK_BYTES, &bw) != FR_OK || bw != COW_BLOCK_BYTES)
				{
					gfx_printf(" fail\n");
					f_close(&fp);
					free(sb_buf);
					EPRINTF("Failed to write superblock.");
					goto fail;
				}
			}

			memset(sb_buf, 0, COW_BLOCK_BYTES);
			for (u32 s = 0U; s < 2U * l1_slots; s++)
			{
				bw = 0U;
				if (f_write(&fp, sb_buf, COW_BLOCK_BYTES, &bw) != FR_OK || bw != COW_BLOCK_BYTES)
				{
					gfx_printf(" fail\n");
					f_close(&fp);
					free(sb_buf);
					EPRINTF("Failed to write L1 table.");
					goto fail;
				}
			}
			gfx_printf(" ok\n");
		}

		f_sync(&fp);
		f_close(&fp);
		remaining -= cur_size;
		part_idx++;
	}
	free(sb_buf);
	if (remaining > 0ULL)
	{
		EPRINTF("Pool exceeds max split parts.");
		goto fail;
	}

	/* 4. Create file_based marker, lifecycle subdirs, cow_config, and /emuMMC/emummc.ini */
	gfx_printf("[5/5] Writing markers & config...");
	FIL mf;
	_cow_join_path(fpath, base_path, "/file_based");
	if (f_open(&mf, fpath, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
		f_close(&mf);

	_cow_join_path(fpath, base_path, "/saves");             _cow_mkdir_p(fpath);
	_cow_join_path(fpath, base_path, "/snapshots");         _cow_mkdir_p(fpath);
	_cow_join_path(fpath, base_path, "/cow_state");         _cow_mkdir_p(fpath);
	_cow_join_path(fpath, base_path, "/cow_state/tickets"); _cow_mkdir_p(fpath);

	_cow_join_path(fpath, base_path, "/cow_config");
	if (f_stat(fpath, NULL) != FR_OK)
	{
		_cow_write_text_file(
			fpath,
			"auto_reset_on_drift=1\n"
			"auto_snapshot=1\n"
			"auto_rollback=1\n"
			"rollback_threshold=2\n");
	}

	if (f_stat("emuMMC/emummc.ini", NULL) != FR_OK)
	{
		if (f_open(&mf, "emuMMC/emummc.ini", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
		{
			char *ini = (char *)buf;
			strcpy(ini, "[emummc]\nenabled=1\nsector=0x0\nid=0x");
			_cow_append_hex32(ini, emu_id ? emu_id : COW_MAGIC);
			strcat(ini, "\npath=");
			strcat(ini, base_path);
			strcat(ini, "\nnintendo_path=");
			strcat(ini, base_path);
			strcat(ini, "/Nintendo\n");
			UINT bw = 0U;
			f_write(&mf, ini, (UINT)strlen(ini), &bw);
			f_close(&mf);
		}
	}

	gfx_printf(" ok\n\n");
	gfx_printf("%kcowMMC auto-init complete!%k\n\n", COW_CLR_GREEN, COW_CLR_DEF);
	free(buf);
	return 0;

fail:
	free(buf);
	return 1;
}

static int _cow_lookup_data_slot(cow_rt_mount_t *rt, u32 block_idx, u32 *out_slot)
{
	*out_slot = 0U;
	u32 l1_idx = block_idx >> COW_L2_SHIFT;
	u32 l2_idx = block_idx & (COW_L2_ENTRIES - 1U);
	if (l1_idx >= rt->l1_entries || rt->l1_table[l1_idx] == 0U)
		return 0;

	u32 l2_slot = rt->l1_table[l1_idx];
	if (l2_slot < rt->first_data_slot || l2_slot >= rt->hdr.total_slots)
		return 1;

	if (rt->cached_l1_idx != l1_idx)
	{
		u64 prefix_bytes = (u64)rt->hdr.prefix_sectors * COW_SECTOR_SIZE;
		u64 l2_off = prefix_bytes + ((u64)l2_slot * COW_BLOCK_BYTES);
		if (_cow_archive_rw(rt->emmc_dir, rt->part_sizes, rt->num_parts, l2_off, rt->l2_buf, COW_BLOCK_BYTES, false))
			return 1;
		rt->cached_l1_idx = l1_idx;
	}

	u32 data_slot = rt->l2_buf[l2_idx];
	if (data_slot != 0U && (data_slot < rt->first_data_slot || data_slot >= rt->hdr.total_slots))
		return 1;
	*out_slot = data_slot;
	return 0;
}

static int _cow_read_gpp_rt(cow_rt_mount_t *rt, u32 sector, u32 num_sectors, void *buf)
{
	if (!rt || !rt->active || !buf)
		return 1;
	if (num_sectors == 0U)
		return 0;

	u64 total_sects = rt->hdr.gpp_total_sectors;
	u64 req_end = (u64)sector + (u64)num_sectors;
	if (req_end > total_sects)
		return 1;

	u64 prefix_sects = (u64)rt->hdr.prefix_sectors;
	u64 phys_base = (rt->hdr.physical_base_sectors > 0ULL)
		? rt->hdr.physical_base_sectors
		: total_sects;
	u64 prefix_bytes = prefix_sects * COW_SECTOR_SIZE;

	u8 *dst = (u8 *)buf;
	u32 cur = sector;
	u32 rem = num_sectors;

	while (rem > 0U)
	{
		u32 block_idx = cur >> COW_BLOCK_SHIFT;
		u32 sect_in_block = cur & (COW_BLOCK_SECTORS - 1U);
		u32 chunk = MIN(COW_BLOCK_SECTORS - sect_in_block, rem);

		u32 data_slot = 0U;
		if (_cow_lookup_data_slot(rt, block_idx, &data_slot))
			return 1;

		if (data_slot != 0U)
		{
			u64 cow_off = prefix_bytes + ((u64)data_slot * COW_BLOCK_BYTES) + ((u64)sect_in_block * COW_SECTOR_SIZE);
			if (_cow_archive_rw(rt->emmc_dir, rt->part_sizes, rt->num_parts, cow_off, dst, chunk * COW_SECTOR_SIZE, false))
				return 1;
		}
		else
		{
			/* Coalesce adjacent clean blocks into single archive / physical / zero-fill reads */
			while (chunk < rem)
			{
				u32 next_block = (cur + chunk) >> COW_BLOCK_SHIFT;
				u32 next_slot = 0U;
				if (_cow_lookup_data_slot(rt, next_block, &next_slot))
					return 1;
				if (next_slot != 0U)
					break;
				u32 add = MIN(COW_BLOCK_SECTORS, rem - chunk);
				chunk += add;
			}

			u32 clean_rem = chunk;
			u32 clean_cur = cur;
			u8 *clean_dst = dst;
			while (clean_rem > 0U)
			{
				if ((u64)clean_cur < prefix_sects)
				{
					u32 span = (u32)MIN((u64)clean_rem, prefix_sects - (u64)clean_cur);
					if (_cow_archive_rw(rt->emmc_dir, rt->part_sizes, rt->num_parts,
					                    (u64)clean_cur * COW_SECTOR_SIZE, clean_dst, span * COW_SECTOR_SIZE, false))
						return 1;
					clean_cur += span;
					clean_dst += span * COW_SECTOR_SIZE;
					clean_rem -= span;
				}
				else if ((u64)clean_cur < phys_base)
				{
					u32 span = (u32)MIN((u64)clean_rem, phys_base - (u64)clean_cur);
					if (_cow_read_mmc_span(0U, clean_cur, span, clean_dst, NULL))
						return 1;
					clean_cur += span;
					clean_dst += span * COW_SECTOR_SIZE;
					clean_rem -= span;
				}
				else
				{
					return 1;
				}
			}
		}

		cur += chunk;
		dst += chunk * COW_SECTOR_SIZE;
		rem -= chunk;
	}

	return 0;
}

#ifdef COW_BSO_MODULE

static void _cow_rt_clear(void)
{
	if (g_rt.l1_table)
	{
		free(g_rt.l1_table);
		g_rt.l1_table = NULL;
	}
	if (g_rt.l2_buf)
	{
		free(g_rt.l2_buf);
		g_rt.l2_buf = NULL;
	}
	memset(&g_rt, 0, sizeof(g_rt));
	g_rt.cached_l1_idx = 0xFFFFFFFFU;
}

static int _cow_read_gpp_entry(u32 sector, u32 num_sectors, void *buf)
{
	return _cow_read_gpp_rt(&g_rt, sector, num_sectors, buf);
}

static int _cow_run_init(cow_bso_ctx_t *ctx)
{
	if (!ctx || (ctx->api_magic != COW_BSO_MAGIC && ctx->api_magic != COW_BSO_MAGIC_V2))
		return 1;

	g_ops = ctx;
	_cow_rt_clear();
	ctx->active = 0U;
	ctx->rc = 0U;

	if (ctx->force_disable || !ctx->base_path || !*ctx->base_path)
		return 0;

	char part0[0x240];
	strcpy(part0, ctx->base_path);
	strcat(part0, "/eMMC/00");

	FILINFO fno;
	bool did_autoinit = false;
	if (f_stat(part0, &fno) != FR_OK)
	{
		char auto_cfg[0x240];
		strcpy(auto_cfg, ctx->base_path);
		strcat(auto_cfg, "/cow_autoinit");
		if (f_stat(auto_cfg, NULL) == FR_OK)
		{
			if (_cow_autoinit(ctx->base_path, auto_cfg, ctx->emu_id))
			{
				EPRINTF("Failed cowMMC auto-init.");
				ctx->rc = 1U;
				return 1;
			}
			did_autoinit = true;
		}
		else
		{
			return 0;
		}
	}

	if (_cow_preboot_check_and_grow_impl(ctx->base_path, &g_rt, did_autoinit))
	{
		_cow_rt_clear();
		ctx->rc = 1U;
		return 1;
	}

	if (did_autoinit)
		gfx_printf("cowMMC: ready, booting HOS...\n\n");

	if (g_rt.active)
		ctx->active = 1U;

	return 0;
}

__attribute__((visibility("default")))
void cow_entry(void *moduleConfig, bdk_params_t *bp)
{
	cow_bso_ctx_t *ctx = (cow_bso_ctx_t *)moduleConfig;
	g_bdk = bp;
	if (!ctx || (ctx->api_magic != COW_BSO_MAGIC && ctx->api_magic != COW_BSO_MAGIC_V2))
		return;

	ctx->fn_run_init = _cow_run_init;
	ctx->fn_read_gpp = _cow_read_gpp_entry;
	ctx->loaded = 1U;
	_cow_run_init(ctx);
}

#endif /* COW_BSO_MODULE */
