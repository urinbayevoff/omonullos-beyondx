// SPDX-License-Identifier: GPL-2.0
/*
 * DECON scanout for the Galaxy S10 5G (beyondx, Exynos 9820) under mainline.
 *
 * Added for OmonullOS, October 2026, from the S9+ port's s9p-decon.c
 * (third_party/s9plus/src/gpu/drm/s9p), the same idea on the next DPU:
 * S-Boot leaves DECON_F (0x19030000) scanning out its framebuffer at
 * 0xca000000 through DSIM0, panel configured and running (MIPI command mode,
 * hardware TE trigger: stock DT psr_mode 2, trig_mode 0). This driver adopts
 * that pipeline unchanged and owns only the IDMA scanout base address of the
 * live window and the shadow-update request that makes a new one visible at
 * the next TE edge. Window geometry, format, sizes, channel map, DSIM, panel,
 * clocks and power stay S-Boot's and are never written.
 *
 * What is different from the 9810 (docs/s10-5g/log.md, 2026-10-17):
 *  - the IDMA channels are 0x19071000 (GF0, dpp0), 72000 (GF1, dpp2),
 *    73000 (VG, dpp4), 74000 (VGS, dpp5), 75000 (VGF, dpp3), 76000 (VGRFS,
 *    dpp1): in the stock dpp nodes the first reg is the IDMA, the second the
 *    DPP (dpp_drv.c resource 0 = dma_regs);
 *  - there is no +0x800 shadow bank: the normal registers read back, and
 *    S-Boot's window 5 sits on channel 0 (CHMAP 0) with IN_BASE_ADDR_Y
 *    0xca000000, SRC/IMG_SIZE 1440x3040, IN_CON 0x02000000 (ARGB8888);
 *  - writing that base and requesting the window's shadow update moved the
 *    picture on the panel (the owner watched, zdecon-test.sh).
 *
 * A flip completes when DECON clears the window's shadow request at the TE
 * edge that latches it: that is when the new buffer is the one being read,
 * so the event goes out then and the old buffer is only handed back after.
 * No interrupt is used yet. Display off sends one black frame and then the
 * panel DCS display off (tried with the owner watching, 2026-10-17: the
 * panel goes dark and comes back clean). Sleep in / out is there but off:
 * S-Boot's panel setup (DSC, PPS and the rest of Samsung's S6E3HA9
 * init-seq) is not resent after sleep out, and without it the bottom of
 * the panel showed broken dashes until the next boot.
 *
 * The driver name is "exynos" so Mesa pairs it with Panfrost (kmsro).
 */
#include <linux/aperture.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#define DRIVER_NAME "exynos"

/* S-Boot's framebuffer (s3cfb.bootloaderfb=0xca000000 on its command line) */
#define BX_SBOOT_FB_BASE		0xca000000
#define PANEL_W				1440
#define PANEL_H				3040

/* DECON_F, 0x19030000 (cal_9820 regs-decon.h) */
#define DECON_SHADOW_REG_UPDATE_REQ	0x0060
#define  DECON_SHADOW_REQ_WIN(win)	BIT(win)
#define DECON_HW_SW_TRIG_CONTROL	0x0070
#define  DECON_HW_TRIG_EN		BIT(0)
#define  DECON_HW_TRIG_MASK_DECON	BIT(4)
#define DECON_DATA_PATH_CONTROL_0	0x0214
#define  DECON_WIN_EN(win)		BIT(4 * (win) + 0)
#define DECON_DATA_PATH_CONTROL_1	0x0218
#define  DECON_WIN_CHMAP(v, win)	(((v) >> (4 * (win))) & 0x7)
#define DECON_MAX_WIN			6
#define WIN_START_POSITION(win)		(0x1008 + (win) * 0x30)
#define WIN_END_POSITION(win)		(0x100c + (win) * 0x30)

/* DPU_DMA, 0x19070000: one IDMA page per channel, by DPP id */
static const u32 bx_idma_offset[DECON_MAX_WIN] = {
	0x1000, 0x6000, 0x2000, 0x5000, 0x3000, 0x4000,
};
#define IDMA_IN_CON			0x0008
#define IDMA_SRC_SIZE			0x0010
#define IDMA_IMG_SIZE			0x0018
#define IDMA_IN_BASE_ADDR_Y		0x0040

