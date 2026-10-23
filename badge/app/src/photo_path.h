/*
 * File: app/src/photo_path.h
 * Purpose: Photo path, staging directory, and directory helper interface.
 */

#ifndef BADGE_PHOTO_PATH_H
#define BADGE_PHOTO_PATH_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Keep in-progress JPEGs outside the MTP storage. uMTP-Responder watches the
 * storage with inotify; seeing a .tmp file that disappears after rename can
 * leave a nameless object in the host's MTP cache (displayed as "<none>").
 */
#define PHOTO_STAGING_DIR "/userdata/.badge_photo_staging"

int photo_path_format(const char *dir, time_t epoch, unsigned int seq,
                      char *out, size_t outsz);
int photo_path_make_temp(const char *path, const char *staging_dir,
                         char *out, size_t outsz);
int photo_path_next(const char *dir, char *out, size_t outsz);
int photo_path_ensure_dir(const char *dir);
int photo_path_clean_staging(const char *staging_dir);

#endif
