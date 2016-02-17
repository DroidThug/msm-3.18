/*************************************************************************/ /*!
@File
@Title          System Configuration
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    System Configuration functions
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>

#include "physheap.h"
#include "pvrsrv_device.h"
#include "rgxdevice.h"
#include "syscommon.h"

#include "mt8173_mfgsys.h"

#define SYS_RGX_ACTIVE_POWER_LATENCY_MS 100
#define RGX_HW_SYSTEM_NAME "RGX HW"
#define RGX_HW_CORE_CLOCK_SPEED 395000000

static RGX_TIMING_INFORMATION	gsRGXTimingInfo;
static RGX_DATA			gsRGXData;
static PVRSRV_DEVICE_CONFIG	gsDevice;
static PVRSRV_SYSTEM_CONFIG	gsSysConfig;

static PHYS_HEAP_FUNCTIONS	gsPhysHeapFuncs;
static PHYS_HEAP_CONFIG		gsPhysHeapConfig;

static IMG_UINT32 gauiBIFTilingHeapXStrides[RGXFWIF_NUM_BIF_TILING_CONFIGS] = {
	0, /* BIF tiling heap 1 x-stride */
	1, /* BIF tiling heap 2 x-stride */
	2, /* BIF tiling heap 3 x-stride */
	3  /* BIF tiling heap 4 x-stride */
};

/*
 * CPU to Device physical address translation
 */
static
void UMAPhysHeapCpuPAddrToDevPAddr(IMG_HANDLE hPrivData,
				   IMG_UINT32 ui32NumOfAddr,
				   IMG_DEV_PHYADDR *psDevPAddr,
				   IMG_CPU_PHYADDR *psCpuPAddr)
{
	PVR_UNREFERENCED_PARAMETER(hPrivData);

	/* Optimise common case */
	psDevPAddr[0].uiAddr = psCpuPAddr[0].uiAddr;
	if (ui32NumOfAddr > 1) {
		IMG_UINT32 ui32Idx;
		for (ui32Idx = 1; ui32Idx < ui32NumOfAddr; ++ui32Idx)
			psDevPAddr[ui32Idx].uiAddr = psCpuPAddr[ui32Idx].uiAddr;
	}
}

/*
 * Device to CPU physical address translation
 */
static
void UMAPhysHeapDevPAddrToCpuPAddr(IMG_HANDLE hPrivData,
				   IMG_UINT32 ui32NumOfAddr,
				   IMG_CPU_PHYADDR *psCpuPAddr,
				   IMG_DEV_PHYADDR *psDevPAddr)
{
	PVR_UNREFERENCED_PARAMETER(hPrivData);

	/* Optimise common case */
	psCpuPAddr[0].uiAddr = psDevPAddr[0].uiAddr;
	if (ui32NumOfAddr > 1) {
		IMG_UINT32 ui32Idx;
		for (ui32Idx = 1; ui32Idx < ui32NumOfAddr; ++ui32Idx)
			psCpuPAddr[ui32Idx].uiAddr = psDevPAddr[ui32Idx].uiAddr;
	}
}

static PVRSRV_ERROR MTKSysDevPrePowerState(
		PVRSRV_DEV_POWER_STATE eNewPowerState,
		PVRSRV_DEV_POWER_STATE eCurrentPowerState,
		IMG_BOOL bForced)
{
	/* HACK: IMG should pass context; for now access global. */
	struct mtk_mfg *mfg = gsDevice.hSysData;

	mtk_mfg_debug("MTKSysDevPrePowerState (%d->%d), bForced = %d\n",
		      eCurrentPowerState, eNewPowerState, bForced);

	mutex_lock(&mfg->set_power_state);

	if ((PVRSRV_DEV_POWER_STATE_OFF == eNewPowerState) &&
	    (PVRSRV_DEV_POWER_STATE_ON == eCurrentPowerState))
		mtk_mfg_disable(mfg);

	mutex_unlock(&mfg->set_power_state);
	return PVRSRV_OK;
}

