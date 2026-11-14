// SPDX-License-Identifier: GPL-2.0
/*
 * snd-zrn-abox-bx: sound for the Galaxy S10 5G (Exynos 9820) on a mainline
 * kernel. One ALSA card, one playback device (48 kHz, 16 bit, stereo) out of
 * the two CS35L40 amplifiers: the bottom speaker (right channel) and the
 * earpiece (left).
 *
 * Built on ztOS's audio/driver/snd-zrn-abox.c (the S9, Exynos 9810, ABOX
 * 1.x, one Cortex-A7, MAX98512s); what differs here, from the stock device
 * tree (img/dt/00_id0_rev0.dts, abox@18c50000; dtbo/08 cs35l40@40/41) and
 * Samsung's GPL drivers in the G977N kernel (sound/soc/samsung/abox,
 * ABOX_VERSION 0x20001, register 0x02000100 on this chip):
 *  - two DSP cores, each with SRAM and DRAM firmware (calliope_sram.bin at
 *    0, calliope_dram.bin at 0; calliope_sram_2.bin at SRAM 0x32000,
 *    calliope_dram_2.bin at DRAM 0x280000), powered by PMU 0x2d80 / 0x2e00
 *    bit 0 (Samsung's abox_core_power; nothing else is set to start them);
 *  - extra firmware in DRAM (calliope2.dt at 0x11fe000, ...), 28 MB of DRAM;
 *  - blocks at 0x18c..: SFR 18c50000, SYSREG 18c10000 (MISC_CON 0x504),
 *    SRAM 18d00000 (0x59000), GIC 18cf1000/18cf2000, SysMMU 18e00000,
 *    PMU pad retention 0x1920 bit 11, the interrupt to us SPI 46;
 *  - messages (ABOX 2.0, Samsung's abox_msg.c): two rings in DRAM, to the
 *    DSP at 0x850000 and from it at 0x854000 (0x4000 each), each a ring of
 *    16 commands and a ring of data (the 1.x ABOX_IPC_MSG words); SGI 15
 *    both ways says there is something new;
 *  - the route (stock mixer_paths "media-speaker" without the DSP's speaker
 *    protection): RDMA0 -> SPUS stream 0 -> SIFS1 -> UAIF1, I2S on gpb1-0..3;
 *    ABOX 2.0.1 field layouts from abox_soc.h / abox_cmpnt_v20.c;
 *  - the amplifiers sit on PERIC1's gph0-6 (SCL) / gph0-7 (SDA), stock
 *    hsi2c_38, with nRESET on gph1-1/gph1-2. Neither PERIC1's pinctrl nor
 *    its I2C has a driver here, so the bus is bit-banged by i2c-algo-bit on
 *    those pins' registers (registered as i2c-38, for i2c-dev too), and the
 *    amplifiers are set up through mainline's cs35l41 library (errata, OTP
 *    trims, power-up), built into this module.
 * The audio power domain (PMU 0x1904) and PLL_AUD0 (1179.648 MHz) are as
 * S-Boot leaves them. Samsung reaches the CMU with 32-bit accesses only.
 *
 * Cirrus' speaker protection (spk-prot firmware on the amplifiers' own DSP)
 * only with amp_dsp (see "the amps' own DSP"); without it the volume stops well
 * short of full scale (AMP_VOL_MAX).
 *
 * Modified for OmonullOS, October 2026 (new file).
 * Modified for OmonullOS, November 2026: the CS47L93 codec and the two
 * microphones on UAIF0 (codec_state, mic_probe, mic: see "the codec" below).
 * Modified for OmonullOS, November 2026: the amps are set up again after
 * every system resume (amps_rescue, amp_rescue).
 * Modified for OmonullOS, November 2026: WDMA1/WDMA7 buffers mapped as stock
 * (bridge_bufs). Modified for OmonullOS, November 2026: the VSS mark is cleared at probe and
 * before every ABOX_START_VSS (ztOS 47f1a79); call_diag (ztOS a9bcb04 and
 * 34e3d32, read where this chip's call DMAs really point).
 * Modified for OmonullOS, November 2026: dsp_vol_boost (the protected speaker
 * above AMP_VOL_MAX) and dsp_diag.
 * Modified for OmonullOS, November 2026: the amps' speaker protection from
 * stock VENDOR (amp_dsp, cs_dsp; see "the amps' own DSP" below).
 * Modified for OmonullOS, November 2026: call_vi, WDMA4 (stock vi-sensing)
 * through a call: REFEXT4CH's source, whose update starts the VSS uplink.
 *
 * ztOS a9bcb04 (the S9's call: microphones, echo reference, voice
 * parameters, DSM) on this phone: the voice parameters are mapped here since
 * 19.10 (CP_VPARAM_PA below, the S9's is 0xf9000000); the uplink is
 * zcall-uplink.sh's stand-in (NSRC0/NSRC1 <- SIFS0) until the CS47L93 has a
 * driver, and only then the microphones (NSRC <- UAIF0, MIC_EN) and ztOS's
 * codec pin check after a sleep (34e3d32); the echo reference the VSS takes
 * is WDMA3 (the speaker graph's SIFS0 -> NSRC2 copy), not the S9's RSRC0 ->
 * RECP, which our calls leave at 0 and our notes of stock's incall paths
 * here do not name; there is no speaker management (DSM) to take off.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/firmware/cirrus/cs_dsp.h>
#include <linux/firmware/cirrus/wmfw.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/cs35l41.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>

#define PMU_BASE		0x15860000
#define PMU_AUD_CONF		0x1900
#define PMU_AUD_STATUS		0x1904
#define PMU_PAD_ABOX		0x1920
#define  PAD_NORMAL		BIT(11)
static const u32 core_power[2] = { 0x2d80, 0x2e00 };

#define ABOX_SFR		0x18c50000
#define  ABOX_IP_ID		0x0000
#define  ABOX_VERSION		0x0004
#define  ABOX_SYSPOWER_CTRL	0x0010
#define  ABOX_SYSPOWER_STATUS	0x0014
#define  ABOX_QCHANNEL_DISABLE	0x0038
#define   QCH_BCLK_UAIF1	BIT(3)	/* enum qchannel ABOX_BCLK_UAIF1 */
#define  ABOX_ROUTE_CTRL0	0x0040
#define  ABOX_ROUTE_CTRL1	0x0044
#define  ABOX_ROUTE_CTRL2	0x0048
#define  ABOX_SPUS_CTRL0	0x0200
#define  ABOX_SPUS_CTRL1	0x0204
#define  ABOX_SPUS_SBANK_RDMA0	0x0220
#define  ABOX_SPUS_CTRL_FC1	0x0218
#define  ABOX_SIFS0_CNT		0x0290
#define  ABOX_SIFS1_CNT		0x0294
#define  ABOX_SPUM_CTRL0	0x0300
#define  ABOX_UAIF0_CTRL0	0x0500
#define  ABOX_UAIF1_CTRL0	0x0510
#define  ABOX_UAIF1_CTRL1	0x0514
#define  ABOX_UAIF1_STATUS	0x051c
#define  ABOX_RDMA0_CTRL0	0x1000
#define  ABOX_RDMA0_STATUS	0x1030
/* any DMA's registers (as rdtest.sh and zcall-uplink.sh's "regs" read them on
 * this chip: RDMA4 0x1400, WDMA2 0x3200, WDMA3 0x3300) */
#define  ABOX_RDMA(x)		(0x1000 + 0x100 * (x))
#define  ABOX_WDMA(x)		(0x3000 + 0x100 * (x))
#define   DMA_BUF_STR		0x08
#define   DMA_BUF_END		0x0c
#define   DMA_BUF_OFFSET	0x10
#define   DMA_STATUS		0x30
#define ABOX_GPR0		0x18c55000	/* core 0's registers, for a hung boot */
#define ABOX_SYSREG		0x18c10000
#define  SYSREG_MISC_CON	0x0504
#define ABOX_SRAM		0x18d00000
#define ABOX_SRAM_SIZE		0x59000
#define ABOX_GICD		0x18cf1000
#define ABOX_GICC		0x18cf2000
#define SYSMMU_AUD		0x18e00000
#define  MMU_CTRL		0x000
#define  MMU_CFG		0x004
#define  MMU_PT_BASE		0x00c
#define  MMU_FLUSH_ALL		0x010
#define  MMU_INT_STATUS		0x060
#define  MMU_INT_CLEAR		0x064
#define  MMU_FAULT_VA		0x070
#define CMU_AUD			0x18c00000
#define  CMU_MUX_UAIF1		0x1010
#define  CMU_DIV_AUDIF		0x1800
#define  CMU_DIV_PLL		0x1828
#define  CMU_DIV_UAIF1		0x1830
#define AUD_PLL_HZ		1179648000UL
#define PINCTRL_AUD		0x18c60000
#define  GPB1_CON		0x0020
#define  GPB1_PUD		0x0028
#define GPIO_PERIC1		0x10830000
#define  GPH0_CON		0x00e0
#define  GPH0_DAT		0x00e4
#define  GPH0_PUD		0x00e8
#define  GPH0_DRV		0x00ec
#define  GPH1_CON		0x0100
#define  GPH1_DAT		0x0104
#define  AMP_SCL		6
#define  AMP_SDA		7

#define IOVA_DRAM		0x80000000UL
#define DRAM_SIZE		0x1c00000	/* CONFIG_SND_SOC_SAMSUNG_ABOX_DRAM_SIZE */
#define IOVA_RDMA0		0x91000000UL	/* Samsung's IOVA_RDMA_BUFFER(0) */
#define IOVA_RDMA7		0x91700000UL	/* IOVA_RDMA_BUFFER(7) */
#define IOVA_WDMA3		0x92300000UL	/* IOVA_WDMA_BUFFER(3) */
#define IOVA_RDMA4		0x91400000UL	/* IOVA_RDMA_BUFFER(4): stock's "call" playback */
#define IOVA_WDMA2		0x92200000UL	/* IOVA_WDMA_BUFFER(2): stock's "call" capture */
#define IOVA_WDMA1		0x92100000UL	/* IOVA_WDMA_BUFFER(1): route-cp-tx-bridge's VPCMIN source */
#define IOVA_WDMA7		0x92700000UL	/* IOVA_WDMA_BUFFER(7): route-cp-tx-bridge's NSRC6 */
#define IOVA_WDMA4		0x92400000UL	/* IOVA_WDMA_BUFFER(4): stock's "vi-sensing" */
#define IOVA_VDMA0		0x94000000UL	/* IOVA_VDMA_BUFFER(0): firmware PCM 100's */
#define IOVA_HW0		0x91100000UL	/* RDMA0's own ring while PCM 100 feeds it */
#define RT_BYTES		SZ_64K		/* each realtime stream's buffer */
#define RT_PERIOD_FRAMES	480		/* 10 ms */
#define RT_PERIODS		4
#define HW0_PERIOD_BYTES	(RT_PERIOD_FRAMES * 2 * 2 * 4)	/* 40 ms, 16 bit 2 ch */
#define IOVA_VSS		0xa0000000UL
#define IOVA_VPARAM		0xa1000000UL
#define VSS_SIZE		SZ_16M		/* a stand-in until the CP's own is mapped */
#define VPARAM_SIZE		SZ_1M
/* the CP's own (stock DT shmem: vss_offset 0x7800000, vparam_offset 0xb700000
 * from 0xf0000000; cbd loads modem.bin's VSS there) */
#define CP_VSS_PA		0xf7800000UL
#define CP_VSS_SIZE		SZ_8M
/* the word the voice firmware marks once it has come up (stock DT abox_vss
 * magic_offset 0x700000; the S9's is 0x600000). Samsung's abox_vss zeroes it
 * at probe, before the CP loads VSS and the DSP starts it. */
#define CP_VSS_MAGIC		0x700000
/* the voice firmware's parameters: stock DT shmem vparam_offset 0xb700000,
 * vparam_size 0x200000 (modem/beyondx/zrn-modem-overlay.dts), as the CP's own
 * map says at boot ("VPA 0xb700000", LOG next at 0xb900000: log.md 14:27,
 * 17.10). Samsung's abox.c maps vparam_size, so nothing is added after it. */
#define CP_VPARAM_PA		0xfb700000UL
#define CP_VPARAM_SIZE		SZ_2M
#define IOVA_PRIVATE		0xe0000000UL
#define PRIVATE_SIZE		SZ_8M
#define LV2_TABLES		128
#define ABOX_SPI		46

#define MSG_TX			0x850000
#define MSG_RX			0x854000
#define MSG_AREA		0x4000
#define MSG_IRQ			15
/* messages, as in Samsung's abox_ipc.h */
#define IPC_SYSTEM		1
#define IPC_PCMPLAYBACK		2
#define IPC_PCMCAPTURE		3
#define IPC_ERAP		5
#define  REALTIME_START		6
#define  REALTIME_STOP		7
#define IPC_ABOX_CONFIG		0xa
#define  ABOX_RESUME		2
#define  ABOX_BOOT_DONE		3
#define  ABOX_CHANGE_GEAR	4
#define  ABOX_CHANGED_GEAR	9
#define  ABOX_REPORT_LOG	0x10
#define  ABOX_SET_MODE		0x50	/* param1: enum audio_mode, 0 NORMAL, 2 IN_CALL */
#define  ABOX_SET_TYPE		0x60	/* param1: enum sound_type, 0 VOICE, 1 SPEAKER */
#define  ABOX_START_VSS		0xa0	/* stock's modem notifier, when the CP is online */
#define  ABOX_REPORT_COMPONENT	0xc0
#define  ABOX_UPDATE_COMPONENT_CONTROL 0xc1
#define  SET_SIFS0_RATE		1
#define  SET_SIFS1_RATE		2
#define  SET_SIFS0_FORMAT	0x10
#define  SET_SIFS1_FORMAT	0x11
#define  SET_SIFM3_RATE		8
#define  SET_SIFM3_FORMAT	0x17
#define  PCM_PLTDAI_OPEN	11
#define  PCM_PLTDAI_CLOSE	12
#define  PCM_PLTDAI_HW_PARAMS	14
#define  PCM_PLTDAI_HW_FREE	15
#define  PCM_PLTDAI_PREPARE	16
#define  PCM_PLTDAI_TRIGGER	17
#define  PCM_PLTDAI_POINTER	18
#define  PCM_SET_BUFFER		20
#define  PCM_PLTDAI_ACK		22
#define  PCM_PLTDAI_REGISTER	50

#define RATE			48000
#define BCLK_HZ			(RATE * 2 * 16)	/* 1.536 MHz: two 16-bit slots */
#define BUFFER_BYTES		SZ_64K
#define AMP_BUS			38		/* stock hsi2c_38 */
#define AMP_BOT			0x40		/* bottom speaker, right channel */
#define AMP_RCV			0x41		/* earpiece, left channel */
/* AMP_DIG_VOL_CTRL's PCM volume: 0.125 dB steps, two's complement, 0 = 0 dB */
#define AMP_VOL_MIN		(-816)		/* -102 dB */
#define AMP_VOL_MAX		(-48)		/* -6 dB; see the note on protection above */
#define AMP_VOL_MAX_DSP		0		/* 0 dB, stock's own, only with the protection */

/*
 * graph=1: stock's speaker graph (mixer_paths media-speaker). The AP's stream
 * (the DSP's RDMA0 entity) -> SPUS OUT0 -> SIFS0 (48 kHz, 32 bit, paced by its
 * counter) -> NSRC2 -> SIFM2 -> WDMA3; the DSP's CHCONV4CH turns WDMA3's two
 * channels into four for RDMA7 -> SPUS OUT7 -> SIFS1 -> UAIF1 (DSP_A, four
 * 32-bit slots) -> both CS35L40s. WDMA3 and RDMA7 are "realtime" streams the
 * stock HAL keeps open; here the driver opens them with the AP's stream.
 * graph=0: the S9's path, RDMA0 -> SIFS1 -> UAIF1 (I2S, two 16-bit slots).
 */
static int graph = 1;
module_param(graph, int, 0444);
MODULE_PARM_DESC(graph, "1: stock's speaker graph through WDMA3/RDMA7, 0: RDMA0 straight to UAIF1");

/* which realtime streams the driver opens with the AP's: bit 0 WDMA3, bit 1
 * RDMA7 (the DSP may drive RDMA7 from CHCONV4CH by itself) */
/* 1: the DSP sees the CP's VSS firmware and parameters, as stock's abox.c maps
 * them (its magic word cleared, vss_mark_clear()); 0: zeroed stand-ins */
static int vss_cp;
module_param(vss_cp, int, 0444);
MODULE_PARM_DESC(vss_cp, "map the CP's VSS region at 0xa0000000 instead of a stand-in");

static int rt_streams = 3;
module_param(rt_streams, int, 0644);

/* the realtime streams' sample widths (both in 32-bit containers), read at
 * every stream start. SIFS0's mixer sets what WDMA3 gets: at 24 bit its
 * samples were right-justified and not sign-extended (0x00fbd100), RDMA7 at
 * 32 sent that word as is and the amps read the slot from its MSB: -48 dB
 * with the negative half folded up, the crackle (OTA 28/29, 19.10). The
 * mixer now runs at 32 bit (MIXP_FORMAT), so WDMA3 holds S32 and 32/32 fits. */
static int rt_wbits = 32, rt_rbits = 32;
module_param(rt_wbits, int, 0644);
module_param(rt_rbits, int, 0644);
/* WDMA3's channels, read at every stream start: stock's HAL opens it as
 * spkamp_reference with 4 (pcm_config_spkamp_reference, 4 x 480 S32). Keep 2:
 * at 4 the VSS faulted as the call began (ABOX_REPORT_FAULT from
 * VHMPcmInThread, the DSP stops taking messages: call-1020-0047/0528.log) */
static int rt_wch = 2;
module_param(rt_wch, int, 0644);
MODULE_PARM_DESC(rt_wch, "WDMA3 channels, 2 or 4 (stock spkamp_reference: 4)");
/* SIFS0's (the mixer's) channels, set at every stream start: with rt_wch=4
 * and the mixer at 2 the call's underruns went but WDMA3 got no sound
 * (20.10). abox_get_format(32, ch) into MIXP_FORMAT, sifsx_cnt_val(12.288
 * MHz, 48000, 32, ch) into SIFS0_CNT and the DSP's SIFS0 format */
static int sifs0_ch = 2;
module_param(sifs0_ch, int, 0644);
MODULE_PARM_DESC(sifs0_ch, "SIFS0 mixer channels, 2 or 4");

static int debug = 20;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "log this many of the DSP's next messages");

static const struct { const char *name; u32 off; bool sram; bool needed; } fws[] = {
	{ "calliope_sram.bin", 0, true, true },
	{ "calliope_sram_2.bin", 0x32000, true, true },
	{ "calliope_dram.bin", 0, false, true },
	{ "calliope_dram_2.bin", 0x280000, false, true },
	{ "calliope2.dt", 0x11fe000, false, false },
	{ "SoundBoosterParam.bin", 0x4fc000, false, false },
	{ "APDV_AUDIO_SLSI.bin", 0x4d0000, false, false },
	{ "txse1.bin", 0x1200000, false, false },
	{ "rxse.bin", 0x1300000, false, false },
	{ "txse2.bin", 0x13b0000, false, false },
};

static struct platform_device *pdev;
static void __iomem *pmu, *sfr, *sysreg, *sram, *gicd, *gicc, *mmu, *gpr, *cmu, *pins, *gpio;
static void *dram, *vss, *vparam, *priv, *lv1, *lv2, *pcm, *rt_w, *rt_r, *hw0, *call_r, *call_w;
/* WDMA1 and WDMA7's buffers: stock's abox_wdma maps every WDMA's buffer at
 * IOVA_WDMA_BUFFER(id) at probe (abox_wdma_new), opened or not, and
 * route-cp-tx-bridge has the firmware run WDMA1 into VPCMIN_DAI0 (the VSS's
 * microphone input) and NSRC6 into WDMA7 with no PCM open. Ours had neither
 * mapped: the tone reached WDMA2 and never the VSS (log.md 11.11 15:20). */
static bool bridge_bufs = true;
module_param(bridge_bufs, bool, 0444);
MODULE_PARM_DESC(bridge_bufs, "map WDMA1/WDMA7 buffers as stock does (at probe)");
#define BRIDGE_BYTES		SZ_64K
static void *wdma1_buf, *wdma7_buf, *wdma4_buf;
static dma_addr_t wdma1_pa, wdma7_pa, wdma4_pa;
static dma_addr_t dram_pa, vss_pa, vparam_pa, priv_pa, lv1_pa, lv2_pa, pcm_pa, rt_w_pa, rt_r_pa, hw0_pa,
	call_r_pa, call_w_pa;
static int lv2_used, abox_irq;
static bool started, booted;
static u32 fw_version;
static DEFINE_SPINLOCK(tx_lock);
static DEFINE_SPINLOCK(rx_lock);
static struct work_struct gear_work;
static struct snd_card *card;
static struct snd_pcm_substream *active;	/* the open stream, for period messages */
static u32 dsp_pointer;				/* last PCM_PLTDAI_POINTER, an IOVA */

/* The firmware's own PCMs (PCM_PLTDAI_REGISTER, Samsung abox_vdma.c), ids
 * from 100, buffers in its DRAM. Stock media playback is one of these: it
 * feeds VPCMOUT_DAI0, which RDMA0 (RDMA0_A) takes, not the AP's channel 0. */
struct vdma {
	u32 id, addr, bytes_max, period_min, period_max, periods_min, periods_max;
	bool capture;
	char name[33];
};
static struct vdma vdmas[8];
static int vdma_count;
static int vdma = 100;	/* the playback vdma id aplay uses; 0: RDMA0's channel 0 */
module_param(vdma, int, 0644);
MODULE_PARM_DESC(vdma, "play through the firmware PCM with this id (100..), 0: channel 0");
static struct vdma *cur_vdma;			/* set at open while vdma is used */
static u32 log_iova;				/* the DSP's text log (ABOX_REPORT_LOG) */
static u32 comp_iova[16];			/* the DSP's components (ABOX_REPORT_COMPONENT) */
static int comp_count;

/* Samsung's ABOX_LOG_BUFFER: a ring of text the firmware writes */
struct log_buffer {
	u32 index_writer, index_reader, size;
	char buffer[];
};

static unsigned int dsplog_skip;
module_param(dsplog_skip, uint, 0644);
MODULE_PARM_DESC(dsplog_skip, "dsplog ends this many bytes before the newest");

/* cat /sys/module/snd_zrn_abox_bx/parameters/dsplog: a page of the log, the
 * newest unless dsplog_skip says how far back */
static int dsplog_get(char *out, const struct kernel_param *kp)
{
	struct log_buffer *l;
	u32 size, w, n, i;

	if (!dram || log_iova < IOVA_DRAM || log_iova >= IOVA_DRAM + DRAM_SIZE - SZ_4K)
		return sysfs_emit(out, "no log\n");
	l = dram + (log_iova - IOVA_DRAM);
	size = READ_ONCE(l->size);
	w = READ_ONCE(l->index_writer);
	if (!size || size > DRAM_SIZE - (log_iova - IOVA_DRAM) || w >= size)
		return sysfs_emit(out, "bad log: size %u writer %u\n", size, w);
	n = min_t(u32, size, PAGE_SIZE - 2);
	for (i = 0; i < n; i++) {
		char c = l->buffer[(w + 2 * size - (dsplog_skip % size) - n + i) % size];

		out[i] = c ? c : ' ';
	}
	out[n] = '\n';
	return n + 1;
}
static const struct kernel_param_ops dsplog_ops = { .get = dsplog_get };
module_param_cb(dsplog, &dsplog_ops, NULL, 0444);

/* ---- the DSP --------------------------------------------------------------- */

static void pmu_update(u32 reg, u32 mask, u32 val)
{
	writel((readl(pmu + reg) & ~mask) | (val & mask), pmu + reg);
}

static void cores_off(void)
{
	int i;

	for (i = 0; i < 2; i++)
		pmu_update(core_power[i], 0x1, 0x0);
	for (i = 0; i < 2; i++)
		if (readl(pmu + core_power[i] + 4) & 0x1)
			msleep(5);
}

/* the S9 driver's page tables: 1 MB sections where they line up, else 4K pages */
static int mmu_map_page(unsigned long iova, phys_addr_t pa)
{
	u32 *l1 = lv1, *l2;
	u32 link = l1[iova >> 20];
	phys_addr_t table;

	if (link == (((u32)(lv2_pa >> 4)) | 1)) {
		if (lv2_used == LV2_TABLES)
			return -ENOMEM;
		lv2_used++;
		table = lv2_pa + lv2_used * SZ_1K;	/* table 0 stays the empty one */
		l1[iova >> 20] = (u32)(table >> 4) | 1;
	} else {
		table = (phys_addr_t)(link & ~0xfU) << 4;
	}
	l2 = lv2 + (table - lv2_pa);
	l2[(iova >> 12) & 0xff] = (u32)(pa >> 4) | (3 << 2) | 2;
	return 0;
}

