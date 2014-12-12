/*
 * Copyright (c) 2013-2014 TRUSTONIC LIMITED
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef _MC_PLATFORM_H_
#define _MC_PLATFORM_H_

/* MobiCore Interrupt for Qualcomm */
#define MC_INTR_SSIQ						280

/* Use SMC for fastcalls */
#define MC_SMC_FASTCALL

/*--------------- Implementation -------------- */
#if defined(CONFIG_ARCH_APQ8084) || defined(CONFIG_ARCH_MSM8916) || \
defined(CONFIG_ARCH_MSM8909)
#include <soc/qcom/scm.h>
#else
#include <mach/scm.h>
#endif
/* from following file */
#define SCM_SVC_MOBICORE		250
#define SCM_CMD_MOBICORE		1

extern int scm_call(u32 svc_id, u32 cmd_id, const void *cmd_buf, size_t cmd_len,
		    void *resp_buf, size_t resp_len);

static inline int smc_fastcall(void *fc_generic, size_t size)
{
	return scm_call(SCM_SVC_MOBICORE, SCM_CMD_MOBICORE,
			fc_generic, size,
			fc_generic, size);
}

/* Enable mobicore mem traces */
#define MC_MEM_TRACES

/* Enable the use of vm_unamp instead of the deprecated do_munmap
 * and other 3.7 features
 */
#ifndef CONFIG_ARCH_MSM8960
#define MC_VM_UNMAP
#endif

#ifndef CONFIG_ARCH_MSM8960
/* Perform clock enable/disable */
#define MC_CRYPTO_CLOCK_MANAGEMENT
#endif

#endif /* _MC_PLATFORM_H_ */
