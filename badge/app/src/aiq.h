/*
 * File: app/src/aiq.h
 * Purpose: Public AIQ lifecycle interface used by badge_photo.
 */

#ifndef BADGE_AIQ_H
#define BADGE_AIQ_H

#include "meta.h"

typedef struct aiq_handle aiq_handle_t;

int aiq_init(aiq_handle_t **out, badge_meta_t *meta, int color_mode,
             int cam_hdr, int fast_handoff);
void aiq_deinit(aiq_handle_t *h);
int aiq_restart(aiq_handle_t *h);

#endif
