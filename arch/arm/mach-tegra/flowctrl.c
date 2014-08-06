/*
 * arch/arm/mach-tegra/flowctrl.c
 *
 * functions and macros to control the flowcontroller
 *
 * Copyright (c) 2010-2012, NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/cpumask.h>
#include <linux/tegra-soc.h>

#include "flowctrl.h"
#include "iomap.h"

#define FLOW_CTRL_RAM_REPAIR	0x40
#define FLOW_CTRL_RAM_REPAIR_BYPASS_EN	BIT(2)

static u8 flowctrl_offset_halt_cpu[] = {
	FLOW_CTRL_HALT_CPU0_EVENTS,
	FLOW_CTRL_HALT_CPU1_EVENTS,
	FLOW_CTRL_HALT_CPU1_EVENTS + 8,
	FLOW_CTRL_HALT_CPU1_EVENTS + 16,
};

static u8 flowctrl_offset_cpu_csr[] = {
	FLOW_CTRL_CPU0_CSR,
	FLOW_CTRL_CPU1_CSR,
	FLOW_CTRL_CPU1_CSR + 8,
	FLOW_CTRL_CPU1_CSR + 16,
};

static void flowctrl_update(u8 offset, u32 value)
{
	void __iomem *addr = IO_ADDRESS(TEGRA_FLOW_CTRL_BASE) + offset;

	writel(value, addr);

	/* ensure the update has reached the flow controller */
	wmb();
	readl_relaxed(addr);
}

static u32 flowctrl_read(u8 offset)
{
	void __iomem *addr = IO_ADDRESS(TEGRA_FLOW_CTRL_BASE) + offset;

	return readl(addr);
}

void flowctrl_ram_repair_enable(void)
{
	u32 reg;
	reg = flowctrl_read(FLOW_CTRL_RAM_REPAIR);
	reg &= ~FLOW_CTRL_RAM_REPAIR_BYPASS_EN;
	flowctrl_update(FLOW_CTRL_RAM_REPAIR, reg);
}

u32 flowctrl_read_cpu_csr(unsigned int cpuid)
{
	return flowctrl_read(flowctrl_offset_cpu_csr[cpuid]);
}

void flowctrl_write_cpu_csr(unsigned int cpuid, u32 value)
{
	return flowctrl_update(flowctrl_offset_cpu_csr[cpuid], value);
}

void flowctrl_write_cpu_halt(unsigned int cpuid, u32 value)
{
	return flowctrl_update(flowctrl_offset_halt_cpu[cpuid], value);
}

void flowctrl_cpu_suspend_enter(unsigned int cpuid)
{
	unsigned int reg;
	int i;

	reg = flowctrl_read_cpu_csr(cpuid);
	switch (tegra_chip_id) {
	case TEGRA20:
		/* clear wfe bitmap */
		reg &= ~TEGRA20_FLOW_CTRL_CSR_WFE_BITMAP;
		/* clear wfi bitmap */
		reg &= ~TEGRA20_FLOW_CTRL_CSR_WFI_BITMAP;
		/* pwr gating on wfe */
		reg |= TEGRA20_FLOW_CTRL_CSR_WFE_CPU0 << cpuid;
		break;
	case TEGRA30:
	case TEGRA114:
	case TEGRA124:
		/* clear wfe bitmap */
		reg &= ~TEGRA30_FLOW_CTRL_CSR_WFE_BITMAP;
		/* clear wfi bitmap */
		reg &= ~TEGRA30_FLOW_CTRL_CSR_WFI_BITMAP;
		/* pwr gating on wfi */
		reg |= TEGRA30_FLOW_CTRL_CSR_WFI_CPU0 << cpuid;
		break;
	}
	reg |= FLOW_CTRL_CSR_INTR_FLAG;			/* clear intr flag */
	reg |= FLOW_CTRL_CSR_EVENT_FLAG;		/* clear event flag */
	reg |= FLOW_CTRL_CSR_ENABLE;			/* pwr gating */
	flowctrl_write_cpu_csr(cpuid, reg);

	for (i = 0; i < num_possible_cpus(); i++) {
		if (i == cpuid)
			continue;
		reg = flowctrl_read_cpu_csr(i);
		reg |= FLOW_CTRL_CSR_EVENT_FLAG;
		reg |= FLOW_CTRL_CSR_INTR_FLAG;
		flowctrl_write_cpu_csr(i, reg);
	}
}

void flowctrl_cpu_suspend_exit(unsigned int cpuid)
{
	unsigned int reg;

	/* Disable powergating via flow controller for CPU0 */
	reg = flowctrl_read_cpu_csr(cpuid);
	switch (tegra_chip_id) {
	case TEGRA20:
		/* clear wfe bitmap */
		reg &= ~TEGRA20_FLOW_CTRL_CSR_WFE_BITMAP;
		/* clear wfi bitmap */
		reg &= ~TEGRA20_FLOW_CTRL_CSR_WFI_BITMAP;
		break;
	case TEGRA30:
	case TEGRA114:
	case TEGRA124:
		/* clear wfe bitmap */
		reg &= ~TEGRA30_FLOW_CTRL_CSR_WFE_BITMAP;
		/* clear wfi bitmap */
		reg &= ~TEGRA30_FLOW_CTRL_CSR_WFI_BITMAP;
		if (tegra_chip_id != TEGRA30)
			flowctrl_ram_repair_enable();
		break;
	}
	reg &= ~FLOW_CTRL_CSR_ENABLE;			/* clear enable */
	reg |= FLOW_CTRL_CSR_INTR_FLAG;			/* clear intr */
	reg |= FLOW_CTRL_CSR_EVENT_FLAG;		/* clear event */
	flowctrl_write_cpu_csr(cpuid, reg);
}

void flowctrl_cpu_rail_enable(void)
{
	u32 reg;

	reg = flowctrl_read(FLOW_CTLR_CPU_PWR_CSR);
	reg |= FLOW_CTRL_CPU_PWR_CSR_RAIL_ENABLE;
	flowctrl_update(FLOW_CTLR_CPU_PWR_CSR, reg);
}

void __init tegra_flowctrl_ram_repair_init(void)
{
	flowctrl_ram_repair_enable();
}