static int mmu_map(unsigned long iova, phys_addr_t pa, size_t size)
{
	size_t off;
	int ret;

	for (off = 0; off < size; ) {
		if (!((iova + off) & (SZ_1M - 1)) && !((pa + off) & (SZ_1M - 1)) &&
				size - off >= SZ_1M) {
			((u32 *)lv1)[(iova + off) >> 20] = (u32)((pa + off) >> 4) | (3 << 4) | 2;
			off += SZ_1M;
			continue;
		}
		ret = mmu_map_page(iova + off, pa + off);
		if (ret)
			return ret;
		off += SZ_4K;
	}
	return 0;
}

/* Samsung's abox_msg.h, packed */
struct msg_cmd {
	s32 id, cmd, arg[3];
	u32 data_idx;
	u64 send, recv, time_put, time_get;
} __packed;
struct msg_queue {
	s32 cmd_s, cmd_e;
	struct msg_cmd elem[16];
	s32 data_s, data_e;
	u32 data_len;
	u8 data[];
} __packed;

static struct msg_queue *msg_q(u32 off)
{
	return dram + off;
}

static void msg_init(void)
{
	u32 len = MSG_AREA - sizeof(struct msg_queue);

	msg_q(MSG_TX)->data_len = len;
	msg_q(MSG_RX)->data_len = len;
}

/* abox_ipc_send + abox_msg_send: the message, padded to a whole ABOX_IPC_MSG
 * as Samsung's always is, into the data ring, a command pointing at it, then
 * SGI 15 to the DSP. Called from the stream's trigger with interrupts off, so
 * it never sleeps. */
#define IPC_MSG_BYTES		768	/* sizeof(ABOX_IPC_MSG) */
static int ipc_send(const u32 *words, int n)
{
	struct msg_queue *q = msg_q(MSG_TX);
	struct msg_cmd c = { .id = words[0], .cmd = words[2] };
	u32 size = 4 + IPC_MSG_BYTES, blank;
	unsigned long flags;
	int ret = 0, tries;

	if (!booted)
		return -ENODEV;
	/* the command's arguments, as abox_ipc.c fills them for each kind */
	if (words[0] != IPC_ERAP)
		c.arg[0] = n > 3 ? words[3] : 0;	/* PCM: the channel */
	if (words[0] == IPC_SYSTEM || words[0] == IPC_ABOX_CONFIG)
		c.arg[1] = n > 4 ? words[4] : 0;
	if (words[0] == IPC_SYSTEM)
		c.arg[2] = n > 5 ? words[5] : 0;
	c.time_put = ktime_get_ns();
	spin_lock_irqsave(&tx_lock, flags);
	/* a full ring waits for the DSP to take some, ringing it again as
	 * abox_ipc_retry does (the start-up burst fills all 16 commands, and a
	 * dropped OPEN leaves the stream closed for good). Only once a
	 * millisecond: stock abox_msg_send spins without ringing, and a ring
	 * every 10 us made the firmware report INTERRUPT STORMING, overflow its
	 * ISR queue and stop taking commands until reboot (11-14 14:48). */
	for (tries = 0; ; tries++) {
		if (READ_ONCE(q->data_s) > q->data_e) {
			blank = q->data_s - q->data_e;
		} else {
			blank = q->data_len - q->data_e;
			if (blank < size) {
				q->data_e = 0;
				blank = READ_ONCE(q->data_s);
			}
		}
		if (blank >= size && READ_ONCE(q->cmd_s) != (q->cmd_e + 1) % 16)
			break;
		if (tries == 2000) {	/* 2000 x 10 us */
			ret = -EBUSY;
			goto out;
		}
		if (tries % 100 == 0)
			writel(BIT(16) | MSG_IRQ, gicd + 0xf00);
		spin_unlock_irqrestore(&tx_lock, flags);
		udelay(10);
		spin_lock_irqsave(&tx_lock, flags);
	}
	if (tries)
		pr_info("snd-zrn-abox-bx: message %u/%u waited %d0 us for room\n",
			words[0], words[2], tries);
	c.data_idx = q->data_e;
	*(s32 *)&q->data[q->data_e] = IPC_MSG_BYTES;
	memcpy(&q->data[q->data_e + 4], words, n * 4);
	memset(&q->data[q->data_e + 4 + n * 4], 0, IPC_MSG_BYTES - n * 4);
	q->data_e = (q->data_e + size) % q->data_len;
	q->elem[q->cmd_e] = c;
	mb();
	q->cmd_e = (q->cmd_e + 1) % 16;
	mb();
	writel(BIT(16) | MSG_IRQ, gicd + 0xf00);
out:
	spin_unlock_irqrestore(&tx_lock, flags);
	if (ret)
		pr_err("snd-zrn-abox-bx: no room for message %u/%u\n", words[0], words[2]);
	return ret;
}

static int dma_msg(u32 ipc, u32 ch, u32 type, u32 a, u32 b, u32 c)
{
	u32 m[7] = { ipc, ch, type, ch, a, b, c };	/* task id = channel */

	return ipc_send(m, 7);
}

static int pcm_msg(u32 type, u32 a, u32 b, u32 c)
{
	/* RDMA0, or the firmware PCM */
	return dma_msg(IPC_PCMPLAYBACK, cur_vdma ? cur_vdma->id : 0, type, a, b, c);
}

/* WDMA3 (capture, 2 x 32 bit) and RDMA7 (playback, 4 x 32 bit): realtime
 * streams as abox_wdma.c / abox_rdma.c drive them, buffers the DSP fills */
#define RT_W(t, a, b, c)	(rt_streams & 1 ? dma_msg(IPC_PCMCAPTURE, 3, t, a, b, c) : 0)
/* With firmware PCM 100 playing, RDMA0 is still a stream of its own (output
 * channel 0, its ring in hw0) whose source is VPCMOUT_DAI0 (RDMA0_A): the DSP
 * copies 100's periods into it, and only with both open does RDMA0 run. */
#define HW0(t, a, b, c)		(cur_vdma ? dma_msg(IPC_PCMPLAYBACK, 0, t, a, b, c) : 0)
#define RT_R(t, a, b, c)	(rt_streams & 2 ? dma_msg(IPC_PCMPLAYBACK, 7, t, a, b, c) : 0)

static int erap_msg(u32 ch, u32 type, u32 on)
{
	/* Samsung reuses the trigger's message: channel and trigger stay in it */
	u32 m[5] = { IPC_ERAP, ch, type, ch, on };

	return ipc_send(m, 5);
}

/* The realtime streams run while the AP's stream or a call needs them: stock's
 * HAL keeps spkamp_reference/playback open through a call. zcell plays
 * silence through the call for that; this keeps them (and the amps) running
 * should that player stop. */
static bool rt_is_open, rt_run, ap_open, ap_run;

static void rt_open(void)
{
	if (rt_is_open)
		return;
	RT_W(PCM_PLTDAI_OPEN, 0, 0, 0);
	RT_R(PCM_PLTDAI_OPEN, 0, 0, 0);
	rt_is_open = true;
}

static int config_msg(u32 type, u32 a, u32 b);

static void rt_hw_params(void)
{
	int wch = rt_wch == 4 ? 4 : 2;
	u32 sch = sifs0_ch == 4 ? 4 : 2;

	writel((readl(sfr + ABOX_SPUS_CTRL1) & ~0x1fU) | (3 << 3) | (sch - 1),
	       sfr + ABOX_SPUS_CTRL1);
	writel(DIV_ROUND_CLOSEST(12288000U * 2, sch * RATE) - 1, sfr + ABOX_SIFS0_CNT);
	config_msg(SET_SIFS0_FORMAT, 32, sch);

	RT_W(PCM_SET_BUFFER, IOVA_WDMA3, RT_PERIOD_FRAMES * wch * 4, RT_PERIODS);
	RT_W(PCM_PLTDAI_HW_PARAMS, RATE, rt_wbits == 32 ? 32 : 24, wch);
	RT_R(PCM_SET_BUFFER, IOVA_RDMA7, RT_PERIOD_FRAMES * 4 * 4, RT_PERIODS);
	RT_R(PCM_PLTDAI_HW_PARAMS, RATE, rt_rbits == 24 ? 24 : 32, 4);
}

static void rt_prepare(void)
{
	memset(rt_w, 0, RT_BYTES);
	memset(rt_r, 0, RT_BYTES);
	RT_W(PCM_PLTDAI_PREPARE, 0, 0, 0);
	RT_R(PCM_PLTDAI_PREPARE, 0, 0, 0);
}

/* How long WDMA3 runs before RDMA7 starts, in us (0: RDMA7 first, as before).
 * The firmware's chconv4ch counts filled RDMA7 periods ([0x80a73ee0+0xe4]):
 * WDMA3's in-full adds one (chconv4ch_source_update, a negative count is
 * clamped to 0 first), every RDMA7 period end takes one (sink_update); at -1
 * it reports the underrun (0x800fada8(7,0)) and in a call the VSS answers
 * with its margin emergency: FEIN1 (WDMA3) stopped and resumed. Resumed in
 * RDMA7's interrupt, WDMA3's periods end ~0.4-0.8 ms after RDMA7's, so the
 * count never gets ahead again and every second RDMA7 period underruns: the
 * 20 ms loop of a call (log.md 10.11 13:07). WDMA3 started a period ahead
 * keeps the count one period up, so a restart costs no underrun. Read at
 * every stream start. */
static int rt_lead_us;
module_param(rt_lead_us, int, 0644);
MODULE_PARM_DESC(rt_lead_us, "start WDMA3 this long before RDMA7, us (0: RDMA7 first)");

static DEFINE_SPINLOCK(rt_lock);
static bool rt_r_on, rt_r_pending;
static unsigned int rt_gen;
static int rt_lead_now;

/* RDMA7's trigger, then (after WDMA3's REALTIME_START: with RDMA7's sent
 * first RDMA7 never ran, 10.11 13:24) its REALTIME_START */
static void rt_r_trigger(void)
{
	RT_R(PCM_PLTDAI_TRIGGER, 1, 0, 0);
}

static void rt_r_erap(void)
{
	/* stop sends RDMA7 REALTIME_STOP: without a start to match, RDMA7
	 * stayed off after the first stream (the amps got nothing) */
	if (rt_streams & 2)
		erap_msg(7, REALTIME_START, 1);
	rt_r_on = true;
}

static void rt_r_start(void)
{
	rt_r_trigger();
	rt_r_erap();
}

static void rt_lead_fn(struct work_struct *w)
{
	unsigned long flags;
	unsigned int gen;

	spin_lock_irqsave(&rt_lock, flags);
	gen = rt_gen;
	spin_unlock_irqrestore(&rt_lock, flags);
	usleep_range(rt_lead_now, rt_lead_now + 200);
	spin_lock_irqsave(&rt_lock, flags);
	if (rt_r_pending && gen == rt_gen) {
		rt_r_pending = false;
		rt_r_start();
	}
	spin_unlock_irqrestore(&rt_lock, flags);
}
static DECLARE_WORK(rt_lead_work, rt_lead_fn);

/* sink first on start (unless rt_lead_us), last on stop */
static void rt_trigger(bool on)
{
	unsigned long flags;

	if (on == rt_run)
		return;
	rt_run = on;
	spin_lock_irqsave(&rt_lock, flags);
	rt_gen++;
	rt_r_pending = false;
	if (on) {
		rt_lead_now = clamp(READ_ONCE(rt_lead_us), 0, 40000);
		/* rt_lead_us 0: exactly the old order, RDMA7 trigger, WDMA3
		 * trigger, WDMA3 REALTIME_START, RDMA7 REALTIME_START */
		if (!rt_lead_now)
			rt_r_trigger();
		RT_W(PCM_PLTDAI_TRIGGER, 1, 0, 0);
		if (rt_streams & 1)
			erap_msg(3, REALTIME_START, 1);
		if (!rt_lead_now)
			rt_r_erap();
		if (rt_lead_now) {
			rt_r_pending = true;
			schedule_work(&rt_lead_work);
		}
	} else {
		RT_W(PCM_PLTDAI_TRIGGER, 0, 0, 0);
		if (rt_streams & 1)
			erap_msg(3, REALTIME_STOP, 0);
		if (rt_r_on) {
			RT_R(PCM_PLTDAI_TRIGGER, 0, 0, 0);
			if (rt_streams & 2)
				erap_msg(7, REALTIME_STOP, 0);
			rt_r_on = false;
		}
	}
	spin_unlock_irqrestore(&rt_lock, flags);
}

static void rt_close(void)
{
	if (!rt_is_open)
		return;
	rt_is_open = false;
	RT_W(PCM_PLTDAI_HW_FREE, 0, 0, 0);
	RT_R(PCM_PLTDAI_HW_FREE, 0, 0, 0);
	RT_W(PCM_PLTDAI_CLOSE, 0, 0, 0);
	RT_R(PCM_PLTDAI_CLOSE, 0, 0, 0);
}

/* echo 1 > call: stock's voice call streams, as libaudioproxy's
 * proxy_start_voice_call opens them (pcm_config_voicerx_playback and
 * voicetx_capture: 2 ch, 32 kHz, S16, 4 x 480 frames, never stopped by an
 * underrun, so no data is written): "Voice Call RX" on RDMA4 and "Voice Call
 * TX Direct" on WDMA2, the two DMAs stock's DT gives samsung,type "call".
 * The firmware joins them to the VSS (its VHMPcmOutThread takes RDMA4, the
 * decoded downlink; VHMPcmInThread WDMA2); their buffers are only for its
 * DMAs. call_rate: stock's 32 kHz needs SPUS ASRC4 to reach the 48 kHz
 * mixer, so 48 kHz here. echo 0 stops and closes them. */
static int call_rate = 48000;
module_param(call_rate, int, 0644);
MODULE_PARM_DESC(call_rate, "the voice call streams' rate (stock 32000)");
/* stock's pcm_config_voicerx_playback / voicetx_capture (libaudioproxy):
 * 480 frames x 4 at 32 kHz; 10 ms periods gave VSS margin emergencies */
static int call_period = 320;	/* 10 ms at 32 kHz, until tested */
module_param(call_period, int, 0644);
MODULE_PARM_DESC(call_period, "the voice call streams' period in frames (stock 480)");
#define CALL_PERIOD_BYTES	(clamp(call_period, 80, 960) * 2 * 2)
#define CALL_BYTES		SZ_16K
#define CALL_R(t, a, b, c)	dma_msg(IPC_PCMPLAYBACK, 4, t, a, b, c)
#define CALL_W(t, a, b, c)	dma_msg(IPC_PCMCAPTURE, 2, t, a, b, c)
static int call_on;

/*
 * WDMA4 through a call (call_vi=1). The VSS runs its uplink (ISR_WDMAInt ->
 * UlVoiceDmaProc -> EnquePreDlyBuffer, the copy of WDMA2's samples SeTx
 * reads) only when callBackForWdma2Proc (vss_interface+0x18) pairs with its
 * own WDMA2 message, and the firmware calls that callback from one place:
 * refext4ch_source_update (calliope 0x800cda94), the echo reference built
 * from REFEXT4CH's source, WDMA4 (stock's route-wdma4-to-refext4ch; stock DT:
 * WDMA4 "vi-sensing", the amps' feedback, NSRC3 <- UAIF1, SIFM3 48k 32 x 4).
 * We never opened WDMA4: the PreDly ring stayed empty, SeTx encoded zeros
 * (vssdump 11.11: PreDlyWrIdx 0, enqueue count 0xa0431920 = 0). A plain
 * capture, as abox_wdma.c drives PLATFORM_VI_SENSING (no ERAP message).
 */
static int call_vi = 1;	/* the uplink needs it (log.md 11.11 16:46) */
module_param(call_vi, int, 0644);
MODULE_PARM_DESC(call_vi, "run WDMA4 (vi-sensing, REFEXT4CH's source) through a call");
static bool vi_run;
#define VI_W(t, a, b, c)	dma_msg(IPC_PCMCAPTURE, 4, t, a, b, c)
#define VI_BYTES		SZ_64K

static void vi_start(void)
{
	int ret;

	if (vi_run || !READ_ONCE(call_vi) || !wdma4_buf)
		return;
	memset(wdma4_buf, 0, VI_BYTES);
	ret = VI_W(PCM_PLTDAI_OPEN, 0, 0, 0) ?:
	      VI_W(PCM_SET_BUFFER, IOVA_WDMA4, RT_PERIOD_FRAMES * 4 * 4, RT_PERIODS) ?:
	      VI_W(PCM_PLTDAI_HW_PARAMS, RATE, 32, 4) ?:
	      VI_W(PCM_PLTDAI_PREPARE, 0, 0, 0) ?:
	      VI_W(PCM_PLTDAI_TRIGGER, 1, 0, 0);
	vi_run = true;
	pr_info("snd-zrn-abox-bx: WDMA4 (vi-sensing) on (%d)\n", ret);
}

static void vi_stop(void)
{
	if (!vi_run)
		return;
	VI_W(PCM_PLTDAI_TRIGGER, 0, 0, 0);
	VI_W(PCM_PLTDAI_HW_FREE, 0, 0, 0);
	VI_W(PCM_PLTDAI_CLOSE, 0, 0, 0);
	vi_run = false;
	pr_info("snd-zrn-abox-bx: WDMA4 (vi-sensing) off\n");
}

static void amps_apply(void);
static void amps_power(bool on);
static void uaif_on(void);
static void uaif_off(void);

/* a call keeps the realtime streams and UAIF1 running without the AP's
 * stream; when it ends, they stop unless the AP's stream runs */
static void rt_for_call(bool on)
{
	if (!graph)
		return;
	if (on) {
		if (rt_run)
			return;
		rt_open();
		rt_hw_params();
		rt_prepare();
		uaif_on();
		rt_trigger(true);
	} else if (!READ_ONCE(ap_run)) {
		rt_trigger(false);
		if (!READ_ONCE(ap_open)) {
			rt_close();
			amps_power(false);
			uaif_off();
		}
	}
}

static int call_set(const char *arg, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(arg, &on);

	if (ret)
		return ret;
	if (!booted || !call_r || !call_w)
		return -ENODEV;
	if (on == !!call_on)
		return 0;
	if (on) {
		WRITE_ONCE(call_on, 1);
		memset(call_r, 0, CALL_BYTES);
		memset(call_w, 0, CALL_BYTES);
		ret = CALL_R(PCM_PLTDAI_OPEN, 0, 0, 0) ?:
		      CALL_W(PCM_PLTDAI_OPEN, 0, 0, 0) ?:
		      CALL_R(PCM_SET_BUFFER, IOVA_RDMA4, CALL_PERIOD_BYTES, 4) ?:
		      CALL_R(PCM_PLTDAI_HW_PARAMS, call_rate, 16, 2) ?:
		      CALL_W(PCM_SET_BUFFER, IOVA_WDMA2, CALL_PERIOD_BYTES, 4) ?:
		      CALL_W(PCM_PLTDAI_HW_PARAMS, call_rate, 16, 2) ?:
		      CALL_R(PCM_PLTDAI_PREPARE, 0, 0, 0) ?:
		      CALL_W(PCM_PLTDAI_PREPARE, 0, 0, 0) ?:
		      CALL_R(PCM_PLTDAI_TRIGGER, 1, 0, 0) ?:
		      CALL_W(PCM_PLTDAI_TRIGGER, 1, 0, 0);
		/* after the VSS has its streams (zcell's silence has them running
		 * by then, as in the calls that worked) */
		rt_for_call(true);
		vi_start();
	} else {
		vi_stop();
		CALL_W(PCM_PLTDAI_TRIGGER, 0, 0, 0);
		CALL_R(PCM_PLTDAI_TRIGGER, 0, 0, 0);
		CALL_W(PCM_PLTDAI_HW_FREE, 0, 0, 0);
		CALL_R(PCM_PLTDAI_HW_FREE, 0, 0, 0);
		CALL_W(PCM_PLTDAI_CLOSE, 0, 0, 0);
		ret = CALL_R(PCM_PLTDAI_CLOSE, 0, 0, 0);
		WRITE_ONCE(call_on, 0);
		rt_for_call(false);
	}
	pr_info("snd-zrn-abox-bx: call streams %s (%d)\n", on ? "on" : "off", ret);
	amps_apply();		/* call_amp_mask while the call runs */
	return ret;
}
static const struct kernel_param_ops call_ops = { .set = call_set, .get = param_get_int };
module_param_cb(call, &call_ops, &call_on, 0644);

/*
 * The loudest sample in up to 8 KB of a DMA's buffer, read where the DMA
 * really points. In our calls the VSS points RDMA4 and WDMA2 into ABOX SRAM
 * (0x18d29f10 and 0x18d29010, 0x280 bytes each: call-1020-0035, call-1031-0832,
 * call-1031-0957), not at the buffers the driver gave them; ztOS found the
 * same on the S9 (its own buffers stayed silent through a heard call, 34e3d32)
 * and looks in the CP's VSS instead. Here: ABOX SRAM, the driver's buffers,
 * the CP's VSS; -1 elsewhere. WIDE: 32-bit samples (WDMA3, RDMA7), their top
 * 16 bits.
 */
static int dma_peak(u32 str, u32 end, bool wide)
{
	static void **const own_buf[] = { &call_r, &call_w, &rt_w, &rt_r };
	static const unsigned long own_iova[] = { IOVA_RDMA4, IOVA_WDMA2, IOVA_WDMA3, IOVA_RDMA7 };
	static const u32 own_size[] = { CALL_BYTES, CALL_BYTES, RT_BYTES, RT_BYTES };
	void __iomem *io = NULL;
	const void *mem = NULL;
	u32 n, i, step = wide ? 4 : 2;
	bool mapped = false;
	int k, v, peak = 0;

	str &= ~(step - 1);
	if (end <= str)
		return -1;
	n = min_t(u32, end - str, SZ_8K) & ~(step - 1);
	if (str >= ABOX_SRAM && str + n <= ABOX_SRAM + ABOX_SRAM_SIZE)
		io = sram + (str - ABOX_SRAM);
	for (k = 0; !io && k < ARRAY_SIZE(own_buf); k++)
		if (*own_buf[k] && str >= own_iova[k] && str + n <= own_iova[k] + own_size[k])
			mem = *own_buf[k] + (str - own_iova[k]);
	if (!io && !mem) {
		if (!vss_cp || str < IOVA_VSS || str + n > IOVA_VSS + CP_VSS_SIZE)
			return -1;
		io = ioremap(CP_VSS_PA + (str - IOVA_VSS), n);	/* modem-if, no-map */
		if (!io)
			return -1;
		mapped = true;
	}
	for (i = 0; i < n; i += step) {
		if (mem)
			v = wide ? READ_ONCE(*(const s32 *)(mem + i)) >> 16 : READ_ONCE(*(const s16 *)(mem + i));
		else
			v = wide ? (s32)readl(io + i) >> 16 : (s16)readw(io + i);
		peak = max(peak, abs(v));
	}
	if (mapped)
		iounmap(io);
	return peak;
}

/*
 * How a call's path stands (ztOS a9bcb04 and 34e3d32's call_diag, on this
 * chip's registers), for audio/test-call-path.sh and zcell's call log: the
 * call streams, the VSS and the word it marks (vss_mark_clear() zeroes it),
 * where NSRC0 and NSRC1 take the uplink from (ROUTE_CTRL1 nibbles: 1 SIFS0,
 * zcall-uplink.sh's stand-in; 8 UAIF0, the microphones once the CS47L93 is
 * up -- stock's UAIF1 is 9), ROUTE_CTRL2 and SPUM_CTRL0 (the S9's echo
 * reference sets those; ours is WDMA3), UAIF0/1, and for each DMA of the call
 * its status, the buffer it points at and the loudest sample there: rx RDMA4
 * (the decoded downlink), tx WDMA2 (the uplink the VSS takes), ref WDMA3 (the
 * speaker graph's copy, the VSS's FEIN1, our echo reference), spk RDMA7 (what
 * the amplifiers get). It only reads, and only the running ABOX.
 */
/* WDMA4's loudest sample on one of its four 32-bit channels (call_vi): ch 0
 * the amp's ASPTX1 (what it plays, the echo reference), ch 2 its VMON */
static int vi_peak(int ch)
{
	const s32 *w = wdma4_buf;
	int i, v, peak = 0;

	if (!w || !vi_run)
		return -1;
	for (i = ch; i < VI_BYTES / 4; i += 4) {
		v = abs(READ_ONCE(w[i]) >> 16);
		peak = max(peak, v);
	}
	return peak;
}

