
/*
 * drivers/video/tegra/dc/sor.h
 *
 * Copyright (c) 2011-2013, NVIDIA Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __DRIVERS_VIDEO_TEGRA_DC_SOR_H__
#define __DRIVERS_VIDEO_TEGRA_DC_SOR_H__

enum {
	training_pattern_disabled	= 0,
	training_pattern_1		= 1,
	training_pattern_2		= 2,
	training_pattern_3		= 3,
	training_pattern_none		= 0xff
};

enum tegra_dc_sor_protocol {
	SOR_DP,
	SOR_LVDS,
};

#define SOR_LINK_SPEED_G1_62	6
#define SOR_LINK_SPEED_G2_7	10
#define SOR_LINK_SPEED_G5_4	20
#define SOR_LINK_SPEED_LVDS	7

struct tegra_dc_dp_link_config {
	bool	is_valid;

	/* Supported configuration */
	u8	max_link_bw;
	u8	max_lane_count;
	bool	downspread;
	bool	support_enhanced_framing;
	u32	bits_per_pixel;
	bool	alt_scramber_reset_cap; /* true for eDP */
	bool	only_enhanced_framing;	/* enhanced_frame_en ignored */

	/* Actual configuration */
	u8	link_bw;
	u8	lane_count;
	bool	enhanced_framing;
	bool	scramble_ena;

	u32	activepolarity;
	u32	active_count;
	u32	tu_size;
	u32	active_frac;
	u32	watermark;

	s32	hblank_sym;
	s32	vblank_sym;

	/* Training data */
	u32	drive_current;
	u32     preemphasis;
	u32	postcursor;
};


struct tegra_dc_sor_data {
	struct tegra_dc	*dc;

	void __iomem	*base;
	void __iomem	*pmc_base;
	struct resource	*base_res;
	struct clk	*sor_clk;
	struct clk	*sor_clk_safe;
	struct clk	*sor_clk_edp;

	u8					 portnum;	/* 0 or 1 */
	const struct tegra_dc_dp_link_config	*link_cfg;

	bool   power_is_up;
};

#define TEGRA_SOR_TIMEOUT_MS		1000
#define TEGRA_SOR_ATTACH_TIMEOUT_MS	1000
#define TEGRA_DC_POLL_TIMEOUT_MS	50

#define CHECK_RET(x)			\
	do {				\
		ret = (x);		\
		if (ret != 0)		\
			return ret;	\
	} while (0)


struct tegra_dc_sor_data *tegra_dc_sor_init(struct tegra_dc *dc,
	const struct tegra_dc_dp_link_config *cfg);

void tegra_dc_sor_destroy(struct tegra_dc_sor_data *sor);
void tegra_dc_sor_enable_dp(struct tegra_dc_sor_data *sor);
void tegra_dc_sor_attach(struct tegra_dc_sor_data *sor);
void tegra_dc_sor_enable_lvds(struct tegra_dc_sor_data *sor,
	bool balanced, bool conforming);
void tegra_dc_sor_disable(struct tegra_dc_sor_data *sor, bool is_lvds);

void tegra_dc_sor_set_internal_panel(struct tegra_dc_sor_data *sor,
	bool is_int);
void tegra_dc_sor_read_link_config(struct tegra_dc_sor_data *sor,
	u8 *link_bw, u8 *lane_count);
void tegra_dc_sor_set_link_bandwidth(struct tegra_dc_sor_data *sor,
	u8 link_bw);
void tegra_dc_sor_set_lane_count(struct tegra_dc_sor_data *sor, u8 lane_count);
void tegra_dc_sor_set_panel_power(struct tegra_dc_sor_data *sor,
	bool power_up);
void tegra_dc_sor_set_pwm(struct tegra_dc_sor_data *sor, u32 pwm_div,
	u32 pwm_dutycycle, u32 pwm_clksrc);
void tegra_dc_sor_set_dp_lanedata(struct tegra_dc_sor_data *sor,
	u32 lane, u32 pre_emphasis, u32 drive_current, u32 tx_pu);
void tegra_dc_sor_set_dp_linkctl(struct tegra_dc_sor_data *sor, bool ena,
	u8 training_pattern, const struct tegra_dc_dp_link_config *cfg);
void tegra_dc_sor_setup_clk(struct tegra_dc_sor_data *sor, struct clk *clk,
	bool is_lvds);
void tegra_dc_sor_set_lane_parm(struct tegra_dc_sor_data *sor,
	const struct tegra_dc_dp_link_config *cfg);
int tegra_dc_sor_set_power_state(struct tegra_dc_sor_data *sor,
	int pu_pd);
void tegra_dc_sor_power_down_unused_lanes(struct tegra_dc_sor_data *sor);
void tegra_dc_sor_set_voltage_swing(struct tegra_dc_sor_data *sor,
	u32 cust_drive_current, u32 cust_preemphasis);

void tegra_dc_detach(struct tegra_dc_sor_data *sor);
#endif
