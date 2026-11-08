/*
 * Config file reading from the FAT RAM disk, via the FatFs API
 * (needed because the generic Zephyr FS API does not expose mtime).
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>

#include "config.h"
#include "config_fs.h"
#include "secure_zero.h"

#define ROOT_SNAPSHOT_MAX  512

static FATFS fat_fs;

static char read_buf[CONFIG_FS_TEXT_SIZE];

struct file_signature {
	uint64_t name_hash;
	uint64_t mtime;
	uint64_t content_hash;
	uint32_t size;
	bool exists;
};

struct root_signature {
	uint64_t name_hash;
	uint32_t fat_stamp;
	uint32_t size;
};

static struct root_signature root_baseline[ROOT_SNAPSHOT_MAX];
static size_t root_baseline_count;
static bool root_baseline_valid;
static struct file_signature stable_candidate;
static bool stable_candidate_valid;
volatile int dbg_disk_wipe_count;
volatile int dbg_disk_wipe_err;

/* FNV-1a is used only as a compact change detector, never as authentication. */
static uint64_t hash_bytes(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint64_t hash = 14695981039346656037ULL;

	while (len-- > 0) {
		hash ^= *p++;
		hash *= 1099511628211ULL;
	}
	return hash;
}

static uint64_t hash_name(const char *name)
{
	return hash_bytes(name, strlen(name));
}

/* FatFs "RTC" hook (CONFIG_FS_FATFS_HAS_RTC). The device has no real time
 * source for file stamps, so report 1980-01-01 00:00:00 — below the clock-anchor
 * sanity floor (TIME_SANE_MIN) — so device-written files are never mistaken for
 * a host-written clock anchor. The host writes its own mtime via SCSI, bypassing
 * this, so the clock still anchors to host-written files. */
DWORD get_fattime(void)
{
	return 0x210000UL; /* 1980-01-01 00:00:00 */
}