static int call_diag_get(char *buf, const struct kernel_param *kp)
{
	u32 r4 = ABOX_RDMA(4), w2 = ABOX_WDMA(2), w3 = ABOX_WDMA(3), r7 = ABOX_RDMA(7);
	u32 route1, mark = 0;
	void __iomem *io;

	if (!booted)
		return sysfs_emit(buf, "off\n");
	if (vss_cp) {
		io = ioremap(CP_VSS_PA + CP_VSS_MAGIC, 4);	/* modem-if, no-map */
		if (io) {
			mark = readl(io);
			iounmap(io);
		}
	}
	route1 = readl(sfr + ABOX_ROUTE_CTRL1);
	return sysfs_emit(buf,
		"call=%d vss_cp=%d mark=%08x nsrc0=%u nsrc1=%u route2=%08x spum0=%08x uaif0=%08x uaif1=%08x "
		"rdma4=%08x rx_buf=%08x rx=%d wdma2=%08x tx_buf=%08x tx=%d "
		"wdma3=%08x ref=%d rdma7=%08x spk=%d vi0=%d vi1=%d vi2=%d\n",
		READ_ONCE(call_on), vss_cp, mark, route1 & 0xf, (route1 >> 4) & 0xf,
		readl(sfr + ABOX_ROUTE_CTRL2), readl(sfr + ABOX_SPUM_CTRL0),
		readl(sfr + ABOX_UAIF0_CTRL0), readl(sfr + ABOX_UAIF1_CTRL0),
		readl(sfr + r4 + DMA_STATUS), readl(sfr + r4 + DMA_BUF_STR),
		dma_peak(readl(sfr + r4 + DMA_BUF_STR), readl(sfr + r4 + DMA_BUF_END), false),
		readl(sfr + w2 + DMA_STATUS), readl(sfr + w2 + DMA_BUF_STR),
		dma_peak(readl(sfr + w2 + DMA_BUF_STR), readl(sfr + w2 + DMA_BUF_END), false),
		readl(sfr + w3 + DMA_STATUS),
		dma_peak(readl(sfr + w3 + DMA_BUF_STR), readl(sfr + w3 + DMA_BUF_END), true),
		readl(sfr + r7 + DMA_STATUS),
		dma_peak(readl(sfr + r7 + DMA_BUF_STR), readl(sfr + r7 + DMA_BUF_END), true),
		vi_peak(0), vi_peak(1), vi_peak(2));
}
static const struct kernel_param_ops call_diag_ops = { .get = call_diag_get };
module_param_cb(call_diag, &call_diag_ops, NULL, 0444);

/*
 * The firmware's own controls ("components"): each one it reports is a
 * descriptor in its DRAM (Samsung's ABOX_COMPONENT_DESCRIPTIOR, 32-bit
 * firmware, the control's texts union 8-byte aligned), set through
 * ABOX_UPDATE_COMPONENT_CONTROL. The stock HAL's mixer_paths sets some at
 * start; "RDMA0_EN" On is what lets RDMA0 run at all.
 */
struct comp_ctrl {
	u32 type, id;
	char name[16];
	u32 count;
	s32 min, max;
	u32 flags;
	u32 texts, texts_hi;
} __packed;
struct comp_desc {
	u32 id;
	char name[16];
	u32 count;
	struct comp_ctrl ctrl[];
} __packed;

static void *dsp_addr(u32 iova, size_t size)
{
	if (iova < IOVA_DRAM || iova - IOVA_DRAM + size > DRAM_SIZE)
		return NULL;
	return dram + (iova - IOVA_DRAM);
}

#define VOID_COMP_ID	0xab0c
#define VOID_DT_OFF	0x11fe000		/* calliope2.dt in DRAM, as in fws[] */

static struct comp_ctrl *comp_find(const char *name, u32 *desc_id)
{
	struct comp_ctrl *c;
	int i, j;

	for (i = 0; i < comp_count; i++) {
		struct comp_desc *d = dsp_addr(comp_iova[i], sizeof(*d));

		if (!d || d->count > 256 || !dsp_addr(comp_iova[i], sizeof(*d) + d->count * sizeof(d->ctrl[0])))
			continue;
		for (j = 0; j < d->count; j++)
			if (!strncmp(d->ctrl[j].name, name, sizeof(d->ctrl[j].name))) {
				*desc_id = d->id;
				return &d->ctrl[j];
			}
	}
	/* calliope2.dt's controls (RDMA0_EN among them) the DSP never reports:
	 * Samsung's abox_register_void_component() sets them under its own
	 * descriptor id, 0xAB0C; the file is the bare array, ended by id 0 */
	for (c = dram + VOID_DT_OFF; (void *)(c + 1) <= dram + DRAM_SIZE && c->id; c++)
		if (!strncmp(c->name, name, sizeof(c->name))) {
			*desc_id = VOID_COMP_ID;
			return c;
		}
	return NULL;
}

static void comps_log(void)
{
	int i, j;

	for (i = 0; i < comp_count; i++) {
		struct comp_desc *d = dsp_addr(comp_iova[i], sizeof(*d));

		if (!d || d->count > 256)
			continue;
		for (j = 0; j < d->count; j++) {
			struct comp_ctrl *c = &d->ctrl[j];
			const char *t = c->texts ? dsp_addr(c->texts, 1) : NULL;

			pr_info("snd-zrn-abox-bx: component %u %.16s: %u %.16s type %u count %u %d..%d %.120s\n",
				d->id, d->name, c->id, c->name, c->type, c->count, c->min, c->max,
				t && c->type == 1 ? t : "");
		}
	}
}

static int comp_set(const char *name, int val)
{
	u32 m[7] = { IPC_SYSTEM, 0, ABOX_UPDATE_COMPONENT_CONTROL };
	struct comp_ctrl *c = comp_find(name, &m[3]);

	if (!c) {
		pr_warn("snd-zrn-abox-bx: no DSP control %s\n", name);
		return -ENOENT;
	}
	m[4] = c->id;
	m[6] = val;		/* bundle.param_s32[0]: one value */
	pr_info("snd-zrn-abox-bx: DSP control %s = %d\n", name, val);
	return ipc_send(m, 7);
}

/* an enumerated control by the text of its value */
static int comp_set_enum(const char *name, const char *text)
{
	u32 desc_id;
	struct comp_ctrl *c = comp_find(name, &desc_id);
	const char *t = c && c->texts ? dsp_addr(c->texts, 1) : NULL;
	size_t len = strlen(text);
	int i = 0;

	while (t && *t && t < (const char *)dram + DRAM_SIZE - len) {
		if (!strncmp(t, text, len) && (t[len] == ',' || !t[len]))
			return comp_set(name, i);
		t = strchr(t, ',');
		if (!t)
			break;
		t++;
		i++;
	}
	pr_warn("snd-zrn-abox-bx: DSP control %s has no value %s\n", name, text);
	return -EINVAL;
}

/* echo NAME=VALUE > /sys/module/snd_zrn_abox_bx/parameters/dspctl (a number,
 * or an enumerated value's text) */
static int dspctl_set(const char *arg, const struct kernel_param *kp)
{
	char name[17], text[33];
	int val;

	if (sscanf(arg, "%16[^=]=%d", name, &val) == 2)
		return comp_set(name, val);
	if (sscanf(arg, "%16[^=]=%32s", name, text) == 2)
		return comp_set_enum(name, text);
	return -EINVAL;
}
static const struct kernel_param_ops dspctl_ops = { .set = dspctl_set };
module_param_cb(dspctl, &dspctl_ops, NULL, 0200);

/*
 * The voice firmware marks CP_VSS_MAGIC once it has come up. Samsung clears it
 * only at probe, and its DSP is not restarted under it. Here the word was never
 * cleared (it kept whatever the last VSS left, across warm reboots too), and
 * ztOS saw a stale mark keep RDMA4 idle while WDMA2 ran, the earpiece silent
 * (47f1a79, 30 Oct: the first call after a SoC sleep that followed a call) --
 * as in our calls whose VHMPcmOutThread had no frames (20.10 09:53). So it is
 * cleared at probe and before every ABOX_START_VSS. The link to the mark is
 * ztOS's inference; a real call decides.
 */
static void vss_mark_clear(const char *when)
{
	void __iomem *magic;

	if (!vss_cp)
		return;
	magic = ioremap(CP_VSS_PA + CP_VSS_MAGIC, 4);	/* modem-if, no-map */
	if (!magic)
		return;
	pr_info("snd-zrn-abox-bx: VSS mark was %08x: cleared (%s)\n", readl(magic), when);
	writel(0, magic);
	iounmap(magic);
}

/* echo "ipcid task msgtype a b c ..." > ipc: a raw message to the DSP (numbers
 * as kstrtou32 reads them, 0x for hex), for trying what the stock HAL sends.
 * zcell sends ABOX_START_VSS here ("1 0 0xa0"): the VSS mark is cleared first. */
static int ipc_set(const char *arg, const struct kernel_param *kp)
{
	char buf[160], *p = buf, *tok;
	u32 m[12];
	int n = 0;

	strscpy(buf, arg, sizeof(buf));
	while ((tok = strsep(&p, " \t\n")) && n < ARRAY_SIZE(m)) {
		if (!*tok)
			continue;
		if (kstrtou32(tok, 0, &m[n++]))
			return -EINVAL;
	}
	if (n < 3)
		return -EINVAL;
	if (m[0] == IPC_SYSTEM && m[2] == ABOX_START_VSS)
		vss_mark_clear("START_VSS");
	return ipc_send(m, n);
}
static const struct kernel_param_ops ipc_ops = { .set = ipc_set };
module_param_cb(ipc, &ipc_ops, NULL, 0200);

static int config_msg(u32 type, u32 a, u32 b)
{
	u32 m[6] = { IPC_ABOX_CONFIG, 0, type, a, b, 0 };

	return ipc_send(m, 6);
}

/* The firmware asks for a "gear" and waits to hear the core clock it got;
 * the clock is left as S-Boot set it (PLL / (DIV_PLL + 1)) and told. */
static u32 core_hz(void)
{
	return AUD_PLL_HZ / ((readl(cmu + CMU_DIV_PLL) & 0xf) + 1);
}

static void gear_tell(struct work_struct *work)
{
	u64 now = ktime_get_ns();
	u32 m[6] = { IPC_SYSTEM, 0, ABOX_CHANGED_GEAR, core_hz(), 0, 0 };

	m[5] = do_div(now, NSEC_PER_SEC);
	m[4] = (u32)now;
	ipc_send(m, 6);
}

/* abox_msg_recv, until the ring from the DSP is empty; true if a period went */
static bool rx_one(void)
{
	struct msg_queue *q = msg_q(MSG_RX);
	unsigned long flags;
	bool period = false;
	u32 w[24];

	spin_lock_irqsave(&rx_lock, flags);
	while (READ_ONCE(q->cmd_s) != READ_ONCE(q->cmd_e)) {
		struct msg_cmd *c = &q->elem[q->cmd_s];
		u32 idx = c->data_idx % q->data_len;
		s32 size = *(s32 *)&q->data[idx];

		memset(w, 0, sizeof(w));
		memcpy(w, &q->data[idx + 4], clamp_t(s32, size, 0, sizeof(w)));
		q->data_s = (idx + 4 + size) % q->data_len;
		q->cmd_s = (q->cmd_s + 1) % 16;
		if (debug > 0) {
			debug--;
			pr_info("snd-zrn-abox-bx: from DSP: id %u task %d: %u %08x %08x %08x (cmd %d/%d)\n",
				w[0], w[1], w[2], w[3], w[4], w[5], c->id, c->cmd);
		}
		if (w[0] == IPC_SYSTEM && w[2] == ABOX_BOOT_DONE) {
			fw_version = w[5];
			booted = true;
		} else if (w[0] == IPC_SYSTEM && w[2] == ABOX_REPORT_COMPONENT &&
			   comp_count < ARRAY_SIZE(comp_iova)) {
			comp_iova[comp_count++] = w[3];
		} else if (w[0] == IPC_SYSTEM && w[2] == ABOX_REPORT_LOG && w[3] == 0) {
			log_iova = w[4];
		} else if (w[0] == IPC_SYSTEM && w[2] == ABOX_CHANGE_GEAR) {
			schedule_work(&gear_work);
		} else if (w[0] == IPC_PCMPLAYBACK && w[2] == PCM_PLTDAI_POINTER &&
			   w[3] == (cur_vdma ? cur_vdma->id : 0)) {
			WRITE_ONCE(dsp_pointer, w[4]);
			period = true;
		} else if ((w[0] == IPC_PCMPLAYBACK || w[0] == IPC_PCMCAPTURE) &&
			   w[2] == PCM_PLTDAI_REGISTER && vdma_count < ARRAY_SIZE(vdmas)) {
			/* PCMTASK_HARDWARE: name[32], addr, width min/max, rate
			 * min/max, channels min/max, buffer max, period min/max,
			 * periods min/max */
			struct vdma *d = &vdmas[vdma_count++];

			d->id = w[3];
			d->capture = w[0] == IPC_PCMCAPTURE;
			memcpy(d->name, &w[4], 32);
			d->name[32] = 0;
			d->addr = w[12];
			d->bytes_max = w[19];
			d->period_min = w[20];
			d->period_max = w[21];
			d->periods_min = w[22];
			d->periods_max = w[23];
			pr_info("snd-zrn-abox-bx: firmware PCM %u %s '%s' at %08x: %u-%u bit, %u-%u Hz, %u-%u ch, buffer %u, period %u-%u x %u-%u\n",
				d->id, d->capture ? "capture" : "playback", d->name, d->addr,
				w[13], w[14], w[15], w[16], w[17], w[18], w[19], w[20], w[21], w[22], w[23]);
		}
	}
	spin_unlock_irqrestore(&rx_lock, flags);
	return period;
}

static irqreturn_t abox_isr(int irq, void *unused)
{
	irqreturn_t ret = IRQ_NONE;
	int i;

	for (i = 0; i < 32; i++) {
		u32 stat = readl(gicc + 0x00c), nr = stat & 0x3ff;

		if (nr >= 1020)
			break;
		writel(stat, gicc + 0x010);
		if (nr < 16)
			writel(stat, gicc + 0x1000);
		if (debug > 0 && nr != MSG_IRQ) {
			debug--;
			pr_info("snd-zrn-abox-bx: DSP interrupt %u (%08x)\n", nr, stat);
		}
		ret = IRQ_HANDLED;
	}
	/* outside rx_lock: an xrun here stops the stream, which sends a message */
	if (rx_one()) {
		struct snd_pcm_substream *s = READ_ONCE(active);

		if (s)
			snd_pcm_period_elapsed(s);
	}
	return ret;
}

static int map_irq(void)
{
	struct device_node *gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	struct irq_fwspec spec = { .param_count = 3, .param = { 0, ABOX_SPI, IRQ_TYPE_LEVEL_HIGH } };
	int irq;

	if (!gic)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	return irq > 0 ? irq : -EINVAL;
}

static void cmu_div(u32 reg, u32 ratio)
{
	int i;

	writel((readl(cmu + reg) & ~0xfU) | ratio, cmu + reg);
	for (i = 0; i < 1000 && (readl(cmu + reg) & BIT(16)); i++)
		udelay(10);
}

/* SPUS stream 0 from RDMA0 to SIFS1, SIFS1 to UAIF1, UAIF1 as I2S master at
 * 48 kHz, two 16-bit slots (abox_cmpnt_v20.c, abox_if.c) */
static void route_setup_stock(void);

static void route_setup(void)
{
	if (graph) {
		route_setup_stock();
		return;
	}
	/* stream 0: ASRC off, OUT 0 = SIFS1, IN 0 = RDMA (FUNC_CHAIN_SRC, 5 bits) */
	writel(readl(sfr + ABOX_SPUS_CTRL0) & ~0x1fU, sfr + ABOX_SPUS_CTRL0);
	/* SIFS1 takes SPUS OUT0 (SIFS_OUT_SEL(1), bits 14:12) */
	writel(readl(sfr + ABOX_SPUS_CTRL1) & ~0x7000U, sfr + ABOX_SPUS_CTRL1);
	/* UAIF1's speaker side listens to SIFS1 (ROUTE_UAIF_SPK(1) = 2) */
	writel((readl(sfr + ABOX_ROUTE_CTRL0) & ~0xf0U) | (2 << 4), sfr + ABOX_ROUTE_CTRL0);
	/* paced by UAIF1, not by a counter */
	writel(0, sfr + ABOX_SIFS1_CNT);
	/* RDMA0's SRAM bank, 512 bytes (periods over 10 ms) in 16-byte units */
	writel((readl(sfr + ABOX_SPUS_SBANK_RDMA0) & ~GENMASK(29, 20)) | (512 << 16),
	       sfr + ABOX_SPUS_SBANK_RDMA0);
	/* bit clock: PLL 1179.648 MHz / 16 = 73.728 MHz, / 48 = 1.536 MHz */
	cmu_div(CMU_DIV_AUDIF, 15);
	cmu_div(CMU_DIV_UAIF1, 47);
	writel(readl(cmu + CMU_MUX_UAIF1) & ~1U, cmu + CMU_MUX_UAIF1);
	/* gpb1-0..3 to the I2S function, pull-down as stock */
	writel((readl(pins + GPB1_CON) & ~0xffffU) | 0x2222, pins + GPB1_CON);
	/* format 9 (16 bit, 2 ch), BCLK polarity 1, I2S, slot max 1, 16-bit slots */
	writel((9 << 24) | BIT(23) | (1 << 18) | (15 << 12), sfr + ABOX_UAIF1_CTRL1);
	writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(2), sfr + ABOX_UAIF1_CTRL0);	/* master */
}

/* stock's media-speaker graph (see graph above); bit positions from Samsung's
 * abox_soc.h for ABOX 2.0.1 */
static void route_setup_stock(void)
{
	/* stream 0: ASRC off, IN 0 = RDMA, OUT 0 = SIFS0 (spus_outx_texts 1) */
	writel((readl(sfr + ABOX_SPUS_CTRL0) & ~0x1fU) | (1 << 1), sfr + ABOX_SPUS_CTRL0);
	/* stream 7 (SPUS_CTRL_FC1 bits 7:3): ASRC off, IN = RDMA, OUT = SIFS1 */
	writel(readl(sfr + ABOX_SPUS_CTRL_FC1) & ~0xf8U, sfr + ABOX_SPUS_CTRL_FC1);
	/* SIFS1 takes SPUS OUT7; SIFS0's mixer: 32 bit, 2 ch (MIXP_FORMAT; stock
	 * runs it at 24, but then WDMA3's samples are not sign-extended) */
	writel((readl(sfr + ABOX_SPUS_CTRL1) & ~(0x7000U | 0x1fU)) | (7 << 12) | 0x19,
	       sfr + ABOX_SPUS_CTRL1);
	/* UAIF1's speaker side listens to SIFS1; NSRC2 takes SIFS0; SIFM2 to WDMA;
	 * the amps' feedback: NSRC3 takes UAIF1 (nsrcx_texts 9), SIFM3 to WDMA
	 * (WDMA4: stock's route-uaif1-to-nsrc3, route-nsrc3-to-wdma4) */
	writel((readl(sfr + ABOX_ROUTE_CTRL0) & ~0xf0U) | (2 << 4), sfr + ABOX_ROUTE_CTRL0);
	writel((readl(sfr + ABOX_ROUTE_CTRL1) & ~0xff00U) | (1 << 8) | (9 << 12),
	       sfr + ABOX_ROUTE_CTRL1);
	writel(readl(sfr + ABOX_SPUM_CTRL0) & ~(BIT(15) | BIT(19)), sfr + ABOX_SPUM_CTRL0);
	/* SIFS0 is a capture end (into WDMA3), paced by its counter:
	 * sifsx_cnt_val(12.288 MHz, 48000, 32-bit physical, 2 ch) = 255 */
	writel(255, sfr + ABOX_SIFS0_CNT);
	writel(0, sfr + ABOX_SIFS1_CNT);	/* paced by UAIF1 */
	writel((readl(sfr + ABOX_SPUS_SBANK_RDMA0) & ~GENMASK(29, 20)) | (512 << 16),
	       sfr + ABOX_SPUS_SBANK_RDMA0);
	/* bit clock 48 kHz x 4 x 32 = 6.144 MHz: 73.728 MHz / 12 */
	cmu_div(CMU_DIV_AUDIF, 15);
	cmu_div(CMU_DIV_UAIF1, 11);
	writel(readl(cmu + CMU_MUX_UAIF1) & ~1U, cmu + CMU_MUX_UAIF1);
	writel((readl(pins + GPB1_CON) & ~0xffffU) | 0x2222, pins + GPB1_CON);
	/* stock uaif1: dsp_a, bitclock-inversion (BCLK_POLARITY 0, WS_MODE 1),
	 * format 0x1b (32 bit, 4 ch), SLOT_MAX 3, SBIT_MAX 31, VALID 31/31 */
	writel((0x1b << 24) | BIT(22) | (3 << 18) | (31 << 12) | (31 << 6) | 31,
	       sfr + ABOX_UAIF1_CTRL1);
	writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(2), sfr + ABOX_UAIF1_CTRL0);	/* master */
}

/* ---- the amplifiers' bus: i2c-algo-bit on gph0-6/7 ------------------------- */

static DEFINE_SPINLOCK(gpio_lock);

/* open drain by hand: DAT stays 0; low = output, high = input (pull-ups on the board) */
static void pin_set(int pin, int high)
{
	unsigned long flags;
	u32 con;

	spin_lock_irqsave(&gpio_lock, flags);
	con = readl(gpio + GPH0_CON) & ~(0xfU << (pin * 4));
	if (!high)
		con |= 1U << (pin * 4);
	writel(con, gpio + GPH0_CON);
	spin_unlock_irqrestore(&gpio_lock, flags);
}

static void bb_setsda(void *data, int state) { pin_set(AMP_SDA, state); }
static void bb_setscl(void *data, int state) { pin_set(AMP_SCL, state); }
static int bb_getsda(void *data) { return (readl(gpio + GPH0_DAT) >> AMP_SDA) & 1; }
static int bb_getscl(void *data) { return (readl(gpio + GPH0_DAT) >> AMP_SCL) & 1; }

static struct i2c_algo_bit_data bb_data = {
	.setsda = bb_setsda, .setscl = bb_setscl,
	.getsda = bb_getsda, .getscl = bb_getscl,
	.udelay = 5,		/* about 100 kHz */
	.timeout = HZ / 10,
};
static struct i2c_adapter bb_adap = {
	.owner = THIS_MODULE,
	.name = "zrn-abox-bx amplifiers (gph0-6/7)",
	.algo_data = &bb_data,
	.nr = AMP_BUS,
};
static bool bb_added;

static void bus_pins_setup(void)
{
	u32 m = (0xfU << (AMP_SCL * 4)) | (0xfU << (AMP_SDA * 4));

	writel(readl(gpio + GPH0_PUD) & ~m, gpio + GPH0_PUD);
	writel(readl(gpio + GPH0_DAT) & ~(BIT(AMP_SCL) | BIT(AMP_SDA)), gpio + GPH0_DAT);
	writel(readl(gpio + GPH0_CON) & ~m, gpio + GPH0_CON);
}

/* gph1-1 / gph1-2: the amplifiers' nRESET, high to run */
static void amps_reset(bool run)
{
	u32 bits = BIT(1) | BIT(2), m = (0xfU << 4) | (0xfU << 8);
	u32 dat = readl(gpio + GPH1_DAT) & ~bits;

	writel(dat | (run ? bits : 0), gpio + GPH1_DAT);
	writel((readl(gpio + GPH1_CON) & ~m) | (1 << 4) | (1 << 8), gpio + GPH1_CON);
}

/* ---- the amplifiers: CS35L40, through mainline's cs35l41 library ------------ */

struct amp {
	u8 addr;
	unsigned int slot;		/* the I2S slot it plays */
	const char *name;
	bool boost;			/* stock: boost on the speaker, off the earpiece */
	struct i2c_client *client;
	struct regmap *regmap;
	struct cs_dsp dsp;		/* the speaker protection (amp_dsp), else only read */
	bool on;
	const char *fw;			/* stock VENDOR's name: cs35l40-<fw>-dsp1-spk-prot.* */
	bool dsp_init, fw_loaded, dsp_failed;
	bool dsp_routed;		/* it plays the DSP's output (dsp_vol_boost) */
	bool dsp_reinit_due;		/* run, not yet REINIT with the amp on (dsp_run_reinit) */
	bool loading;			/* spk_load() running: no power-up meanwhile */
};
static struct amp amps[] = {
	{ AMP_BOT, 1, "bottom speaker", true, .fw = "bot" },
	{ AMP_RCV, 0, "earpiece", false, .fw = "rcv" },
};

/* which amps play: bit 0 the bottom speaker (stock media-speaker), bit 1 the
 * earpiece (handset calls, or stereo with both) */
static int amp_mask = 1;
static int amp_mask_set(const char *arg, const struct kernel_param *kp)
{
	int ret = param_set_int(arg, kp);

	if (!ret)
		amps_apply();
	return ret;
}
static const struct kernel_param_ops amp_mask_ops = { .set = amp_mask_set, .get = param_get_int };
module_param_cb(amp_mask, &amp_mask_ops, &amp_mask, 0644);
MODULE_PARM_DESC(amp_mask, "bit 0 bottom speaker, bit 1 earpiece (applied at once)");
/* stock's incall handset route: the call plays on the earpiece (Left amp) */
static int call_amp_mask = 2;
module_param_cb(call_amp_mask, &amp_mask_ops, &call_amp_mask, 0644);
MODULE_PARM_DESC(call_amp_mask, "amp_mask while call=1 (stock handset: 2, earpiece)");
static int amp_volume = -160;		/* -20 dB */
/* added to the volume of an amp playing through its protection (amp_dsp), up to
 * 0 dB; the others stay at AMP_VOL_MAX. 0.125 dB steps, 0 = as before */
