/* Copyright (c) 2007-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/iommu.h>
#include <linux/qcom_iommu.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/clk/msm-clk.h>

#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/of_platform.h>
#include <linux/msm_dma_iommu_mapping.h>

#include <asm/dma-iommu.h>
#include "soc/qcom/secure_buffer.h"

#include "mdss.h"
#include "mdss_mdp.h"
#include "mdss_smmu.h"

static DEFINE_MUTEX(mdp_iommu_lock);

void mdss_iommu_lock(void)
{
	mutex_lock(&mdp_iommu_lock);
}

void mdss_iommu_unlock(void)
{
	mutex_unlock(&mdp_iommu_lock);
}

static int mdss_smmu_util_parse_dt_clock(struct platform_device *pdev,
		struct dss_module_power *mp)
{
	u32 i = 0, rc = 0;
	const char *clock_name;
	u32 clock_rate;

	mp->num_clk = of_property_count_strings(pdev->dev.of_node,
			"clock-names");
	if (mp->num_clk <= 0) {
		pr_err("clocks are not defined\n");
		goto clk_err;
	}

	mp->clk_config = devm_kzalloc(&pdev->dev,
			sizeof(struct dss_clk) * mp->num_clk, GFP_KERNEL);
	if (!mp->clk_config) {
		pr_err("clock configuration allocation failed\n");
		rc = -ENOMEM;
		mp->num_clk = 0;
		goto clk_err;
	}

	for (i = 0; i < mp->num_clk; i++) {
		of_property_read_string_index(pdev->dev.of_node, "clock-names",
							i, &clock_name);
		strlcpy(mp->clk_config[i].clk_name, clock_name,
				sizeof(mp->clk_config[i].clk_name));

		of_property_read_u32_index(pdev->dev.of_node, "clock-rate",
							i, &clock_rate);
		mp->clk_config[i].rate = clock_rate;

		if (!clock_rate)
			mp->clk_config[i].type = DSS_CLK_AHB;
		else
			mp->clk_config[i].type = DSS_CLK_PCLK;
	}

clk_err:
	return rc;
}

static int mdss_smmu_clk_register(struct platform_device *pdev,
		struct dss_module_power *mp)
{
	int i, ret;
	struct clk *clk;

	ret = mdss_smmu_util_parse_dt_clock(pdev, mp);
	if (ret) {
		pr_err("unable to parse clocks\n");
		return -EINVAL;
	}

	for (i = 0; i < mp->num_clk; i++) {
		clk = devm_clk_get(&pdev->dev,
				mp->clk_config[i].clk_name);
		if (IS_ERR(clk)) {
			pr_err("unable to get clk: %s\n",
					mp->clk_config[i].clk_name);
			return PTR_ERR(clk);
		}
		mp->clk_config[i].clk = clk;
	}
	return 0;
}

static int mdss_smmu_enable_power(struct mdss_smmu_client *mdss_smmu,
	bool enable)
{
	int rc = 0;
	struct dss_module_power *mp;

	if (!mdss_smmu)
		return -EINVAL;

	mp = &mdss_smmu->mp;

	if (enable) {
		rc = msm_dss_enable_vreg(mp->vreg_config, mp->num_vreg, true);
		if (rc) {
			pr_err("vreg enable failed - rc:%d\n", rc);
			goto end;
		}
		mdss_update_reg_bus_vote(mdss_smmu->reg_bus_clt,
			VOTE_INDEX_19_MHZ);
		rc = msm_dss_enable_clk(mp->clk_config, mp->num_clk, true);
		if (rc) {
			pr_err("clock enable failed - rc:%d\n", rc);
			mdss_update_reg_bus_vote(mdss_smmu->reg_bus_clt,
				VOTE_INDEX_DISABLE);
			msm_dss_enable_vreg(mp->vreg_config, mp->num_vreg,
				false);
			goto end;
		}
	} else {
		msm_dss_enable_clk(mp->clk_config, mp->num_clk, false);
		mdss_update_reg_bus_vote(mdss_smmu->reg_bus_clt,
			VOTE_INDEX_DISABLE);
		msm_dss_enable_vreg(mp->vreg_config, mp->num_vreg, false);
	}
end:
	return rc;
}

/*
 * mdss_smmu_v2_attach()
 *
 * Associates each configured VA range with the corresponding smmu context
 * bank device. Enables the clks as smmu_v2 requires voting it before the usage.
 * And iommu attach is done only once during the initial attach and it is never
 * detached as smmu v2 uses a feature called 'retention'.
 */