/* DSIM0, 0x19080000 (cal_9820 regs-dsim.h, the same as the 9810's) */
#define DSIM_PHYS			0x19080000
#define DSIM_INTSRC			0x0050
#define  DSIM_INTSRC_PH_FIFO_EMPTY	BIT(28)
#define DSIM_PKTHDR			0x0058
#define  DSIM_PKTHDR_ID(x)		((x) << 0)
#define  DSIM_PKTHDR_DATA0(x)		((x) << 8)
#define  DSIM_PKTHDR_DATA1(x)		((x) << 16)
#define DSIM_PAYLOAD			0x005c
#define MIPI_DCS_SHORT_WRITE		0x05
#define MIPI_DCS_SHORT_WRITE_PARAM	0x15
#define MIPI_DSI_DSC_PRA		0x07	/* compression mode */
#define MIPI_DSI_DSC_PPS		0x0a
#define MIPI_DCS_LONG_WRITE		0x39
#define MIPI_DCS_ENTER_SLEEP_MODE	0x10
#define MIPI_DCS_EXIT_SLEEP_MODE	0x11
#define MIPI_DCS_SET_DISPLAY_OFF	0x28
#define MIPI_DCS_SET_DISPLAY_ON		0x29
#define PANEL_SLEEP_DELAY_MS		120
#define PANEL_FRAME_DELAY_MS		20

static bool bx_panel_dcs = true;
module_param_named(panel_dcs, bx_panel_dcs, bool, 0644);
MODULE_PARM_DESC(panel_dcs, "send the panel DCS display off/on when blanking");

static bool bx_panel_sleep;
module_param_named(panel_sleep, bx_panel_sleep, bool, 0644);
MODULE_PARM_DESC(panel_sleep, "also sleep in/out, with the panel's init sequence on the way out");

struct bx_decon {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	void __iomem *decon;
	void __iomem *dma;		/* DPU_DMA, 0x19070000 */
	void __iomem *dsim;
	unsigned int win;		/* the window S-Boot left scanning */
	unsigned int chan;		/* the IDMA channel behind it */
	bool enabled;
	bool asleep;		/* sleep in sent: the way out re-inits the panel */
	void *black_cpu;		/* one all-black frame, for display off */
	dma_addr_t black_dma;
};

static void __iomem *bx_chan(const struct bx_decon *priv)
{
	return priv->dma + bx_idma_offset[priv->chan];
}

static const u32 bx_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* --------------------------------------------------------------- scanout */

/* See s9p-decon.c: a quarter-millisecond poll keeps the wait for the next TE
 * (up to 16.7 ms) at 64 wake-ups a frame instead of 1600. */
#define SHADOW_POLL_US		250
#define SHADOW_TIMEOUT_US	50000

static int bx_wait_shadow_idle(struct bx_decon *priv)
{
	u32 v;

	return readl_poll_timeout(priv->decon + DECON_SHADOW_REG_UPDATE_REQ,
				  v, (v & DECON_SHADOW_REQ_WIN(priv->win)) == 0,
				  SHADOW_POLL_US, SHADOW_TIMEOUT_US);
}

static void bx_hw_trigger_unmask(struct bx_decon *priv)
{
	u32 v;

	/* read-modify-write: uniLoader's 0x1281 and S-Boot's bits stay */
	v = readl_relaxed(priv->decon + DECON_HW_SW_TRIG_CONTROL);
	v |= DECON_HW_TRIG_EN;
	v &= ~DECON_HW_TRIG_MASK_DECON;
	writel_relaxed(v, priv->decon + DECON_HW_SW_TRIG_CONTROL);
}

static void bx_program_address(struct bx_decon *priv, dma_addr_t addr)
{
	writel_relaxed((u32)addr, bx_chan(priv) + IDMA_IN_BASE_ADDR_Y);
	wmb();
	writel_relaxed(readl_relaxed(priv->decon + DECON_SHADOW_REG_UPDATE_REQ) |
		       DECON_SHADOW_REQ_WIN(priv->win),
		       priv->decon + DECON_SHADOW_REG_UPDATE_REQ);
	bx_hw_trigger_unmask(priv);
}

/* program a buffer and wait until DECON reads it: 0 once latched */
static int bx_flip(struct bx_decon *priv, dma_addr_t addr)
{
	if (bx_wait_shadow_idle(priv)) {
		drm_err_ratelimited(&priv->drm, "previous shadow update stuck, flip skipped\n");
		return -ETIMEDOUT;
	}
	bx_program_address(priv, addr);
	if (bx_wait_shadow_idle(priv)) {
		drm_err_ratelimited(&priv->drm, "flip did not latch within 50 ms\n");
		return -ETIMEDOUT;
	}
	return 0;
}