static int dsp_vol_boost;

/* AMP_GAIN_PCM: 0 is mute (mainline's amp_gain_tlv), n = n - 0.5 dB, and it
 * resets to 0; stock leaves it to the DSP's protection tuning */
static int amp_gain = 20;		/* owner: bottom speaker fair at -6 dB */
module_param(amp_gain, int, 0444);
MODULE_PARM_DESC(amp_gain, "amplifier PCM gain, 1..20 (n - 0.5 dB), 0 mutes");
static DEFINE_MUTEX(amp_lock);
static struct work_struct amp_on_work;

/* mainline cs35l41.c's tables for what is used here */
static const struct reg_sequence pup_patch[] = {
	{ CS35L41_TEST_KEY_CTL, 0x00000055 },
	{ CS35L41_TEST_KEY_CTL, 0x000000AA },
	{ 0x00002084, 0x002F1AA0 },
	{ CS35L41_TEST_KEY_CTL, 0x000000CC },
	{ CS35L41_TEST_KEY_CTL, 0x00000033 },
};
static const struct reg_sequence pdn_patch[] = {
	{ CS35L41_TEST_KEY_CTL, 0x00000055 },
	{ CS35L41_TEST_KEY_CTL, 0x000000AA },
	{ 0x00002084, 0x002F1AA3 },
	{ CS35L41_TEST_KEY_CTL, 0x000000CC },
	{ CS35L41_TEST_KEY_CTL, 0x00000033 },
};
#define PLL_REFCLK_1536000	0x1b	/* cs35l41_pll_sysclk */
#define FS_MON_1536000		(52 | (84 << CS35L41_FS2_WINDOW_SHIFT))	/* cs35l41_fs_mon */
#define PLL_REFCLK_6144000	0x28
#define FS_MON_6144000		(16 | (24 << CS35L41_FS2_WINDOW_SHIFT))
#define GLOBAL_FS_48K		0x03	/* cs35l41_fs_rates */
#define BST_IPK_4100MA		(((4100 - 1600) / 50) + 0x10)	/* stock boost-peak-milliamp */

static void amp_volume_write(struct amp *a)
{
	int v = a->dsp_routed ? min(amp_volume + dsp_vol_boost, AMP_VOL_MAX_DSP) :
				min(amp_volume, AMP_VOL_MAX);

	regmap_update_bits(a->regmap, CS35L41_AMP_DIG_VOL_CTRL, CS35L41_AMP_PCM_VOL_MASK,
			   (v & 0x7ff) << CS35L41_AMP_PCM_VOL_SHIFT);
}

/* mainline cs35l41_probe and the codec's dai set-up, as far as used here */
static int amp_probe(struct amp *a)
{
	struct i2c_board_info info = { .addr = a->addr };
	struct device *dev;
	unsigned int devid, revid, status;
	unsigned int rx_slot = a->slot;
	int ret;

	strscpy(info.type, "cs35l40", sizeof(info.type));
	a->client = i2c_new_client_device(&bb_adap, &info);
	if (IS_ERR(a->client)) {
		ret = PTR_ERR(a->client);
		a->client = NULL;
		return ret;
	}
	dev = &a->client->dev;
	a->regmap = regmap_init_i2c(a->client, &cs35l41_regmap_i2c);
	if (IS_ERR(a->regmap)) {
		ret = PTR_ERR(a->regmap);
		a->regmap = NULL;
		return ret;
	}
	ret = regmap_read(a->regmap, CS35L41_DEVID, &devid);
	if (!ret)
		ret = regmap_read(a->regmap, CS35L41_REVID, &revid);
	if (ret || devid != CS35L41_CHIP_ID) {
		dev_err(dev, "no CS35L40 at %#x (%d, id %#x)\n", a->addr, ret, devid);
		return ret ? ret : -ENODEV;
	}
	ret = regmap_read_poll_timeout(a->regmap, CS35L41_IRQ1_STATUS4, status,
				       status & CS35L41_OTP_BOOT_DONE, 1000, 100000);
	if (ret) {
		dev_err(dev, "OTP boot did not finish\n");
		return ret;
	}
	ret = cs35l41_register_errata_patch(dev, a->regmap, revid);
	if (!ret)
		ret = cs35l41_otp_unpack(dev, a->regmap);
	if (ret)
		return ret;

	/* stock: boost peak current only, its own trims for the rest; DOUT hi-Z */
	regmap_update_bits(a->regmap, CS35L41_BSTCVRT_PEAK_CUR, CS35L41_BST_IPK_MASK, BST_IPK_4100MA);
	regmap_update_bits(a->regmap, CS35L41_SP_HIZ_CTRL, CS35L41_ASP_DOUT_HIZ_MASK, 3);
	/* the SoC drives both clocks, one slot in: graph=0 I2S with 16-bit slots
	 * and words; graph=1 as stock's uaif1, DSP_A with SCLK inverted, 32 bit */
	regmap_update_bits(a->regmap, CS35L41_SP_FORMAT,
			   CS35L41_SCLK_MSTR_MASK | CS35L41_LRCLK_MSTR_MASK | CS35L41_ASP_FMT_MASK |
			   CS35L41_LRCLK_INV_MASK | CS35L41_SCLK_INV_MASK | CS35L41_ASP_WIDTH_RX_MASK,
			   graph ? CS35L41_SCLK_INV_MASK | (32 << CS35L41_ASP_WIDTH_RX_SHIFT) :
				   (2 << CS35L41_ASP_FMT_SHIFT) | (16 << CS35L41_ASP_WIDTH_RX_SHIFT));
	regmap_update_bits(a->regmap, CS35L41_SP_RX_WL, CS35L41_ASP_RX_WL_MASK,
			   (graph ? 32 : 16) << CS35L41_ASP_RX_WL_SHIFT);
	cs35l41_set_channels(dev, a->regmap, 0, NULL, 1, &rx_slot);
	regmap_update_bits(a->regmap, CS35L41_GLOBAL_CLK_CTRL, CS35L41_GLOBAL_FS_MASK,
			   GLOBAL_FS_48K << CS35L41_GLOBAL_FS_SHIFT);
	/* the PLL runs from SCLK (component_set_sysclk, dai_set_sysclk) */
	regmap_update_bits(a->regmap, CS35L41_PLL_CLK_CTRL, CS35L41_PLL_OPENLOOP_MASK,
			   1 << CS35L41_PLL_OPENLOOP_SHIFT);
	regmap_update_bits(a->regmap, CS35L41_PLL_CLK_CTRL, CS35L41_REFCLK_FREQ_MASK,
			   (graph ? PLL_REFCLK_6144000 : PLL_REFCLK_1536000) << CS35L41_REFCLK_FREQ_SHIFT);
	regmap_update_bits(a->regmap, CS35L41_PLL_CLK_CTRL, CS35L41_PLL_CLK_EN_MASK, 0);
	regmap_update_bits(a->regmap, CS35L41_PLL_CLK_CTRL, CS35L41_PLL_CLK_SEL_MASK,
			   CS35L41_PLLSRC_SCLK);
	regmap_update_bits(a->regmap, CS35L41_PLL_CLK_CTRL, CS35L41_PLL_OPENLOOP_MASK, 0);
	regmap_update_bits(a->regmap, CS35L41_PLL_CLK_CTRL, CS35L41_PLL_CLK_EN_MASK,
			   1 << CS35L41_PLL_CLK_EN_SHIFT);
	regmap_write(a->regmap, CS35L41_TST_FS_MON0, graph ? FS_MON_6144000 : FS_MON_1536000);
	/* the amplifier plays ASPRX1 itself, not its DSP; ASPRX1 on */
	regmap_write(a->regmap, CS35L41_DAC_PCM1_SRC, CS35L41_INPUT_SRC_ASPRX1);
	regmap_update_bits(a->regmap, CS35L41_SP_ENABLES, BIT(CS35L41_ASP_RX1_EN_SHIFT),
			   BIT(CS35L41_ASP_RX1_EN_SHIFT));
	/* the echo reference (stock mixer_paths dev-handset): ASPTX1 the samples
	 * the amp plays (ASPRX1), ASPTX2 VMON, 32-bit words in 32-bit slots, back
	 * over UAIF1 -> NSRC3 -> WDMA4 (call_vi) -> REFEXT4CH, the VSS's echo
	 * canceller's reference. Without it the far end heard itself on the
	 * speaker and the uplink broke up (owner, 11.11 18:15). Slots and
	 * enables: amp_tx(), with the amp. */
	if (graph) {
		regmap_update_bits(a->regmap, CS35L41_SP_FORMAT, CS35L41_ASP_WIDTH_TX_MASK,
				   32 << CS35L41_ASP_WIDTH_TX_SHIFT);
		regmap_update_bits(a->regmap, CS35L41_SP_TX_WL, CS35L41_ASP_TX_WL_MASK,
				   32 << CS35L41_ASP_TX_WL_SHIFT);
		regmap_write(a->regmap, CS35L41_ASP_TX1_SRC, CS35L41_INPUT_SRC_ASPRX1);
		regmap_write(a->regmap, CS35L41_ASP_TX2_SRC, CS35L41_INPUT_SRC_VMON);
	}
	amp_volume_write(a);
	regmap_update_bits(a->regmap, CS35L41_AMP_GAIN_CTRL, CS35L41_AMP_GAIN_PCM_MASK,
			   clamp(amp_gain, 0, CS35L41_AMP_GAIN_PCM_MAX) << CS35L41_AMP_GAIN_PCM_SHIFT);
	dev_info(dev, "CS35L40 rev %02x at %#x: %s, slot %u\n", revid, a->addr, a->name, a->slot);
	return 0;
}

/*
 * ---- the amps' own DSP: Cirrus speaker protection (CSPL) -------------------
 *
 * Stock plays the speaker through the CS35L40's HALO DSP running Cirrus'
 * speaker protection, which watches the coil (VMON/IMON) and keeps it in its
 * limits; only with it does stock go to 0 dB (AMP_VOL_MAX is -6 dB here
 * without it). The firmware and its tuning are stock VENDOR's own
 * (cs35l40-{bot,rcv}-dsp1-spk-prot.{wmfw,bin}, "BYX_18dB": tuned for AMP PCM
 * Gain 18), copied to /usr/lib/firmware by zvendor-firmware. As mainline's
 * cs35l41_hda.c does it (cs_dsp, no ASoC): loaded in the background after the
 * amps are set up (~26 KB per amp over the bit-banged bus, a few seconds),
 * started on the first play (it needs the bit clock), paused when the amp goes
 * off, loaded again after a resume (the amps lose power). Until it runs, or if
 * it fails, the amp plays ASPRX1 itself as before. Calibration as stock's
 * cirrus-cal.c without saved values (this phone's EFS has no cirrus/): RDC
 * 8580, 23 C. See docs/s10-5g/SPK-PROT.md.
 */
#define CSPL_ALG		0xcd		/* the firmware's CSPL algorithm */
#define HALO_ALG		0x400a4		/* FIRMWARE_HALO_CSPL: HALO_STATE */
#define HALO_STATE_RUN		2
static int amp_dsp;
static int amp_dsp_set(const char *arg, const struct kernel_param *kp);
static const struct kernel_param_ops amp_dsp_ops = { .set = amp_dsp_set, .get = param_get_int };
module_param_cb(amp_dsp, &amp_dsp_ops, &amp_dsp, 0644);
MODULE_PARM_DESC(amp_dsp, "speaker protection on: bit 0 bottom speaker, bit 1 earpiece (0: as before)");
static int dsp_gain = 18;		/* stock mixer_gains.xml, the tuning's "18dB" */
module_param(dsp_gain, int, 0444);
MODULE_PARM_DESC(dsp_gain, "AMP PCM Gain while the protection runs (stock 18)");

/*
 * HALO RX/TX sample-rate selectors at the first run. Stock beyondx has no
 * cirrus,use-fsync-errata, so wm_adsp writes its kcalloc'd rate cache: all 0.
 * Mainline's cs35l41_write_fs_errata writes 1, which belongs with
 * VIMON_SPKMON_RESYNC = 0; here that register reads back 1 (as on stock), and
 * with rates 1 the protection saw no IMON/VMON (ipeak/vpeak 0, temp flat at
 * ambient with spikes to 151 C). 0 = stock (default), 1 = mainline errata.
 */
static int dsp_fs_errata;
module_param(dsp_fs_errata, int, 0644);
MODULE_PARM_DESC(dsp_fs_errata, "DSP rate selectors at run: 0 stock (all 0), 1 mainline fs errata (all 1)");
/*
 * the coil's RDC the protection models from. Stock reads it from
 * /efs/cirrus/rdc_cal (absent here) else 8580; a good dsp_calibrate sets it
 * to the measured value. Writable for a saved value at boot (Q13 of
 * 1/5.85714 ohm: 5600..14000 = about 4..10 ohm), taken at the next run/REINIT.
 */
#define SPK_RDC_MIN		5600
#define SPK_RDC_MAX		14000
static int dsp_cal_r = 8580, dsp_cal_ambient = 23;	/* stock's defaults */
static int dsp_cal_r_set(const char *arg, const struct kernel_param *kp)
{
	int v, ret = kstrtoint(arg, 0, &v);

	if (ret)
		return ret;
	if (v < SPK_RDC_MIN || v > SPK_RDC_MAX)
		return -EINVAL;
	WRITE_ONCE(dsp_cal_r, v);
	return 0;
}
static const struct kernel_param_ops dsp_cal_r_ops = { .set = dsp_cal_r_set, .get = param_get_int };
module_param_cb(dsp_cal_r, &dsp_cal_r_ops, &dsp_cal_r, 0644);
MODULE_PARM_DESC(dsp_cal_r, "bottom speaker RDC for the protection (stock default 8580; 5600..14000)");
module_param(dsp_cal_ambient, int, 0444);
/*
 * IMON/VMON reached the protection only after a REINIT (ipeak/vpeak 0 and the
 * coil temperature flat at ambient until a dsp_calibrate; then they moved).
 * 1 = once per run, dsp_reinit_ms after the amp first goes on with it:
 * STOP_PRE_REINIT, the calibration again, REINIT. 0 = not (default for now).
 * Done ~46 ms after the run (ed124b1) it left CSPL_STATE 1 (error) for good,
 * and a dsp_calibrate was then refused; so later, and only from state 0.
 * dsp_reinit (write 1) does the same REINIT by hand, the amp on.
 */
static int dsp_run_reinit;
module_param(dsp_run_reinit, int, 0644);
MODULE_PARM_DESC(dsp_run_reinit, "one REINIT of the protection dsp_reinit_ms after the first amp on of a run (1) or not (0, default)");
static int dsp_reinit_ms = 1000;
module_param(dsp_reinit_ms, int, 0644);
/*
 * After a resume the protection is loaded again (~6 s of 100 kHz I2C, the amp's
 * bus taken: a sound then waits it out and the notification is lost, 14.11
 * 15:44 and 16:18). So it waits: dsp_resume_ms after the wake, and then until
 * nothing has played for 5 s. A wake for an SMS sleeps again within ~20 s and
 * never loads it; its sounds play plain (the amp alone, as before amp_dsp).
 */
static int dsp_resume_ms = 30000;
module_param(dsp_resume_ms, int, 0644);
MODULE_PARM_DESC(dsp_resume_ms, "after a resume: ms before the protection is loaded again");
static unsigned long last_sound;	/* jiffies: a stream started or closed; 0 none yet */
#define DSP_QUIET	(5 * HZ)
MODULE_PARM_DESC(dsp_reinit_ms, "how long the amp plays through the protection before dsp_run_reinit's REINIT (ms, 100..10000)");
static struct delayed_work dsp_reinit_work;
static char dsp_state[64] = "off";
module_param_string(dsp_state, dsp_state, sizeof(dsp_state), 0444);
MODULE_PARM_DESC(dsp_state, "the protection now, per amp: off, loading, loaded, running, failed");

static int dsp_vol_boost_set(const char *arg, const struct kernel_param *kp)
{
	int v, i, ret = kstrtoint(arg, 0, &v);

	if (ret)
		return ret;
	mutex_lock(&amp_lock);
	dsp_vol_boost = clamp(v, 0, AMP_VOL_MAX_DSP - AMP_VOL_MIN);
	for (i = 0; i < ARRAY_SIZE(amps); i++)
		if (amps[i].regmap)
			amp_volume_write(&amps[i]);
	mutex_unlock(&amp_lock);
	pr_info("snd-zrn-abox-bx: dsp_vol_boost %d (+%d.%03d dB)\n", dsp_vol_boost,
		dsp_vol_boost / 8, dsp_vol_boost % 8 * 125);
	return 0;
}
static const struct kernel_param_ops dsp_vol_boost_ops = { .set = dsp_vol_boost_set, .get = param_get_int };
module_param_cb(dsp_vol_boost, &dsp_vol_boost_ops, &dsp_vol_boost, 0644);
MODULE_PARM_DESC(dsp_vol_boost, "added to the volume while the protection runs, 0.125 dB steps, total up to 0 dB");
static DEFINE_MUTEX(dsp_lock);		/* a->dsp; taken after amp_lock, never before it */
static struct delayed_work dsp_load_work;
/* freezable: a suspend waits for a load to end, never cuts its I2C */
static void dsp_load_later(unsigned int ms)
{
	mod_delayed_work(system_freezable_wq, &dsp_load_work, msecs_to_jiffies(ms));
}
static const struct cs_dsp_client_ops spk_client_ops;	/* no ALSA controls for them */
static const char spk_fw_name[] = "spk-prot";

static void dsp_state_update(void)
{
	int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(amps); i++) {
		struct amp *a = &amps[i];

		n += scnprintf(dsp_state + n, sizeof(dsp_state) - n, "%s%s:%s", i ? " " : "", a->fw,
			       a->dsp_failed ? "failed" : a->dsp.running ? "running" :
			       a->fw_loaded ? "loaded" : (amp_dsp & BIT(i)) ? "loading" : "off");
	}
}

static int spk_ctl_write(struct amp *a, const char *name, unsigned int alg, u32 v)
{
	__be32 b = cpu_to_be32(v);
	struct cs_dsp_coeff_ctl *ctl;
	int ret;

	mutex_lock(&a->dsp.pwr_lock);
	ctl = cs_dsp_get_ctl(&a->dsp, name, WMFW_ADSP2_XM, alg);
	ret = ctl ? cs_dsp_coeff_write_ctrl(ctl, 0, &b, sizeof(b)) : -ENOENT;
	mutex_unlock(&a->dsp.pwr_lock);
	return ret < 0 ? ret : 0;	/* 1 = written and the value changed */
}

static int spk_ctl_read(struct amp *a, const char *name, unsigned int alg, u32 *v)
{
	struct cs_dsp_coeff_ctl *ctl;
	__be32 b = 0;
	int ret;

	mutex_lock(&a->dsp.pwr_lock);
	ctl = cs_dsp_get_ctl(&a->dsp, name, WMFW_ADSP2_XM, alg);
	ret = ctl ? cs_dsp_coeff_read_ctrl(ctl, 0, &b, sizeof(b)) : -ENOENT;
	mutex_unlock(&a->dsp.pwr_lock);
	*v = be32_to_cpu(b);
	return ret;
}

/* wmfw and tuning into the DSP's memories, the calibration after; dsp_lock held */
static int spk_load(struct amp *a)
{
	const struct firmware *wmfw = NULL, *coeff = NULL;
	struct device *dev = &a->client->dev;
	char wname[64], cname[64];
	int ret;

	if (!a->dsp_init) {
		cs35l41_configure_cs_dsp(dev, a->regmap, &a->dsp);
		a->dsp.client_ops = &spk_client_ops;
		ret = cs_dsp_halo_init(&a->dsp);
		if (ret)
			return ret;
		a->dsp_init = true;
	}
	snprintf(wname, sizeof(wname), "cs35l40-%s-dsp1-spk-prot.wmfw", a->fw);
	snprintf(cname, sizeof(cname), "cs35l40-%s-dsp1-spk-prot.bin", a->fw);
	ret = request_firmware(&wmfw, wname, dev);
	if (!ret)
		ret = request_firmware(&coeff, cname, dev);
	if (!ret)
		ret = cs_dsp_power_up(&a->dsp, wmfw, wname, coeff, cname, spk_fw_name);
	release_firmware(wmfw);
	release_firmware(coeff);
	if (ret)
		return ret;
	dev_info(dev, "%s: speaker protection loaded (fw %06x v%06x)\n",
		 a->name, a->dsp.fw_id, a->dsp.fw_id_version);
	return 0;
}

/*
 * stock cirrus_cal_apply_*(): status 1, checksum = status + rdc. CAL_R, CAL_STATUS
 * and CAL_CHECKSUM are volatile in this wmfw (flags 7): cs_dsp writes them only
 * while the core runs (-EPERM before), so this goes after cs_dsp_run().
 */
static int spk_cal(struct amp *a)
{
	int ret;

	ret = spk_ctl_write(a, "CAL_R", CSPL_ALG, dsp_cal_r);
	if (!ret)
		ret = spk_ctl_write(a, "CAL_AMBIENT", CSPL_ALG, dsp_cal_ambient);
	if (!ret)
		ret = spk_ctl_write(a, "CAL_STATUS", CSPL_ALG, 1);
	if (!ret)
		ret = spk_ctl_write(a, "CAL_CHECKSUM", CSPL_ALG, 1 + dsp_cal_r);
	return ret;
}

/* the first play: start the core with the bit clock running; dsp_lock held */
static int spk_run(struct amp *a)
{
	struct device *dev = &a->client->dev;
	unsigned int sts = 0;
	u32 halo = 0;
	int i, ret;

	regmap_write(a->regmap, CS35L41_DSP_CLK_CTRL, 0x3);	/* DSP clock on */
	if (dsp_fs_errata) {
		ret = cs35l41_write_fs_errata(dev, a->regmap);
	} else {
		for (i = 0, ret = 0; i < 8 && !ret; i++) {
			ret = regmap_write(a->regmap, CS35L41_DSP1_RX1_RATE + 8 * i, 0);
			if (!ret)
				ret = regmap_write(a->regmap, CS35L41_DSP1_TX1_RATE + 8 * i, 0);
		}
		usleep_range(300, 400);		/* wm_halo_set_rate_block's udelay */
	}
	if (!ret)
		ret = cs_dsp_run(&a->dsp);
	if (ret)
		goto fail;
	for (i = 0; i < 50; i++) {
		if (!spk_ctl_read(a, "HALO_STATE", HALO_ALG, &halo) && halo == HALO_STATE_RUN)
			break;
		usleep_range(1000, 1100);
	}
	regmap_read(a->regmap, CS35L41_DSP_MBOX_2, &sts);
	if (halo != HALO_STATE_RUN || (sts != CSPL_MBOX_STS_RUNNING && sts != CSPL_MBOX_STS_PAUSED)) {
		dev_err(dev, "%s: protection did not start (HALO_STATE %u, mbox %u)\n", a->name, halo, sts);
		ret = -ETIMEDOUT;
		goto fail;
	}
	ret = spk_cal(a);
	if (ret) {
		dev_err(dev, "%s: calibration not written (%d)\n", a->name, ret);
		goto fail;
	}
	ret = cs35l41_set_cspl_mbox_cmd(dev, a->regmap, CSPL_MBOX_CMD_PAUSE);
	if (ret)
		goto fail;
	dev_info(dev, "%s: speaker protection running, cal R %d ambient %d\n",
		 a->name, dsp_cal_r, dsp_cal_ambient);
	return 0;
fail:
	cs_dsp_stop(&a->dsp);
	a->dsp_failed = true;
	dev_err(dev, "%s: speaker protection off (%d), the amp plays as before\n", a->name, ret);
	return ret;
}

/* the monitor ADCs the protection reads: VPMON 8, VBSTMON 9, TEMPMON 10, VMON 12, IMON 13 */
#define SPK_MON_EN	(BIT(8) | BIT(9) | BIT(10) | BIT(12) | BIT(13))

/*
 * the DSP's inputs as stock cs35l41.c wires this CSPL firmware:
 * RX3/4 VMON/IMON, RX5 VPMON, RX6 CLASSH, RX7 TEMPMON (cs35l41_dsp_init;
 * mainline hda's RX6 VBSTMON is for its newer firmware), and every
 * monitor ADC the "DSP1" widget needs powered (VMON, IMON, VPMON,
 * VBSTMON, TEMPMON: PWR_CTRL2 bits 12, 13, 8, 9, 10). With only VMON
 * and IMON on, the coil model saturated at 151 C at -3 dB. Stock sets the
 * sources at codec init, so before the core first runs; so here too.
 */