/* Days since 1970-01-01 from a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

uint64_t fat_time_to_unix(uint16_t fdate, uint16_t ftime)
{
	int year = 1980 + ((fdate >> 9) & 0x7F);
	int month = (fdate >> 5) & 0x0F;
	int day = fdate & 0x1F;
	int hour = (ftime >> 11) & 0x1F;
	int minute = (ftime >> 5) & 0x3F;
	int second = (ftime & 0x1F) * 2;

	return (uint64_t)days_from_civil(year, month, day) * 86400ULL +
	       (uint64_t)hour * 3600ULL +
	       (uint64_t)minute * 60ULL +
	       (uint64_t)second;
}

static int read_config_raw(struct file_signature *sig)
{
	FIL fp;
	FILINFO before;
	FILINFO after;
	FRESULT res;
	UINT br;
	uint32_t size;

	secure_zero(read_buf, sizeof(read_buf));
	res = f_mount(&fat_fs, "RAM:", 1);
	if (res != FR_OK) {
		return -EIO;
	}

	res = f_stat("RAM:/config.ini", &before);
	if (res == FR_NO_FILE) {
		memset(sig, 0, sizeof(*sig));
		f_mount(NULL, "RAM:", 0);
		return 0;
	}
	if (res != FR_OK || (before.fattrib & AM_DIR)) {
		f_mount(NULL, "RAM:", 0);
		return -EIO;
	}
	if (before.fsize >= sizeof(read_buf)) {
		f_mount(NULL, "RAM:", 0);
		return -EFBIG;
	}
	size = (uint32_t)before.fsize;

	res = f_open(&fp, "RAM:/config.ini", FA_READ);
	if (res == FR_NO_FILE) {
		f_mount(NULL, "RAM:", 0);
		return -EAGAIN;
	}
	if (res != FR_OK) {
		f_mount(NULL, "RAM:", 0);
		return -EIO;
	}

	br = 0;
	res = f_read(&fp, read_buf, size, &br);
	if (f_close(&fp) != FR_OK && res == FR_OK) {
		res = FR_DISK_ERR;
	}
	if (res == FR_OK) {
		res = f_stat("RAM:/config.ini", &after);
	}
	f_mount(NULL, "RAM:", 0);

	if (res != FR_OK || br != size) {
		secure_zero(read_buf, sizeof(read_buf));
		return -EIO;
	}
	if (before.fsize != after.fsize || before.fdate != after.fdate ||
	    before.ftime != after.ftime) {
		secure_zero(read_buf, sizeof(read_buf));
		return -EAGAIN;
	}
	read_buf[br] = '\0';

	memset(sig, 0, sizeof(*sig));
	sig->exists = true;
	sig->name_hash = hash_name("config.ini");
	sig->mtime = fat_time_to_unix(after.fdate, after.ftime);
	sig->content_hash = hash_bytes(read_buf, br);
	sig->size = size;
	return 0;
}

static bool signatures_equal(const struct file_signature *a,
			     const struct file_signature *b)
{
	return a->exists == b->exists && a->name_hash == b->name_hash &&
	       a->mtime == b->mtime && a->content_hash == b->content_hash &&
	       a->size == b->size;
}

int config_fs_read(struct totp_config *cfg)
{
	struct file_signature sig;
	int err;

	err = read_config_raw(&sig);
	if (err != 0) {
		return err;
	}
	if (!sig.exists) {
		secure_zero(read_buf, sizeof(read_buf));
		return -ENOENT;
	}
	err = config_parse(read_buf, sig.size, cfg) == 0 ? 0 : -EINVAL;
	secure_zero(read_buf, sizeof(read_buf));

	return err;
}

void config_fs_stable_reset(void)
{
	memset(&stable_candidate, 0, sizeof(stable_candidate));
	stable_candidate_valid = false;
}

int config_fs_read_stable(struct totp_config *cfg)
{
	struct file_signature sig;
	int err;

	err = read_config_raw(&sig);
	if (err != 0) {
		config_fs_stable_reset();
		return err;
	}
	if (!stable_candidate_valid ||
	    !signatures_equal(&sig, &stable_candidate)) {
		stable_candidate = sig;
		stable_candidate_valid = true;
		secure_zero(read_buf, sizeof(read_buf));
		return -EAGAIN;
	}

	config_fs_stable_reset();
	if (!sig.exists) {
		secure_zero(read_buf, sizeof(read_buf));
		return -ENOENT;
	}
	err = config_parse(read_buf, sig.size, cfg) == 0 ? 0 : -EINVAL;
	secure_zero(read_buf, sizeof(read_buf));
	return err;
}

static void root_signature_from_info(struct root_signature *sig,
				     const FILINFO *fno)
{
	memset(sig, 0, sizeof(*sig));
	sig->name_hash = hash_name(fno->fname);
	sig->fat_stamp = ((uint32_t)fno->fdate << 16) | fno->ftime;
	sig->size = (uint32_t)fno->fsize;
}

int config_fs_capture_baseline(void)
{
	DIR dir;
	FILINFO fno;
	FRESULT res;
	int err = 0;

	root_baseline_valid = false;
	root_baseline_count = 0;
	res = f_mount(&fat_fs, "RAM:", 1);
	if (res != FR_OK) {
		return -EIO;
	}

	res = f_opendir(&dir, "RAM:");
	if (res != FR_OK) {
		f_mount(NULL, "RAM:", 0);
		return -EIO;
	}

	while (1) {
		res = f_readdir(&dir, &fno);
		if (res != FR_OK) {
			err = -EIO;
			break;
		}
		if (fno.fname[0] == '\0') {
			break;
		}
		if (fno.fattrib & AM_DIR) {
			continue; /* only regular files carry a useful mtime */
		}
		if (root_baseline_count >= ROOT_SNAPSHOT_MAX) {
			err = -ENOSPC;
			break;
		}
		root_signature_from_info(&root_baseline[root_baseline_count++],
					 &fno);
	}
	if (f_closedir(&dir) != FR_OK && err == 0) {
		err = -EIO;
	}
	f_mount(NULL, "RAM:", 0);

	if (err == 0) {
		root_baseline_valid = true;
	}
	return err;
}

static bool baseline_has(const struct root_signature *sig)
{
	size_t i;

	for (i = 0; i < root_baseline_count; i++) {
		const struct root_signature *old = &root_baseline[i];

		if (old->name_hash == sig->name_hash &&
		    old->fat_stamp == sig->fat_stamp && old->size == sig->size) {
			return true;
		}
	}
	return false;
}

int config_fs_latest_session_change(struct config_fs_change *change)
{
	DIR dir;
	FILINFO fno;
	FRESULT res;
	struct root_signature sig;
	uint64_t mtime;
	bool found = false;
	int err = 0;

	if (!root_baseline_valid) {
		return -EINVAL;
	}
	res = f_mount(&fat_fs, "RAM:", 1);
	if (res != FR_OK) {
		return -EIO;
	}
	res = f_opendir(&dir, "RAM:");
	if (res != FR_OK) {
		f_mount(NULL, "RAM:", 0);
		return -EIO;
	}

	while (1) {
		res = f_readdir(&dir, &fno);
		if (res != FR_OK) {
			err = -EIO;
			break;
		}
		if (fno.fname[0] == '\0') {
			break;
		}
		if (fno.fattrib & AM_DIR) {
			continue;
		}
		root_signature_from_info(&sig, &fno);
		if (baseline_has(&sig)) {
			continue;
		}
		mtime = fat_time_to_unix(fno.fdate, fno.ftime);
		if (!found || mtime > change->mtime ||
		    (mtime == change->mtime && sig.name_hash > change->name_hash)) {
			memset(change, 0, sizeof(*change));
			change->mtime = mtime;
			change->name_hash = sig.name_hash;
			change->size = sig.size;
			strncpy(change->name, fno.fname, sizeof(change->name) - 1);
			found = true;
		}
	}
	if (f_closedir(&dir) != FR_OK && err == 0) {
		err = -EIO;
	}
	f_mount(NULL, "RAM:", 0);

	if (err != 0) {
		return err;
	}
	return found ? 0 : -ENOENT;
}

