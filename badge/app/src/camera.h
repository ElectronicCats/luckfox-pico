/*
 * File: app/src/camera.h
 * Purpose: Camera pipeline lifecycle and capture interface.
 */

#ifndef BADGE_CAMERA_H
#define BADGE_CAMERA_H

typedef struct camera_ctx camera_ctx_t;

int camera_init(camera_ctx_t **out, int width, int height, int quality);
long camera_last_frame_time(const camera_ctx_t *ctx);
int camera_warmup(camera_ctx_t *ctx, int timeout_ms);
int camera_poll(camera_ctx_t *ctx);
int camera_capture(camera_ctx_t *ctx, const char *path, int timeout_ms, int retries);
int camera_preview(camera_ctx_t *ctx);
void camera_deinit(camera_ctx_t *ctx);

#endif