static void spk_inputs(struct amp *a)
{
	regmap_write(a->regmap, CS35L41_DSP1_RX1_SRC, CS35L41_INPUT_SRC_ASPRX1);
	regmap_write(a->regmap, CS35L41_DSP1_RX2_SRC, CS35L41_INPUT_SRC_ASPRX1);
	regmap_write(a->regmap, CS35L41_DSP1_RX3_SRC, CS35L41_INPUT_SRC_VMON);
	regmap_write(a->regmap, CS35L41_DSP1_RX4_SRC, CS35L41_INPUT_SRC_IMON);
	regmap_write(a->regmap, CS35L41_DSP1_RX5_SRC, CS35L41_INPUT_SRC_VPMON);
	regmap_write(a->regmap, CS35L41_DSP1_RX6_SRC, CS35L41_INPUT_SRC_CLASSH);
	regmap_write(a->regmap, CS35L41_DSP1_RX7_SRC, CS35L41_INPUT_SRC_TEMPMON);
	regmap_update_bits(a->regmap, CS35L41_PWR_CTRL2, SPK_MON_EN, SPK_MON_EN);
}

/* what the amp plays, set before it goes on: the DSP's output, or ASPRX1 */
static bool spk_route(struct amp *a, int i)
{
	bool dsp = false;

	if ((READ_ONCE(amp_dsp) & BIT(i)) && mutex_trylock(&dsp_lock)) {
		if (a->fw_loaded && !a->dsp_failed) {
			if (a->dsp.running) {
				dsp = true;
			} else {
				spk_inputs(a);
				if (!spk_run(a)) {
					dsp = true;
					a->dsp_reinit_due = true;
				}
			}
		}
		mutex_unlock(&dsp_lock);
	}
	if (dsp) {
		spk_inputs(a);
		regmap_write(a->regmap, CS35L41_DAC_PCM1_SRC, CS35L41_INPUT_DSP_TX1);
	} else {
		regmap_write(a->regmap, CS35L41_DAC_PCM1_SRC, CS35L41_INPUT_SRC_ASPRX1);
	}
	regmap_update_bits(a->regmap, CS35L41_AMP_GAIN_CTRL, CS35L41_AMP_GAIN_PCM_MASK,
			   clamp(dsp ? dsp_gain : amp_gain, 0, CS35L41_AMP_GAIN_PCM_MAX) <<
			   CS35L41_AMP_GAIN_PCM_SHIFT);
	a->dsp_routed = dsp;
	amp_volume_write(a);
	dsp_state_update();
	return dsp;
}

/* the DSP's state goes with the amp's power (amps_rescue): dsp_lock held */
static void spk_forget(struct amp *a)
{
	if (a->dsp_init)
		cs_dsp_remove(&a->dsp);
	memset(&a->dsp, 0, sizeof(a->dsp));
	a->dsp_init = a->fw_loaded = a->dsp_failed = a->dsp_routed = false;
	a->dsp_reinit_due = false;
}

static void amp_power(struct amp *a, bool on);

/*
 * the background load: never holds amp_lock while it holds dsp_lock. It
 * starts only after 5 s with nothing played (re-checked each second). A sound
 * during it cannot play anyway (the bus is the load's: its power-up came only
 * at the end and failed -110, 14.11 16:18), so that amp is left off and
 * powered up after the load.
 */
static void dsp_load_fn(struct work_struct *work)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(amps); i++) {
		struct amp *a = &amps[i];
		bool replay;

		if (!(READ_ONCE(amp_dsp) & BIT(i)) || READ_ONCE(a->fw_loaded) || a->dsp_failed)
			continue;
		mutex_lock(&amp_lock);
		if (READ_ONCE(active) || READ_ONCE(call_on) ||
		    (READ_ONCE(last_sound) && time_before(jiffies, READ_ONCE(last_sound) + DSP_QUIET))) {
			mutex_unlock(&amp_lock);
			dsp_load_later(1000);
			return;
		}
		WRITE_ONCE(a->loading, true);
		mutex_unlock(&amp_lock);
		mutex_lock(&dsp_lock);
		if (!a->regmap || a->fw_loaded || a->dsp_failed) {
			WRITE_ONCE(a->loading, false);
			mutex_unlock(&dsp_lock);
			continue;
		}
		ret = spk_load(a);
		if (ret) {
			a->dsp_failed = true;
			dev_err(&a->client->dev, "%s: speaker protection not loaded (%d)\n", a->name, ret);
		}
		WRITE_ONCE(a->fw_loaded, !ret);
		WRITE_ONCE(a->loading, false);
		mutex_unlock(&dsp_lock);
		dsp_state_update();
		/* playing already: through the DSP from now on */
		mutex_lock(&amp_lock);
		replay = a->on && !ret;
		if (replay) {
			amp_power(a, false);
			amp_power(a, true);
		}
		mutex_unlock(&amp_lock);
		/* a sound that started while it loaded: its power-up was left out */
		if (READ_ONCE(active) || READ_ONCE(call_on))
			amps_power(true);
	}
}

static int amp_dsp_set(const char *arg, const struct kernel_param *kp)
{
	int ret = param_set_int(arg, kp);

	if (ret)
		return ret;
	dsp_state_update();
	if (bb_added)
		dsp_load_later(0);
	return 0;
}

/*
 * dsp_diag: what the running protection reports, raw (read to watch the coil
 * while the volume goes up): HALO_HEARTBEAT grows while it runs, CSPL_STATE 0 =
 * running (1 error), the BDLOG_* maxima/counters since it was loaded (OVER_*
 * and ABNORMAL_MUTE should stay 0), and the last peaks and output power.
 */
static const struct { const char *tag, *ctl; unsigned int alg; } spk_diag_ctls[] = {
	{ "hb", "HALO_HEARTBEAT", HALO_ALG },
	{ "state", "CSPL_STATE", CSPL_ALG },
	{ "temp", "CSPL_TEMPERATURE", CSPL_ALG },
	{ "calset", "CAL_SET_STATUS", CSPL_ALG },
	{ "maxtemp", "BDLOG_MAX_TEMP", CSPL_ALG },
	{ "maxexc", "BDLOG_MAX_EXC", CSPL_ALG },
	{ "overtemp", "BDLOG_OVER_TEMP_COUNT", CSPL_ALG },
	{ "overexc", "BDLOG_OVER_EXC_COUNT", CSPL_ALG },
	{ "mute", "BDLOG_ABNORMAL_MUTE", CSPL_ALG },
	{ "power", "CSPL_SPK_OUTPUT_POWER", CSPL_ALG },
	{ "ipeak", "CSPL_CURRENT_PEAK", CSPL_ALG },
	{ "vpeak", "CSPL_VOLTAGE_PEAK", CSPL_ALG },
	{ "gain", "CSPL_OVERSIGHT_GAIN", CSPL_ALG },
	{ "reduce", "REDUCE_POWER", CSPL_ALG },
};

static int dsp_diag_get(char *buf, const struct kernel_param *kp)
{
	int i, j, n = 0;

	mutex_lock(&dsp_lock);
	for (i = 0; i < ARRAY_SIZE(amps); i++) {
		struct amp *a = &amps[i];

		if (!a->dsp.running)
			continue;
		n += scnprintf(buf + n, PAGE_SIZE - n, "%s%s:", n ? " | " : "", a->fw);
		for (j = 0; j < ARRAY_SIZE(spk_diag_ctls); j++) {
			u32 v;
			int ret = spk_ctl_read(a, spk_diag_ctls[j].ctl, spk_diag_ctls[j].alg, &v);

			if (ret < 0)
				n += scnprintf(buf + n, PAGE_SIZE - n, " %s=E%d", spk_diag_ctls[j].tag, ret);
			else
				n += scnprintf(buf + n, PAGE_SIZE - n, " %s=%u", spk_diag_ctls[j].tag, v);
		}
		/* the inputs as the amp has them now (RX1..RX7 sources, PWR_CTRL2) */
		{
			unsigned int r, pwr = 0;

			n += scnprintf(buf + n, PAGE_SIZE - n, " rx=");
			for (j = 0; j < 7; j++) {
				r = 0;
				regmap_read(a->regmap, CS35L41_DSP1_RX1_SRC + 4 * j, &r);
				n += scnprintf(buf + n, PAGE_SIZE - n, "%s%02x", j ? "," : "", r);
			}
			regmap_read(a->regmap, CS35L41_PWR_CTRL2, &pwr);
			n += scnprintf(buf + n, PAGE_SIZE - n, " pwr2=%04x", pwr);
			/* rate selectors RX1/RX3/RX4 and VIMON resync (dsp_fs_errata) */
			n += scnprintf(buf + n, PAGE_SIZE - n, " rate=");
			for (j = 0; j < 4; j++) {
				if (j == 1)
					continue;
				r = 0xff;
				regmap_read(a->regmap, CS35L41_DSP1_RX1_RATE + 8 * j, &r);
				n += scnprintf(buf + n, PAGE_SIZE - n, "%s%x", j ? "," : "", r);
			}
			r = 0xff;
			regmap_read(a->regmap, CS35L41_VIMON_SPKMON_RESYNC, &r);
			n += scnprintf(buf + n, PAGE_SIZE - n, " resync=%x", r);
		}
	}
	mutex_unlock(&dsp_lock);
	if (!n)
		n = scnprintf(buf, PAGE_SIZE, "not running");
	n += scnprintf(buf + n, PAGE_SIZE - n, "\n");
	return n;
}
static const struct kernel_param_ops dsp_diag_ops = { .get = dsp_diag_get };
module_param_cb(dsp_diag, &dsp_diag_ops, NULL, 0444);
MODULE_PARM_DESC(dsp_diag, "the running protection's state and coil log, raw");

/*
 * dsp_calibrate: stock cirrus-cal.c's measurement of the coil's resistance
 * through VMON/IMON, here to see whether those reach the protection at all
 * (and, later, for this phone's own RDC). Write the ambient temperature in C
 * while the bottom speaker plays through the protection (silence is enough):
 * STOP_PRE_REINIT, the -spk-prot-calib.bin tuning, DSP inputs and noise gate
 * off, the amp at 0 dB, REINIT, CAL_STATUS polled up to dsp_cal_ms; then the
 * playback tuning, volume and calibration back the same way. dsp_cal_result
 * has the outcome; a good one (status 1, checksum ok, RDC in range) becomes
 * dsp_cal_r until reboot.
 */
int zrn_cs_dsp_load_coeff(struct cs_dsp *dsp, const struct firmware *fw, const char *file);
static char dsp_cal_result[192] = "none";
module_param_string(dsp_cal_result, dsp_cal_result, sizeof(dsp_cal_result), 0444);
MODULE_PARM_DESC(dsp_cal_result, "the last dsp_calibrate: CAL_STATUS, CAL_R (ohms), checksum, CSPL_STATE");
#define CS35L41_NGATE_CH2_CFG	(CS35L41_MIXER_NGATE_CH1_CFG + 4)
#define SPK_NG_EN		BIT(16)
#define SPK_CAL_MS		1250	/* stock CS35L41_CAL_COMPLETE_DELAY_MS */
static unsigned int dsp_cal_ms = SPK_CAL_MS;
module_param(dsp_cal_ms, uint, 0644);
MODULE_PARM_DESC(dsp_cal_ms, "dsp_calibrate: the longest wait for CAL_STATUS, ms (stock 1250, 300..10000)");

/* stock's mailbox handshake: the command, then up to ~150 ms for the state */
static int spk_mbox(struct amp *a, u32 cmd, u32 want)
{
	unsigned int sts = 0xdead;
	int i;

	regmap_write(a->regmap, CS35L41_DSP_VIRT1_MBOX_1, cmd);
	for (i = 0; i < 100; i++) {
		usleep_range(1000, 1500);
		if (!regmap_read(a->regmap, CS35L41_DSP_MBOX_2, &sts) && sts == want)
			return 0;
	}
	dev_err(&a->client->dev, "%s: mailbox %u: state %u, not %u\n", a->name, cmd, sts, want);
	return -ETIMEDOUT;
}

static int spk_coeff(struct amp *a, const char *what)
{
	const struct firmware *fw;
	char name[64];
	int ret;

	snprintf(name, sizeof(name), "cs35l40-%s-dsp1-%s.bin", a->fw, what);
	ret = request_firmware(&fw, name, &a->client->dev);
	if (ret)
		return ret;
	ret = zrn_cs_dsp_load_coeff(&a->dsp, fw, name);
	release_firmware(fw);
	return ret;
}

/* amp_lock and dsp_lock held, the amp on and playing through the protection */
static int spk_calibrate(struct amp *a, int ambient)
{
	struct device *dev = &a->client->dev;
	u32 st = 0, r = 0, ck = 0, cs = 0, amb = 0;
	unsigned int rx1 = 0, rx2 = 0, ng1 = 0, ng2 = 0, vol = 0;
	unsigned long long ohm;
	bool kept = false;
	char trace[96] = "";
	int ret, ret2;

	spk_ctl_read(a, "CSPL_STATE", CSPL_ALG, &cs);
	if (cs == 1)
		dev_warn(dev, "%s: CSPL_STATE 1 already before the calibration\n", a->name);
	spk_ctl_write(a, "CAL_STATUS", CSPL_ALG, 0);
	spk_ctl_write(a, "CAL_R", CSPL_ALG, 0);
	spk_ctl_write(a, "CAL_CHECKSUM", CSPL_ALG, 0);
	regmap_read(a->regmap, CS35L41_DSP1_RX1_SRC, &rx1);
	regmap_read(a->regmap, CS35L41_DSP1_RX2_SRC, &rx2);
	regmap_read(a->regmap, CS35L41_MIXER_NGATE_CH1_CFG, &ng1);
	regmap_read(a->regmap, CS35L41_NGATE_CH2_CFG, &ng2);
	regmap_read(a->regmap, CS35L41_AMP_DIG_VOL_CTRL, &vol);

	ret = spk_mbox(a, CSPL_MBOX_CMD_STOP_PRE_REINIT, CSPL_MBOX_STS_RDY_FOR_REINIT);
	if (ret)
		goto restore;
	ret = spk_coeff(a, "spk-prot-calib");
	if (ret) {
		dev_err(dev, "%s: calibration tuning not loaded (%d)\n", a->name, ret);
		goto restore;
	}
	regmap_write(a->regmap, CS35L41_DSP1_RX1_SRC, 0);
	regmap_write(a->regmap, CS35L41_DSP1_RX2_SRC, 0);
	regmap_write(a->regmap, CS35L41_MIXER_NGATE_CH1_CFG, ng1 & ~SPK_NG_EN);
	regmap_write(a->regmap, CS35L41_NGATE_CH2_CFG, ng2 & ~SPK_NG_EN);
	/*
	 * stock measures at the amp's 0 dB: the protection's own cal signal goes
	 * through AMP_DIG_VOL, and at -24 dB (ctl 624) CAL_STATUS stayed 2
	 * (incomplete) for 8 s; at 0 dB it was 1 after ~950 ms. The DSP inputs
	 * are muted above, so nothing of the stream plays at 0 dB.
	 */
	regmap_update_bits(a->regmap, CS35L41_AMP_DIG_VOL_CTRL, CS35L41_AMP_PCM_VOL_MASK,
			   (AMP_VOL_MAX_DSP & 0x7ff) << CS35L41_AMP_PCM_VOL_SHIFT);
	spk_ctl_write(a, "CAL_AMBIENT", CSPL_ALG, ambient);
	ret = spk_mbox(a, CSPL_MBOX_CMD_REINIT, CSPL_MBOX_STS_RUNNING);
	if (ret)
		goto restore;
	msleep(85);
	spk_ctl_read(a, "CSPL_STATE", CSPL_ALG, &cs);
	if (cs == 1) {
		dev_err(dev, "%s: calibration tuning refused (CSPL_STATE 1)\n", a->name);
		ret = -EIO;
		goto restore;
	}
	/*
	 * stock sleeps 1.25 s and reads once; here CAL_STATUS is polled every
	 * 100 ms up to dsp_cal_ms, each change logged (status@ms), so a slow
	 * measurement (2 = incomplete) shows when, or whether, it ends
	 */
	{
		unsigned long t0 = jiffies;
		unsigned int ms, last = ~0u;
		int n = 0;

		trace[0] = 0;
		for (;;) {
			msleep(100);
			ms = jiffies_to_msecs(jiffies - t0);
			spk_ctl_read(a, "CAL_STATUS", CSPL_ALG, &st);
			if (st != last && n < (int)sizeof(trace) - 16) {
				n += scnprintf(trace + n, sizeof(trace) - n, " %u@%u", st, ms);
				last = st;
			}
			if ((st != 0 && st != 2 && ms >= 300) || ms >= dsp_cal_ms)
				break;
		}
		n += scnprintf(trace + n, sizeof(trace) - n, " end@%u", ms);
	}
	spk_ctl_read(a, "CAL_STATUS", CSPL_ALG, &st);
	spk_ctl_read(a, "CAL_R", CSPL_ALG, &r);
	spk_ctl_read(a, "CAL_AMBIENT", CSPL_ALG, &amb);
	spk_ctl_read(a, "CAL_CHECKSUM", CSPL_ALG, &ck);
	spk_ctl_read(a, "CSPL_STATE", CSPL_ALG, &cs);
	/* a good measurement is the RDC from now on (until reboot) */
	if (st == 1 && ck == st + r && r >= SPK_RDC_MIN && r <= SPK_RDC_MAX && cs != 1) {
		WRITE_ONCE(dsp_cal_r, r);
		kept = true;
	}
restore:
	ret2 = spk_mbox(a, CSPL_MBOX_CMD_STOP_PRE_REINIT, CSPL_MBOX_STS_RDY_FOR_REINIT);
	if (!ret2)
		ret2 = spk_coeff(a, spk_fw_name);
	regmap_write(a->regmap, CS35L41_MIXER_NGATE_CH1_CFG, ng1);
	regmap_write(a->regmap, CS35L41_NGATE_CH2_CFG, ng2);
	if (!ret2)
		ret2 = spk_cal(a);
	if (!ret2)
		ret2 = spk_mbox(a, CSPL_MBOX_CMD_REINIT, CSPL_MBOX_STS_RUNNING);
	/* the volume back before the stream reaches the DSP again */
	regmap_update_bits(a->regmap, CS35L41_AMP_DIG_VOL_CTRL, CS35L41_AMP_PCM_VOL_MASK,
			   vol & CS35L41_AMP_PCM_VOL_MASK);
	regmap_write(a->regmap, CS35L41_DSP1_RX1_SRC, rx1);
	regmap_write(a->regmap, CS35L41_DSP1_RX2_SRC, rx2);
	if (ret2) {
		/* the protection's state unknown: off, the amp plays ASPRX1 itself */
		cs_dsp_stop(&a->dsp);
		a->dsp_failed = true;
		regmap_write(a->regmap, CS35L41_DAC_PCM1_SRC, CS35L41_INPUT_SRC_ASPRX1);
		regmap_update_bits(a->regmap, CS35L41_AMP_GAIN_CTRL, CS35L41_AMP_GAIN_PCM_MASK,
				   clamp(amp_gain, 0, CS35L41_AMP_GAIN_PCM_MAX) << CS35L41_AMP_GAIN_PCM_SHIFT);
		a->dsp_routed = false;
		amp_volume_write(a);
		dsp_state_update();
		dev_err(dev, "%s: playback tuning not back (%d), protection off\n", a->name, ret2);
	}
	ohm = (unsigned long long)r * 292857 / 50000;	/* stock cs35l41_rdc_to_ohms, Q13 */
	scnprintf(dsp_cal_result, sizeof(dsp_cal_result),
		  "%s: ret %d back %d status %u r %u (%llu.%04llu ohm) ambient %u checksum %u (%s) cspl_state %u %s trace%s",
		  a->fw, ret, ret2, st, r, ohm >> 13, (ohm & 8191) * 10000 >> 13, amb, ck,
		  ck == st + r ? "ok" : "bad", cs, kept ? "kept" : "not-kept", trace);
	dev_info(dev, "%s: calibration %s\n", a->name, dsp_cal_result);
	return ret ?: ret2;
}

static int dsp_calibrate_set(const char *arg, const struct kernel_param *kp)
{
	struct amp *a = &amps[0];	/* the bottom speaker */
	int ambient, ret = kstrtoint(arg, 0, &ambient);

	if (ret)
		return ret;
	if (ambient < 0 || ambient > 60)
		return -EINVAL;
	mutex_lock(&amp_lock);
	mutex_lock(&dsp_lock);
	if (!a->regmap || !a->on || !a->dsp_routed || !a->dsp.running || a->dsp_failed)
		ret = -EBUSY;
	else
		ret = spk_calibrate(a, ambient);
	mutex_unlock(&dsp_lock);
	mutex_unlock(&amp_lock);
	return ret;
}
static const struct kernel_param_ops dsp_calibrate_ops = { .set = dsp_calibrate_set };
module_param_cb(dsp_calibrate, &dsp_calibrate_ops, NULL, 0200);
MODULE_PARM_DESC(dsp_calibrate, "measure the bottom speaker's RDC (write the ambient C) while it plays through the protection");

/*
 * ASPTX1/2 on with the amp, off (DOUT hi-Z) without it. One amp alone sends
 * in slots 0 and 2 (stock dev-handset, dev-speaker); both, as stock's
 * dev-dual-speaker, the earpiece 0/2 and the bottom speaker 1/3 (a->slot).
 */
static void amp_tx(struct amp *a, bool on, int mask)
{
	unsigned int tx[2];
	unsigned int en = BIT(CS35L41_ASP_TX1_EN_SHIFT) | BIT(CS35L41_ASP_TX2_EN_SHIFT);

	if (!graph)
		return;
	if (on) {
		tx[0] = (mask & 3) == 3 ? a->slot : 0;
		tx[1] = tx[0] + 2;
		cs35l41_set_channels(&a->client->dev, a->regmap, 2, tx, 0, NULL);
	}
	regmap_update_bits(a->regmap, CS35L41_SP_ENABLES, en, on ? en : 0);
}

/*
 * dsp_run_reinit: the run's one REINIT, the amp just on and playing through
 * the protection (amp_lock held). As dsp_calibrate's way back: inputs muted,
 * STOP_PRE_REINIT, the playback tuning and (volatile) calibration again,
 * REINIT, inputs back. A failed
 * handshake turns the protection off as there.
 */
static void spk_first_reinit(struct amp *a)
{
	struct device *dev = &a->client->dev;
	u32 cs0 = 0, cs1 = 0;
	unsigned int rx1 = 0, rx2 = 0;
	int ret;

	if (!mutex_trylock(&dsp_lock))
		return;			/* still due: the next amp on */
	if (!a->dsp.running || a->dsp_failed) {
		a->dsp_reinit_due = false;
		goto out;
	}
	a->dsp_reinit_due = false;
	spk_ctl_read(a, "CSPL_STATE", CSPL_ALG, &cs0);
	if (cs0 != 0) {
		dev_warn(dev, "%s: protection REINIT skipped, CSPL_STATE %u\n", a->name, cs0);
		goto out;
	}
	/*
	 * exactly dsp_calibrate's way back, the only REINIT seen to work: the
	 * DSP inputs muted, STOP_PRE_REINIT, the playback tuning loaded AGAIN,
	 * the calibration, REINIT, the inputs back. A REINIT without the tuning
	 * reloaded left CSPL_STATE 1 (error) and IMON/VMON 0 (814b55b by hand,
	 * ed124b1 at 46 ms) - not the timing as first thought.
	 */
	regmap_read(a->regmap, CS35L41_DSP1_RX1_SRC, &rx1);
	regmap_read(a->regmap, CS35L41_DSP1_RX2_SRC, &rx2);
	regmap_write(a->regmap, CS35L41_DSP1_RX1_SRC, 0);
	regmap_write(a->regmap, CS35L41_DSP1_RX2_SRC, 0);
	ret = spk_mbox(a, CSPL_MBOX_CMD_STOP_PRE_REINIT, CSPL_MBOX_STS_RDY_FOR_REINIT);
	if (!ret)
		ret = spk_coeff(a, spk_fw_name);
	if (!ret)
		ret = spk_cal(a);
	if (!ret)
		ret = spk_mbox(a, CSPL_MBOX_CMD_REINIT, CSPL_MBOX_STS_RUNNING);
	regmap_write(a->regmap, CS35L41_DSP1_RX1_SRC, rx1);
	regmap_write(a->regmap, CS35L41_DSP1_RX2_SRC, rx2);
	if (!ret) {
		msleep(85);		/* as spk_calibrate before its CSPL_STATE check */
		spk_ctl_read(a, "CSPL_STATE", CSPL_ALG, &cs1);
	}
	if (ret) {
		cs_dsp_stop(&a->dsp);
		a->dsp_failed = true;
		regmap_write(a->regmap, CS35L41_DAC_PCM1_SRC, CS35L41_INPUT_SRC_ASPRX1);
		regmap_update_bits(a->regmap, CS35L41_AMP_GAIN_CTRL, CS35L41_AMP_GAIN_PCM_MASK,
				   clamp(amp_gain, 0, CS35L41_AMP_GAIN_PCM_MAX) << CS35L41_AMP_GAIN_PCM_SHIFT);
		a->dsp_routed = false;
		amp_volume_write(a);
		dsp_state_update();
		dev_err(dev, "%s: protection REINIT failed (%d), protection off\n", a->name, ret);
	} else {
		dev_info(dev, "%s: protection REINIT with the amp on, cal R %d, CSPL_STATE %u\n",
			 a->name, dsp_cal_r, cs1);
	}
out:
	mutex_unlock(&dsp_lock);
}

