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

#ifdef MOOSWITCH_UPDATE_HOST_TEST
#include <string.h>
#include "mooswitch_update.h"
#else
#include <stddef.h>
#include <utils/types.h>
#include <libs/fatfs/ff.h>
#include <storage/mooswitch_update.h>

extern void *memcpy(void *dst, const void *src, size_t n);
extern int   memcmp(const void *a, const void *b, size_t n);
#endif

#define MSW_MANIFEST_FORMAT "mooswitch-manifest-v1"
#define MSW_HASH_BUCKETS    4096U
#define MSW_U64_MAX_DIV10   1844674407370955161ULL

typedef struct _msw_entry_t
{
	char path[MOOSWITCH_PATH_MAX];
	u64  size;
} msw_entry_t;

typedef struct _msw_rm_work_t
{
	char    dir_stack[MOOSWITCH_DIR_STACK_MAX][MOOSWITCH_JOIN_MAX];
	char    child[MOOSWITCH_JOIN_MAX];
	DIR     dir;
	FILINFO fno;
} msw_rm_work_t;

typedef struct _msw_work_t
{
	msw_entry_t   entries[MOOSWITCH_MAX_ENTRIES];
	u16           hash_buckets[MSW_HASH_BUCKETS];
	char          src[MOOSWITCH_JOIN_MAX];
	char          dst[MOOSWITCH_JOIN_MAX];
	char          parent[MOOSWITCH_JOIN_MAX];
	char          version[64];
	char          variant[32];
	u32           entry_count;
	u32           expected_count;
	bool          format_ok;
	bool          has_file_count;
	bool          has_files;
	msw_rm_work_t rm;
} msw_work_t;

static u32 _msw_strlen(const char *s)
{
	u32 n = 0U;
	while (s && s[n])
		n++;
	return n;
}

static int _msw_strcmp(const char *a, const char *b)
{
	while (*a && (*a == *b))
	{
		a++;
		b++;
	}
	return (int)(u8)*a - (int)(u8)*b;
}

static int _msw_strncmp(const char *a, const char *b, u32 n)
{
	for (u32 i = 0U; i < n; i++)
	{
		if (a[i] != b[i] || a[i] == '\0')
			return (int)(u8)a[i] - (int)(u8)b[i];
	}
	return 0;
}

static bool _msw_join_path(char *out, u32 cap, const char *a, const char *b)
{
	u32 alen = _msw_strlen(a);
	u32 blen = _msw_strlen(b);

	if (alen + 1U + blen >= cap)
		return false;

	memcpy(out, a, alen);
	out[alen] = '/';
	memcpy(out + alen + 1U, b, blen);
	out[alen + 1U + blen] = '\0';
	return true;
}

static void _msw_dirname(const char *path, char *out, u32 cap)
{
	u32 len = _msw_strlen(path);
	u32 last_slash = 0U;
	bool found = false;

	for (u32 i = 0U; i < len; i++)
	{
		if (path[i] == '/')
		{
			last_slash = i;
			found = true;
		}
	}

	if (!found || last_slash == 0U || last_slash >= cap)
	{
		out[0] = '\0';
		return;
	}

	memcpy(out, path, last_slash);
	out[last_slash] = '\0';
}

static bool _msw_validate_rel_path(const char *path)
{
	if (!path || path[0] == '\0')
		return false;

	u32 len = _msw_strlen(path);
	if (len >= MOOSWITCH_PATH_MAX)
		return false;

	if (path[0] == '/' || path[len - 1U] == '/')
		return false;

	if (_msw_strcmp(path, MOOSWITCH_MANIFEST_DST) == 0)
		return false;

	if (_msw_strcmp(path, MOOSWITCH_STAGING_DIR) == 0)
		return false;

	u32 staging_len = _msw_strlen(MOOSWITCH_STAGING_DIR);
	if (_msw_strncmp(path, MOOSWITCH_STAGING_DIR, staging_len) == 0 && path[staging_len] == '/')
		return false;

	u32 seg_start = 0U;
	for (u32 i = 0U; i <= len; i++)
	{
		unsigned char c = (unsigned char)path[i];
		if (c == '\0' || c == '/')
		{
			u32 seg_len = i - seg_start;
			if (seg_len == 0U)
				return false;
			if (seg_len == 1U && path[seg_start] == '.')
				return false;
			if (seg_len == 2U && path[seg_start] == '.' && path[seg_start + 1U] == '.')
				return false;
			seg_start = i + 1U;
		}
		else
		{
			if (c < 0x20U || c == 0x7FU || c == '\\' || c == ':')
				return false;
		}
	}

	return true;
}