static int mdss_smmu_attach_v2(struct mdss_data_type *mdata)
{
	struct mdss_smmu_client *mdss_smmu;
	int i, rc = 0;

	for (i = 0; i < MDSS_IOMMU_MAX_DOMAIN; i++) {
		if (!mdss_smmu_is_valid_domain_type(mdata, i))
			continue;

		mdss_smmu = mdss_smmu_get_cb(i);
		if (mdss_smmu->dev) {
			if (!mdss_smmu->handoff_pending) {
				rc = mdss_smmu_enable_power(mdss_smmu, true);
				if (rc) {
					pr_err("power enable failed - domain:[%d] rc:%d\n",
						i, rc);
					goto err;
				}
			}
			mdss_smmu->handoff_pending = false;

			if (!mdss_smmu->domain_attached) {
				rc = arm_iommu_attach_device(mdss_smmu->dev,
						mdss_smmu->mmu_mapping);
				if (rc) {
					pr_err("iommu attach device failed for domain[%d] with err:%d\n",
						i, rc);
					mdss_smmu_enable_power(mdss_smmu,
						false);
					goto err;
				}
				mdss_smmu->domain_attached = true;
				pr_debug("iommu v2 domain[%i] attached\n", i);
			}
		} else {
			pr_err("iommu device not attached for domain[%d]\n", i);
			return -ENODEV;
		}
	}
	return 0;

err:
	for (i--; i >= 0; i--) {
		mdss_smmu = mdss_smmu_get_cb(i);
		if (mdss_smmu->dev) {
			arm_iommu_detach_device(mdss_smmu->dev);
			mdss_smmu_enable_power(mdss_smmu, false);
			mdss_smmu->domain_attached = false;
		}
	}
	return rc;
}

/*
 * mdss_smmu_v2_detach()
 *
 * Only disables the clks as it is not required to detach the iommu mapped
 * VA range from the device in smmu_v2 as explained in the mdss_smmu_v2_attach
 */
static int mdss_smmu_detach_v2(struct mdss_data_type *mdata)
{
	struct mdss_smmu_client *mdss_smmu;
	int i;

	for (i = 0; i < MDSS_IOMMU_MAX_DOMAIN; i++) {
		if (!mdss_smmu_is_valid_domain_type(mdata, i))
			continue;

		mdss_smmu = mdss_smmu_get_cb(i);
		if (mdss_smmu->dev && !mdss_smmu->handoff_pending)
			mdss_smmu_enable_power(mdss_smmu, false);
	}
	return 0;
}

static int mdss_smmu_get_domain_id_v2(u32 type)
{
	return type;
}

/*
 * mdss_smmu_dma_buf_attach_v2()
 *
 * Same as mdss_smmu_dma_buf_attach except that the device is got from
 * the configured smmu v2 context banks.
 */
static struct dma_buf_attachment *mdss_smmu_dma_buf_attach_v2(
		struct dma_buf *dma_buf, struct device *dev, int domain)
{
	struct mdss_smmu_client *mdss_smmu = mdss_smmu_get_cb(domain);
	if (!mdss_smmu) {
		pr_err("not able to get smmu context\n");
		return NULL;
	}

	return dma_buf_attach(dma_buf, mdss_smmu->dev);
}

/*
 * mdss_smmu_map_dma_buf_v2()
 *
 * Maps existing buffer (by struct scatterlist) into SMMU context bank device.
 * From which we can take the virtual address and size allocated.
 * msm_map_dma_buf is depricated with smmu v2 and it uses dma_map_sg instead
 */
