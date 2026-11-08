/* Config file reading from the FAT volume — public API */

#ifndef CONFIG_FS_H
#define CONFIG_FS_H

#include <stdint.h>
#include "config.h"

#define CONFIG_FS_TEXT_SIZE       8192
#define CONFIG_FS_ANCHOR_NAME_MAX 64

/* A host-written regular file that differs from the root-directory snapshot
 * taken before USB enumeration. The timestamp is in the host-local FAT time
 * scale; timezone conversion belongs at TOTP generation time. */
struct config_fs_change {
	uint64_t mtime;
	uint64_t name_hash;
	uint32_t size;
	char name[CONFIG_FS_ANCHOR_NAME_MAX];
};

/* Read config.ini from the RAM disk FAT volume.
 * On success fills @cfg. Returns 0 on success, -ENOENT if the file is
 * absent, -EFBIG if it is at least CONFIG_FS_TEXT_SIZE bytes, -EINVAL if it
 * fails to parse, -EAGAIN if it changes during the read, or -EIO on a media
 * error. */
int config_fs_read(struct totp_config *cfg);

/* Read config.ini only after the same {existence, mtime, size, content hash}
 * has been observed twice. The first/new observation returns -EAGAIN; a
 * confirmed absence returns -ENOENT. */
int config_fs_read_stable(struct totp_config *cfg);
void config_fs_stable_reset(void);

/* Capture the device-generated root directory before USB enumeration, then
 * find the newest regular file that is new or changed relative to it. */
int config_fs_capture_baseline(void);
int config_fs_latest_session_change(struct config_fs_change *change);

/* Unmount, erase every physical RAM-disk sector, and create a fresh FAT
 * filesystem. All file contents and in-module read buffers are cleared. */
int config_fs_wipe(void);

/* Write the given text (NUL-terminated) to config.ini on the RAM disk,
 * overwriting any previous file. Returns 0 on success, -EIO on a media error. */
int config_fs_write_config(const char *text);

/* Write a diagnostic message to log.txt on the RAM disk. */
int config_fs_log(const char *msg);

/* Convert FAT date/time (fdate/ftime) to Unix seconds (local time). */
uint64_t fat_time_to_unix(uint16_t fdate, uint16_t ftime);

/* Self-test. Returns 0 on success. */
int config_fs_selftest(void);

#endif /* CONFIG_FS_H */