/*
 * One DSI packet through DSIM's FIFOs (s9p-decon.c; dsim_write_data in
 * Samsung's dpu20): a long one goes into the payload FIFO four bytes a word,
 * then its header carries the length.
 */
static void bx_dsi_pkt(struct bx_decon *priv, u8 id, const u8 *buf, size_t len)
{
	u32 v, d0 = buf[0], d1 = len > 1 ? buf[1] : 0;
	size_t i;

	if (!priv->dsim || !bx_panel_dcs)
		return;
	writel_relaxed(DSIM_INTSRC_PH_FIFO_EMPTY, priv->dsim + DSIM_INTSRC);
	if (id == MIPI_DSI_DSC_PPS || id == MIPI_DCS_LONG_WRITE) {
		for (i = 0; i < len; i += 4) {
			v = buf[i];
			if (i + 1 < len)
				v |= buf[i + 1] << 8;
			if (i + 2 < len)
				v |= buf[i + 2] << 16;
			if (i + 3 < len)
				v |= buf[i + 3] << 24;
			writel_relaxed(v, priv->dsim + DSIM_PAYLOAD);
		}
		d0 = len & 0xff;
		d1 = len >> 8;
	}
	writel_relaxed(DSIM_PKTHDR_ID(id) | DSIM_PKTHDR_DATA0(d0) |
		       DSIM_PKTHDR_DATA1(d1), priv->dsim + DSIM_PKTHDR);
	if (readl_poll_timeout(priv->dsim + DSIM_INTSRC, v,
			       v & DSIM_INTSRC_PH_FIFO_EMPTY, 10, 20000))
		dev_warn_once(priv->drm.dev, "panel did not take packet %02x %02x (INTSRC=%08x)\n",
			      id, buf[0], v);
}

/* a DCS write, short or long by its length, as Samsung's panel driver picks */
static void bx_dcs_write(struct bx_decon *priv, const u8 *buf, size_t len)
{
	bx_dsi_pkt(priv, len == 1 ? MIPI_DCS_SHORT_WRITE :
		   len == 2 ? MIPI_DCS_SHORT_WRITE_PARAM : MIPI_DCS_LONG_WRITE, buf, len);
}

#define bx_seq(priv, ...) do { \
	static const u8 b_[] = { __VA_ARGS__ }; \
	bx_dcs_write(priv, b_, sizeof(b_)); \
} while (0)

static void bx_dcs(struct bx_decon *priv, u8 command)
{
	bx_dcs_write(priv, &command, 1);
}

/*
 * S6E3HA9 on beyondx at 1440x3040, from Samsung's
 * panel/s6e3ha9/s6e3ha9_beyondx_a3_s0_panel.h (init_cmdtbl, the 1440x3040
 * rows of its dsc/pps/scaler/caset/paset maps). Sleep in loses the DSC and
 * the window, which is what left the dashes at the bottom before.
 */
static const u8 bx_pps[128] = {
	0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x0B, 0xE0,
	0x05, 0xA0, 0x00, 0x28, 0x02, 0xD0, 0x02, 0xD0,
	0x02, 0x00, 0x02, 0x68, 0x00, 0x20, 0x04, 0x6C,
	0x00, 0x0A, 0x00, 0x0C, 0x02, 0x77, 0x01, 0xE9,
	0x18, 0x00, 0x10, 0xF0, 0x03, 0x0C, 0x20, 0x00,
	0x06, 0x0B, 0x0B, 0x33, 0x0E, 0x1C, 0x2A, 0x38,
	0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7B,
	0x7D, 0x7E, 0x01, 0x02, 0x01, 0x00, 0x09, 0x40,
	0x09, 0xBE, 0x19, 0xFC, 0x19, 0xFA, 0x19, 0xF8,
	0x1A, 0x38, 0x1A, 0x78, 0x1A, 0xB6, 0x2A, 0xF6,
	0x2B, 0x34, 0x2B, 0x74, 0x3B, 0x74, 0x6B, 0xF4,
};