static PVRSRV_ERROR MTKSysDevPostPowerState(
		PVRSRV_DEV_POWER_STATE eNewPowerState,
		PVRSRV_DEV_POWER_STATE eCurrentPowerState,
		IMG_BOOL bForced)
{
	/* HACK: IMG should pass context; for now access global. */
	struct mtk_mfg *mfg = gsDevice.hSysData;
	PVRSRV_ERROR ret;

	mtk_mfg_debug("MTKSysDevPostPowerState (%d->%d)\n",
		      eCurrentPowerState, eNewPowerState);

	mutex_lock(&mfg->set_power_state);

	if ((PVRSRV_DEV_POWER_STATE_ON == eNewPowerState) &&
	    (PVRSRV_DEV_POWER_STATE_OFF == eCurrentPowerState)) {
		if (mtk_mfg_enable(mfg)) {
			ret = PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
			goto done;
		}
	}

	ret = PVRSRV_OK;
done:
	mutex_unlock(&mfg->set_power_state);

	return ret;
}

static void SetFrequency(IMG_UINT32 freq)
{
	struct mtk_mfg *mfg = gsDevice.hSysData;

	/* freq is in kHz */
	mtk_mfg_freq_set(mfg, freq * 1000);
}

static void SetVoltage(IMG_UINT32 volt)
{
	struct mtk_mfg *mfg = gsDevice.hSysData;

	mtk_mfg_volt_set(mfg, volt);
}

static int SetupDVFSInfo(struct device *dev, PVRSRV_DVFS *hDVFS)
{
	IMG_OPP *opp_table;
	IMG_DVFS_DEVICE_CFG *img_dvfs_cfg;
	int i;
	int count;
	unsigned long freq;

	/* Start RCU read-side critical section to access device opp_list. */
	rcu_read_lock();
	count = dev_pm_opp_get_opp_count(dev);
	if (count < 0) {
		dev_err(dev, "Could not fetch OPP count, %d\n", count);
		rcu_read_unlock();
		return count;
	}

	opp_table = devm_kcalloc(dev, count, sizeof(*opp_table), GFP_KERNEL);
	if (!opp_table) {
		rcu_read_unlock();
		return -ENOMEM;
	}

	img_dvfs_cfg = &hDVFS->sDVFSDeviceCfg;

	/*
	 * Iterate over OPP table.
	 * Iteration 0 finds "opp w/ freq >= 0 Hz".
	 * Iteration n > 0 finds "opp w/ freq >= (opp[n-1].freq + 1)".
	 */
	for (i = 0, freq = 0; i < count; i++, freq++) {
		struct dev_pm_opp *opp = dev_pm_opp_find_freq_ceil(dev, &freq);

		opp_table[i].ui32Freq = freq / 1000;
		opp_table[i].ui32Volt = dev_pm_opp_get_voltage(opp);

		dev_info(dev, "opp[%d/%d]: (%u kHz, %u uV)\n", i + 1, count,
			 opp_table[i].ui32Freq, opp_table[i].ui32Volt);
	}
	rcu_read_unlock();

	img_dvfs_cfg->bIdleReq = IMG_FALSE;
	img_dvfs_cfg->pasOPPTable = opp_table;
	img_dvfs_cfg->ui32OPPTableSize = count;
	img_dvfs_cfg->ui32FreqMin = opp_table[0].ui32Freq;
	img_dvfs_cfg->ui32FreqMax = opp_table[count - 1].ui32Freq;
	img_dvfs_cfg->pfnSetFrequency = SetFrequency;
	img_dvfs_cfg->pfnSetVoltage = SetVoltage;
	img_dvfs_cfg->ui32PollMs = MTK_DVFS_SWITCH_INTERVAL;

	hDVFS->sDVFSGovernorCfg.ui32UpThreshold = 90;
	hDVFS->sDVFSGovernorCfg.ui32DownDifferential = 10;

	return 0;
}