static int mdss_smmu_map_dma_buf_v2(struct dma_buf *dma_buf,
		struct sg_table *table, int domain, dma_addr_t *iova,
		unsigned long *size, int dir)
{
	int rc;
	struct mdss_smmu_client *mdss_smmu = mdss_smmu_get_cb(domain);
	if (!mdss_smmu) {
		pr_err("not able to get smmu context\n");
		return -EINVAL;
	}
	ATRACE_BEGIN("map_buffer");
	rc = msm_dma_map_sg_lazy(mdss_smmu->dev, table->sgl, table->nents, dir,
		dma_buf);
	if (rc != table->nents) {
		pr_err("dma map sg failed\n");
		return -ENOMEM;
	}
	ATRACE_END("map_buffer");
	*iova = table->sgl->dma_address;
	*size = table->sgl->dma_length;
	return 0;
}

static void mdss_smmu_unmap_dma_buf_v2(struct sg_table *table, int domain,
		int dir, struct dma_buf *dma_buf)
{
	struct mdss_smmu_client *mdss_smmu = mdss_smmu_get_cb(domain);
	if (!mdss_smmu) {
		pr_err("not able to get smmu context\n");
		return;
	}

	ATRACE_BEGIN("unmap_buffer");
	msm_dma_unmap_sg(mdss_smmu->dev, table->sgl, table->nents, dir,
		 dma_buf);
	ATRACE_END("unmap_buffer");
}

/*
 * mdss_smmu_dma_alloc_coherent_v2()
 *
 * Allocates buffer same as mdss_smmu_dma_alloc_coherent_v1, but in addition it
 * also maps to the SMMU domain with the help of the respective SMMU context
 * bank device
 */
static int mdss_smmu_dma_alloc_coherent_v2(struct device *dev, size_t size,
		dma_addr_t *phys, dma_addr_t *iova, void *cpu_addr,
		gfp_t gfp, int domain)
{
	struct mdss_smmu_client *mdss_smmu = mdss_smmu_get_cb(domain);
	if (!mdss_smmu) {
		pr_err("not able to get smmu context\n");
		return -EINVAL;
	}

	cpu_addr = dma_alloc_coherent(mdss_smmu->dev, size, iova, gfp);
	if (!cpu_addr) {
		pr_err("dma alloc coherent failed!\n");
		return -ENOMEM;
	}
	*phys = iommu_iova_to_phys(mdss_smmu->mmu_mapping->domain,
			*iova);
	return 0;
}

static void mdss_smmu_dma_free_coherent_v2(struct device *dev, size_t size,
		void *cpu_addr, dma_addr_t phys, dma_addr_t iova, int domain)
{
	struct mdss_smmu_client *mdss_smmu = mdss_smmu_get_cb(domain);
	if (!mdss_smmu) {
		pr_err("not able to get smmu context\n");
		return;
	}

	dma_free_coherent(mdss_smmu->dev, size, cpu_addr, iova);
}

/*
 * mdss_smmu_map_v1()
 *
 * Same as mdss_smmu_map_v1, just that it maps to the appropriate domain
 * referred by the smmu context bank handles.
 */
static int mdss_smmu_map_v2(int domain, phys_addr_t iova, phys_addr_t phys,
		int gfp_order, int prot)
{
	struct mdss_smmu_client *mdss_smmu = mdss_smmu_get_cb(domain);
	if (!mdss_smmu) {
		pr_err("not able to get smmu context\n");
		return -EINVAL;
	}

	return iommu_map(mdss_smmu->mmu_mapping->domain,
			iova, phys, gfp_order, prot);
}

static void mdss_smmu_unmap_v2(int domain, unsigned long iova, int gfp_order)
{
	struct mdss_smmu_client *mdss_smmu = mdss_smmu_get_cb(domain);
	if (!mdss_smmu) {
		pr_err("not able to get smmu context\n");
		return;
	}

	iommu_unmap(mdss_smmu->mmu_mapping->domain, iova, gfp_order);
}

/*
 * mdss_smmUdsi_alloc_buf_v2()
 *
 * Allocates the buffer and mapping is done later
 */
static char *mdss_smmu_dsi_alloc_buf_v2(struct device *dev, int size,
		dma_addr_t *dmap, gfp_t gfp)
{
	return kzalloc(size, GFP_KERNEL);
}