/* dsp_run_reinit's REINIT, the amp still on and playing through the protection */
static void dsp_reinit_fn(struct work_struct *work)
{
	int i;

	mutex_lock(&amp_lock);
	for (i = 0; i < ARRAY_SIZE(amps); i++) {
		struct amp *a = &amps[i];

		if (a->regmap && a->on && a->dsp_routed && a->dsp_reinit_due)
			spk_first_reinit(a);
	}
	mutex_unlock(&amp_lock);
}

static int dsp_reinit_set(const char *arg, const struct kernel_param *kp)
{
	struct amp *a = &amps[0];	/* the bottom speaker */
	int v, ret = kstrtoint(arg, 0, &v);

	if (ret)
		return ret;
	if (v != 1)
		return -EINVAL;
	mutex_lock(&amp_lock);
	if (!a->regmap || !a->on || !a->dsp_routed || !a->dsp.running || a->dsp_failed) {
		ret = -EBUSY;
	} else {
		a->dsp_reinit_due = true;
		spk_first_reinit(a);
		ret = a->dsp_reinit_due ? -EBUSY : 0;	/* dsp_lock was taken */
		a->dsp_reinit_due = false;
	}
	mutex_unlock(&amp_lock);
	return ret;
}
static const struct kernel_param_ops dsp_reinit_ops = { .set = dsp_reinit_set };
module_param_cb(dsp_reinit, &dsp_reinit_ops, NULL, 0200);
MODULE_PARM_DESC(dsp_reinit, "write 1: REINIT the bottom speaker's protection now (it playing through it)");

/* the "Main AMP" widget's events; only with the bit clock running */
static void amp_power(struct amp *a, bool on)
{
	struct device *dev;
	unsigned int st;
	int ret;

	if (!a->regmap || a->on == on)
		return;
	dev = &a->client->dev;
	if (on && READ_ONCE(a->loading)) {	/* dsp_load_fn powers it up after */
		if (debug > 0)
			dev_info(dev, "%s on: after the protection's load\n", a->name);
		return;
	}
	if (on) {
		bool dsp = spk_route(a, a - amps);

		/* stock's "Boost Enable" and "AMP Enable Switch" (Main AMP, PWR_CTRL2
		 * bit 0); without them the amp takes the data and plays nothing */
		regmap_update_bits(a->regmap, CS35L41_PWR_CTRL2,
				   CS35L41_BST_EN_MASK | CS35L41_AMP_EN_MASK,
				   (a->boost ? CS35L41_BST_EN_DEFAULT << CS35L41_BST_EN_SHIFT : 0) | 1);
		regmap_multi_reg_write_bypassed(a->regmap, pup_patch, ARRAY_SIZE(pup_patch));
		ret = cs35l41_global_enable(dev, a->regmap, CS35L41_INT_BOOST, 1, &a->dsp);
		/* stock cs35l41_main_amp_event: the protection resumes with the amp */
		if (!ret && dsp)
			ret = cs35l41_set_cspl_mbox_cmd(dev, a->regmap, CSPL_MBOX_CMD_RESUME);
		if (!ret && dsp && a->dsp_reinit_due && READ_ONCE(dsp_run_reinit))
			schedule_delayed_work(&dsp_reinit_work,
					      msecs_to_jiffies(clamp(READ_ONCE(dsp_reinit_ms), 100, 10000)));
	} else {
		if (a->dsp.running)
			cs35l41_set_cspl_mbox_cmd(dev, a->regmap, CSPL_MBOX_CMD_PAUSE);
		ret = cs35l41_global_enable(dev, a->regmap, CS35L41_INT_BOOST, 0, &a->dsp);
		regmap_multi_reg_write_bypassed(a->regmap, pdn_patch, ARRAY_SIZE(pdn_patch));
		regmap_update_bits(a->regmap, CS35L41_PWR_CTRL2,
				   CS35L41_BST_EN_MASK | CS35L41_AMP_EN_MASK, 0);
	}
	regmap_read(a->regmap, CS35L41_IRQ1_STATUS1, &st);
	if (ret || debug > 0)
		dev_info(dev, "%s %s: %d, IRQ1_STATUS1 %08x\n", a->name, on ? "on" : "off", ret, st);
	a->on = on && !ret;
}

static void amps_power(bool on)
{
	int mask = READ_ONCE(call_on) ? call_amp_mask : amp_mask;
	int i, pass;

	/* PUP_DONE needs the bit clock: a stream prepared but not yet started
	 * (active set, UAIF1 off) failed "Enable(1): -110" (14.11 after a resume);
	 * uaif_on() powers them up 20 ms after it starts */
	if (on && !(readl(sfr + ABOX_UAIF1_CTRL0) & BIT(0)))
		return;
	mutex_lock(&amp_lock);
	/* the ones going off first: two amps never drive one DOUT slot */
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < ARRAY_SIZE(amps); i++) {
			bool a_on = on && (mask & BIT(i));

			if (a_on != pass || !amps[i].regmap || amps[i].on == a_on)
				continue;
			amp_tx(&amps[i], a_on, mask);
			amp_power(&amps[i], a_on);
		}
	mutex_unlock(&amp_lock);
}

/* a new mask (or the call starting/ending) while a stream runs */
static void amps_apply(void)
{
	if (READ_ONCE(active) || READ_ONCE(call_on))
		amps_power(true);
}

/* the bit clock runs without waiting for data (abox_uaif_set_tristate(0): the
 * amps' PLL locks on it), then SPK_ENABLE, and MIC_ENABLE in the stock graph:
 * the amps' feedback */
static void uaif_on(void)
{
	writel(readl(sfr + ABOX_QCHANNEL_DISABLE) | QCH_BCLK_UAIF1, sfr + ABOX_QCHANNEL_DISABLE);
	writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(0) | (graph ? BIT(1) : 0),
	       sfr + ABOX_UAIF1_CTRL0);
	schedule_work(&amp_on_work);	/* I2C sleeps; not here */
}

static void uaif_off(void)
{
	writel(readl(sfr + ABOX_UAIF1_CTRL0) & ~(BIT(0) | BIT(1)), sfr + ABOX_UAIF1_CTRL0);
	writel(readl(sfr + ABOX_QCHANNEL_DISABLE) & ~QCH_BCLK_UAIF1, sfr + ABOX_QCHANNEL_DISABLE);
}

static void amp_on_fn(struct work_struct *work)
{
	msleep(20);		/* let the PLL lock on the bit clock */
	if (READ_ONCE(active) || READ_ONCE(call_on))
		amps_power(true);
}

/*
 * After a system sleep (SYS_SLEEP, 12.11 4c) both amps NAKed every transfer
 * ("bottom speaker on: -6") until a reboot: the TOP domain and its pins come
 * back from reset, and the amps may have seen their rails or nRESET move. So
 * after every resume (and on amp_rescue=1) they are set up from scratch as at
 * probe: the bus pins, nRESET low then high, amp_probe() again. The pins are
 * logged before, to see which part had gone.
 */
static unsigned int amp_rescues;
module_param(amp_rescues, uint, 0444);
MODULE_PARM_DESC(amp_rescues, "times the amps were set up again (resume, amp_rescue)");
static int amp_rescue_ret;
module_param(amp_rescue_ret, int, 0444);
MODULE_PARM_DESC(amp_rescue_ret, "the last set-up's result (first failing amp)");
static struct work_struct amp_rescue_work;
static bool pm_nb_added;

static void amps_rescue(void)
{
	int i, ret, first = 0;

	mutex_lock(&amp_lock);
	pr_info("snd-zrn-abox-bx: amps again: gph0 con %08x dat %02x pud %08x pdn %04x, gph1 con %08x dat %02x pud %08x pdn %04x\n",
		readl(gpio + GPH0_CON), readl(gpio + GPH0_DAT), readl(gpio + GPH0_PUD),
		readl(gpio + GPH0_CON + 0x10), readl(gpio + GPH1_CON), readl(gpio + GPH1_DAT),
		readl(gpio + GPH1_CON + 0x8), readl(gpio + GPH1_CON + 0x10));
	for (i = 0; i < ARRAY_SIZE(amps); i++) {
		struct amp *a = &amps[i];

		mutex_lock(&dsp_lock);
		spk_forget(a);
		mutex_unlock(&dsp_lock);
		if (a->regmap)
			regmap_exit(a->regmap);
		if (a->client)
			i2c_unregister_device(a->client);
		a->regmap = NULL;
		a->client = NULL;
		a->on = false;
	}
	bus_pins_setup();
	amps_reset(false);
	usleep_range(2000, 3000);	/* nRESET low: over 1 ms */
	amps_reset(true);
	usleep_range(5000, 6000);	/* out of reset, before I2C */
	for (i = 0; i < ARRAY_SIZE(amps); i++) {
		ret = amp_probe(&amps[i]);
		if (ret) {
			pr_warn("snd-zrn-abox-bx: %s not set up again (%d)\n", amps[i].name, ret);
			if (amps[i].regmap) {
				regmap_exit(amps[i].regmap);
				amps[i].regmap = NULL;
			}
			if (!first)
				first = ret;
		}
	}
	amp_rescues++;
	amp_rescue_ret = first;
	mutex_unlock(&amp_lock);
	dsp_state_update();
	if (amp_dsp)
		dsp_load_later(clamp(READ_ONCE(dsp_resume_ms), 0, 600000));
	/* a ring may have started while they were away */
	if (READ_ONCE(active) || READ_ONCE(call_on))
		amps_power(true);
}

static void amp_rescue_fn(struct work_struct *work)
{
	amps_rescue();
}

static int amp_rescue_set(const char *arg, const struct kernel_param *kp)
{
	if (!bb_added)
		return -ENODEV;
	amps_rescue();
	return 0;
}
static const struct kernel_param_ops amp_rescue_ops = { .set = amp_rescue_set };
module_param_cb(amp_rescue, &amp_rescue_ops, NULL, 0200);
MODULE_PARM_DESC(amp_rescue, "write 1: set the amps up again now (as after a resume)");

static int abox_pm_notify(struct notifier_block *nb, unsigned long event, void *unused)
{
	if (event == PM_POST_SUSPEND)
		schedule_work(&amp_rescue_work);
	return NOTIFY_DONE;
}
static struct notifier_block abox_pm_nb = { .notifier_call = abox_pm_notify };

/* ---- ALSA ------------------------------------------------------------------ */

static const struct snd_pcm_hardware abox_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = RATE, .rate_max = RATE,
	.channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = BUFFER_BYTES,
	.period_bytes_min = SZ_2K,	/* over 10 ms: the 512-byte SRAM bank */
	.period_bytes_max = BUFFER_BYTES / 2,
	.periods_min = 2, .periods_max = 32,
};

static int abox_pcm_open(struct snd_pcm_substream *s)
{
	struct snd_pcm_hardware *hw = &s->runtime->hw;
	int i;

	*hw = abox_hw;
	cur_vdma = NULL;
	for (i = 0; vdma && i < vdma_count; i++)
		if (vdmas[i].id == vdma && !vdmas[i].capture)
			cur_vdma = &vdmas[i];
	/* a buffer of its own in DRAM, or none (addr 0): then the host's, at
	 * IOVA_VDMA_BUFFER (only 100's is mapped, our pcm buffer) */
	if (vdma && (!cur_vdma || (cur_vdma->addr ? cur_vdma->addr < IOVA_DRAM ||
		     cur_vdma->addr - IOVA_DRAM + cur_vdma->bytes_max > DRAM_SIZE :
		     cur_vdma->id != 100))) {
		pr_err("snd-zrn-abox-bx: no firmware PCM %d to play through\n", vdma);
		cur_vdma = NULL;
		return -ENODEV;
	}
	if (cur_vdma) {
		/* the firmware learns of new data only through ACK: with mmap
		 * (PipeWire) the core calls .ack only if the application
		 * pointer is synced by ioctl; without it the DSP played the
		 * stale buffer, silent, and the pointer crawled */
		hw->info |= SNDRV_PCM_INFO_SYNC_APPLPTR;
		/* its buffer is in the firmware's DRAM: no mmap, as Samsung's */
		if (cur_vdma->addr) {
			hw->info &= ~(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID);
			hw->buffer_bytes_max = cur_vdma->bytes_max;
		}
		if (cur_vdma->period_min)
			hw->period_bytes_min = cur_vdma->period_min;
		if (cur_vdma->period_max)
			hw->period_bytes_max = cur_vdma->period_max;
		if (cur_vdma->periods_min)
			hw->periods_min = cur_vdma->periods_min;
		if (cur_vdma->periods_max)
			hw->periods_max = cur_vdma->periods_max;
	}
	snd_pcm_hw_constraint_integer(s->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	snd_pcm_hw_constraint_step(s->runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	/* the DSP moves one of our periods into RDMA0's ring per period of that
	 * ring, so the two must match: 10 ms periods played at a quarter speed,
	 * aplay's large ones at double; a 10 ms ring the DSP could not keep
	 * full (clicks). 40 ms both sides. */
	if (cur_vdma)
		snd_pcm_hw_constraint_minmax(s->runtime, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					     HW0_PERIOD_BYTES, HW0_PERIOD_BYTES);
	WRITE_ONCE(ap_open, true);
	if (graph)
		rt_open();
	HW0(PCM_PLTDAI_OPEN, 0, 0, 0);
	return pcm_msg(PCM_PLTDAI_OPEN, 0, 0, 0);
}

static int abox_pcm_close(struct snd_pcm_substream *s)
{
	WRITE_ONCE(active, NULL);
	WRITE_ONCE(last_sound, jiffies);
	WRITE_ONCE(ap_open, false);
	cancel_work_sync(&amp_on_work);
	if (!READ_ONCE(call_on)) {
		amps_power(false);
		uaif_off();
	}
	pcm_msg(PCM_PLTDAI_CLOSE, 0, 0, 0);
	HW0(PCM_PLTDAI_HW_FREE, 0, 0, 0);
	HW0(PCM_PLTDAI_CLOSE, 0, 0, 0);
	cur_vdma = NULL;
	if (graph)
		rt_close();
	return 0;
}

static int abox_pcm_hw_params(struct snd_pcm_substream *s, struct snd_pcm_hw_params *p)
{
	struct snd_pcm_runtime *rt = s->runtime;
	int ret;

	/* always the same memory: the DSP sees it at IOVA_RDMA0, or the
	 * firmware PCM's own buffer */
	u32 iova = cur_vdma ? (cur_vdma->addr ?: IOVA_VDMA0) : IOVA_RDMA0;
	bool own = cur_vdma && cur_vdma->addr;

	rt->dma_area = own ? dram + (iova - IOVA_DRAM) : pcm;
	rt->dma_addr = own ? dram_pa + (iova - IOVA_DRAM) : pcm_pa;
	rt->dma_bytes = params_buffer_bytes(p);
	ret = pcm_msg(PCM_SET_BUFFER, iova, params_period_bytes(p), params_periods(p));
	if (!ret)
		ret = pcm_msg(PCM_PLTDAI_HW_PARAMS, params_rate(p), 16, params_channels(p));
	if (!ret)
		ret = HW0(PCM_SET_BUFFER, IOVA_HW0, HW0_PERIOD_BYTES, RT_PERIODS);
	if (!ret)
		ret = HW0(PCM_PLTDAI_HW_PARAMS, RATE, 16, 2);
	if (!ret && graph && !rt_run)
		rt_hw_params();
	return ret;
}

static int abox_pcm_hw_free(struct snd_pcm_substream *s)
{
	WRITE_ONCE(active, NULL);
	pcm_msg(PCM_PLTDAI_HW_FREE, 0, 0, 0);
	s->runtime->dma_area = NULL;
	s->runtime->dma_addr = 0;
	s->runtime->dma_bytes = 0;
	return 0;
}

static int abox_pcm_prepare(struct snd_pcm_substream *s)
{
	memset(s->runtime->dma_area, 0, s->runtime->dma_bytes);
	WRITE_ONCE(dsp_pointer, 0);
	WRITE_ONCE(active, s);
	if (graph && !rt_run)
		rt_prepare();
	HW0(PCM_PLTDAI_PREPARE, 0, 0, 0);
	return pcm_msg(PCM_PLTDAI_PREPARE, 0, 0, 0);
}

static int abox_pcm_trigger(struct snd_pcm_substream *s, int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		WRITE_ONCE(ap_run, true);
		WRITE_ONCE(last_sound, jiffies);
		uaif_on();
		HW0(PCM_PLTDAI_TRIGGER, 1, 0, 0);
		if (graph)
			rt_trigger(true);
		return pcm_msg(PCM_PLTDAI_TRIGGER, 1, 0, 0);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(ap_run, false);
		pcm_msg(PCM_PLTDAI_TRIGGER, 0, 0, 0);
		if (graph && !READ_ONCE(call_on))
			rt_trigger(false);
		HW0(PCM_PLTDAI_TRIGGER, 0, 0, 0);
		return 0;
	}
	return -EINVAL;
}

/* Where the DSP is reading: its last pointer message if any (an IOVA, as in
 * Samsung's abox_rdma_pointer), else RDMA0's status: the period it is in
 * (bits 27:12) and how far into it (11:0), both in 16-byte units as the S9's
 * ABOX 1.x turned out to count them. */
static snd_pcm_uframes_t abox_pcm_pointer(struct snd_pcm_substream *s)
{
	u32 status = readl(sfr + ABOX_RDMA0_STATUS), p = READ_ONCE(dsp_pointer);
	size_t buffer = snd_pcm_lib_buffer_bytes(s);
	u32 base = cur_vdma ? (cur_vdma->addr ?: IOVA_VDMA0) : IOVA_RDMA0;
	size_t pos;

	if (!buffer)
		return 0;
	if (p >= base && p < base + buffer) {
		pos = p - base;
	} else if (cur_vdma) {
		return 0;
	} else {
		if (!(status & BIT(31)))
			return 0;
		pos = ((size_t)((status >> 12) & 0xffff) << 4) + ((size_t)(status & 0xfff) << 4);
	}
	return bytes_to_frames(s->runtime, pos % buffer);
}

/* abox_vdma_ack: a firmware PCM is told where the application has written to */
static int abox_pcm_ack(struct snd_pcm_substream *s)
{
	struct snd_pcm_runtime *rt = s->runtime;

	if (!cur_vdma)
		return 0;
	return pcm_msg(PCM_PLTDAI_ACK,
		       frames_to_bytes(rt, rt->control->appl_ptr % rt->buffer_size), 0, 0);
}

static int abox_pcm_mmap(struct snd_pcm_substream *s, struct vm_area_struct *vma)
{
	return dma_mmap_coherent(&pdev->dev, vma, pcm, pcm_pa, BUFFER_BYTES);
}

static const struct snd_pcm_ops abox_pcm_ops = {
	.open = abox_pcm_open,
	.close = abox_pcm_close,
	.hw_params = abox_pcm_hw_params,
	.hw_free = abox_pcm_hw_free,
	.prepare = abox_pcm_prepare,
	.trigger = abox_pcm_trigger,
	.pointer = abox_pcm_pointer,
	.ack = abox_pcm_ack,
	.mmap = abox_pcm_mmap,
};

static int vol_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = AMP_VOL_MAX - AMP_VOL_MIN;
	return 0;
}

static int vol_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	v->value.integer.value[0] = amp_volume - AMP_VOL_MIN;
	return 0;
}

static int vol_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	int val = clamp_t(int, v->value.integer.value[0], 0, AMP_VOL_MAX - AMP_VOL_MIN) + AMP_VOL_MIN;
	int i;

	if (val == amp_volume)
		return 0;
	mutex_lock(&amp_lock);
	amp_volume = val;
	for (i = 0; i < ARRAY_SIZE(amps); i++)
		if (amps[i].regmap)
			amp_volume_write(&amps[i]);
	mutex_unlock(&amp_lock);
	return 1;
}

static const DECLARE_TLV_DB_MINMAX(vol_tlv, AMP_VOL_MIN * 125 / 10, AMP_VOL_MAX * 125 / 10);

static const struct snd_kcontrol_new vol_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Speaker Playback Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = vol_info, .get = vol_get, .put = vol_put,
	.tlv.p = vol_tlv,
};

/* ---- the codec (CS47L93): the call's microphones ----------------------------- */

/*
 * The microphones: DMIC1 (the main one, at the bottom) on the CS47L93's IN3L
 * and DMIC2 on IN4L (stock beyondx DT samsung,routing; mixer_paths
 * dev-dual-mic: main left, second right). The codec sits on stock spi_2
 * (PERIC0 USI01, spi@10470000), chip select 0, 25 MHz max. Mainline has no
 * PERIC0 pinctrl or SPI here, so the pins are driven through their bank
 * registers, as the amps' I2C is: gpp0-4 CLK, gpp0-5 MOSI, gpp0-6 MISO,
 * gpp0-7 CS (Exynos USI pin order; stock spi2-bus, spi2-cs-func), reset on
 * gpg1-0 (stock codec_reset). PERIC0's GPIO block is clocked at handover
 * (pcie-exynos9820 drives gpg0 there).
 *
 * Supplies: LDO17M vdd_pmic_codec_1p8 (AVDD, DBVDD1, CPVDD1; always on in
 * the stock DT) and LDO18M vdd_codec_1p2 (DCVDD, CPVDD2; NOT always on).
 * Nothing here writes to the PMIC: if the bootloader left LDO18M off, the ID
 * read fails, the pins go back as they were and nothing else is written.
 *
 * The codec side is ztOS's S9 code (audio/driver/snd-zrn-abox.c: 42a0ae9,
 * f47bf94, 34e3d32; the same CS47L93 there): FLL1 locked to UAIF0's bit
 * clock, so neither the 26 MHz MCLK nor the codec's rate converters are
 * needed. The S10 differences: the second mic is IN4L (DMIC2), not IN3R.
 *
 * One step per parameter, so each can be tried alone:
 *   codec_state (read): the pins, UAIF0 and its clock, read only;
 *   mic_probe=1: reset pulse, boot wait, ID 0x6371 (GPIO writes only);
 *   mic=1/0: UAIF0 as I2S master at 48 kHz, 16 bit, 2 ch, its bit clock on,
 *     the codec's path IN3L/IN4L -> AIF1TX1/2, MIC_ENABLE. The routes (NSRC0
 *     and NSRC1 <- UAIF0, nsrcx_texts 8) stay zcall-uplink.sh's (ZCALL_MIC=1).
 * Modified for OmonullOS, November 2026.
 */
#define GPIO_PERIC0		0x10430000
#define  GPP0_CON		0x0000
#define  GPP0_DAT		0x0004
#define  GPP0_PUD		0x0008
#define  GPG1_CON		0x00a0
#define  GPG1_DAT		0x00a4
#define  GPG1_PUD		0x00a8
#define SPI_CLK			4
#define SPI_MOSI		5
#define SPI_MISO		6
#define SPI_CS			7
#define CODEC_RST		0		/* gpg1-0, low = reset */
#define  GPB0_CON		0x0000		/* gpb0-1..4: aud-i2s0-bus, function 2 */
#define  GPB0_DAT		0x0004
#define  GPB0_PUD		0x0008
#define  ABOX_UAIF0_CTRL1	0x0504
#define  ABOX_UAIF0_STATUS	0x050c
#define   QCH_BCLK_UAIF0	BIT(2)		/* enum qchannel ABOX_BCLK_UAIF0 */
#define  CMU_MUX_UAIF0		0x100c		/* cmucal-sfr.c (exynos9820) */
#define  CMU_DIV_UAIF0		0x182c

