/* stand-in: see zrn_compat.h. The L1 substate users of the vendor's
 * exynos_pcie_host_v1_l1ss_ctrl (a no-op in pcie-exynos9820-ch1.ko) */
#pragma once
#define PCIE_L1SS_CTRL_ARGOS		(0x1 << 0)
#define PCIE_L1SS_CTRL_BOOT		(0x1 << 1)
#define PCIE_L1SS_CTRL_CAMERA		(0x1 << 2)
#define PCIE_L1SS_CTRL_MODEM_IF		(0x1 << 3)
#define PCIE_L1SS_CTRL_WIFI		(0x1 << 4)
#define PCIE_L1SS_CTRL_MST		(0x1 << 5)
#define PCIE_L1SS_CTRL_TEST		(0x1 << 31)