#define KEY1_ON(p)	bx_seq(p, 0x9F, 0xA5, 0xA5)
#define KEY1_OFF(p)	bx_seq(p, 0x9F, 0x5A, 0x5A)
#define KEY2_ON(p)	bx_seq(p, 0xF0, 0x5A, 0x5A)
#define KEY2_OFF(p)	bx_seq(p, 0xF0, 0xA5, 0xA5)
#define KEY3_ON(p)	bx_seq(p, 0xFC, 0x5A, 0x5A)
#define KEY3_OFF(p)	bx_seq(p, 0xFC, 0xA5, 0xA5)

static void bx_panel_init(struct bx_decon *priv)
{
	static const u8 dsc_on = 0x01;

	KEY1_ON(priv);
	bx_dsi_pkt(priv, MIPI_DSI_DSC_PRA, &dsc_on, 1);
	bx_dsi_pkt(priv, MIPI_DSI_DSC_PPS, bx_pps, sizeof(bx_pps));
	KEY1_OFF(priv);
	bx_dcs(priv, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(PANEL_SLEEP_DELAY_MS);
	KEY2_ON(priv);
	bx_seq(priv, 0xBA, 0x01, 0x26, 0x08, 0x08, 0xF3);	/* scaler off */
	KEY2_OFF(priv);
	bx_seq(priv, 0x2A, 0x00, 0x00, 0x05, 0x9F);		/* columns 0..1439 */
	bx_seq(priv, 0x2B, 0x00, 0x00, 0x0B, 0xDF);		/* rows 0..3039 */
	KEY1_ON(priv);
	bx_seq(priv, 0x1A, 0x1F, 0x00, 0x00, 0x00, 0x00);	/* set area */
	bx_seq(priv, 0x35, 0x00);				/* TE on */
	KEY1_OFF(priv);
	KEY2_ON(priv);
	bx_seq(priv, 0xED, 0x04, 0x4C);				/* err_fg */
	KEY3_ON(priv);
	bx_seq(priv, 0xC5, 0x0D, 0x10, 0xB4, 0x3E, 0x01);	/* ffc */
	KEY3_OFF(priv);
	bx_seq(priv, 0xB9, 0x01, 0xB0, 0xE1, 0x09, 0x00, 0x00, 0x00, 0x11, 0x03);
	bx_seq(priv, 0xF6, 0x43);				/* isc */
	bx_seq(priv, 0x86, 0x01, 0x08, 0x10, 0x21, 0x11, 0x42, 0x01, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);	/* stm */
	KEY2_OFF(priv);
	/* not Samsung's gamma mode 1 (B1 00 01 00 01 00 00): without the
	 * brightness sequence after it the panel came up red and dim */
}

/*
 * Sleep out reloads the panel's registers from its own memory (upstream's
 * s9p-decon, the same on the S9's S6E3HA8), brightness included.
 * Samsung's default AOR (B1 00 14, the brightest normal level),
 * latched by gamma update (F7 03).
 */
static void bx_panel_brightness(struct bx_decon *priv)
{
	KEY2_ON(priv);
	bx_seq(priv, 0xB1, 0x00, 0x14);
	bx_seq(priv, 0xF7, 0x03);
	KEY2_OFF(priv);
}

static void bx_panel_down(struct bx_decon *priv)
{
	bx_dcs(priv, MIPI_DCS_SET_DISPLAY_OFF);
	if (!bx_panel_dcs || !bx_panel_sleep)
		return;
	msleep(PANEL_FRAME_DELAY_MS);
	KEY1_ON(priv);
	bx_dcs(priv, MIPI_DCS_ENTER_SLEEP_MODE);
	KEY1_OFF(priv);
	priv->asleep = true;
	msleep(PANEL_SLEEP_DELAY_MS);
}

/*
 * After a sleep the panel's frame memory holds noise (the white flash), so
 * as upstream does: sleep out, let a frame of the client's buffer go out,
 * brightness, and only then display on. Without the sleep too the frame
 * goes out first: display on before it showed white stripes for a moment
 * when the screen came back on (2026-10-19, at the lock screen).
 */
static void bx_panel_up(struct bx_decon *priv, dma_addr_t addr)
{
	if (bx_panel_dcs && bx_panel_sleep && priv->asleep) {
		bx_panel_init(priv);
		bx_flip(priv, addr);
		msleep(PANEL_FRAME_DELAY_MS);
		bx_panel_brightness(priv);
		priv->asleep = false;
	} else if (!bx_flip(priv, addr) && bx_panel_dcs) {
		msleep(2 * PANEL_FRAME_DELAY_MS);	/* into the panel's memory */
	}
	bx_dcs(priv, MIPI_DCS_SET_DISPLAY_ON);
}

/* ----------------------------------------------------------------- pipe */

static void bx_send_event(struct drm_crtc *crtc)
{
	unsigned long flags;

	if (!crtc->state->event)
		return;
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	drm_crtc_send_vblank_event(crtc, crtc->state->event);
	crtc->state->event = NULL;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void bx_pipe_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state,
			   struct drm_plane_state *plane_state)
{
	struct bx_decon *priv = container_of(pipe, struct bx_decon, pipe);
	dma_addr_t addr = to_drm_gem_dma_obj(plane_state->fb->obj[0])->dma_addr;

	bx_panel_up(priv, addr);	/* flips the buffer, then display on */
	priv->enabled = true;
	bx_send_event(&pipe->crtc);
}

/*
 * A command-mode panel keeps showing the last frame it got, so display off
 * first sends an all-black one (on AMOLED black pixels do not emit), then,
 * when switched on, the DCS display off and sleep. The black frame stays
 * programmed: the client's old buffer goes back to it once the output is
 * off and is drawn into on the way back on (enable flips its own buffer).
 */
static void bx_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct bx_decon *priv = container_of(pipe, struct bx_decon, pipe);

	priv->enabled = false;
	if (priv->black_cpu && !bx_flip(priv, priv->black_dma)) {
		msleep(PANEL_FRAME_DELAY_MS + 30);	/* one frame leaves */
		bx_panel_down(priv);
	}
	bx_send_event(&pipe->crtc);
}

