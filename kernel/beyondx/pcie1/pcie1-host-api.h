/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The vendor PCIe host API (pcie-exynos-host-v1.c, s5100_pcie.h) that
 * pcie-exynos9820-ch1.ko exports to the modem driver. Added for OmonullOS,
 * October 2026.
 */
#ifndef PCIE1_HOST_API_H
#define PCIE1_HOST_API_H

#include <linux/exynos-pci-noti.h>

int exynos_check_pcie_link_status(int ch_num);
int exynos_pcie_host_v1_poweron(int ch_num);
int exynos_pcie_host_v1_poweroff(int ch_num);
int exynos_pcie_host_v1_register_event(struct exynos_pcie_register_event *reg);
int exynos_pcie_host_v1_deregister_event(struct exynos_pcie_register_event *reg);
int exynos_pcie_host_v1_l1ss_ctrl(int enable, int id);
int exynos_pcie_host_v1_lanechange(int ch_num, int lane);
int pcie_iommu_map(int ch_num, unsigned long iova, phys_addr_t paddr, size_t size, int prot);
void exynos_pcie_host_v1_register_dump(int ch_num);

#endif