/*
 * mdss_smmu_dsi_map_buffer_v2()
 *
 * Maps the buffer allocated in mdss_smmu_dsi_alloc_buffer_v2 with the SMMU
 * domain and uses dma_map_single as msm_iommu_map_contig_buffer is depricated
 * in smmu v2.
 */
static int mdss_smmu_dsi_map_buffer_v2(phys_addr_t phys, unsigned int domain,
		unsigned long size, dma_addr_t *dma_addr, void *cpu_addr,
		int dir)
{
	struct mdss_smmu_client *mdss_smmu = mdss_smmu_get_cb(domain);
	if (!mdss_smmu) {
		pr_err("not able to get smmu context\n");
		return -EINVAL;
	}

	*dma_addr = dma_map_single(mdss_smmu->dev, cpu_addr, size, dir);
	if (IS_ERR_VALUE(*dma_addr)) {
		pr_err("dma map single failed\n");
		return -ENOMEM;
	}
	return 0;
}

static void mdss_smmu_dsi_unmap_buffer_v2(dma_addr_t dma_addr, int domain,
		unsigned long size, int dir)
{
	struct mdss_smmu_client *mdss_smmu = mdss_smmu_get_cb(domain);
	if (!mdss_smmu) {
		pr_err("not able to get smmu context\n");
		return;
	}

	if (is_mdss_iommu_attached())
		dma_unmap_single(mdss_smmu->dev, dma_addr, size, dir);
}



static void mdss_smmu_deinit_v2(struct mdss_data_type *mata)
{
	int i;
	struct mdss_smmu_client *mdss_smmu;

	for (i = 0; i < MDSS_IOMMU_MAX_DOMAIN; i++) {
		mdss_smmu = mdss_smmu_get_cb(i);
		if (mdss_smmu->dev)
			arm_iommu_release_mapping(mdss_smmu->mmu_mapping);
	}
}

static void mdss_smmu_ops_init(struct mdss_data_type *mdata)
{
	mdata->smmu_ops.smmu_attach = mdss_smmu_attach_v2;
	mdata->smmu_ops.smmu_detach = mdss_smmu_detach_v2;
	mdata->smmu_ops.smmu_get_domain_id = mdss_smmu_get_domain_id_v2;
	mdata->smmu_ops.smmu_dma_buf_attach =
				mdss_smmu_dma_buf_attach_v2;
	mdata->smmu_ops.smmu_map_dma_buf = mdss_smmu_map_dma_buf_v2;
	mdata->smmu_ops.smmu_unmap_dma_buf = mdss_smmu_unmap_dma_buf_v2;
	mdata->smmu_ops.smmu_dma_alloc_coherent =
				mdss_smmu_dma_alloc_coherent_v2;
	mdata->smmu_ops.smmu_dma_free_coherent =
				mdss_smmu_dma_free_coherent_v2;
	mdata->smmu_ops.smmu_map = mdss_smmu_map_v2;
	mdata->smmu_ops.smmu_unmap = mdss_smmu_unmap_v2;
	mdata->smmu_ops.smmu_dsi_alloc_buf = mdss_smmu_dsi_alloc_buf_v2;
	mdata->smmu_ops.smmu_dsi_map_buffer =
				mdss_smmu_dsi_map_buffer_v2;
	mdata->smmu_ops.smmu_dsi_unmap_buffer =
				mdss_smmu_dsi_unmap_buffer_v2;
	mdata->smmu_ops.smmu_deinit = mdss_smmu_deinit_v2;
}

/*
 * mdss_smmu_device_create()
 * @dev: mdss_mdp device
 *
 * For smmu_v2, each context bank is a seperate child device of mdss_mdp.
 * Platform devices are created for those smmu related child devices of
 * mdss_mdp here. This would facilitate probes to happen for these devices in
 * which the smmu mapping and initilization is handled.
 */
void mdss_smmu_device_create(struct device *dev)
{
	struct device_node *parent, *child;
	parent = dev->of_node;
	for_each_child_of_node(parent, child) {
		if (is_mdss_smmu_compatible_device(child->name))
			of_platform_device_create(child, NULL, dev);
	}
}