#define CODEC_ID		0x6371
#define CODEC_SOFTWARE_RESET	0x0000
#define CODEC_HW_REVISION	0x0001
#define CODEC_SYSTEM_CLOCK_1	0x0101
#define CODEC_SAMPLE_RATE_1	0x0102
#define CODEC_FLL1		0x0170	/* fll->base: FLL1 Control 1 is +1 */
#define CODEC_MIC_CP_1		0x0200
#define CODEC_LDO2_CONTROL_1	0x0213
#define CODEC_MIC_BIAS_CTRL_1	0x0218
#define CODEC_MIC_BIAS_CTRL_5	0x021c
#define  CODEC_MICB1A_ENA	0x0001
#define  CODEC_MICB1B_ENA	0x0010
#define CODEC_INPUT_ENABLES	0x0300
#define  CODEC_IN3L_ENA		0x0020
#define  CODEC_IN4L_ENA		0x0080
#define CODEC_IN3L_CONTROL	0x0320
#define CODEC_ADC_VOL_3L	0x0321
#define CODEC_DMIC3L_CONTROL	0x0322
#define CODEC_IN4L_CONTROL	0x0328
#define CODEC_ADC_VOL_4L	0x0329
#define CODEC_DMIC4L_CONTROL	0x032a
#define CODEC_AIF1_BCLK_CTRL	0x0500
#define CODEC_AIF1_RATE_CTRL	0x0503
#define CODEC_AIF1_FORMAT	0x0504
#define CODEC_AIF1_RX_BCLK_RATE	0x0506
#define CODEC_AIF1_FRAME_CTRL_1	0x0507
#define CODEC_AIF1_FRAME_CTRL_2	0x0508
#define CODEC_AIF1_TX_ENABLES	0x0519
#define CODEC_AIF1TX1MIX_SRC1	0x0700
#define CODEC_AIF1TX2MIX_SRC1	0x0708
#define CODEC_TONE_GENERATOR_1	0x0020
#define  CODEC_TONE1_ENA	0x0001
#define CODEC_SRC_TONE1		0x04	/* madera_mixer_values: Tone Generator 1 */
#define CODEC_MIX_VOL_0DB	0x80	/* mixer input volume: (0x40 + dB) << 1 */
#define CODEC_MIX_VOL_M20DB	0x58
#define CODEC_GPIO5_CTRL_1	0x1708	/* GPIO5..8 = AIF1 (pinctrl-cs47l92: pins 4..7) */
#define CODEC_IRQ1_STATUS_1	0x1800
#define CODEC_IRQ1_RAW_STATUS_1	0x1880
#define  CODEC_BOOT_DONE	0x0080
#define CODEC_IRQ1_RAW_STATUS_2	0x1881
#define  CODEC_FLL1_LOCK	0x0100
#define CODEC_SRC_IN3L		0x14	/* madera_mixer_values */
#define CODEC_SRC_IN4L		0x16
#define CODEC_IN_VU		0x0200
#define CODEC_IN_MUTE		0x0100

/* cs47l92_reva_16_patch, as mainline madera-core applies it after boot */
static const u16 codec_patch[][2] = {
	{ 0x3A2, 0x2C29 }, { 0x3A3, 0x0E00 }, { 0x281, 0x0000 }, { 0x282, 0x0000 },
	{ 0x4EA, 0x0100 }, { 0x22B, 0x0000 }, { 0x4A0, 0x0080 }, { 0x4A1, 0x0000 },
	{ 0x4A2, 0x0000 }, { 0x180B, 0x033F }, { 0x190B, 0x033F }, { 0x442, 0x0304 },
	{ 0x34C, 0x0003 }, { 0x124, 0x0C49 }, { 0x120, 0x0345 }, { 0x120, 0x0305 },
	{ 0x4FA, 0x5064 }, { 0x1300, 0x050E }, { 0x1302, 0x0101 }, { 0x1380, 0x02E0 },
	{ 0x1381, 0xF942 }, { 0x1382, 0x04CE }, { 0x1383, 0xFF06 }, { 0x1390, 0x0304 },
	{ 0x1391, 0xF8FF }, { 0x1392, 0x04F3 }, { 0x1393, 0xFF00 }, { 0x13A0, 0x02E0 },
	{ 0x13A1, 0xF942 }, { 0x13A2, 0x04CE }, { 0x13A3, 0xFF06 }, { 0x13B0, 0x0304 },
	{ 0x13B1, 0xF8FF }, { 0x13B2, 0x04F3 }, { 0x13B3, 0xFF00 }, { 0x412, 0x0005 },
	{ 0x41A, 0x0005 }, { 0x422, 0x0005 },
};

static void __iomem *gpio0;		/* PERIC0's GPIO banks */
static DEFINE_MUTEX(codec_lock);
static bool codec_up, mic_on, pins_saved;
static u32 saved_gpp0[3], saved_gpg1[3];	/* CON, DAT, PUD as found */
static u16 codec_id, codec_rev, codec_boot;
static unsigned int codec_errors;
module_param(codec_errors, uint, 0444);

/*
 * The microphones' digital gain over 0 dB, in 0.5 dB steps (0..63), from the
 * next mic=1. The S9 needed +20 dB for recordings (its HAL adds gain, ours
 * not); in a call the voice firmware has its own gain, so this starts at 0.
 */
static int mic_gain = 12;	/* +6 dB: at 0 the far end found the voice a bit low (11.11 17:27) */
module_param(mic_gain, int, 0644);

/*
 * mic_tone=1: from the next mic=1 the codec sends its own tone generator
 * (1 kHz, at -20 dB through the AIF1TX mixers) instead of the microphones,
 * to tell a dead microphone from an uplink that never reaches the far end.
 */
static bool mic_tone;
module_param(mic_tone, bool, 0644);

/*
 * mic_bits: UAIF0's and the codec's AIF1 slot width, 16 or 32, from the next
 * mic=1. With 16 the uplink carried the microphones at twice their pitch, its
 * two channels one 48 kHz sample apart (log.md 11.11 17:10-17:20): the NSRC
 * took one 16-bit value a frame and paired them. Stock never captures UAIF0 at
 * 16 bit (mixer_paths: ABOX UAIF0 Width 24). 32: bit clock 64 fs (3.072 MHz,
 * CMU div 24), FLL1 N 16, AIF1 32-bit words in 32-bit slots.
 */
static int mic_bits = 32;	/* 32: the call 11.11 17:27 was clear */
module_param(mic_bits, int, 0644);
MODULE_PARM_DESC(mic_bits, "UAIF0/AIF1 slot width for the microphones, 16 or 32, from the next mic=1");
static int mic_bits_now = 16;	/* what the running mic=1 used */
MODULE_PARM_DESC(mic_tone, "1: the codec's 1 kHz tone (-20 dB) instead of the microphones, from the next mic=1");
MODULE_PARM_DESC(mic_gain, "microphone gain over 0 dB in 0.5 dB steps, 0..63, from the next mic=1");

/* PERIC0 bank pins: CON 4 bits a pin (0 in, 1 out), DAT 1 bit. codec_lock
 * held; nothing else in this kernel touches gpp0 or gpg1. */
static void cpin_fn(u32 con, int pin, u32 fn)
{
	writel((readl(gpio0 + con) & ~(0xfU << (pin * 4))) | (fn << (pin * 4)), gpio0 + con);
}

static void cpin_set(u32 dat, int pin, int v)
{
	writel((readl(gpio0 + dat) & ~BIT(pin)) | (v ? BIT(pin) : 0), gpio0 + dat);
}

static int cpin_get(u32 dat, int pin)
{
	return (readl(gpio0 + dat) >> pin) & 1;
}

/* SPI mode 0, MSB first, about 250 kHz; codec_lock held */
static void codec_xfer(const u8 *tx, u8 *rx, int n)
{
	int i, b;

	cpin_set(GPP0_DAT, SPI_CS, 0);
	udelay(1);
	for (i = 0; i < n; i++) {
		u8 in = 0;

		for (b = 7; b >= 0; b--) {
			cpin_set(GPP0_DAT, SPI_MOSI, (tx[i] >> b) & 1);
			udelay(2);
			cpin_set(GPP0_DAT, SPI_CLK, 1);
			udelay(1);
			in = (in << 1) | cpin_get(GPP0_DAT, SPI_MISO);
			udelay(1);
			cpin_set(GPP0_DAT, SPI_CLK, 0);
		}
		rx[i] = in;
	}
	udelay(1);
	cpin_set(GPP0_DAT, SPI_CS, 1);
	udelay(2);
}

/* 32-bit address (bit 31 = read), 16 bits of padding, 16-bit value, big
 * endian (mainline cs47l92_16bit_spi_regmap) */
static void codec_write(u32 reg, u16 val)
{
	u8 tx[8] = { reg >> 24, reg >> 16, reg >> 8, reg, 0, 0, val >> 8, val }, rx[8];

	codec_xfer(tx, rx, 8);
}

static u16 codec_read(u32 reg)
{
	u8 tx[8] = { (reg >> 24) | 0x80, reg >> 16, reg >> 8, reg, 0, 0, 0, 0 }, rx[8];

	codec_xfer(tx, rx, 8);
	return (rx[6] << 8) | rx[7];
}

static void codec_update(u32 reg, u16 mask, u16 val)
{
	codec_write(reg, (codec_read(reg) & ~mask) | (val & mask));
}

/* codec_lock held: the SPI pins as GPIOs (CS high, CLK and MOSI low, MISO in,
 * no pulls on the four), the reset pin an output, low */
static void codec_pins_take(void)
{
	int p;

	if (!pins_saved) {
		saved_gpp0[0] = readl(gpio0 + GPP0_CON);
		saved_gpp0[1] = readl(gpio0 + GPP0_DAT);
		saved_gpp0[2] = readl(gpio0 + GPP0_PUD);
		saved_gpg1[0] = readl(gpio0 + GPG1_CON);
		saved_gpg1[1] = readl(gpio0 + GPG1_DAT);
		saved_gpg1[2] = readl(gpio0 + GPG1_PUD);
		pins_saved = true;
	}
	cpin_set(GPP0_DAT, SPI_CS, 1);
	cpin_set(GPP0_DAT, SPI_CLK, 0);
	cpin_set(GPP0_DAT, SPI_MOSI, 0);
	for (p = SPI_CLK; p <= SPI_CS; p++)
		writel(readl(gpio0 + GPP0_PUD) & ~(0xfU << (p * 4)), gpio0 + GPP0_PUD);
	cpin_fn(GPP0_CON, SPI_MISO, 0);
	cpin_fn(GPP0_CON, SPI_CS, 1);
	cpin_fn(GPP0_CON, SPI_CLK, 1);
	cpin_fn(GPP0_CON, SPI_MOSI, 1);
	cpin_set(GPG1_DAT, CODEC_RST, 0);
	cpin_fn(GPG1_CON, CODEC_RST, 1);
}

/* codec_lock held: the four SPI pins and the reset pin as they were found
 * (only those pins' fields: the other pins of both banks are left alone) */
static void codec_pins_give_back(void)
{
	u32 m = 0xffffU << (SPI_CLK * 4), r = 0xfU << (CODEC_RST * 4);

	if (!pins_saved)
		return;
	writel((readl(gpio0 + GPG1_DAT) & ~BIT(CODEC_RST)) | (saved_gpg1[1] & BIT(CODEC_RST)),
	       gpio0 + GPG1_DAT);
	writel((readl(gpio0 + GPG1_CON) & ~r) | (saved_gpg1[0] & r), gpio0 + GPG1_CON);
	writel((readl(gpio0 + GPG1_PUD) & ~r) | (saved_gpg1[2] & r), gpio0 + GPG1_PUD);
	writel((readl(gpio0 + GPP0_CON) & ~m) | (saved_gpp0[0] & m), gpio0 + GPP0_CON);
	writel((readl(gpio0 + GPP0_DAT) & ~0xf0U) | (saved_gpp0[1] & 0xf0U), gpio0 + GPP0_DAT);
	writel((readl(gpio0 + GPP0_PUD) & ~m) | (saved_gpp0[2] & m), gpio0 + GPP0_PUD);
}

/* Reset, boot, identify, patch: as mainline madera_dev_init. A wrong ID
 * (no DCVDD, wrong pin roles) writes nothing more and gives the pins back. */
static int codec_power_up(void)
{
	u16 raw = 0;
	int i;

	mutex_lock(&codec_lock);
	if (codec_up) {
		/* after a reset of the codec (its rail dropped) AIF1's pins are
		 * GPIOs again (S9, 34e3d32): then all over again */
		u16 fn = codec_read(CODEC_GPIO5_CTRL_1) & 0x03ff;

		if (!fn) {
			mutex_unlock(&codec_lock);
			return 0;
		}
		pr_info("snd-zrn-abox-bx: the codec was reset (GP5 fn %03x): setting it up again\n", fn);
		codec_up = false;
	}
	codec_pins_take();
	usleep_range(2000, 3000);
	cpin_set(GPG1_DAT, CODEC_RST, 1);
	usleep_range(2000, 3000);
	for (i = 0; i < 50; i++) {
		raw = codec_read(CODEC_IRQ1_RAW_STATUS_1);
		if (raw != 0xffff && (raw & CODEC_BOOT_DONE))
			break;
		usleep_range(1000, 2000);
	}
	codec_boot = raw;
	codec_id = codec_read(CODEC_SOFTWARE_RESET);
	codec_rev = codec_read(CODEC_HW_REVISION);
	pr_info("snd-zrn-abox-bx: codec id %04x rev %04x, boot status %04x\n",
		codec_id, codec_rev, codec_boot);
	if (codec_id != CODEC_ID) {
		cpin_set(GPG1_DAT, CODEC_RST, 0);
		codec_pins_give_back();
		mutex_unlock(&codec_lock);
		codec_errors++;
		pr_err("snd-zrn-abox-bx: no CS47L93 on the codec bus (id %04x): LDO18M (DCVDD) off, or the SPI pins wrong; pins given back\n",
		       codec_id);
		return -ENODEV;
	}
	codec_write(CODEC_IRQ1_STATUS_1, CODEC_BOOT_DONE);	/* ack, as madera_wait_for_boot */
	for (i = 0; i < ARRAY_SIZE(codec_patch); i++)
		codec_write(codec_patch[i][0], codec_patch[i][1]);
	/* AIF1's four pins to their audio function (GPn_FN 0, default 1 = GPIO) */
	for (i = 0; i < 4; i++)
		codec_update(CODEC_GPIO5_CTRL_1 + 2 * i, 0x03ff, 0x0000);
	codec_up = true;
	mutex_unlock(&codec_lock);
	return 0;
}

/*
 * The codec's clocks from the SoC's bit clock: FLL1 referenced to AIF1BCLK
 * (1.536 MHz = 48 kHz x 16 bit x 2), out 49.152 MHz, SYSCLK 98.304 MHz.
 * Values from mainline madera_fllhj_apply() for fin 1536000, fout 49152000
 * (refdiv 0, gains 0x21f0, lockdet_thr 8, fbdiv 1, N 32, theta 0, lambda 1,
 * hp 0); sequence as madera_fllhj_enable(). codec_lock held.
 */
static int codec_fll_on(void)
{
	int i;

	codec_update(CODEC_FLL1 + 0x1, 0x0004, 0x0004);	/* HOLD */
	codec_write(CODEC_FLL1 + 0x2, 512 / mic_bits_now);	/* N (49.152 MHz / BCLK), CTRL_UPD clear */
	codec_update(CODEC_FLL1 + 0x3, 0xffff, 0);		/* THETA */
	codec_update(CODEC_FLL1 + 0x4, 0xffff, 1);		/* LAMBDA */
	codec_update(CODEC_FLL1 + 0x5, 0x03ff, 1);		/* FB_DIV */
	codec_update(CODEC_FLL1 + 0x6, 0x00c0, 0);		/* REFCLK_DIV */
	codec_update(CODEC_FLL1 + 0x8, 0xffff, 0x21f0);	/* gains */
	codec_update(CODEC_FLL1 + 0xa, 0xd000, 0x1000);	/* HP 0, PHASEDET_ENA */
	codec_update(CODEC_FLL1 + 0xb, 0x001e, 8 << 1);	/* LOCKDET_THR */
	codec_update(CODEC_FLL1 + 0xd, 0x0103, 0);		/* no EFS sync, no fast VCO */
	codec_update(CODEC_FLL1 + 0x1, 0xf000, 8 << 12);	/* REFCLK_SRC AIF1BCLK */
	codec_update(CODEC_FLL1 + 0x1, 0x0001, 0x0001);	/* ENA */
	codec_update(CODEC_FLL1 + 0xb, 0x0001, 0x0001);	/* LOCKDET */
	codec_update(CODEC_FLL1 + 0x2, 0x8000, 0x8000);	/* CTRL_UPD */
	codec_update(CODEC_FLL1 + 0x1, 0x0004, 0);		/* HOLD off: lock */
	for (i = 0; i < 30; i++) {
		if (codec_read(CODEC_IRQ1_RAW_STATUS_2) & CODEC_FLL1_LOCK)
			return 0;
		usleep_range(1000, 2000);
	}
	return -ETIMEDOUT;
}

static void codec_fll_off(void)
{
	codec_update(CODEC_FLL1 + 0xb, 0x0001, 0);		/* LOCKDET */
	codec_update(CODEC_FLL1 + 0x1, 0x0004, 0x0004);	/* HOLD */
	codec_update(CODEC_FLL1 + 0x1, 0x0001, 0);		/* ENA */
	usleep_range(1000, 2000);
	codec_update(CODEC_FLL1 + 0x2, 0x8000, 0);		/* CTRL_UPD */
}

/* IN3L (DMIC1) -> AIF1TX1, IN4L (DMIC2) -> AIF1TX2; UAIF0's bit clock must
 * already run */
static int codec_start(void)
{
	int ret, g = clamp(mic_gain, 0, 63);

	mutex_lock(&codec_lock);
	ret = codec_fll_on();
	if (ret) {
		pr_err("snd-zrn-abox-bx: codec FLL1 did not lock on the bit clock (%04x)\n",
		       codec_read(CODEC_IRQ1_RAW_STATUS_2));
		codec_fll_off();
		codec_errors++;
		goto out;
	}
	codec_write(CODEC_SAMPLE_RATE_1, 0x0003);		/* 48 kHz */
	codec_write(CODEC_SYSTEM_CLOCK_1, 0x0444);		/* 98.304 MHz, on, from FLL1 */
	/* MICVDD 3.0 V from the charge pump (stock DT micvdd), not bypassed */
	codec_update(CODEC_LDO2_CONTROL_1, 0x07e0, 0x24 << 5);
	codec_update(CODEC_MIC_CP_1, 0x0003, 0x0001);
	usleep_range(3000, 4000);
	/* MICBIAS1 1.8 V, external capacitor (stock DT MICBIAS1); 1A for DMIC1,
	 * 1B for DMIC2 (stock samsung,routing) */
	codec_update(CODEC_MIC_BIAS_CTRL_1, 0x81e1, 0x8000 | (3 << 5) | 0x0001);
	codec_update(CODEC_MIC_BIAS_CTRL_5, CODEC_MICB1A_ENA | CODEC_MICB1B_ENA,
		     CODEC_MICB1A_ENA | CODEC_MICB1B_ENA);
	/* IN3 and IN4: digital, 3.072 MHz (stock "IN3/IN4 OSR"), supplied from
	 * MICBIAS1 (stock cirrus,dmic-ref <0 0 1 1>), high-pass on (stock
	 * "IN3L/IN4L HPF Switch") */
	codec_update(CODEC_IN3L_CONTROL, 0x9800, 0x8800);
	codec_update(CODEC_IN4L_CONTROL, 0x9800, 0x8800);
	codec_update(CODEC_DMIC3L_CONTROL, 0x0700, 5 << 8);
	codec_update(CODEC_DMIC4L_CONTROL, 0x0700, 5 << 8);
	/* AIF1: slave, I2S, 16-bit words in 16-bit slots, 32 bit clocks a frame,
	 * at SYSCLK rate 1 */
	codec_update(CODEC_AIF1_BCLK_CTRL, 0x0020, 0);
	codec_update(CODEC_AIF1_RATE_CTRL, 0xf800, 0);
	codec_update(CODEC_AIF1_FORMAT, 0x0007, 2);
	codec_write(CODEC_AIF1_RX_BCLK_RATE, 2 * mic_bits_now);
	codec_write(CODEC_AIF1_FRAME_CTRL_1, (mic_bits_now << 8) | mic_bits_now);
	codec_write(CODEC_AIF1_FRAME_CTRL_2, (mic_bits_now << 8) | mic_bits_now);
	if (mic_tone) {
		codec_update(CODEC_TONE_GENERATOR_1, CODEC_TONE1_ENA, CODEC_TONE1_ENA);
		codec_update(CODEC_AIF1TX1MIX_SRC1 + 1, 0x00fe, CODEC_MIX_VOL_M20DB);
		codec_update(CODEC_AIF1TX2MIX_SRC1 + 1, 0x00fe, CODEC_MIX_VOL_M20DB);
		codec_write(CODEC_AIF1TX1MIX_SRC1, CODEC_SRC_TONE1);
		codec_write(CODEC_AIF1TX2MIX_SRC1, CODEC_SRC_TONE1);
	} else {
		codec_update(CODEC_AIF1TX1MIX_SRC1 + 1, 0x00fe, CODEC_MIX_VOL_0DB);
		codec_update(CODEC_AIF1TX2MIX_SRC1 + 1, 0x00fe, CODEC_MIX_VOL_0DB);
		codec_write(CODEC_AIF1TX1MIX_SRC1, CODEC_SRC_IN3L);
		codec_write(CODEC_AIF1TX2MIX_SRC1, CODEC_SRC_IN4L);
	}
	codec_update(CODEC_AIF1_TX_ENABLES, 0x0003, 0x0003);
	codec_update(CODEC_INPUT_ENABLES, CODEC_IN3L_ENA | CODEC_IN4L_ENA,
		     CODEC_IN3L_ENA | CODEC_IN4L_ENA);
	usleep_range(1000, 3000);
	/* unmuted at mic_gain over 0 dB (0x80), then the volume update (madera_in_ev) */
	codec_update(CODEC_ADC_VOL_3L, 0x01ff, 0x0080 + g);
	codec_update(CODEC_ADC_VOL_4L, 0x01ff, 0x0080 + g);
	codec_update(CODEC_ADC_VOL_3L, CODEC_IN_VU, CODEC_IN_VU);
	codec_update(CODEC_ADC_VOL_4L, CODEC_IN_VU, CODEC_IN_VU);
	pr_info("snd-zrn-abox-bx: codec mics on (inputs %04x, AIF1 TX %04x, sysclk %04x, gain +%d.%d dB%s)\n",
		codec_read(CODEC_INPUT_ENABLES), codec_read(CODEC_AIF1_TX_ENABLES),
		codec_read(CODEC_SYSTEM_CLOCK_1), g / 2, (g & 1) * 5,
		mic_tone ? ", TONE instead of the mics" : "");
out:
	mutex_unlock(&codec_lock);
	return ret;
}

static void codec_stop(void)
{
	mutex_lock(&codec_lock);
	codec_update(CODEC_ADC_VOL_3L, CODEC_IN_MUTE | CODEC_IN_VU, CODEC_IN_MUTE | CODEC_IN_VU);
	codec_update(CODEC_ADC_VOL_4L, CODEC_IN_MUTE | CODEC_IN_VU, CODEC_IN_MUTE | CODEC_IN_VU);
	codec_update(CODEC_INPUT_ENABLES, CODEC_IN3L_ENA | CODEC_IN4L_ENA, 0);
	codec_update(CODEC_AIF1_TX_ENABLES, 0x0003, 0);
	codec_write(CODEC_AIF1TX1MIX_SRC1, 0);
	codec_write(CODEC_AIF1TX2MIX_SRC1, 0);
	codec_update(CODEC_TONE_GENERATOR_1, CODEC_TONE1_ENA, 0);
	codec_update(CODEC_MIC_BIAS_CTRL_5, CODEC_MICB1A_ENA | CODEC_MICB1B_ENA, 0);
	codec_update(CODEC_MIC_BIAS_CTRL_1, 0x0001, 0);
	codec_update(CODEC_MIC_CP_1, 0x0003, 0x0002);		/* off, bypass: reset default */
	codec_update(CODEC_SYSTEM_CLOCK_1, 0x0040, 0);
	codec_fll_off();
	mutex_unlock(&codec_lock);
}

/* UAIF0 as I2S master at 48 kHz, two 16-bit slots (as UAIF1 with graph=0),
 * its bit clock forced on (abox_uaif_set_tristate(0)) before the codec locks
 * to it; MIC_ENABLE so the uplink takes its data */
static void uaif0_on(void)
{
	u32 fmt = mic_bits_now == 32 ? (3 << 3) | 1 : (1 << 3) | 1;	/* abox_get_format */

	/* PLL / 16 (CMU_DIV_AUDIF, set by route_setup) / 48 = 1.536 MHz (16 bit),
	 * / 24 = 3.072 MHz (32 bit) */
	cmu_div(CMU_DIV_UAIF0, mic_bits_now == 32 ? 23 : 47);
	writel(readl(cmu + CMU_MUX_UAIF0) & ~1U, cmu + CMU_MUX_UAIF0);
	writel((readl(pins + GPB0_CON) & ~0xffff0U) | 0x22220, pins + GPB0_CON);
	writel((fmt << 24) | BIT(23) | (1 << 18) | ((mic_bits_now - 1) << 12), sfr + ABOX_UAIF0_CTRL1);
	writel(readl(sfr + ABOX_UAIF0_CTRL0) | BIT(2), sfr + ABOX_UAIF0_CTRL0);	/* master */
	writel(readl(sfr + ABOX_QCHANNEL_DISABLE) | QCH_BCLK_UAIF0, sfr + ABOX_QCHANNEL_DISABLE);
}

