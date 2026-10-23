/*
 * File: app/src/overlay.h
 * Purpose: Hardware RGN overlay interface for badge_photo.
 */

#ifndef BADGE_OVERLAY_H
#define BADGE_OVERLAY_H

int overlay_init(void);
void overlay_poll(void);
void overlay_refresh(void);
void overlay_deinit(void);

#endif