int mdss_smmu_init(struct mdss_data_type *mdata, struct device *dev)
{
	mdss_smmu_device_create(dev);
	mdss_smmu_ops_init(mdata);
	mdata->mdss_util->iommu_lock = mdss_iommu_lock;
	mdata->mdss_util->iommu_unlock = mdss_iommu_unlock;
	return 0;
}

static int mdss_mdp_unsec = MDSS_IOMMU_DOMAIN_UNSECURE;
static int mdss_rot_unsec = MDSS_IOMMU_DOMAIN_ROT_UNSECURE;
static int mdss_mdp_sec = MDSS_IOMMU_DOMAIN_SECURE;
static int mdss_rot_sec = MDSS_IOMMU_DOMAIN_ROT_SECURE;

static const struct of_device_id mdss_smmu_dt_match[] = {
	{ .compatible = "qcom,smmu_mdp_unsec", .data = &mdss_mdp_unsec},
	{ .compatible = "qcom,smmu_rot_unsec", .data = &mdss_rot_unsec},
	{ .compatible = "qcom,smmu_mdp_sec", .data = &mdss_mdp_sec},
	{ .compatible = "qcom,smmu_rot_sec", .data = &mdss_rot_sec},
	{}
};
MODULE_DEVICE_TABLE(of, mdss_smmu_dt_match);

/*
 * mdss_smmu_probe()
 * @pdev: platform device
 *
 * Each smmu context acts as a separate device and the context banks are
 * configured with a VA range.
 * Registeres the clks as each context bank has its own clks, for which voting
 * has to be done everytime before using that context bank.
 */
int mdss_smmu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	struct mdss_smmu_client *mdss_smmu;
	int rc = 0;
	u32 domain;
	size_t va_start, va_size;
	const struct of_device_id *match;
	struct dss_module_power *mp;
	int disable_htw = 1;
	char name[MAX_CLIENT_NAME_LEN];

	if (!mdata) {
		pr_err("probe failed as mdata is not initialized\n");
		return -EPROBE_DEFER;
	}

	match = of_match_device(mdss_smmu_dt_match, &pdev->dev);
	if (!match || !match->data) {
		pr_err("probe failed as match data is invalid\n");
		return -EINVAL;
	}

	domain = *(int *) (match->data);
	if (domain >= MDSS_IOMMU_MAX_DOMAIN) {
		pr_err("no matching device found\n");
		return -EINVAL;
	}

	mdss_smmu = &mdata->mdss_smmu[domain];

	if (domain == MDSS_IOMMU_DOMAIN_UNSECURE ||
		domain == MDSS_IOMMU_DOMAIN_ROT_UNSECURE) {
		va_start = SZ_128K;
		va_size = SZ_1G - SZ_128K;
	} else if (domain == MDSS_IOMMU_DOMAIN_SECURE ||
		domain == MDSS_IOMMU_DOMAIN_ROT_SECURE) {
		va_start = SZ_1G;
		va_size = SZ_2G;
	} else {
		pr_err("invalid smmu domain type\n");
		return -EINVAL;
	}

	mp = &mdss_smmu->mp;

	mp->vreg_config = devm_kzalloc(&pdev->dev,
		sizeof(struct dss_vreg), GFP_KERNEL);
	if (!mp->vreg_config) {
		pr_err("can't alloc vreg mem\n");
		return -ENOMEM;
	}

	strlcpy(mp->vreg_config->vreg_name, "gdsc-mmagic-mdss",
		sizeof(mp->vreg_config->vreg_name));

	mp->num_vreg = 1;

	rc = msm_dss_config_vreg(&pdev->dev, mp->vreg_config,
		mp->num_vreg, true);
	if (rc) {
		pr_err("vreg config failed rc=%d\n", rc);
		return rc;
	}

	rc = mdss_smmu_clk_register(pdev, mp);
	if (rc) {
		pr_err("smmu clk register failed for domain[%d] with err:%d\n",
			domain, rc);
		msm_dss_config_vreg(&pdev->dev, mp->vreg_config, mp->num_vreg,
			false);
		return rc;
	}

	snprintf(name, MAX_CLIENT_NAME_LEN, "smmu:%u", domain);
	mdss_smmu->reg_bus_clt = mdss_reg_bus_vote_client_create(name);
	if (IS_ERR_OR_NULL(mdss_smmu->reg_bus_clt)) {
		pr_err("mdss bus client register failed\n");
		msm_dss_config_vreg(&pdev->dev, mp->vreg_config, mp->num_vreg,
			false);
		return PTR_ERR(mdss_smmu->reg_bus_clt);
	}

	rc = mdss_smmu_enable_power(mdss_smmu, true);
	if (rc) {
		pr_err("power enable failed - domain:[%d] rc:%d\n", domain, rc);
		goto bus_client_destroy;
	}

	mdss_smmu->mmu_mapping = arm_iommu_create_mapping(
		msm_iommu_get_bus(dev), va_start, va_size);
	if (IS_ERR(mdss_smmu->mmu_mapping)) {
		pr_err("iommu create mapping failed for domain[%d]\n", domain);
		rc = PTR_ERR(mdss_smmu->mmu_mapping);
		goto disable_power;
	}

	rc = iommu_domain_set_attr(mdss_smmu->mmu_mapping->domain,
		DOMAIN_ATTR_COHERENT_HTW_DISABLE, &disable_htw);
	if (rc) {
		pr_err("couldn't disable coherent HTW\n");
		goto release_mapping;
	}

	if (domain == MDSS_IOMMU_DOMAIN_SECURE ||
		domain == MDSS_IOMMU_DOMAIN_ROT_SECURE) {
		int secure_vmid = VMID_CP_PIXEL;
		rc = iommu_domain_set_attr(mdss_smmu->mmu_mapping->domain,
			DOMAIN_ATTR_SECURE_VMID, &secure_vmid);
		if (rc) {
			pr_err("couldn't set secure pixel vmid\n");
			goto release_mapping;
		}
	}

	if (!mdata->handoff_pending)
		mdss_smmu_enable_power(mdss_smmu, false);
	else
		mdss_smmu->handoff_pending = true;

	mdss_smmu->dev = dev;
	pr_info("iommu v2 domain[%d] mapping and clk register successful!\n",
			domain);
	return 0;