static u32 _msw_hash_path(const char *s)
{
	u32 h = 2166136261U;
	while (*s)
	{
		h ^= (u8)*s++;
		h *= 16777619U;
	}
	return h;
}

static FRESULT _msw_unlink_force(const msw_fs_ops_t *ops, const char *path)
{
	FRESULT res = ops->unlink(path);
	if (res == FR_DENIED)
	{
		ops->chmod(path, 0, AM_RDO);
		res = ops->unlink(path);
	}
	return res;
}

static bool _msw_rm_tree_iterative(const msw_fs_ops_t *ops, msw_rm_work_t *rw, const char *root)
{
	FRESULT res = ops->stat(root, &rw->fno);
	if (res == FR_NO_FILE || res == FR_NO_PATH)
		return true;
	if (res != FR_OK)
		return false;

	if (!(rw->fno.fattrib & AM_DIR))
		return (_msw_unlink_force(ops, root) == FR_OK);

	u32 root_len = _msw_strlen(root);
	if (root_len == 0U || root_len >= MOOSWITCH_JOIN_MAX)
		return false;

	memcpy(rw->dir_stack[0], root, root_len + 1U);
	u32 depth = 1U;
	bool ok = true;

	while (depth > 0U)
	{
		const char *cur = rw->dir_stack[depth - 1U];
		if (ops->opendir(&rw->dir, cur) != FR_OK)
		{
			ok = false;
			break;
		}

		bool descended = false;
		for (;;)
		{
			res = ops->readdir(&rw->dir, &rw->fno);
			if (res != FR_OK || rw->fno.fname[0] == '\0')
			{
				if (res != FR_OK)
					ok = false;
				break;
			}

			if (rw->fno.fname[0] == '.' &&
				(rw->fno.fname[1] == '\0' || (rw->fno.fname[1] == '.' && rw->fno.fname[2] == '\0')))
				continue;

			if (!_msw_join_path(rw->child, sizeof(rw->child), cur, rw->fno.fname))
			{
				ok = false;
				break;
			}

			if (rw->fno.fattrib & AM_DIR)
			{
				if (depth >= MOOSWITCH_DIR_STACK_MAX)
				{
					ok = false;
					break;
				}
				u32 clen = _msw_strlen(rw->child);
				memcpy(rw->dir_stack[depth], rw->child, clen + 1U);
				depth++;
				descended = true;
				break;
			}

			if (rw->fno.fattrib & AM_RDO)
				ops->chmod(rw->child, 0, AM_RDO);

			if (_msw_unlink_force(ops, rw->child) != FR_OK)
				ok = false;
		}

		ops->closedir(&rw->dir);

		if (!ok)
			break;

		if (!descended)
		{
			if (_msw_unlink_force(ops, cur) != FR_OK)
			{
				ok = false;
				break;
			}
			depth--;
		}
	}

	return ok;
}

static FRESULT _msw_mkdir_p(const msw_fs_ops_t *ops, msw_work_t *w, const char *dst_path)
{
	_msw_dirname(dst_path, w->parent, sizeof(w->parent));
	if (w->parent[0] == '\0')
		return FR_OK;

	if (ops->stat(w->parent, &w->rm.fno) == FR_OK)
		return (w->rm.fno.fattrib & AM_DIR) ? FR_OK : FR_DENIED;

	for (u32 i = 1U; w->parent[i] != '\0'; i++)
	{
		if (w->parent[i] == '/')
		{
			w->parent[i] = '\0';
			FRESULT res = ops->mkdir(w->parent);
			w->parent[i] = '/';
			if (res != FR_OK && res != FR_EXIST)
				return res;
		}
	}

	FRESULT res = ops->mkdir(w->parent);
	if (res != FR_OK && res != FR_EXIST)
		return res;

	return FR_OK;
}