int config_fs_wipe(void)
{
	uint32_t sector_count;
	FRESULT res;
	int err;

	dbg_disk_wipe_count++;
	dbg_disk_wipe_err = 0;

	root_baseline_valid = false;
	root_baseline_count = 0;
	config_fs_stable_reset();
	secure_zero(root_baseline, sizeof(root_baseline));
	secure_zero(read_buf, sizeof(read_buf));

	/* No filesystem operation may race the raw physical erase. */
	res = f_mount(NULL, "RAM:", 0);
	if (res != FR_OK) {
		dbg_disk_wipe_err = -EIO;
		return -EIO;
	}

	err = disk_access_ioctl("RAM", DISK_IOCTL_GET_SECTOR_COUNT,
				&sector_count);
	if (err != 0 || sector_count == 0) {
		dbg_disk_wipe_err = err != 0 ? err : -EINVAL;
		return -EIO;
	}
	err = disk_access_erase("RAM", 0, sector_count,
				DISK_ACCESS_ERASE_PHYSICAL);
	if (err != 0) {
		dbg_disk_wipe_err = err;
		return -EIO;
	}
	err = fs_mkfs(FS_FATFS, (uintptr_t)"RAM:", NULL, 0);
	dbg_disk_wipe_err = err;
	return err == 0 ? 0 : -EIO;
}

/* Write config.ini from a NUL-terminated buffer, overwriting any previous
 * file. Used to dump the current config for the host to read/edit. */
int config_fs_write_config(const char *text)
{
	FIL fp;
	FRESULT res;
	FRESULT close_res;
	FRESULT unmount_res;
	UINT bw;
	size_t len = strlen(text);

	res = f_mount(&fat_fs, "RAM:", 1);
	if (res != FR_OK) {
		return -EIO;
	}

	res = f_open(&fp, "RAM:/config.ini", FA_WRITE | FA_CREATE_ALWAYS);
	if (res != FR_OK) {
		f_mount(NULL, "RAM:", 0);
		return -EIO;
	}

	res = f_write(&fp, text, len, &bw);
	close_res = f_close(&fp);
	unmount_res = f_mount(NULL, "RAM:", 0);

	if (res != FR_OK || bw != len || close_res != FR_OK ||
	    unmount_res != FR_OK) {
		return -EIO;
	}
	return 0;
}
/* Write a diagnostic message to log.txt on the RAM disk (readable by the host
 * on the next plug). Overwrites any previous log. */
int config_fs_log(const char *msg)
{
	FIL fp;
	FRESULT res;
	FRESULT close_res;
	FRESULT unmount_res;
	UINT bw;
	size_t len = strlen(msg);

	res = f_mount(&fat_fs, "RAM:", 1);
	if (res != FR_OK) {
		return -EIO;
	}

	res = f_open(&fp, "RAM:/log.txt", FA_WRITE | FA_CREATE_ALWAYS);
	if (res != FR_OK) {
		f_mount(NULL, "RAM:", 0);
		return -EIO;
	}

	res = f_write(&fp, msg, len, &bw);
	close_res = f_close(&fp);
	unmount_res = f_mount(NULL, "RAM:", 0);

	if (res != FR_OK || bw != len || close_res != FR_OK ||
	    unmount_res != FR_OK) {
		return -EIO;
	}
	return 0;
}

int config_fs_selftest(void)
{
	/* 2020-01-01 00:00:00 = 1577836800 */
	if (fat_time_to_unix(((2020 - 1980) << 9) | (1 << 5) | 1, 0) !=
	    1577836800ULL) {
		return -1;
	}
	/* 2024-02-29 12:34:56 = 1709210096 */
	if (fat_time_to_unix(((2024 - 1980) << 9) | (2 << 5) | 29,
			     (12 << 11) | (34 << 5) | (56 / 2)) !=
	    1709210096ULL) {
		return -1;
	}
	/* 2026-09-03 10:00:00 = 1788429600 */
	if (fat_time_to_unix(((2026 - 1980) << 9) | (9 << 5) | 3,
			     (10 << 11)) != 1788429600ULL) {
		return -1;
	}
	return 0;
}