release_mapping:
	arm_iommu_release_mapping(mdss_smmu->mmu_mapping);
disable_power:
	mdss_smmu_enable_power(mdss_smmu, false);
bus_client_destroy:
	mdss_reg_bus_vote_client_destroy(mdss_smmu->reg_bus_clt);
	mdss_smmu->reg_bus_clt = NULL;
	msm_dss_config_vreg(&pdev->dev, mp->vreg_config, mp->num_vreg,
			false);
	return rc;
}

int mdss_smmu_remove(struct platform_device *pdev)
{
	int i;
	struct mdss_smmu_client *mdss_smmu;

	for (i = 0; i < MDSS_IOMMU_MAX_DOMAIN; i++) {
		mdss_smmu = mdss_smmu_get_cb(i);
		if (mdss_smmu->dev && mdss_smmu->dev == &pdev->dev)
			arm_iommu_release_mapping(mdss_smmu->mmu_mapping);
	}
	return 0;
}

static struct platform_driver mdss_smmu_driver = {
	.probe = mdss_smmu_probe,
	.remove = mdss_smmu_remove,
	.shutdown = NULL,
	.driver = {
		.name = "mdss_smmu",
		.of_match_table = mdss_smmu_dt_match,
	},
};

static int mdss_smmu_register_driver(void)
{
	return platform_driver_register(&mdss_smmu_driver);
}

static int __init mdss_smmu_driver_init(void)
{
	int ret;
	ret = mdss_smmu_register_driver();
	if (ret)
		pr_err("mdss_smmu_register_driver() failed!\n");

	return ret;
}
module_init(mdss_smmu_driver_init);

static void __exit mdss_smmu_driver_cleanup(void)
{
	platform_driver_unregister(&mdss_smmu_driver);
}
module_exit(mdss_smmu_driver_cleanup);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MDSS SMMU driver");