PVRSRV_ERROR SysCreateConfigData(PVRSRV_SYSTEM_CONFIG **ppsSysConfig, void *hDevice)
{
	struct platform_device *pDevice = hDevice;
	struct device *dev = &pDevice->dev;
	struct mtk_mfg *mfg = dev_get_platdata(dev);
	int ret;

	if (!pDevice) {
		PVR_DPF((PVR_DBG_ERROR, "pDevice = NULL!"));
		return PVRSRV_ERROR_INIT_FAILURE;
	}

	/* Setup information about physical memory heap(s) we have */
	gsPhysHeapFuncs.pfnCpuPAddrToDevPAddr = UMAPhysHeapCpuPAddrToDevPAddr;
	gsPhysHeapFuncs.pfnDevPAddrToCpuPAddr = UMAPhysHeapDevPAddrToCpuPAddr;

	gsPhysHeapConfig.ui32PhysHeapID = 0;
	gsPhysHeapConfig.pszPDumpMemspaceName = "SYSMEM";
	gsPhysHeapConfig.eType = PHYS_HEAP_TYPE_UMA;
	gsPhysHeapConfig.psMemFuncs = &gsPhysHeapFuncs;
	gsPhysHeapConfig.hPrivData = &gsSysConfig;
	gsPhysHeapConfig.sStartAddr.uiAddr = 0;
	gsPhysHeapConfig.uiSize = 0;

	gsSysConfig.pasPhysHeaps = &gsPhysHeapConfig;
	gsSysConfig.ui32PhysHeapCount = 1;
	gsSysConfig.pui32BIFTilingHeapConfigs = gauiBIFTilingHeapXStrides;
	gsSysConfig.ui32BIFTilingHeapCount = IMG_ARR_NUM_ELEMS(gauiBIFTilingHeapXStrides);

	/* Setup RGX specific timing data */
	gsRGXTimingInfo.ui32CoreClockSpeed = RGX_HW_CORE_CLOCK_SPEED;

	gsRGXTimingInfo.bEnableActivePM = IMG_TRUE;
	gsRGXTimingInfo.ui32ActivePMLatencyms = SYS_RGX_ACTIVE_POWER_LATENCY_MS;

	/* for HWAPM enable */
	gsRGXTimingInfo.bEnableRDPowIsland = IMG_TRUE;

	/* Setup RGX specific data */
	gsRGXData.psRGXTimingInfo = &gsRGXTimingInfo;

	/* Setup RGX device */
	gsDevice.eDeviceType = PVRSRV_DEVICE_TYPE_RGX;
	gsDevice.pszName = "RGX";

	gsDevice.ui32IRQ = mfg->rgx_irq;
	gsDevice.bIRQIsShared = IMG_FALSE;
	gsDevice.eIRQActiveLevel = PVRSRV_DEVICE_IRQ_ACTIVE_LOW;

	gsDevice.sRegsCpuPBase.uiAddr = mfg->rgx_start;
	gsDevice.ui32RegsSize = mfg->rgx_size;

	ret = SetupDVFSInfo(dev, &gsDevice.sDVFS);
	if (ret)
		return PVRSRV_ERROR_INIT_FAILURE;

	/* Device's physical heap IDs */
	gsDevice.aui32PhysHeapID[PVRSRV_DEVICE_PHYS_HEAP_GPU_LOCAL] = 0;
	gsDevice.aui32PhysHeapID[PVRSRV_DEVICE_PHYS_HEAP_CPU_LOCAL] = 0;
	gsDevice.aui32PhysHeapID[PVRSRV_DEVICE_PHYS_HEAP_FW_LOCAL] = 0;

	/* power management on  HW system */
	gsDevice.pfnPrePowerState = MTKSysDevPrePowerState;
	gsDevice.pfnPostPowerState = MTKSysDevPostPowerState;

	/* clock frequency */
	gsDevice.pfnClockFreqGet = NULL;

	/* interrupt handled */
	gsDevice.pfnInterruptHandled = NULL;

	gsDevice.hDevData = &gsRGXData;
	gsDevice.hSysData = mfg;

	/* Setup system config */
	gsSysConfig.pszSystemName = RGX_HW_SYSTEM_NAME;
	gsSysConfig.uiDeviceCount = 1;
	gsSysConfig.pasDevices = &gsDevice;

	/* cache snooping */
	gsSysConfig.eCacheSnoopingMode = 0;

	gsSysConfig.uiSysFlags = 0;

	/* Setup other system specific stuff */
	*ppsSysConfig = &gsSysConfig;

	return PVRSRV_OK;
}

void SysDestroyConfigData(PVRSRV_SYSTEM_CONFIG *psSysConfig)
{
	PVR_UNREFERENCED_PARAMETER(psSysConfig);
}

PVRSRV_ERROR SysDebugInfo(PVRSRV_SYSTEM_CONFIG *psSysConfig,
			  DUMPDEBUG_PRINTF_FUNC *pfnDumpDebugPrintf)
{
	PVR_UNREFERENCED_PARAMETER(psSysConfig);
	PVR_UNREFERENCED_PARAMETER(pfnDumpDebugPrintf);

	return PVRSRV_OK;
}

SYS_PHYS_ADDRESS_MASK SysDevicePhysAddressMask(void)
{
	return SYS_PHYS_ADDRESS_64_BIT;
}