static FRESULT _msw_move_file_overwrite(const msw_fs_ops_t *ops, msw_work_t *w, const char *src, const char *dst)
{
	if (ops->stat(src, &w->rm.fno) == FR_OK && (w->rm.fno.fattrib & AM_RDO))
		ops->chmod(src, 0, AM_RDO);

	FRESULT res = ops->stat(dst, &w->rm.fno);
	if (res == FR_OK)
	{
		if (w->rm.fno.fattrib & AM_DIR)
			return FR_DENIED;
		if (w->rm.fno.fattrib & AM_RDO)
			ops->chmod(dst, 0, AM_RDO);
		res = ops->unlink(dst);
		if (res != FR_OK && res != FR_NO_FILE && res != FR_NO_PATH)
			return res;
	}
	else if (res != FR_NO_FILE && res != FR_NO_PATH)
	{
		return res;
	}

	return ops->rename(src, dst);
}

/* Minimal strict JSON parser for MooSwitch pack-manifest.json (zero recursion, -fpie jump-table free). */

typedef struct _msw_json_t
{
	const char *p;
	const char *end;
} msw_json_t;

static void _json_skip_ws(msw_json_t *j)
{
	while (j->p < j->end)
	{
		char c = *j->p;
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			j->p++;
		else
			break;
	}
}

static int _hex_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool _json_parse_string(msw_json_t *j, char *out, u32 out_cap, bool fail_on_trunc)
{
	_json_skip_ws(j);
	if (j->p >= j->end || *j->p != '"')
		return false;
	j->p++;

	u32 len = 0U;
	bool overflow = false;

	while (j->p < j->end)
	{
		unsigned char c = (unsigned char)*j->p++;
		if (c == '"')
		{
			if (out && out_cap > 0U)
				out[(len < out_cap) ? len : (out_cap - 1U)] = '\0';
			return !(fail_on_trunc && overflow);
		}

		if (c < 0x20U)
			return false;

		unsigned char emit = c;
		if (c == '\\')
		{
			if (j->p >= j->end)
				return false;
			char esc = *j->p++;
			/* Use explicit if/else chain to prevent compiler jump tables in -fpie BSO modules. */
			if (esc == '"')       emit = '"';
			else if (esc == '\\') emit = '\\';
			else if (esc == '/')  emit = '/';
			else if (esc == 'b')  emit = '\b';
			else if (esc == 'f')  emit = '\f';
			else if (esc == 'n')  emit = '\n';
			else if (esc == 'r')  emit = '\r';
			else if (esc == 't')  emit = '\t';
			else if (esc == 'u')
			{
				if (j->end - j->p < 4)
					return false;
				u32 cp = 0U;
				for (u32 i = 0U; i < 4U; i++)
				{
					int hv = _hex_val(j->p[i]);
					if (hv < 0)
						return false;
					cp = (cp << 4) | (u32)hv;
				}
				j->p += 4;
				emit = (cp >= 0x20U && cp <= 0x7FU) ? (unsigned char)cp : '?';
			}
			else
			{
				return false;
			}
		}

		if (out && out_cap > 0U)
		{
			if (len + 1U < out_cap)
				out[len++] = (char)emit;
			else
				overflow = true;
		}
	}

	return false;
}

static bool _json_parse_u64(msw_json_t *j, u64 *out)
{
	_json_skip_ws(j);
	if (j->p >= j->end)
		return false;

	char first = *j->p;
	if (first < '0' || first > '9')
		return false;

	j->p++;
	u64 acc = (u64)(first - '0');
	if (first == '0')
	{
		if (j->p < j->end && *j->p >= '0' && *j->p <= '9')
			return false;
	}
	else
	{
		while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
		{
			u64 d = (u64)(*j->p++ - '0');
			if (acc > MSW_U64_MAX_DIV10 || (acc == MSW_U64_MAX_DIV10 && d > 5ULL))
				return false;
			acc = (acc << 3) + (acc << 1) + d;
		}
	}

	if (j->p < j->end && (*j->p == '.' || *j->p == 'e' || *j->p == 'E'))
		return false;

	*out = acc;
	return true;
}

