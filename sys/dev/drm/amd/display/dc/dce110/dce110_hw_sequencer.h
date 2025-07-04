/*
* Copyright 2012-15 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#ifndef __DC_HWSS_DCE110_H__
#define __DC_HWSS_DCE110_H__

#include "core_types.h"

#define GAMMA_HW_POINTS_NUM 256
struct dc;
struct dc_state;
struct dm_pp_display_configuration;

void dce110_hw_sequencer_construct(struct dc *dc);

enum dc_status dce110_apply_ctx_to_hw(
		struct dc *dc,
		struct dc_state *context);



void dce110_enable_stream(struct pipe_ctx *pipe_ctx);

void dce110_disable_stream(struct pipe_ctx *pipe_ctx, int option);

void dce110_unblank_stream(struct pipe_ctx *pipe_ctx,
		struct dc_link_settings *link_settings);

void dce110_blank_stream(struct pipe_ctx *pipe_ctx);

void dce110_enable_audio_stream(struct pipe_ctx *pipe_ctx);
void dce110_disable_audio_stream(struct pipe_ctx *pipe_ctx, int option);

void dce110_update_info_frame(struct pipe_ctx *pipe_ctx);

void dce110_set_avmute(struct pipe_ctx *pipe_ctx, bool enable);
void dce110_enable_accelerated_mode(struct dc *dc, struct dc_state *context);

void dce110_power_down(struct dc *dc);

void dce110_set_safe_displaymarks(
		struct resource_context *res_ctx,
		const struct resource_pool *pool);

void dce110_fill_display_configs(
	const struct dc_state *context,
	struct dm_pp_display_configuration *pp_display_cfg);

uint32_t dce110_get_min_vblank_time_us(const struct dc_state *context);

#if 0 /* redundant */
void dp_receiver_power_ctrl(struct dc_link *link, bool on);
#endif

void hwss_edp_power_control(
		struct dc_link *link,
		bool power_up);

void hwss_edp_backlight_control(
	struct dc_link *link,
	bool enable);

void hwss_edp_wait_for_hpd_ready(
		struct dc_link *link,
		bool power_up);

#endif /* __DC_HWSS_DCE110_H__ */