static void uaif0_off(void)
{
	writel(readl(sfr + ABOX_UAIF0_CTRL0) & ~BIT(1), sfr + ABOX_UAIF0_CTRL0);
	writel(readl(sfr + ABOX_QCHANNEL_DISABLE) & ~QCH_BCLK_UAIF0, sfr + ABOX_QCHANNEL_DISABLE);
}

static int mic_set(const char *arg, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(arg, &on);

	if (ret)
		return ret;
	if (!booted || !gpio0)
		return -ENODEV;
	if (on == mic_on)
		return 0;
	if (on) {
		ret = codec_power_up();
		if (ret)
			return ret;
		mic_bits_now = mic_bits == 32 ? 32 : 16;
		uaif0_on();
		usleep_range(1000, 2000);
		ret = codec_start();
		if (ret) {
			uaif0_off();
			return ret;
		}
		writel(readl(sfr + ABOX_UAIF0_CTRL0) | BIT(1), sfr + ABOX_UAIF0_CTRL0);	/* MIC_ENABLE */
		mic_on = true;
	} else {
		uaif0_off();
		if (codec_up)
			codec_stop();
		mic_on = false;
	}
	pr_info("snd-zrn-abox-bx: mic %s (%d bit)\n", on ? "on" : "off", mic_bits_now);
	return 0;
}

static int mic_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", mic_on);
}

static const struct kernel_param_ops mic_ops = { .set = mic_set, .get = mic_get };
module_param_cb(mic, &mic_ops, NULL, 0644);
MODULE_PARM_DESC(mic, "1: the CS47L93's microphones on UAIF0 (codec + UAIF0 master), 0: off");

/* mic_probe=1: reset, boot and ID only; reading gives the result */
static int mic_probe_set(const char *arg, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(arg, &on);

	if (ret)
		return ret;
	if (!gpio0)
		return -ENODEV;
	return on ? codec_power_up() : 0;
}

static int mic_probe_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "up=%d id=%04x rev=%04x boot=%04x errors=%u\n",
			  codec_up, codec_id, codec_rev, codec_boot, codec_errors);
}

static const struct kernel_param_ops mic_probe_ops = { .set = mic_probe_set, .get = mic_probe_get };
module_param_cb(mic_probe, &mic_probe_ops, NULL, 0644);
MODULE_PARM_DESC(mic_probe, "1: reset the CS47L93 and read its ID (GPIO writes only); read: the result");

/* read only: what the pins, UAIF0 and its clock hold now */
static int codec_state_get(char *buf, const struct kernel_param *kp)
{
	if (!gpio0)
		return -ENODEV;
	return sysfs_emit(buf,
		"gpp0 con=%08x dat=%02x pud=%08x  gpg1 con=%08x dat=%02x pud=%08x  "
		"gpb0 con=%08x dat=%02x pud=%08x  uaif0 ctrl0=%08x ctrl1=%08x status=%08x  "
		"qch=%08x cmu mux=%08x div=%08x  up=%d mic=%d\n",
		readl(gpio0 + GPP0_CON), readl(gpio0 + GPP0_DAT) & 0xff, readl(gpio0 + GPP0_PUD),
		readl(gpio0 + GPG1_CON), readl(gpio0 + GPG1_DAT) & 0xff, readl(gpio0 + GPG1_PUD),
		readl(pins + GPB0_CON), readl(pins + GPB0_DAT) & 0xff, readl(pins + GPB0_PUD),
		readl(sfr + ABOX_UAIF0_CTRL0), readl(sfr + ABOX_UAIF0_CTRL1),
		readl(sfr + ABOX_UAIF0_STATUS), readl(sfr + ABOX_QCHANNEL_DISABLE),
		readl(cmu + CMU_MUX_UAIF0), readl(cmu + CMU_DIV_UAIF0), codec_up, mic_on);
}

static const struct kernel_param_ops codec_state_ops = { .get = codec_state_get };
module_param_cb(codec_state, &codec_state_ops, NULL, 0444);

/* codec_peek: write a register number (hex), read its value (codec up only) */
static u32 peek_reg;

static int codec_peek_set(const char *arg, const struct kernel_param *kp)
{
	return kstrtou32(arg, 16, &peek_reg);
}

static int codec_peek_get(char *buf, const struct kernel_param *kp)
{
	u16 v;

	if (!codec_up)
		return -ENODEV;
	mutex_lock(&codec_lock);
	v = codec_read(peek_reg);
	mutex_unlock(&codec_lock);
	return sysfs_emit(buf, "%04x: %04x\n", peek_reg, v);
}

static const struct kernel_param_ops codec_peek_ops = { .set = codec_peek_set, .get = codec_peek_get };
module_param_cb(codec_peek, &codec_peek_ops, NULL, 0644);

/* ---- starting and stopping -------------------------------------------------- */

static void teardown(void)
{
	struct device *dev = &pdev->dev;
	int i;

	if (card)
		snd_card_free(card);
	if (pm_nb_added)
		unregister_pm_notifier(&abox_pm_nb);
	cancel_work_sync(&amp_rescue_work);
	cancel_delayed_work_sync(&dsp_load_work);
	cancel_delayed_work_sync(&dsp_reinit_work);
	cancel_work_sync(&amp_on_work);
	cancel_work_sync(&gear_work);
	cancel_work_sync(&rt_lead_work);
	for (i = 0; i < ARRAY_SIZE(amps); i++) {
		amp_power(&amps[i], false);
		spk_forget(&amps[i]);
		if (amps[i].regmap)
			regmap_exit(amps[i].regmap);
		if (amps[i].client)
			i2c_unregister_device(amps[i].client);
	}
	if (bb_added)
		i2c_del_adapter(&bb_adap);
	if (gpio)
		amps_reset(false);
	if (abox_irq > 0)
		free_irq(abox_irq, &abox_irq);
	if (started) {
		cores_off();
		writel(0x0, sfr + ABOX_SYSPOWER_CTRL);
		writel(0x0, mmu + MMU_CTRL);
		writel(0x0, mmu + MMU_CFG);
	}
	if (dram)
		dma_free_coherent(dev, DRAM_SIZE, dram, dram_pa);
	if (vss)
		dma_free_coherent(dev, VSS_SIZE, vss, vss_pa);
	if (vparam)
		dma_free_coherent(dev, VPARAM_SIZE, vparam, vparam_pa);
	if (priv)
		dma_free_coherent(dev, PRIVATE_SIZE, priv, priv_pa);
	if (pcm)
		dma_free_coherent(dev, BUFFER_BYTES, pcm, pcm_pa);
	if (rt_w)
		dma_free_coherent(dev, RT_BYTES, rt_w, rt_w_pa);
	if (rt_r)
		dma_free_coherent(dev, RT_BYTES, rt_r, rt_r_pa);
	if (hw0)
		dma_free_coherent(dev, RT_BYTES, hw0, hw0_pa);
	if (call_r)
		dma_free_coherent(dev, CALL_BYTES, call_r, call_r_pa);
	if (call_w)
		dma_free_coherent(dev, CALL_BYTES, call_w, call_w_pa);
	if (wdma1_buf)
		dma_free_coherent(dev, BRIDGE_BYTES, wdma1_buf, wdma1_pa);
	if (wdma7_buf)
		dma_free_coherent(dev, BRIDGE_BYTES, wdma7_buf, wdma7_pa);
	if (wdma4_buf)
		dma_free_coherent(dev, VI_BYTES, wdma4_buf, wdma4_pa);
	if (lv1)
		dma_free_coherent(dev, SZ_16K, lv1, lv1_pa);
	if (lv2)
		dma_free_coherent(dev, (LV2_TABLES + 1) * SZ_1K, lv2, lv2_pa);
	platform_device_unregister(pdev);
}

/* calliope_dram.bin's chconv4ch_sink_update (0x800ca750), at a count of -1:
 * `cmn r0, #1; beq 0x800ca7cc` reports the RDMA7 underrun (0x800fada8(7,0))
 * and clears the buffer. In a call the VSS answers the report a period later
 * with its margin emergency, FEIN1 (WDMA3) stopped and resumed, which costs
 * WDMA3 a period, so the next RDMA7 period underruns again: one underrun
 * becomes a loop every 20 ms for the rest of the call, the choppy sound
 * (log.md 10.11). rt_lead_us does not stop it (13:47 call). With this the
 * beq becomes a nop: an underrun is printed and plays one stale period, and
 * no emergency follows. Applied at load, only if the words are as expected. */
static bool fw_no_uf_report = true;
module_param(fw_no_uf_report, bool, 0444);
MODULE_PARM_DESC(fw_no_uf_report, "patch out the firmware's RDMA7 underrun report (VSS emergency loop)");
#define FW_UF_OFF	0xca7bc		/* in calliope_dram.bin, at DRAM 0 */

static void patch_firmware(void)
{
	u32 *w = dram + FW_UF_OFF;

	if (!fw_no_uf_report)
		return;
	if (w[0] != 0xe3700001 || w[1] != 0x0a000001) {
		pr_warn("snd-zrn-abox-bx: firmware at 0x%x is %08x %08x, not patched\n",
			FW_UF_OFF, w[0], w[1]);
		return;
	}
	w[1] = 0xe320f000;	/* nop */
	pr_info("snd-zrn-abox-bx: RDMA7 underrun report patched out (0x%x)\n", FW_UF_OFF + 4);
}

static int load_firmware(struct device *dev)
{
	const struct firmware *fw;
	int i, ret;

	memset_io(sram, 0, ABOX_SRAM_SIZE);
	memset(dram, 0, DRAM_SIZE);
	for (i = 0; i < ARRAY_SIZE(fws); i++) {
		ret = fws[i].needed ? request_firmware(&fw, fws[i].name, dev) :
			firmware_request_nowarn(&fw, fws[i].name, dev);
		if (ret) {
			if (fws[i].needed) {
				pr_err("snd-zrn-abox-bx: %s is not in /lib/firmware (%d)\n", fws[i].name, ret);
				return ret;
			}
			continue;
		}
		if (fws[i].off + fw->size > (fws[i].sram ? ABOX_SRAM_SIZE : DRAM_SIZE)) {
			pr_err("snd-zrn-abox-bx: %s is too large (%zu)\n", fws[i].name, fw->size);
			release_firmware(fw);
			return -EINVAL;
		}
		if (fws[i].sram)
			memcpy_toio(sram + fws[i].off, fw->data, fw->size);
		else
			memcpy(dram + fws[i].off, fw->data, fw->size);
		release_firmware(fw);
	}
	patch_firmware();
	return 0;
}

static int dsp_start(struct device *dev)
{
	u32 m[6] = { 0 };
	u32 *l1;
	char v[4];
	int i, ret;

	if (!(readl(pmu + PMU_AUD_STATUS) & 0x1)) {
		pr_err("snd-zrn-abox-bx: the audio power domain is off\n");
		return -ENODEV;
	}
	if (readl(sfr + ABOX_IP_ID) != 0x41424f58) {
		pr_err("snd-zrn-abox-bx: no ABOX at %#x\n", ABOX_SFR);
		return -ENODEV;
	}

	dram = dma_alloc_coherent(dev, DRAM_SIZE, &dram_pa, GFP_KERNEL);
	vss = dma_alloc_coherent(dev, VSS_SIZE, &vss_pa, GFP_KERNEL);
	vparam = dma_alloc_coherent(dev, VPARAM_SIZE, &vparam_pa, GFP_KERNEL);
	priv = dma_alloc_coherent(dev, PRIVATE_SIZE, &priv_pa, GFP_KERNEL);
	pcm = dma_alloc_coherent(dev, BUFFER_BYTES, &pcm_pa, GFP_KERNEL);
	rt_w = dma_alloc_coherent(dev, RT_BYTES, &rt_w_pa, GFP_KERNEL);
	rt_r = dma_alloc_coherent(dev, RT_BYTES, &rt_r_pa, GFP_KERNEL);
	hw0 = dma_alloc_coherent(dev, RT_BYTES, &hw0_pa, GFP_KERNEL);
	call_r = dma_alloc_coherent(dev, CALL_BYTES, &call_r_pa, GFP_KERNEL);
	call_w = dma_alloc_coherent(dev, CALL_BYTES, &call_w_pa, GFP_KERNEL);
	lv1 = dma_alloc_coherent(dev, SZ_16K, &lv1_pa, GFP_KERNEL);
	lv2 = dma_alloc_coherent(dev, (LV2_TABLES + 1) * SZ_1K, &lv2_pa, GFP_KERNEL);
	if (!dram || !vss || !vparam || !priv || !pcm || !rt_w || !rt_r || !hw0 || !call_r || !call_w ||
	    !lv1 || !lv2)
		return -ENOMEM;
	memset(vss, 0, VSS_SIZE);
	memset(vparam, 0, VPARAM_SIZE);
	memset(priv, 0, PRIVATE_SIZE);
	memset(pcm, 0, BUFFER_BYTES);
	memset(lv2, 0, (LV2_TABLES + 1) * SZ_1K);
	l1 = lv1;
	for (i = 0; i < 4096; i++)
		l1[i] = (u32)(lv2_pa >> 4) | 1;
	ret = mmu_map(IOVA_DRAM, dram_pa, DRAM_SIZE);
	if (!ret)
		ret = mmu_map(IOVA_RDMA0, pcm_pa, BUFFER_BYTES);
	if (!ret)	/* the same memory, for firmware PCM 100 */
		ret = mmu_map(IOVA_VDMA0, pcm_pa, BUFFER_BYTES);
	if (!ret)
		ret = mmu_map(IOVA_WDMA3, rt_w_pa, RT_BYTES);
	if (!ret)
		ret = mmu_map(IOVA_RDMA7, rt_r_pa, RT_BYTES);
	if (!ret)
		ret = mmu_map(IOVA_HW0, hw0_pa, RT_BYTES);
	if (!ret)
		ret = mmu_map(IOVA_RDMA4, call_r_pa, CALL_BYTES);
	if (!ret)
		ret = mmu_map(IOVA_WDMA2, call_w_pa, CALL_BYTES);
	if (!ret && bridge_bufs) {
		wdma1_buf = dma_alloc_coherent(dev, BRIDGE_BYTES, &wdma1_pa, GFP_KERNEL);
		wdma7_buf = dma_alloc_coherent(dev, BRIDGE_BYTES, &wdma7_pa, GFP_KERNEL);
		if (wdma1_buf && wdma7_buf) {
			memset(wdma1_buf, 0, BRIDGE_BYTES);
			memset(wdma7_buf, 0, BRIDGE_BYTES);
			ret = mmu_map(IOVA_WDMA1, wdma1_pa, BRIDGE_BYTES);
			if (!ret)
				ret = mmu_map(IOVA_WDMA7, wdma7_pa, BRIDGE_BYTES);
			if (!ret)
				pr_info("snd-zrn-abox-bx: WDMA1/WDMA7 buffers mapped (bridge)\n");
		} else {
			pr_warn("snd-zrn-abox-bx: no memory for the WDMA1/WDMA7 buffers\n");
		}
	}
	if (!ret) {	/* WDMA4, for call_vi; mapped at probe as abox_wdma_new does */
		wdma4_buf = dma_alloc_coherent(dev, VI_BYTES, &wdma4_pa, GFP_KERNEL);
		if (wdma4_buf) {
			memset(wdma4_buf, 0, VI_BYTES);
			ret = mmu_map(IOVA_WDMA4, wdma4_pa, VI_BYTES);
		}
	}
	if (!ret)
		ret = vss_cp ? mmu_map(IOVA_VSS, CP_VSS_PA, CP_VSS_SIZE) :
			mmu_map(IOVA_VSS, vss_pa, VSS_SIZE);
	if (!ret)
		ret = vss_cp ? mmu_map(IOVA_VPARAM, CP_VPARAM_PA, CP_VPARAM_SIZE) :
			mmu_map(IOVA_VPARAM, vparam_pa, VPARAM_SIZE);
	if (!ret && vss_cp) {
		pr_info("snd-zrn-abox-bx: VSS from the CP at %#lx, parameters at %#lx\n",
			CP_VSS_PA, CP_VPARAM_PA);
		vss_mark_clear("probe");
	}
	if (!ret)
		ret = mmu_map(IOVA_PRIVATE, priv_pa, PRIVATE_SIZE);
	if (!ret)
		ret = mmu_map(0x10000000, 0x10000000, SZ_4K);	/* the chip ID */
	if (ret)
		return ret;

	ret = load_firmware(dev);
	if (ret)
		return ret;
	msg_init();

	cores_off();
	writel(0x7, mmu + MMU_CTRL);
	writel(readl(mmu + MMU_INT_STATUS), mmu + MMU_INT_CLEAR);
	writel((15 << 7) | BIT(20) | BIT(24) | BIT(2), mmu + MMU_CFG);
	writel((u32)(lv1_pa >> 12), mmu + MMU_PT_BASE);
	writel(1, mmu + MMU_FLUSH_ALL);
	writel(0x5, mmu + MMU_CTRL);

	/* Samsung's abox_gic_init_gic (GIC_IS_SECURE_FREE) */
	writel(0xff, gicc + 0x004);
	writel(0x3, gicd + 0x000);
	for (i = 0; i < 40; i++)
		writel(0x10101010, gicd + 0x400 + i * 4);
	writel(0x3, gicc + 0x000);
	writel(0x1, sysreg + SYSREG_MISC_CON);

	abox_irq = map_irq();
	if (abox_irq > 0 && request_irq(abox_irq, abox_isr, 0, "snd-zrn-abox-bx", &abox_irq))
		abox_irq = -1;
	if (abox_irq <= 0) {
		pr_err("snd-zrn-abox-bx: no interrupt (%d)\n", abox_irq);
		return -ENODEV;
	}

	pmu_update(PMU_PAD_ABOX, PAD_NORMAL, PAD_NORMAL);
	writel(0x1, sfr + ABOX_SYSPOWER_CTRL);
	started = true;
	for (i = 0; i < 2; i++)
		pmu_update(core_power[i], 0x1, 0x1);

	for (i = 0; i < 300 && !booted; i++) {
		msleep(10);
		if (i > 100)
			rx_one();
	}
	if (!booted) {
		pr_err("snd-zrn-abox-bx: the DSP did not boot (pc %08x lr %08x, status %08x, iommu %08x at %08x)\n",
			readl(gpr + 0x3c), readl(gpr + 0x38), readl(pmu + core_power[0] + 4),
			readl(mmu + MMU_INT_STATUS), readl(mmu + MMU_FAULT_VA));
		return -ETIMEDOUT;
	}
	memcpy(v, &fw_version, 4);
	pr_info("snd-zrn-abox-bx: DSP firmware %c%c%c%c running after %d ms, core %u Hz\n",
		v[3], v[2], v[1], v[0], i * 10, core_hz());
	m[0] = IPC_SYSTEM; m[2] = ABOX_RESUME;
	ipc_send(m, 6);
	msleep(20);
	gear_tell(NULL);
	if (graph) {
		/* stock's start-up values (mixer_paths), then media-speaker's DSP side */
		config_msg(SET_SIFS0_RATE, RATE, 0);
		config_msg(SET_SIFS0_FORMAT, 32, 2);
		config_msg(SET_SIFS1_RATE, RATE, 0);
		config_msg(SET_SIFS1_FORMAT, 32, 4);
		config_msg(SET_SIFM3_RATE, RATE, 0);
		config_msg(SET_SIFM3_FORMAT, 32, 4);
	} else {
		config_msg(SET_SIFS1_RATE, RATE, 0);
		config_msg(SET_SIFS1_FORMAT, 16, 2);
	}
	/* stock mixer_paths: ABOX Audio Mode NORMAL; Sound Type SPEAKER (media) */
	m[0] = IPC_SYSTEM; m[2] = ABOX_SET_MODE; m[3] = 0;
	ipc_send(m, 6);
	m[2] = ABOX_SET_TYPE; m[3] = 1;
	ipc_send(m, 6);
	if (debug > 0)
		comps_log();
	/* stock mixer_paths defaults: RDMA0 runs, fed by the AP's PCM (virtual DAI 0) */
	comp_set_enum("VPCMOUT_DAI0_EN", "On");	/* the stock HAL's three at start */
	comp_set_enum("RDMA0_A", "VPCMOUT_DAI0");
	comp_set_enum("RDMA0_EN", "On");
	if (graph) {
		comp_set_enum("RDMA7_EN", "On");
		comp_set_enum("CHCONV4CH_EN", "On");
		comp_set_enum("RDMA7_A", "CHCONV4CH");
		comp_set_enum("CHCONV4CH_A", "WDMA3");
		/* erap-amp-reference-dsm-bypass's route-wdma4-to-refext4ch */
		comp_set_enum("REFEXT4CH_EN", "On");
		comp_set_enum("REFEXT4CH_A", "WDMA4");
	}
	return 0;
}

static int __init abox_init(void)
{
	struct snd_pcm *snd_pcm;
	struct device *dev;
	char v[4];
	int i, ret;

	INIT_WORK(&gear_work, gear_tell);
	INIT_WORK(&amp_on_work, amp_on_fn);
	INIT_WORK(&amp_rescue_work, amp_rescue_fn);
	INIT_DELAYED_WORK(&dsp_load_work, dsp_load_fn);
	INIT_DELAYED_WORK(&dsp_reinit_work, dsp_reinit_fn);
	pdev = platform_device_register_simple("snd-zrn-abox-bx", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	dev = &pdev->dev;
	dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(36));

	ret = -ENOMEM;
	pmu = devm_ioremap(dev, PMU_BASE, 0x4000);
	sfr = devm_ioremap(dev, ABOX_SFR, 0x10000);
	gpr = devm_ioremap(dev, ABOX_GPR0, 0x100);
	sysreg = devm_ioremap(dev, ABOX_SYSREG, 0x1000);
	sram = devm_ioremap(dev, ABOX_SRAM, ABOX_SRAM_SIZE);
	gicd = devm_ioremap(dev, ABOX_GICD, 0x1000);
	gicc = devm_ioremap(dev, ABOX_GICC, 0x2000);
	mmu = devm_ioremap(dev, SYSMMU_AUD, 0x1000);
	cmu = devm_ioremap(dev, CMU_AUD, 0x8000);
	pins = devm_ioremap(dev, PINCTRL_AUD, 0x1000);
	gpio = devm_ioremap(dev, GPIO_PERIC1, 0x1000);
	gpio0 = devm_ioremap(dev, GPIO_PERIC0, 0x100);	/* mapped only; mic_probe/mic write */
	if (!pmu || !sfr || !gpr || !sysreg || !sram || !gicd || !gicc || !mmu || !cmu || !pins || !gpio || !gpio0)
		goto err;

	/* the amplifiers first: out of reset, they need a moment before I2C */
	bus_pins_setup();
	amps_reset(true);

	ret = dsp_start(dev);
	if (ret)
		goto err;
	route_setup();

	ret = i2c_bit_add_numbered_bus(&bb_adap);
	if (ret)
		goto err;
	bb_added = true;
	for (i = 0; i < ARRAY_SIZE(amps); i++) {
		ret = amp_probe(&amps[i]);
		if (ret) {
			pr_warn("snd-zrn-abox-bx: %s not set up (%d)\n", amps[i].name, ret);
			if (amps[i].regmap) {
				regmap_exit(amps[i].regmap);
				amps[i].regmap = NULL;
			}
		}
	}

	ret = snd_card_new(dev, SNDRV_DEFAULT_IDX1, "ZrnAboxBx", THIS_MODULE, 0, &card);
	if (ret)
		goto err;
	memcpy(v, &fw_version, 4);
	strscpy(card->driver, "snd-zrn-abox-bx", sizeof(card->driver));
	strscpy(card->shortname, "Galaxy S10 5G speakers", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "Galaxy S10 5G speakers (ABOX %c%c%c%c, CS35L40)", v[3], v[2], v[1], v[0]);
	ret = snd_pcm_new(card, "Speakers", 0, 1, 0, &snd_pcm);
	if (!ret) {
		strscpy(snd_pcm->name, "Speakers", sizeof(snd_pcm->name));
		snd_pcm_set_ops(snd_pcm, SNDRV_PCM_STREAM_PLAYBACK, &abox_pcm_ops);
		ret = snd_ctl_add(card, snd_ctl_new1(&vol_control, NULL));
	}
	if (!ret)
		ret = snd_card_register(card);
	if (ret)
		goto err;
	pm_nb_added = !register_pm_notifier(&abox_pm_nb);
	dsp_state_update();
	if (amp_dsp)
		dsp_load_later(0);
	return 0;

err:
	teardown();
	return ret;
}

static void __exit abox_exit(void)
{
	teardown();
}

module_init(abox_init);
module_exit(abox_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S10 5G (Exynos 9820) speakers: ABOX DSP and CS35L40 amplifiers");
MODULE_FIRMWARE("calliope_sram.bin");
MODULE_FIRMWARE("calliope_dram.bin");
MODULE_SOFTDEP("pre: i2c-algo-bit snd-pcm");