static bool _json_skip_primitive_number(msw_json_t *j)
{
	if (j->p < j->end && *j->p == '-')
		j->p++;
	if (j->p >= j->end || *j->p < '0' || *j->p > '9')
		return false;
	if (*j->p == '0')
	{
		j->p++;
		if (j->p < j->end && *j->p >= '0' && *j->p <= '9')
			return false;
	}
	else
	{
		while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
			j->p++;
	}
	if (j->p < j->end && *j->p == '.')
	{
		j->p++;
		if (j->p >= j->end || *j->p < '0' || *j->p > '9')
			return false;
		while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
			j->p++;
	}
	if (j->p < j->end && (*j->p == 'e' || *j->p == 'E'))
	{
		j->p++;
		if (j->p < j->end && (*j->p == '+' || *j->p == '-'))
			j->p++;
		if (j->p >= j->end || *j->p < '0' || *j->p > '9')
			return false;
		while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
			j->p++;
	}
	return true;
}

static bool _json_match_lit(msw_json_t *j, const char *lit, u32 len)
{
	if ((u32)(j->end - j->p) < len)
		return false;
	if (memcmp(j->p, lit, len) != 0)
		return false;
	j->p += len;
	return true;
}

static bool _json_skip_value(msw_json_t *j)
{
	enum { SK_MAX = 16 };
	u8 kind[SK_MAX];
	u8 state[SK_MAX];
	u32 sp = 0U;

	for (;;)
	{
		_json_skip_ws(j);
		if (j->p >= j->end)
			return false;

		if (sp > 0U)
		{
			u32 top = sp - 1U;
			if (kind[top] == 1U)
			{
				if (state[top] == 0U || state[top] == 3U)
				{
					if (*j->p == '}')
					{
						j->p++;
						sp--;
						goto container_done;
					}
					if (state[top] == 3U)
					{
						if (*j->p != ',')
							return false;
						j->p++;
						_json_skip_ws(j);
					}
					if (!_json_parse_string(j, NULL, 0U, false))
						return false;
					_json_skip_ws(j);
					if (j->p >= j->end || *j->p != ':')
						return false;
					j->p++;
					state[top] = 2U;
					_json_skip_ws(j);
					if (j->p >= j->end)
						return false;
				}
			}
			else
			{
				if (state[top] == 0U || state[top] == 1U)
				{
					if (*j->p == ']')
					{
						j->p++;
						sp--;
						goto container_done;
					}
					if (state[top] == 1U)
					{
						if (*j->p != ',')
							return false;
						j->p++;
						_json_skip_ws(j);
						if (j->p >= j->end)
							return false;
					}
				}
			}
		}

		char c = *j->p;
		if (c == '{')
		{
			if (sp >= SK_MAX)
				return false;
			j->p++;
			kind[sp] = 1U;
			state[sp] = 0U;
			sp++;
			continue;
		}
		else if (c == '[')
		{
			if (sp >= SK_MAX)
				return false;
			j->p++;
			kind[sp] = 2U;
			state[sp] = 0U;
			sp++;
			continue;
		}
		else if (c == '"')
		{
			if (!_json_parse_string(j, NULL, 0U, false))
				return false;
		}
		else if (c == 't')
		{
			if (!_json_match_lit(j, "true", 4U))
				return false;
		}
		else if (c == 'f')
		{
			if (!_json_match_lit(j, "false", 5U))
				return false;
		}
		else if (c == 'n')
		{
			if (!_json_match_lit(j, "null", 4U))
				return false;
		}
		else if (c == '-' || (c >= '0' && c <= '9'))
		{
			if (!_json_skip_primitive_number(j))
				return false;
		}
		else
		{
			return false;
		}

container_done:
		if (sp == 0U)
			return true;

		if (kind[sp - 1U] == 1U)
			state[sp - 1U] = 3U;
		else
			state[sp - 1U] = 1U;
	}
}

