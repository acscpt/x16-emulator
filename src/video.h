// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#ifndef _VIDEO_H_
#define _VIDEO_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <SDL.h>
#include "glue.h"

bool video_init(int window_scale, float screen_x_scale, char *quality, bool fullscreen, float opacity);
bool video_is_alive(void);
void video_reset(void);
bool video_step(float mhz, float steps, bool midline);
bool video_update(void);
void video_end(void);
bool video_get_irq_out(void);
void video_save(SDL_RWops *f);

// Write the current composited framebuffer to a PNG. path may be NULL or empty
// for a timestamped default in the working directory; the resolved path is
// copied into out_path. Returns true on success.
bool video_save_screenshot(const char *path, char *out_path, size_t out_len);

uint8_t video_read(uint8_t reg, bool debugOn);
void video_write(uint8_t reg, uint8_t value);
void video_update_title(const char* window_title);

uint8_t via1_read(uint8_t reg, bool debug);
void via1_write(uint8_t reg, uint8_t value);

// For debugging purposes only:
uint8_t video_space_read(uint32_t address);
void video_space_write(uint32_t address, uint8_t value);

bool video_is_tilemap_address(int addr);
bool video_is_tiledata_address(int addr);
bool video_is_special_address(int addr);

uint32_t video_get_address(uint8_t sel);
uint32_t video_get_fx_accum(void);
uint8_t video_get_dc_value(uint8_t reg);

#endif
