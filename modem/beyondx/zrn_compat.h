/* What Samsung's kernel had and mainline does not, for the modem driver. */
#pragma once

/* the stock S10 5G kernel's choices (exynos9820-beyondxks_defconfig): the LTE
 * modem (S5000AP, shared memory) and the S5100 5G modem on PCIe */
#define CONFIG_SOC_EXYNOS9820 1
#define CONFIG_MCU_IPC 1
#define CONFIG_SHM_IPC 1
#define CONFIG_SEC_SIPC_DUAL_MODEM_IF 1
#define CONFIG_SEC_MODEM_S5000AP 1
#define CONFIG_UMTS_MODEM_S5000AP 1
#define CONFIG_LINK_DEVICE_SHMEM 1
#define CONFIG_UMTS_MODEM_S5100 1
#define CONFIG_LINK_DEVICE_PCIE 1
#define CONFIG_LINK_DEVICE_NAPI 1
#define CONFIG_CP_SECURE_BOOT 1
#define CONFIG_CP_PMUCAL 1
/* the CP's memory map (in its firmware) puts the zero-copy buffers in
 * modem_spare at 0xd0000000 (CP 0x50000000): stock's linear workaround */
#define CONFIG_CP_LINEAR_WA 1
/* the AP wakes the CP before it sends (ap2cp_wakeup / cp2ap_wakeup) and lets
 * it sleep after: without it, data sent after INIT_END lies unread */
#define CONFIG_LINK_POWER_MANAGEMENT 1
#define CONFIG_CP_ETDAC_OTP_WA 1	/* stock: y */
/* CONFIG_CP_ZEROCOPY, CONFIG_CP_DIT and CONFIG_CP_PKTPROC_V2 (stock: y) are
 * left off for now: packet data is not needed for calls and SMS, and
 * zerocopy builds skbs with __build_skb, which mainline does not export */
#define CONFIG_MODEM_IF_QOS 1
/* the stock kernel had PCIe on (for the S5100), which shapes the shared
 * memory map and the link/modem structures the LTE side uses too: kept so
 * (link_device_memory.h, modem_prj.h), without the PCIe code itself */
#define ZRN_STOCK_LAYOUT 1
#define CONFIG_MODEM_IF_NET_GRO 1

#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/sched/clock.h>
#include <linux/timer.h>
#include <linux/hrtimer.h>
#include <linux/etherdevice.h>
#include <linux/proc_fs.h>
#include <linux/pm_wakeup.h>
#include <linux/arm-smccc.h>

/* ---- time: the 32-bit timespec family is gone; the 64-bit one stands in */
#define timespec		timespec64
#define getnstimeofday		ktime_get_real_ts64
#define timespec_sub		timespec64_sub
#define timespec_to_ns		timespec64_to_ns
#define time_to_tm		time64_to_tm
#define cpu_clock(cpu)		local_clock()

/* ---- renamed */
#define strlcpy			strscpy
#define PDE_DATA		pde_data
#define random_ether_addr	eth_random_addr
#define del_timer		timer_delete
#define del_timer_sync		timer_delete_sync

/* ---- Android wakelocks, on mainline's wakeup sources */
#define WAKE_LOCK_SUSPEND	0
struct wake_lock {
	struct wakeup_source *ws;
};
static inline void wake_lock_init(struct wake_lock *lock, int type, const char *name)
{
	lock->ws = wakeup_source_register(NULL, name);
}
static inline void wake_lock_destroy(struct wake_lock *lock)
{
	wakeup_source_unregister(lock->ws);
	lock->ws = NULL;
}
static inline void wake_lock(struct wake_lock *lock)
{
	if (lock->ws)
		__pm_stay_awake(lock->ws);
}
static inline void wake_unlock(struct wake_lock *lock)
{
	if (lock->ws)
		__pm_relax(lock->ws);
}
static inline void wake_lock_timeout(struct wake_lock *lock, long timeout)
{
	if (lock->ws)
		__pm_wakeup_event(lock->ws, jiffies_to_msecs(timeout));
}
static inline int wake_lock_active(struct wake_lock *lock)
{
	return lock->ws && lock->ws->active;
}

/* ---- Exynos performance requests: this kernel scales no bus or cluster
 * floor on request, so they are accepted and ignored. Mainline's struct
 * pm_qos_request is used as it is. */
#include <linux/pm_qos.h>
#define PM_QOS_CLUSTER0_FREQ_MIN	1
#define PM_QOS_CLUSTER1_FREQ_MIN	2
#define PM_QOS_BUS_THROUGHPUT		3
#define PM_QOS_DEVICE_THROUGHPUT	4
#define pm_qos_add_request(req, class, value)	do { } while (0)
#define pm_qos_update_request(req, value)	do { } while (0)
#define pm_qos_remove_request(req)		do { } while (0)

/* ---- Samsung debug hooks with nothing behind them here */
#define exynos_ss_set_enable(name, en)	do { } while (0)
#define acpm_stop_log()			do { } while (0)
static inline int gpio_export(unsigned int gpio, bool direction_may_change) { return 0; }
static inline int gpio_export_link(struct device *dev, const char *name, unsigned int gpio) { return 0; }

/* ---- the secure monitor: Samsung's call takes a command and three
 * arguments and returns x0 (drivers/soc/samsung/exynos-smc.c) */
static inline int exynos_smc(unsigned long cmd, unsigned long arg1, unsigned long arg2,
		unsigned long arg3)
{
	struct arm_smccc_res res;
	arm_smccc_smc(cmd, arg1, arg2, arg3, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

/* ---- the CP's power sequences (zrn_cp_pmu.c) */
int cal_cp_init(void);
int cal_cp_status(void);
int cal_cp_reset_assert(void);
int cal_cp_reset_release(void);
void cal_cp_active_clear(void);
void cal_cp_reset_req_clear(void);
void cal_cp_enable_dump_pc_no_pg(void);
void cal_cp_disable_dump_pc_no_pg(void);

/* the CP's secure service: modes as in modem_prj.h (enum cp_boot_mode), 3 and 4 read and
 * write CP_CTRL (drivers/soc/samsung/cal-if/pmucal_cp.c uses the same ID) */
#define SMC_ID				0x82000700

#define CONFIG_CP_RAM_LOGGING 1	/* stock: y */

/* ---- networking */
#include <linux/netdevice.h>
/* GRO: mainline flushes it itself when the poll completes */
#define napi_gro_flush(napi, flush_old)	do { } while (0)
#define GRO_DROP			((gro_result_t)-1)	/* gone: never returned */
/* Samsung's kernel told a receive path which NAPI was polling it; here each
 * memory link (shared memory, PCIe) has one, found from the link the packet
 * came in on (modem_io_device.c's rx path has it as "ld") */
struct link_device;
struct napi_struct *zrn_link_napi(struct link_device *ld);
#define napi_get_current()		zrn_link_napi(ld)

/* ---- hrtimers are set up with their function now */
static inline enum hrtimer_restart zrn_hrtimer_unset(struct hrtimer *timer)
{
	return HRTIMER_NORESTART;
}
#define hrtimer_init(timer, clock, mode)	hrtimer_setup(timer, zrn_hrtimer_unset, clock, mode)

/* Samsung's memblock helper for handing reserved memory back */
#define free_memsize_reserved(base, size)	do { } while (0)

/* see shm_ipc.c */
dma_addr_t shm_zmb_dma_addr(void *v);

/* modem_ctrl_s5000ap.c; Samsung declared it in a header of another driver */
int modem_force_crash_exit_ext(void);