static bool _json_parse_files_array(msw_json_t *j, msw_work_t *w)
{
	_json_skip_ws(j);
	if (j->p >= j->end || *j->p != '[')
		return false;
	j->p++;
	_json_skip_ws(j);

	if (j->p < j->end && *j->p == ']')
	{
		j->p++;
		w->has_files = true;
		return true;
	}

	char key[32];
	for (;;)
	{
		if (w->entry_count >= MOOSWITCH_MAX_ENTRIES)
			return false;

		_json_skip_ws(j);
		if (j->p >= j->end || *j->p != '{')
			return false;
		j->p++;
		_json_skip_ws(j);

		msw_entry_t *ent = &w->entries[w->entry_count];
		ent->path[0] = '\0';
		ent->size = 0ULL;
		bool has_path = false;
		bool has_size = false;

		if (j->p >= j->end || *j->p == '}')
			return false;

		for (;;)
		{
			if (!_json_parse_string(j, key, sizeof(key), false))
				return false;
			_json_skip_ws(j);
			if (j->p >= j->end || *j->p != ':')
				return false;
			j->p++;

			if (_msw_strcmp(key, "path") == 0)
			{
				if (has_path || !_json_parse_string(j, ent->path, sizeof(ent->path), true))
					return false;
				if (!_msw_validate_rel_path(ent->path))
					return false;
				has_path = true;
			}
			else if (_msw_strcmp(key, "size") == 0)
			{
				if (has_size || !_json_parse_u64(j, &ent->size))
					return false;
				has_size = true;
			}
			else
			{
				if (!_json_skip_value(j))
					return false;
			}

			_json_skip_ws(j);
			if (j->p >= j->end)
				return false;
			if (*j->p == '}')
			{
				j->p++;
				break;
			}
			if (*j->p != ',')
				return false;
			j->p++;
		}

		if (!has_path || !has_size)
			return false;

		u32 slot = _msw_hash_path(ent->path) & (MSW_HASH_BUCKETS - 1U);
		while (w->hash_buckets[slot] != 0U)
		{
			u32 existing_idx = (u32)(w->hash_buckets[slot] - 1U);
			if (_msw_strcmp(w->entries[existing_idx].path, ent->path) == 0)
				return false;
			slot = (slot + 1U) & (MSW_HASH_BUCKETS - 1U);
		}
		w->hash_buckets[slot] = (u16)(w->entry_count + 1U);
		w->entry_count++;

		_json_skip_ws(j);
		if (j->p >= j->end)
			return false;
		if (*j->p == ']')
		{
			j->p++;
			w->has_files = true;
			return true;
		}
		if (*j->p != ',')
			return false;
		j->p++;
	}
}

static bool _msw_parse_manifest(const char *buf, u32 len, msw_work_t *w)
{
	msw_json_t j = { buf, buf + len };
	char key[48];
	char fmt[48];

	_json_skip_ws(&j);
	if (j.p >= j.end || *j.p != '{')
		return false;
	j.p++;
	_json_skip_ws(&j);

	if (j.p >= j.end || *j.p == '}')
		return false;

	for (;;)
	{
		if (!_json_parse_string(&j, key, sizeof(key), false))
			return false;
		_json_skip_ws(&j);
		if (j.p >= j.end || *j.p != ':')
			return false;
		j.p++;

		if (_msw_strcmp(key, "format") == 0)
		{
			if (!_json_parse_string(&j, fmt, sizeof(fmt), true))
				return false;
			w->format_ok = (_msw_strcmp(fmt, MSW_MANIFEST_FORMAT) == 0);
		}
		else if (_msw_strcmp(key, "version") == 0)
		{
			if (!_json_parse_string(&j, w->version, sizeof(w->version), false))
				return false;
		}
		else if (_msw_strcmp(key, "variant") == 0)
		{
			if (!_json_parse_string(&j, w->variant, sizeof(w->variant), false))
				return false;
		}
		else if (_msw_strcmp(key, "file_count") == 0)
		{
			u64 cnt = 0ULL;
			if (!_json_parse_u64(&j, &cnt))
				return false;
			if (cnt == 0ULL || cnt > (u64)MOOSWITCH_MAX_ENTRIES)
				return false;
			w->expected_count = (u32)cnt;
			w->has_file_count = true;
		}
		else if (_msw_strcmp(key, "files") == 0)
		{
			if (w->has_files || !_json_parse_files_array(&j, w))
				return false;
		}
		else
		{
			if (!_json_skip_value(&j))
				return false;
		}

		_json_skip_ws(&j);
		if (j.p >= j.end)
			return false;
		if (*j.p == '}')
		{
			j.p++;
			break;
		}
		if (*j.p != ',')
			return false;
		j.p++;
	}

	_json_skip_ws(&j);
	if (j.p != j.end)
		return false;

	return w->format_ok && w->has_file_count && w->has_files &&
	       w->entry_count > 0U && w->entry_count == w->expected_count;
}