static void bx_pipe_update(struct drm_simple_display_pipe *pipe,
			   struct drm_plane_state *old_state)
{
	struct bx_decon *priv = container_of(pipe, struct bx_decon, pipe);
	struct drm_framebuffer *fb = pipe->plane.state->fb;

	/* the event goes out once DECON reads the new buffer, not before:
	 * otherwise the old one is handed back and drawn into while scanned */
	if (fb && priv->enabled && fb != old_state->fb)
		bx_flip(priv, to_drm_gem_dma_obj(fb->obj[0])->dma_addr);
	bx_send_event(&pipe->crtc);
}

static const struct drm_simple_display_pipe_funcs bx_pipe_funcs = {
	.enable = bx_pipe_enable,
	.disable = bx_pipe_disable,
	.update = bx_pipe_update,
};

/* ------------------------------------------------------------- connector */

static int bx_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_cvt_mode(connector->dev, PANEL_W, PANEL_H, 60, false, false, false);
	if (!mode)
		return 0;
	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	/* 6.7 inch, 1440x3040 at about 505 ppi */
	connector->display_info.width_mm = 72;
	connector->display_info.height_mm = 153;
	return 1;
}

static enum drm_connector_status bx_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static const struct drm_connector_helper_funcs bx_conn_help = {
	.get_modes = bx_get_modes,
};

static const struct drm_connector_funcs bx_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = bx_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ------------------------------------------------------------- drm device */

static const struct drm_mode_config_funcs bx_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_FOPS(bx_fops);

static const struct drm_driver bx_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	DRM_GEM_DMA_DRIVER_OPS,
	.fbdev_probe = drm_fbdev_dma_driver_fbdev_probe,
	.fops = &bx_fops,
	.name = DRIVER_NAME,
	.desc = "Galaxy S10 5G DECON scanout",
	.major = 1,
	.minor = 0,
};

/* ---------------------------------------------------------------- probe */