static bool _msw_validate_ops(const msw_fs_ops_t *ops)
{
	return ops &&
	       ops->stat &&
	       ops->mkdir &&
	       ops->unlink &&
	       ops->rename &&
	       ops->chmod &&
	       ops->opendir &&
	       ops->readdir &&
	       ops->closedir &&
	       ops->read_file &&
	       ops->malloc &&
	       ops->zalloc &&
	       ops->free &&
	       ops->show_console &&
	       ops->printf &&
	       ops->msleep &&
	       ops->btn_wait;
}

bool mooswitch_apply_staged_update_with_ops(const msw_fs_ops_t *ops)
{
	if (!_msw_validate_ops(ops))
		return false;

	/* Fast path: purge any incomplete staging dir without touching the display. */
	FILINFO fast_fno;
	FRESULT man_res = ops->stat(MOOSWITCH_STAGING_MANIFEST, &fast_fno);
	if (man_res != FR_OK || (fast_fno.fattrib & AM_DIR) ||
		fast_fno.fsize == 0ULL || fast_fno.fsize > (FSIZE_t)MOOSWITCH_MAX_MANIFEST_BYTES)
	{
		if (ops->stat(MOOSWITCH_STAGING_DIR, &fast_fno) == FR_OK)
		{
			msw_rm_work_t *rm = (msw_rm_work_t *)ops->zalloc(sizeof(msw_rm_work_t));
			if (rm)
			{
				_msw_rm_tree_iterative(ops, rm, MOOSWITCH_STAGING_DIR);
				ops->free(rm);
			}
		}
		return false;
	}

	msw_work_t *w = (msw_work_t *)ops->zalloc(sizeof(msw_work_t));
	if (!w)
		return false;

	ops->show_console();
	ops->printf("MooSwitch Pre-Boot Updater\nValidating staged update...\n");

	u32 raw_size = 0U;
	void *raw_buf = ops->read_file(MOOSWITCH_STAGING_MANIFEST, &raw_size);
	if (!raw_buf || raw_size == 0U || raw_size > MOOSWITCH_MAX_MANIFEST_BYTES)
	{
		if (raw_buf)
			ops->free(raw_buf);
		ops->printf("Failed to read staged manifest!\nRemoving staged files...\n");
		_msw_rm_tree_iterative(ops, &w->rm, MOOSWITCH_STAGING_DIR);
		ops->printf("Press any key...\n");
		ops->btn_wait();
		ops->free(w);
		return false;
	}

	char *json_buf = (char *)ops->malloc(raw_size + 1U);
	if (!json_buf)
	{
		ops->free(raw_buf);
		ops->free(w);
		return false;
	}
	memcpy(json_buf, raw_buf, raw_size);
	json_buf[raw_size] = '\0';
	ops->free(raw_buf);

	bool parsed_ok = _msw_parse_manifest(json_buf, raw_size, w);
	ops->free(json_buf);

	if (!parsed_ok)
	{
		ops->printf("Invalid staged pack-manifest.json!\nDiscarding staged update...\n");
		_msw_rm_tree_iterative(ops, &w->rm, MOOSWITCH_STAGING_DIR);
		ops->printf("No live files modified. Press any key...\n");
		ops->btn_wait();
		ops->free(w);
		return false;
	}

	/* Pre-flight size & presence verification over every staged file. */
	for (u32 i = 0U; i < w->entry_count; i++)
	{
		if (!_msw_join_path(w->src, sizeof(w->src), MOOSWITCH_STAGING_DIR, w->entries[i].path))
		{
			ops->printf("Staged path too long: %s\n", w->entries[i].path);
			_msw_rm_tree_iterative(ops, &w->rm, MOOSWITCH_STAGING_DIR);
			ops->printf("No live files modified. Press any key...\n");
			ops->btn_wait();
			ops->free(w);
			return false;
		}

		FRESULT sres = ops->stat(w->src, &w->rm.fno);
		if (sres != FR_OK || (w->rm.fno.fattrib & AM_DIR) ||
			(u64)w->rm.fno.fsize != w->entries[i].size)
		{
			ops->printf("Staging preflight failed:\n  %s (err %d)\n", w->entries[i].path, (int)sres);
			_msw_rm_tree_iterative(ops, &w->rm, MOOSWITCH_STAGING_DIR);
			ops->printf("No live files modified. Press any key...\n");
			ops->btn_wait();
			ops->free(w);
			return false;
		}
	}

	const char *ver_str = w->version[0] ? w->version : "unknown";
	const char *var_str = w->variant[0] ? w->variant : "update";
	ops->printf("Applying MooSwitch %s (%s, %d files)...\n", ver_str, var_str, (int)w->entry_count);

	bool has_existing_cow = (ops->stat("emuMMC/COW0/eMMC/00", &w->rm.fno) == FR_OK);
	for (u32 i = 0U; i < w->entry_count; i++)
	{
		const char *rel = w->entries[i].path;
		if (!_msw_join_path(w->src, sizeof(w->src), MOOSWITCH_STAGING_DIR, rel))
		{
			_msw_unlink_force(ops, MOOSWITCH_STAGING_MANIFEST);
			ops->printf("Apply failed (path overflow): %s\nPress any key...\n", rel);
			ops->btn_wait();
			ops->free(w);
			return false;
		}

		if (has_existing_cow && _msw_strcmp(rel, "emuMMC/COW0/cow_autoinit") == 0)
		{
			_msw_unlink_force(ops, w->src);
			continue;
		}

		FRESULT dres = _msw_mkdir_p(ops, w, rel);
		if (dres != FR_OK)
		{
			_msw_unlink_force(ops, MOOSWITCH_STAGING_MANIFEST);
			ops->printf("Apply mkdir failed on %s (err %d)\nPress any key...\n", rel, (int)dres);
			ops->btn_wait();
			ops->free(w);
			return false;
		}

		FRESULT mres = _msw_move_file_overwrite(ops, w, w->src, rel);
		if (mres != FR_OK)
		{
			_msw_unlink_force(ops, MOOSWITCH_STAGING_MANIFEST);
			ops->printf("Apply failed on %s (err %d)\nPress any key...\n", rel, (int)mres);
			ops->btn_wait();
			ops->free(w);
			return false;
		}

		if ((i & 31U) == 31U)
			ops->printf(".");
	}
	ops->printf("\n");

	FRESULT man_move = _msw_move_file_overwrite(ops, w, MOOSWITCH_STAGING_MANIFEST, MOOSWITCH_MANIFEST_DST);
	if (man_move != FR_OK)
	{
		_msw_unlink_force(ops, MOOSWITCH_STAGING_MANIFEST);
		ops->printf("Failed to install pack-manifest.json (err %d)\nPress any key...\n", (int)man_move);
		ops->btn_wait();
		ops->free(w);
		return false;
	}

	if (!_msw_rm_tree_iterative(ops, &w->rm, MOOSWITCH_STAGING_DIR))
		ops->printf("Warning: partial staging cleanup\n");

	ops->printf("Update applied (%d files). Reloading bootloader...\n", (int)w->entry_count);
	ops->msleep(600U);
	ops->free(w);
	return true;
}