static int bx_decon_probe(struct platform_device *pdev)
{
	struct bx_decon *priv;
	struct drm_device *drm;
	u32 path0, path1, end, base;
	int ret, i;

	priv = devm_drm_dev_alloc(&pdev->dev, &bx_drm_driver, struct bx_decon, drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);
	drm = &priv->drm;

	priv->decon = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->decon))
		return PTR_ERR(priv->decon);
	priv->dma = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(priv->dma))
		return PTR_ERR(priv->dma);

	/*
	 * The live window: enabled and fullscreen. Only the DECON block is
	 * read here, and then the one IDMA channel that window is mapped to
	 * (the active fetch): reading an idle DPU block can wedge the SoC.
	 */
	path0 = readl_relaxed(priv->decon + DECON_DATA_PATH_CONTROL_0);
	path1 = readl_relaxed(priv->decon + DECON_DATA_PATH_CONTROL_1);
	priv->win = DECON_MAX_WIN;
	for (i = 0; i < DECON_MAX_WIN; i++) {
		if (!(path0 & DECON_WIN_EN(i)))
			continue;
		end = readl_relaxed(priv->decon + WIN_END_POSITION(i));
		dev_info(&pdev->dev, "window %d: enabled, chmap %u, start %08x end %08x\n",
			 i, DECON_WIN_CHMAP(path1, i),
			 readl_relaxed(priv->decon + WIN_START_POSITION(i)), end);
		if (!readl_relaxed(priv->decon + WIN_START_POSITION(i)) &&
		    end == ((PANEL_H - 1) << 16 | (PANEL_W - 1))) {
			priv->win = i;
			priv->chan = DECON_WIN_CHMAP(path1, i);
		}
	}
	if (priv->win == DECON_MAX_WIN || priv->chan >= DECON_MAX_WIN) {
		dev_err(&pdev->dev, "no fullscreen DECON window (path0 %08x path1 %08x)\n",
			path0, path1);
		return -ENODEV;
	}
	base = readl_relaxed(bx_chan(priv) + IDMA_IN_BASE_ADDR_Y);
	dev_info(&pdev->dev, "window %u on IDMA channel %u: base %08x src %08x img %08x con %08x trig %08x\n",
		 priv->win, priv->chan, base,
		 readl_relaxed(bx_chan(priv) + IDMA_SRC_SIZE),
		 readl_relaxed(bx_chan(priv) + IDMA_IMG_SIZE),
		 readl_relaxed(bx_chan(priv) + IDMA_IN_CON),
		 readl_relaxed(priv->decon + DECON_HW_SW_TRIG_CONTROL));
	if (base != BX_SBOOT_FB_BASE) {
		dev_err(&pdev->dev, "channel %u does not scan out S-Boot's framebuffer\n",
			priv->chan);
		return -ENODEV;
	}

	priv->dsim = devm_ioremap(&pdev->dev, DSIM_PHYS, 0x100);
	priv->black_cpu = dma_alloc_wc(&pdev->dev, PANEL_W * PANEL_H * 4,
				       &priv->black_dma, GFP_KERNEL);
	if (priv->black_cpu)
		memset(priv->black_cpu, 0, PANEL_W * PANEL_H * 4);
	else
		dev_warn(&pdev->dev, "no black frame: display off keeps the last image\n");

	/* simpledrm gives the display up; fbcon moves to this driver's fbdev */
	aperture_remove_conflicting_devices(BX_SBOOT_FB_BASE, PANEL_W * PANEL_H * 4,
					    DRIVER_NAME);

	ret = drmm_mode_config_init(drm);
	if (ret)
		goto err_black;
	drm->mode_config.min_width = PANEL_W;
	drm->mode_config.max_width = PANEL_W;
	drm->mode_config.min_height = PANEL_H;
	drm->mode_config.max_height = PANEL_H;
	drm->mode_config.funcs = &bx_mode_funcs;

	ret = drm_connector_init(drm, &priv->connector, &bx_conn_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		goto err_black;
	drm_connector_helper_add(&priv->connector, &bx_conn_help);

	ret = drm_simple_display_pipe_init(drm, &priv->pipe, &bx_pipe_funcs,
					   bx_formats, ARRAY_SIZE(bx_formats), NULL,
					   &priv->connector);
	if (ret)
		goto err_black;
	drm_mode_config_reset(drm);

	platform_set_drvdata(pdev, priv);
	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_black;
	drm_client_setup_with_fourcc(drm, bx_formats[0]);
	drm_info(drm, "DECON scanout %ux%u, window %u, channel %u\n",
		 PANEL_W, PANEL_H, priv->win, priv->chan);
	return 0;

err_black:
	if (priv->black_cpu)
		dma_free_wc(&pdev->dev, PANEL_W * PANEL_H * 4, priv->black_cpu,
			    priv->black_dma);
	return ret;
}

static const struct of_device_id bx_decon_of_match[] = {
	{ .compatible = "samsung,exynos9820-decon" },
	{ },
};
MODULE_DEVICE_TABLE(of, bx_decon_of_match);

static struct platform_driver bx_decon_driver = {
	.probe = bx_decon_probe,
	.driver = {
		.name = "bx-decon",
		.of_match_table = bx_decon_of_match,
		.suppress_bind_attrs = true,	/* no unbind: rmmod is not done here */
	},
};
module_platform_driver(bx_decon_driver);

MODULE_DESCRIPTION("Galaxy S10 5G DECON scanout (bootloader display takeover)");
MODULE_LICENSE("GPL v2");
