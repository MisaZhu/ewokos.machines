#include <stdint.h>
#include <string.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <arch/bcm2712/mailbox.h>
#include <arch/bcm2712/native_hdmi.h>

#define BIT(n)					(1U << (n))
#define VC4_MASK(high, low)			(((uint32_t)(~0U) >> (31 - ((high) - (low)))) << (low))
#define VC4_SHIFT(mask)				(__builtin_ctz(mask))
#define VC4_SET_FIELD(value, mask)		((((uint32_t)(value)) << VC4_SHIFT(mask)) & (mask))

#define PI5_HVS_OFF				0x00580000U
#define PI5_PV0_OFF				0x00410000U
#define PI5_HDMI0_HDMI_OFF			0x00701400U
#define PI5_HDMI0_DVP_OFF			0x00701000U
#define PI5_HDMI0_PHY_OFF			0x00701d00U
#define PI5_HDMI0_RM_OFF			0x00702000U
#define PI5_HDMI0_HD_OFF			0x00720000U

#define RPI_FIRMWARE_GET_CLOCK_RATE		0x00030002u
#define RPI_FIRMWARE_SET_CLOCK_STATE		0x00038001u
#define RPI_FIRMWARE_SET_CLOCK_RATE		0x00038002u
#define RPI_FIRMWARE_SET_DISPLAY_POWER		0x00048019u
#define PI5_FW_CLK_HVS_CORE			4u
#define PI5_FW_CLK_HDMI0_PIXEL			9u
#define PI5_FW_CLK_HDMI0_HSM			13u
#define PI5_FW_CLK_HDMI0_BVB			14u
#define PI5_FW_CLK_HVS_DISP			16u
#define PI5_FW_DISPLAY_HDMI0			2u

#define SCALER_DLIST_START			0x00002000U
#define SCALER5_DLIST_START			0x00004000U
#define HVS_BOOTLOADER_DLIST_END		32U

#define SCALER6_CONTROL				0x00000020U
#define SCALER6_CONTROL_HVS_EN			BIT(31)
#define SCALER6_CONTROL_PF_LINES_MASK		VC4_MASK(22, 18)
#define SCALER6_CONTROL_MAX_REQS_MASK		VC4_MASK(7, 4)
#define SCALER6D0_HVS_ID			0x000000fcU
#define SCALER6D0_PRI_MAP0			0x00000038U
#define SCALER6D0_PRI_MAP1			0x0000003cU
#define SCALER6D0_DISPX_CTRL0_BASE		0x00000100U
#define SCALER6D0_DISPX_CTRL1_BASE		0x00000104U
#define SCALER6D0_DISPX_LPTRS_BASE		0x00000110U
#define SCALER6D0_DISPX_COB_BASE		0x00000114U

#define SCALER6_DISPX_CTRL0(x)			(0x00000030U + ((x) * 0x20U))
#define SCALER6_DISPX_CTRL1(x)			(0x00000034U + ((x) * 0x20U))
#define SCALER6_DISPX_LPTRS(x)			(0x0000003cU + ((x) * 0x20U))
#define SCALER6_DISPX_COB(x)			(0x00000040U + ((x) * 0x20U))
#define SCALER6_DISPX_CTRL0_ENB			BIT(31)
#define SCALER6_DISPX_CTRL0_RESET		BIT(30)
#define SCALER6_DISPX_CTRL0_FWIDTH_MASK		VC4_MASK(28, 16)
#define SCALER6_DISPX_CTRL0_LINES_MASK		VC4_MASK(12, 0)
#define SCALER6_DISPX_CTRL1_INTLACE		BIT(0)
#define SCALER6_DISPX_LPTRS_HEADE_MASK		VC4_MASK(11, 0)
#define SCALER6_DISPX_COB_TOP_MASK		VC4_MASK(31, 16)
#define SCALER6_DISPX_COB_BASE_MASK		VC4_MASK(15, 0)

#define SCALER6_PRI_MAP0			0x000000b8U
#define SCALER6_PRI_MAP1			0x000000bcU

#define SCALER6_CTL0_END			BIT(31)
#define SCALER6_CTL0_VALID			BIT(30)
#define SCALER6_CTL0_NEXT_MASK			VC4_MASK(29, 24)
#define SCALER6_CTL0_ADDR_MODE_MASK		VC4_MASK(22, 20)
#define SCALER6_CTL0_ADDR_MODE_LINEAR		0
#define SCALER6_CTL0_ALPHA_MASK_MASK		VC4_MASK(19, 18)
#define SCALER6_CTL0_ALPHA_MASK_NONE		0
#define SCALER6D_CTL0_ALPHA_MASK_FIXED		3
#define SCALER6_CTL0_UNITY			BIT(15)
#define SCALER6_CTL0_ORDERRGBA_MASK		VC4_MASK(14, 13)
#define SCALER6_CTL0_PIXEL_FORMAT_MASK		VC4_MASK(4, 0)

#define SCALER6_POS0_START_Y_MASK		VC4_MASK(28, 16)
#define SCALER6_POS0_START_X_MASK		VC4_MASK(12, 0)

#define SCALER6_CTL2_ALPHA_MODE_MASK		VC4_MASK(31, 30)
#define SCALER6_CTL2_ALPHA_PREMULT		BIT(29)
#define SCALER6_CTL2_ALPHA_MIX			BIT(28)
#define SCALER5_CTL2_ALPHA_MODE_FIXED		1U
#define SCALER5_CTL2_ALPHA_MODE_SHIFT		30
#define SCALER5_CTL2_ALPHA_SHIFT		4

#define SCALER6_POS2_SRC_LINES_MASK		VC4_MASK(28, 16)
#define SCALER6_POS2_SRC_WIDTH_MASK		VC4_MASK(12, 0)

#define SCALER6_PTR0_UPPER_ADDR_MASK		VC4_MASK(7, 0)
#define SCALER6_PTR2_PITCH_MASK			VC4_MASK(16, 0)

#define HVS_PIXEL_FORMAT_RGB565			4U
#define HVS_PIXEL_FORMAT_RGBA8888		7U
#define HVS_PIXEL_ORDER_XRGB			2U
#define HVS_PIXEL_ORDER_ARGB			2U

#define PV_CONTROL				0x00U
#define PV5_CONTROL_FIFO_LEVEL_HIGH_MASK	VC4_MASK(26, 25)
#define PV_CONTROL_FORMAT_MASK			VC4_MASK(23, 21)
#define PV_CONTROL_FORMAT_24			0U
#define PV_CONTROL_FIFO_LEVEL_MASK		VC4_MASK(20, 15)
#define PV_CONTROL_CLR_AT_START			BIT(14)
#define PV_CONTROL_TRIGGER_UNDERFLOW		BIT(13)
#define PV_CONTROL_WAIT_HSTART			BIT(12)
#define PV_CONTROL_PIXEL_REP_MASK		VC4_MASK(5, 4)
#define PV_CONTROL_CLK_SELECT_MASK		VC4_MASK(3, 2)
#define PV_CONTROL_CLK_SELECT_HDMI0		0U
#define PV_CONTROL_FIFO_CLR			BIT(1)
#define PV_CONTROL_EN				BIT(0)

#define PV_V_CONTROL				0x04U
#define PV_VCONTROL_ODD_TIMING			BIT(29)
#define PV_VCONTROL_CONTINUOUS			BIT(1)
#define PV_VCONTROL_VIDEN			BIT(0)

#define PV_HORZA				0x0cU
#define PV_HORZA_HBP_MASK			VC4_MASK(31, 16)
#define PV_HORZA_HSYNC_MASK			VC4_MASK(15, 0)

#define PV_HORZB				0x10U
#define PV_HORZB_HFP_MASK			VC4_MASK(31, 16)
#define PV_HORZB_HACTIVE_MASK			VC4_MASK(15, 0)

#define PV_VERTA				0x14U
#define PV_VERTA_VBP_MASK			VC4_MASK(31, 16)
#define PV_VERTA_VSYNC_MASK			VC4_MASK(15, 0)

#define PV_VERTB				0x18U
#define PV_VERTB_VFP_MASK			VC4_MASK(31, 16)
#define PV_VERTB_VACTIVE_MASK			VC4_MASK(15, 0)

#define PV_VSYNCD_EVEN				0x08U
#define PV_MUX_CFG				0x34U
#define PV_MUX_CFG_RGB_PIXEL_MUX_MODE_MASK	VC4_MASK(5, 2)
#define PV_MUX_CFG_RGB_PIXEL_MUX_MODE_NO_SWAP	8U
#define PV_PIPE_INIT_CTRL			0x94U
#define PV_PIPE_INIT_CTRL_PV_INIT_WIDTH_MASK	VC4_MASK(11, 8)
#define PV_PIPE_INIT_CTRL_PV_INIT_IDLE_MASK	VC4_MASK(7, 4)
#define PV_PIPE_INIT_CTRL_PV_INIT_EN		BIT(0)

#define HDMI_FIFO_CTL				0x07cU
#define HDMI_SCHEDULER_CONTROL			0x0e8U
#define HDMI_HORZA				0x0ecU
#define HDMI_HORZB				0x0f0U
#define HDMI_VERTA0				0x0f4U
#define HDMI_VERTB0				0x0f8U
#define HDMI_VERTA1				0x100U
#define HDMI_VERTB1				0x104U
#define HDMI_MISC_CONTROL			0x114U
#define HDMI_CLOCK_STOP				0x0bcU

#define VC5_HDMI_HORZA_HFP_MASK			VC4_MASK(28, 16)
#define VC5_HDMI_HORZA_VPOS			BIT(15)
#define VC5_HDMI_HORZA_HPOS			BIT(14)
#define VC5_HDMI_HORZA_HAP_MASK			VC4_MASK(13, 0)
#define VC5_HDMI_HORZB_HBP_MASK			VC4_MASK(26, 16)
#define VC5_HDMI_HORZB_HSP_MASK			VC4_MASK(10, 0)
#define VC5_HDMI_VERTA_VSP_MASK			VC4_MASK(28, 24)
#define VC5_HDMI_VERTA_VFP_MASK			VC4_MASK(22, 16)
#define VC5_HDMI_VERTA_VAL_MASK			VC4_MASK(12, 0)
#define VC5_HDMI_VERTB_VSPO_MASK		VC4_MASK(29, 16)
#define VC4_HDMI_FIFO_CTL_MASTER_SLAVE_N	BIT(0)
#define VC4_HDMI_SCHEDULER_CONTROL_MANUAL_FORMAT BIT(15)
#define VC4_HDMI_SCHEDULER_CONTROL_IGNORE_VSYNC_PREDICTS BIT(5)
#define VC4_HDMI_SCHEDULER_CONTROL_MODE_HDMI	BIT(0)
#define VC4_HDMI_VERTB_VSPO_MASK		VC4_MASK(21, 9)
#define VC4_HDMI_VERTB_VBP_MASK			VC4_MASK(8, 0)
#define VC4_HDMI_MISC_CONTROL_PIXEL_REP_MASK	VC4_MASK(3, 0)
#define VC4_DVP_HT_CLOCK_STOP_PIXEL		BIT(1)

#define HDMI_DVP_CTL				0x0000U
#define HDMI_VID_CTL				0x0044U
#define VC4_HD_VID_CTL_ENABLE			BIT(31)
#define VC4_HD_VID_CTL_UNDERFLOW_ENABLE		BIT(30)
#define VC4_HD_VID_CTL_FRAME_COUNTER_RESET	BIT(29)
#define VC4_HD_VID_CTL_VSYNC_LOW		BIT(28)
#define VC4_HD_VID_CTL_HSYNC_LOW		BIT(27)
#define VC4_HD_VID_CTL_CLRRGB			BIT(23)
#define VC4_HD_VID_CTL_BLANKPIX			BIT(18)
#define VC4_HD_VID_CTL_BLANK_INSERT_EN		BIT(16)

#define HDMI_TX_PHY_RESET_CTL			0x000U
#define HDMI_TX_PHY_POWERUP_CTL			0x004U
#define HDMI_TX_PHY_CTL_0			0x008U
#define HDMI_TX_PHY_CTL_1			0x00cU
#define HDMI_TX_PHY_CTL_2			0x010U
#define HDMI_TX_PHY_CTL_CK			0x014U
#define HDMI_TX_PHY_PLL_REFCLK			0x01cU
#define HDMI_TX_PHY_PLL_POST_KDIV		0x028U
#define HDMI_TX_PHY_PLL_VCOCLK_DIV		0x02cU
#define HDMI_TX_PHY_PLL_CFG			0x044U
#define HDMI_TX_PHY_TMDS_CLK_WORD_SEL		0x054U
#define HDMI_TX_PHY_PLL_MISC_0			0x060U
#define HDMI_TX_PHY_PLL_MISC_1			0x064U
#define HDMI_TX_PHY_PLL_MISC_2			0x068U
#define HDMI_TX_PHY_PLL_MISC_3			0x06cU
#define HDMI_TX_PHY_PLL_MISC_4			0x070U
#define HDMI_TX_PHY_PLL_MISC_5			0x074U
#define HDMI_TX_PHY_PLL_MISC_6			0x078U
#define HDMI_TX_PHY_PLL_MISC_7			0x07cU
#define HDMI_TX_PHY_PLL_MISC_8			0x080U
#define HDMI_TX_PHY_PLL_RESET_CTL		0x190U
#define HDMI_TX_PHY_PLL_POWERUP_CTL		0x194U

#define HDMI_RM_OFFSET				0x018U
#define VC4_HDMI_RM_OFFSET_ONLY			BIT(31)
#define VC4_HDMI_RM_OFFSET_OFFSET_MASK		VC4_MASK(30, 0)

#define VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_BG_PWRUP	BIT(8)
#define VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_LDO_PWRUP	BIT(7)
#define VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_BIAS_PWRUP	BIT(6)
#define VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_TX_CK_PWRUP	BIT(3)
#define VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_TX_2_PWRUP	BIT(2)
#define VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_TX_1_PWRUP	BIT(1)
#define VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_TX_0_PWRUP	BIT(0)

#define VC6_HDMI_TX_PHY_PLL_REFCLK_REFCLK_SEL_CMOS	BIT(13)
#define VC6_HDMI_TX_PHY_PLL_REFCLK_REFFRQ_MASK	VC4_MASK(9, 0)

#define VC6_HDMI_TX_PHY_PLL_POST_KDIV_CLK0_SEL_MASK	VC4_MASK(3, 2)
#define VC6_HDMI_TX_PHY_PLL_POST_KDIV_KDIV_MASK	VC4_MASK(1, 0)

#define VC6_HDMI_TX_PHY_PLL_VCOCLK_DIV_VCODIV_EN	BIT(10)
#define VC6_HDMI_TX_PHY_PLL_VCOCLK_DIV_VCODIV_MASK	VC4_MASK(9, 0)

#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_EXT_CURRENT_CTL_MASK	VC4_MASK(31, 28)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_FFE_ENABLE_MASK		VC4_MASK(27, 27)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_SLEW_RATE_CTL_MASK	VC4_MASK(26, 26)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_FFE_POST_TAP_EN_MASK	VC4_MASK(25, 25)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_LDMOS_BIAS_CTL_MASK	VC4_MASK(24, 23)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_COM_MODE_LDMOS_EN_MASK	VC4_MASK(22, 22)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_EDGE_SEL_MASK		VC4_MASK(21, 21)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_EXT_CURRENT_SRC_HS_EN_MASK VC4_MASK(20, 20)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_TERM_CTL_MASK		VC4_MASK(19, 18)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_EXT_CURRENT_SRC_EN_MASK	VC4_MASK(17, 17)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_INT_CURRENT_SRC_EN_MASK	VC4_MASK(16, 16)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_INT_CURRENT_CTL_MASK	VC4_MASK(15, 12)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_INT_CURRENT_SRC_HS_EN_MASK VC4_MASK(11, 11)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_MAIN_TAP_CURRENT_SELECT_MASK VC4_MASK(10, 8)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_POST_TAP_CURRENT_SELECT_MASK VC4_MASK(7, 5)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_SLEW_CTL_SLOW_LOADING_MASK VC4_MASK(4, 3)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_SLEW_CTL_SLOW_DRIVING_MASK VC4_MASK(2, 1)
#define VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_FFE_PRE_TAP_EN_MASK	VC4_MASK(0, 0)

#define VC6_HDMI_TX_PHY_PLL_RESET_CTL_PLL_RESETB	BIT(0)
#define VC6_HDMI_TX_PHY_PLL_POWERUP_CTL_PLL_PWRUP	BIT(0)

#define VC4_HDMI_TX_PHY_PLL_CFG_PDIV_MASK	VC4_MASK(3, 0)

#define OSCILLATOR_FREQUENCY			54000000ULL
#define VC6_VCO_MIN_FREQ			(8ULL * 1000ULL * 1000ULL * 1000ULL)
#define VC6_VCO_MAX_FREQ			(12ULL * 1000ULL * 1000ULL * 1000ULL)

typedef struct {
    uint32_t buf_size;
    uint32_t code;
    struct {
        uint32_t tag;
        uint32_t val_buf_size;
        uint32_t val_len;
        uint32_t clock_id;
        uint32_t rate_hz;
        uint32_t skip_turbo;
    } tag;
    uint32_t end_tag;
} __attribute__((packed)) fw_set_clock_req_t;

typedef struct {
    uint32_t buf_size;
    uint32_t code;
    struct {
        uint32_t tag;
        uint32_t val_buf_size;
        uint32_t val_len;
        uint32_t clock_id;
        uint32_t rate_hz;
    } tag;
    uint32_t end_tag;
} __attribute__((packed)) fw_get_clock_req_t;

typedef struct {
    uint32_t buf_size;
    uint32_t code;
    struct {
        uint32_t tag;
        uint32_t val_buf_size;
        uint32_t val_len;
        uint32_t clock_id;
        uint32_t state;
    } tag;
    uint32_t end_tag;
} __attribute__((packed)) fw_set_clock_state_req_t;

typedef struct {
    uint32_t buf_size;
    uint32_t code;
    struct {
        uint32_t tag;
        uint32_t val_buf_size;
        uint32_t val_len;
        uint32_t display;
        uint32_t state;
    } tag;
    uint32_t end_tag;
} __attribute__((packed)) fw_set_display_power_req_t;

/* ─── CVT reduced-blanking (v1) mode generation ─── */

#define CVT_RB_H_BLANK			160U
#define CVT_RB_H_SYNC			32U
#define CVT_RB_H_BP			80U
#define CVT_RB_H_FP			(CVT_RB_H_BLANK - CVT_RB_H_SYNC - CVT_RB_H_BP)
#define CVT_RB_MIN_VBLANK_US		460U
#define CVT_RB_V_FP			3U
#define CVT_RB_MIN_V_BP			6U
#define CVT_CLOCK_STEP_HZ		250000U
#define CVT_MIN_PIXEL_CLOCK_HZ		25000000U
#define CVT_MAX_PIXEL_CLOCK_HZ		600000000U

static int _hvs_step_d0 = -1;

static uint32_t cvt_vsync_lines(uint32_t w, uint32_t h) {
    if (w * 3U == h * 4U) {
        return 4U;
    }
    if (w * 9U == h * 16U) {
        return 5U;
    }
    if (w * 10U == h * 16U) {
        return 6U;
    }
    if (w * 4U == h * 5U || w * 9U == h * 15U) {
        return 7U;
    }
    /* Non-standard aspect ratio. */
    return 10U;
}

/*
 * Build CVT-RB timings for an arbitrary resolution instead of relying
 * on a fixed mode table.  Modern HDMI sinks accept CVT-RB for any mode
 * they can display; the reduced blanking also keeps the pixel clock low.
 */
int bcm2712_native_hdmi_cvt_mode(uint32_t w, uint32_t h, uint32_t dep,
        uint32_t refresh_hz, bcm2712_hdmi_mode_t *mode) {
    uint64_t frame_ns;
    uint64_t h_period_est_ns;
    uint32_t vbi_lines;
    uint32_t vsync;
    uint32_t min_vbi;
    uint32_t htotal;
    uint32_t vtotal;
    uint64_t pclk;

    if (mode == NULL) {
        return -1;
    }
    if (dep != 16U && dep != 32U) {
        return -1;
    }
    if (w < 64U || h < 64U || w > 4096U || h > 2160U) {
        return -1;
    }
    if (refresh_hz == 0U) {
        refresh_hz = 60U;
    }

    frame_ns = 1000000000ULL / refresh_hz;
    if (frame_ns <= (uint64_t)CVT_RB_MIN_VBLANK_US * 1000ULL) {
        return -1;
    }
    h_period_est_ns = (frame_ns - (uint64_t)CVT_RB_MIN_VBLANK_US * 1000ULL) / h;
    if (h_period_est_ns == 0ULL) {
        return -1;
    }

    vbi_lines = (uint32_t)(((uint64_t)CVT_RB_MIN_VBLANK_US * 1000ULL) /
            h_period_est_ns) + 1U;
    vsync = cvt_vsync_lines(w, h);
    min_vbi = CVT_RB_V_FP + vsync + CVT_RB_MIN_V_BP;
    if (vbi_lines < min_vbi) {
        vbi_lines = min_vbi;
    }

    htotal = w + CVT_RB_H_BLANK;
    vtotal = h + vbi_lines;
    pclk = (uint64_t)htotal * vtotal * refresh_hz;
    pclk -= pclk % CVT_CLOCK_STEP_HZ;
    if (pclk < CVT_MIN_PIXEL_CLOCK_HZ || pclk > CVT_MAX_PIXEL_CLOCK_HZ) {
        return -1;
    }

    memset(mode, 0, sizeof(*mode));
    mode->width = w;
    mode->height = h;
    mode->depth = dep;
    mode->pixel_clock_hz = (uint32_t)pclk;
    mode->hfp = CVT_RB_H_FP;
    mode->hsync = CVT_RB_H_SYNC;
    mode->hbp = CVT_RB_H_BP;
    mode->vfp = CVT_RB_V_FP;
    mode->vsync = vsync;
    mode->vbp = vbi_lines - CVT_RB_V_FP - vsync;
    /* CVT-RB: hsync positive, vsync negative. */
    mode->hsync_pos = 1U;
    mode->vsync_pos = 0U;
    return 0;
}

static inline void hvs_write(uint32_t off, uint32_t val) {
    put32(_mmio_base + PI5_HVS_OFF + off, val);
}

static inline uint32_t hvs_read(uint32_t off) {
    return get32(_mmio_base + PI5_HVS_OFF + off);
}

static int hvs_is_step_d0(void) {
    uint32_t hvs_id;

    if (_hvs_step_d0 >= 0) {
        return _hvs_step_d0;
    }

    hvs_id = hvs_read(SCALER6D0_HVS_ID);
    _hvs_step_d0 = (hvs_id != 0U && hvs_id != 0xffffffffU) ? 1 : 0;
    return _hvs_step_d0;
}

static inline uint32_t hvs_pri_map0_off(void) {
    return hvs_is_step_d0() ? SCALER6D0_PRI_MAP0 : SCALER6_PRI_MAP0;
}

static inline uint32_t hvs_pri_map1_off(void) {
    return hvs_is_step_d0() ? SCALER6D0_PRI_MAP1 : SCALER6_PRI_MAP1;
}

static inline uint32_t hvs_disp_ctrl0_off(uint32_t chan) {
    return hvs_is_step_d0() ?
            (SCALER6D0_DISPX_CTRL0_BASE + chan * 0x20U) :
            SCALER6_DISPX_CTRL0(chan);
}

static inline uint32_t hvs_disp_ctrl1_off(uint32_t chan) {
    return hvs_is_step_d0() ?
            (SCALER6D0_DISPX_CTRL1_BASE + chan * 0x20U) :
            SCALER6_DISPX_CTRL1(chan);
}

static inline uint32_t hvs_disp_lptrs_off(uint32_t chan) {
    return hvs_is_step_d0() ?
            (SCALER6D0_DISPX_LPTRS_BASE + chan * 0x20U) :
            SCALER6_DISPX_LPTRS(chan);
}

static inline uint32_t hvs_disp_cob_off(uint32_t chan) {
    return hvs_is_step_d0() ?
            (SCALER6D0_DISPX_COB_BASE + chan * 0x20U) :
            SCALER6_DISPX_COB(chan);
}

static inline void pv_write(uint32_t off, uint32_t val) {
    put32(_mmio_base + PI5_PV0_OFF + off, val);
}

static inline uint32_t pv_read(uint32_t off) {
    return get32(_mmio_base + PI5_PV0_OFF + off);
}

static inline void hdmi_write(uint32_t off, uint32_t val) {
    put32(_mmio_base + PI5_HDMI0_HDMI_OFF + off, val);
}

static inline uint32_t hdmi_read(uint32_t off) {
    return get32(_mmio_base + PI5_HDMI0_HDMI_OFF + off);
}

static inline void dvp_write(uint32_t off, uint32_t val) {
    put32(_mmio_base + PI5_HDMI0_DVP_OFF + off, val);
}

static inline uint32_t dvp_read(uint32_t off) {
    return get32(_mmio_base + PI5_HDMI0_DVP_OFF + off);
}

static inline void hd_write(uint32_t off, uint32_t val) {
    put32(_mmio_base + PI5_HDMI0_HD_OFF + off, val);
}

static inline uint32_t hd_read(uint32_t off) {
    return get32(_mmio_base + PI5_HDMI0_HD_OFF + off);
}

static inline void phy_write(uint32_t off, uint32_t val) {
    put32(_mmio_base + PI5_HDMI0_PHY_OFF + off, val);
}

static inline uint32_t phy_read(uint32_t off) {
    return get32(_mmio_base + PI5_HDMI0_PHY_OFF + off);
}

static inline void rm_write(uint32_t off, uint32_t val) {
    put32(_mmio_base + PI5_HDMI0_RM_OFF + off, val);
}

static uint32_t align_up(uint32_t value, uint32_t align) {
    return (value + align - 1U) & (~(align - 1U));
}

static void write_dlist_word(uint32_t word_index, uint32_t value) {
    hvs_write(SCALER5_DLIST_START + word_index * 4U, value);
}

static int firmware_set_clock(uint32_t clock_id, uint32_t rate_hz) {
    fw_set_clock_req_t *req;
    ewokos_addr_t vaddr;
    mail_message_t msg;

    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0) {
        return -1;
    }

    req = (fw_set_clock_req_t *)(uintptr_t)vaddr;
    memset(req, 0, sizeof(*req));
    req->buf_size = sizeof(*req);
    req->tag.tag = RPI_FIRMWARE_SET_CLOCK_RATE;
    req->tag.val_buf_size = 12;
    req->tag.val_len = 12;
    req->tag.clock_id = clock_id;
    req->tag.rate_hz = rate_hz;
    req->tag.skip_turbo = 0;

    memset(&msg, 0, sizeof(msg));
    msg.data = (((uint32_t)dma_phy_addr(0, vaddr)) | MAILBOX_VC_ALIAS_NONCACHED) >> 4;
    msg.channel = PROPERTY_CHANNEL;

    if (bcm2712_mailbox_call_timeout(&msg, 0) != 0 ||
            (req->code & 0x80000000u) == 0) {
        dma_free(0, vaddr);
        return -1;
    }

    dma_free(0, vaddr);
    return 0;
}

static int firmware_set_clock_state(uint32_t clock_id, uint32_t state) {
    fw_set_clock_state_req_t *req;
    ewokos_addr_t vaddr;
    mail_message_t msg;

    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0) {
        return -1;
    }

    req = (fw_set_clock_state_req_t *)(uintptr_t)vaddr;
    memset(req, 0, sizeof(*req));
    req->buf_size = sizeof(*req);
    req->tag.tag = RPI_FIRMWARE_SET_CLOCK_STATE;
    req->tag.val_buf_size = 8;
    req->tag.val_len = 8;
    req->tag.clock_id = clock_id;
    req->tag.state = state;

    memset(&msg, 0, sizeof(msg));
    msg.data = (((uint32_t)dma_phy_addr(0, vaddr)) | MAILBOX_VC_ALIAS_NONCACHED) >> 4;
    msg.channel = PROPERTY_CHANNEL;

    if (bcm2712_mailbox_call_timeout(&msg, 0) != 0 ||
            (req->code & 0x80000000u) == 0) {
        dma_free(0, vaddr);
        return -1;
    }

    dma_free(0, vaddr);
    return 0;
}

static int firmware_set_display_power(uint32_t display, uint32_t state) {
    fw_set_display_power_req_t *req;
    ewokos_addr_t vaddr;
    mail_message_t msg;

    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0) {
        return -1;
    }

    req = (fw_set_display_power_req_t *)(uintptr_t)vaddr;
    memset(req, 0, sizeof(*req));
    req->buf_size = sizeof(*req);
    req->tag.tag = RPI_FIRMWARE_SET_DISPLAY_POWER;
    req->tag.val_buf_size = 8;
    req->tag.val_len = 8;
    req->tag.display = display;
    req->tag.state = state;

    memset(&msg, 0, sizeof(msg));
    msg.data = (((uint32_t)dma_phy_addr(0, vaddr)) | MAILBOX_VC_ALIAS_NONCACHED) >> 4;
    msg.channel = PROPERTY_CHANNEL;

    if (bcm2712_mailbox_call_timeout(&msg, 0) != 0 ||
            (req->code & 0x80000000u) == 0) {
        dma_free(0, vaddr);
        return -1;
    }

    dma_free(0, vaddr);
    return 0;
}

static uint32_t firmware_get_clock(uint32_t clock_id) {
    fw_get_clock_req_t *req;
    ewokos_addr_t vaddr;
    mail_message_t msg;
    uint32_t rate = 0;

    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0) {
        return 0;
    }

    req = (fw_get_clock_req_t *)(uintptr_t)vaddr;
    memset(req, 0, sizeof(*req));
    req->buf_size = sizeof(*req);
    req->tag.tag = RPI_FIRMWARE_GET_CLOCK_RATE;
    req->tag.val_buf_size = 8;
    req->tag.val_len = 4;
    req->tag.clock_id = clock_id;

    memset(&msg, 0, sizeof(msg));
    msg.data = (((uint32_t)dma_phy_addr(0, vaddr)) | MAILBOX_VC_ALIAS_NONCACHED) >> 4;
    msg.channel = PROPERTY_CHANNEL;

    if (bcm2712_mailbox_call_timeout(&msg, 0) == 0 &&
            (req->code & 0x80000000u) != 0 &&
            (req->tag.val_len & 0x80000000u) != 0) {
        rate = req->tag.rate_hz;
    }

    dma_free(0, vaddr);
    return rate;
}

/*
 * Linux vc4 only places a *minimum* rate request on the HVS core/disp
 * clocks (clk_set_min_rate); the firmware keeps them at their default
 * (910MHz on Pi5). Hard-setting 500MHz here instead *downclocked* the
 * firmware CORE/VPU clock tree, which starved unrelated peripherals
 * (SDIO WiFi host, RP1 xHCI) into timeouts. Only raise, never lower.
 */
static void firmware_raise_clock(uint32_t clock_id, uint32_t min_rate_hz) {
    uint32_t cur = firmware_get_clock(clock_id);

    if (cur >= min_rate_hz) {
        return;
    }
    (void)firmware_set_clock(clock_id, min_rate_hz);
}

static void program_required_clocks(const bcm2712_hdmi_mode_t *mode) {
    uint32_t bvb_rate = (mode->pixel_clock_hz > 148500000U) ? 150000000U : 75000000U;
    uint32_t hsm_rate = (mode->pixel_clock_hz / 100U) * 101U;

    if (hsm_rate < 120000000U) {
        hsm_rate = 120000000U;
    }

    (void)firmware_set_clock_state(PI5_FW_CLK_HVS_CORE, 1U);
    (void)firmware_set_clock_state(PI5_FW_CLK_HVS_DISP, 1U);
    (void)firmware_set_clock_state(PI5_FW_CLK_HDMI0_HSM, 1U);
    (void)firmware_set_clock_state(PI5_FW_CLK_HDMI0_PIXEL, 1U);
    (void)firmware_set_clock_state(PI5_FW_CLK_HDMI0_BVB, 1U);
    (void)firmware_set_display_power(PI5_FW_DISPLAY_HDMI0, 1U);
    /* Ignore failures here: firmware may already have these clocks running. */
    (void)firmware_set_clock(PI5_FW_CLK_HDMI0_HSM, hsm_rate);
    (void)firmware_set_clock(PI5_FW_CLK_HDMI0_PIXEL, mode->pixel_clock_hz);
    (void)firmware_set_clock(PI5_FW_CLK_HDMI0_BVB, bvb_rate);
    firmware_raise_clock(PI5_FW_CLK_HVS_CORE, 500000000U);
    firmware_raise_clock(PI5_FW_CLK_HVS_DISP, 500000000U);
}

static uint32_t build_low_rate_phy_ctrl_word(void) {
    return VC4_SET_FIELD(8, VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_EXT_CURRENT_CTL_MASK) |
            VC4_SET_FIELD(1, VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_LDMOS_BIAS_CTL_MASK) |
            VC4_SET_FIELD(1, VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_EXT_CURRENT_SRC_EN_MASK) |
            VC4_SET_FIELD(8, VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_INT_CURRENT_CTL_MASK) |
            VC4_SET_FIELD(7, VC6_HDMI_TX_PHY_HDMI_CTRL_CHX_MAIN_TAP_CURRENT_SELECT_MASK);
}

static uint32_t phy_get_rm_offset(unsigned long long vco_freq) {
    uint64_t offset = vco_freq * 2ULL;

    offset <<= 22;
    offset /= OSCILLATOR_FREQUENCY;
    offset >>= 2;
    return (uint32_t)offset;
}

static unsigned long long vc6_phy_get_vco_freq(unsigned long long tmds_rate,
        unsigned int *vco_div) {
    unsigned int div = 0;
    unsigned int min_div;
    unsigned int max_div;

    while (tmds_rate * div * 10ULL < VC6_VCO_MIN_FREQ) {
        div++;
    }
    min_div = div;

    while (tmds_rate * (div + 1U) * 10ULL < VC6_VCO_MAX_FREQ) {
        div++;
    }
    max_div = div;

    div = min_div + (max_div - min_div) / 2U;
    *vco_div = div;
    return tmds_rate * div * 10ULL;
}

static void hdmi0_phy_init(const bcm2712_hdmi_mode_t *mode) {
    unsigned int vco_div = 0;
    unsigned long long vco_freq = vc6_phy_get_vco_freq(mode->pixel_clock_hz, &vco_div);
    uint32_t phy_ctrl = build_low_rate_phy_ctrl_word();

    phy_write(HDMI_TX_PHY_RESET_CTL, 0);
    phy_write(HDMI_TX_PHY_POWERUP_CTL, 0);

    phy_write(HDMI_TX_PHY_PLL_MISC_0, 0x810c6000);
    phy_write(HDMI_TX_PHY_PLL_MISC_1, 0x00b8c451);
    phy_write(HDMI_TX_PHY_PLL_MISC_2, 0x46402e31);
    phy_write(HDMI_TX_PHY_PLL_MISC_3, 0x00b8c005);
    phy_write(HDMI_TX_PHY_PLL_MISC_4, 0x42410261);
    phy_write(HDMI_TX_PHY_PLL_MISC_5, 0xcc021001);
    phy_write(HDMI_TX_PHY_PLL_MISC_6, 0xc8301c80);
    phy_write(HDMI_TX_PHY_PLL_MISC_7, 0xb0804444);
    phy_write(HDMI_TX_PHY_PLL_MISC_8, 0xf80f8000);

    phy_write(HDMI_TX_PHY_PLL_REFCLK,
            VC6_HDMI_TX_PHY_PLL_REFCLK_REFCLK_SEL_CMOS |
            VC4_SET_FIELD(54, VC6_HDMI_TX_PHY_PLL_REFCLK_REFFRQ_MASK));
    phy_write(HDMI_TX_PHY_RESET_CTL, 0x7f);

    rm_write(HDMI_RM_OFFSET,
            VC4_HDMI_RM_OFFSET_ONLY |
            VC4_SET_FIELD(phy_get_rm_offset(vco_freq), VC4_HDMI_RM_OFFSET_OFFSET_MASK));

    phy_write(HDMI_TX_PHY_PLL_VCOCLK_DIV,
            VC6_HDMI_TX_PHY_PLL_VCOCLK_DIV_VCODIV_EN |
            VC4_SET_FIELD(vco_div, VC6_HDMI_TX_PHY_PLL_VCOCLK_DIV_VCODIV_MASK));
    phy_write(HDMI_TX_PHY_PLL_CFG, VC4_SET_FIELD(0, VC4_HDMI_TX_PHY_PLL_CFG_PDIV_MASK));
    phy_write(HDMI_TX_PHY_PLL_POST_KDIV,
            VC4_SET_FIELD(2, VC6_HDMI_TX_PHY_PLL_POST_KDIV_CLK0_SEL_MASK) |
            VC4_SET_FIELD(1, VC6_HDMI_TX_PHY_PLL_POST_KDIV_KDIV_MASK));

    phy_write(HDMI_TX_PHY_CTL_0, phy_ctrl);
    phy_write(HDMI_TX_PHY_CTL_1, phy_ctrl);
    phy_write(HDMI_TX_PHY_CTL_2, phy_ctrl);
    phy_write(HDMI_TX_PHY_CTL_CK, phy_ctrl);
    phy_write(HDMI_TX_PHY_TMDS_CLK_WORD_SEL, 0);

    phy_write(HDMI_TX_PHY_POWERUP_CTL,
            VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_BG_PWRUP |
            VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_LDO_PWRUP |
            VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_BIAS_PWRUP |
            VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_TX_CK_PWRUP |
            VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_TX_2_PWRUP |
            VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_TX_1_PWRUP |
            VC6_HDMI_TX_PHY_HDMI_POWERUP_CTL_TX_0_PWRUP);
    phy_write(HDMI_TX_PHY_PLL_POWERUP_CTL, VC6_HDMI_TX_PHY_PLL_POWERUP_CTL_PLL_PWRUP);
    phy_write(HDMI_TX_PHY_PLL_RESET_CTL,
            phy_read(HDMI_TX_PHY_PLL_RESET_CTL) & ~VC6_HDMI_TX_PHY_PLL_RESET_CTL_PLL_RESETB);
    phy_write(HDMI_TX_PHY_PLL_RESET_CTL,
            phy_read(HDMI_TX_PHY_PLL_RESET_CTL) | VC6_HDMI_TX_PHY_PLL_RESET_CTL_PLL_RESETB);
}

static void hdmi0_set_timings(const bcm2712_hdmi_mode_t *mode) {
    uint32_t hdisplay = mode->width;
    uint32_t hsync_start = mode->width + mode->hfp;
    uint32_t hsync_end = hsync_start + mode->hsync;
    uint32_t htotal = hsync_end + mode->hbp;
    uint32_t vdisplay = mode->height;
    uint32_t vsync_start = mode->height + mode->vfp;
    uint32_t vsync_end = vsync_start + mode->vsync;
    uint32_t vtotal = vsync_end + mode->vbp;
    uint32_t verta = VC4_SET_FIELD(mode->vsync, VC5_HDMI_VERTA_VSP_MASK) |
            VC4_SET_FIELD(mode->vfp, VC5_HDMI_VERTA_VFP_MASK) |
            VC4_SET_FIELD(vdisplay, VC5_HDMI_VERTA_VAL_MASK);
    uint32_t vertb_even = VC4_SET_FIELD(0, VC5_HDMI_VERTB_VSPO_MASK) |
            VC4_SET_FIELD(vtotal - vsync_end, VC4_HDMI_VERTB_VBP_MASK);
    uint32_t vertb = VC4_SET_FIELD(htotal >> 2, VC5_HDMI_VERTB_VSPO_MASK) |
            VC4_SET_FIELD(vtotal - vsync_end, VC4_HDMI_VERTB_VBP_MASK);

    hdmi_write(HDMI_HORZA,
            (mode->vsync_pos ? VC5_HDMI_HORZA_VPOS : 0) |
            (mode->hsync_pos ? VC5_HDMI_HORZA_HPOS : 0) |
            VC4_SET_FIELD(hdisplay, VC5_HDMI_HORZA_HAP_MASK) |
            VC4_SET_FIELD(hsync_start - hdisplay, VC5_HDMI_HORZA_HFP_MASK));
    hdmi_write(HDMI_HORZB,
            VC4_SET_FIELD(htotal - hsync_end, VC5_HDMI_HORZB_HBP_MASK) |
            VC4_SET_FIELD(hsync_end - hsync_start, VC5_HDMI_HORZB_HSP_MASK));
    hdmi_write(HDMI_VERTA0, verta);
    hdmi_write(HDMI_VERTA1, verta);
    hdmi_write(HDMI_VERTB0, vertb_even);
    hdmi_write(HDMI_VERTB1, vertb);
    hdmi_write(HDMI_MISC_CONTROL,
            hdmi_read(HDMI_MISC_CONTROL) & ~VC4_HDMI_MISC_CONTROL_PIXEL_REP_MASK);
    dvp_write(HDMI_CLOCK_STOP, 0);
}

static void hdmi0_prepare_output(void) {
        hdmi_write(HDMI_SCHEDULER_CONTROL,
                        hdmi_read(HDMI_SCHEDULER_CONTROL) |
                        VC4_HDMI_SCHEDULER_CONTROL_MANUAL_FORMAT |
                        VC4_HDMI_SCHEDULER_CONTROL_IGNORE_VSYNC_PREDICTS);
        hdmi_write(HDMI_FIFO_CTL, VC4_HDMI_FIFO_CTL_MASTER_SLAVE_N);
}

static void hdmi0_reset(void) {
    hd_write(HDMI_DVP_CTL, 0);
    dvp_write(HDMI_CLOCK_STOP, dvp_read(HDMI_CLOCK_STOP) | VC4_DVP_HT_CLOCK_STOP_PIXEL);
}

static void pv0_configure(const bcm2712_hdmi_mode_t *mode) {
    uint32_t fifo_level = 46U;
    uint32_t control;

    pv_write(PV_CONTROL, pv_read(PV_CONTROL) & ~PV_CONTROL_EN);
    pv_write(PV_CONTROL, pv_read(PV_CONTROL) | PV_CONTROL_FIFO_CLR);

    pv_write(PV_HORZA,
            VC4_SET_FIELD(mode->hbp, PV_HORZA_HBP_MASK) |
            VC4_SET_FIELD(mode->hsync, PV_HORZA_HSYNC_MASK));
    pv_write(PV_HORZB,
            VC4_SET_FIELD(mode->hfp, PV_HORZB_HFP_MASK) |
            VC4_SET_FIELD(mode->width, PV_HORZB_HACTIVE_MASK));
    pv_write(PV_V_CONTROL, PV_VCONTROL_CONTINUOUS | PV_VCONTROL_ODD_TIMING);
    pv_write(PV_VSYNCD_EVEN, 0);
    pv_write(PV_VERTA,
            VC4_SET_FIELD(mode->vbp, PV_VERTA_VBP_MASK) |
            VC4_SET_FIELD(mode->vsync, PV_VERTA_VSYNC_MASK));
    pv_write(PV_VERTB,
            VC4_SET_FIELD(mode->vfp, PV_VERTB_VFP_MASK) |
            VC4_SET_FIELD(mode->height, PV_VERTB_VACTIVE_MASK));
    pv_write(PV_MUX_CFG,
            VC4_SET_FIELD(PV_MUX_CFG_RGB_PIXEL_MUX_MODE_NO_SWAP,
                PV_MUX_CFG_RGB_PIXEL_MUX_MODE_MASK));
    pv_write(PV_PIPE_INIT_CTRL,
            VC4_SET_FIELD(1, PV_PIPE_INIT_CTRL_PV_INIT_WIDTH_MASK) |
            VC4_SET_FIELD(1, PV_PIPE_INIT_CTRL_PV_INIT_IDLE_MASK) |
            PV_PIPE_INIT_CTRL_PV_INIT_EN);

    control = VC4_SET_FIELD(PV_CONTROL_FORMAT_24, PV_CONTROL_FORMAT_MASK) |
            VC4_SET_FIELD((fifo_level >> 6) & 0x3U, PV5_CONTROL_FIFO_LEVEL_HIGH_MASK) |
            VC4_SET_FIELD(fifo_level & 0x3fU, PV_CONTROL_FIFO_LEVEL_MASK) |
            VC4_SET_FIELD(0, PV_CONTROL_PIXEL_REP_MASK) |
            VC4_SET_FIELD(PV_CONTROL_CLK_SELECT_HDMI0, PV_CONTROL_CLK_SELECT_MASK) |
            PV_CONTROL_FIFO_CLR |
            PV_CONTROL_CLR_AT_START |
            PV_CONTROL_TRIGGER_UNDERFLOW |
            PV_CONTROL_WAIT_HSTART;
    pv_write(PV_CONTROL, control);
}

static void hvs0_init(const bcm2712_hdmi_mode_t *mode, uint32_t fb_addr, uint32_t pitch) {
    uint32_t ctl0;
    uint32_t ctl2;
    uint32_t word_base = HVS_BOOTLOADER_DLIST_END;

    hvs_write(SCALER6_CONTROL,
            SCALER6_CONTROL_HVS_EN |
            VC4_SET_FIELD(8, SCALER6_CONTROL_PF_LINES_MASK) |
            VC4_SET_FIELD(15, SCALER6_CONTROL_MAX_REQS_MASK));
    hvs_write(hvs_pri_map0_off(), 0xffffffffU);
    hvs_write(hvs_pri_map1_off(), 0xffffffffU);

    hvs_write(hvs_disp_cob_off(2),
            VC4_SET_FIELD(3840, SCALER6_DISPX_COB_TOP_MASK) |
            VC4_SET_FIELD(0, SCALER6_DISPX_COB_BASE_MASK));
    hvs_write(hvs_disp_cob_off(1),
            VC4_SET_FIELD(19200, SCALER6_DISPX_COB_TOP_MASK) |
            VC4_SET_FIELD(3856, SCALER6_DISPX_COB_BASE_MASK));
    hvs_write(hvs_disp_cob_off(0),
            VC4_SET_FIELD(34560, SCALER6_DISPX_COB_TOP_MASK) |
            VC4_SET_FIELD(19216, SCALER6_DISPX_COB_BASE_MASK));

    ctl0 = SCALER6_CTL0_VALID |
            VC4_SET_FIELD(9, SCALER6_CTL0_NEXT_MASK) |
            VC4_SET_FIELD(SCALER6_CTL0_ADDR_MODE_LINEAR, SCALER6_CTL0_ADDR_MODE_MASK) |
            VC4_SET_FIELD(hvs_is_step_d0() ? SCALER6D_CTL0_ALPHA_MASK_FIXED :
                    SCALER6_CTL0_ALPHA_MASK_NONE, SCALER6_CTL0_ALPHA_MASK_MASK) |
            SCALER6_CTL0_UNITY |
            VC4_SET_FIELD(mode->depth == 16 ? HVS_PIXEL_ORDER_XRGB : HVS_PIXEL_ORDER_ARGB,
                SCALER6_CTL0_ORDERRGBA_MASK) |
            VC4_SET_FIELD(mode->depth == 16 ? HVS_PIXEL_FORMAT_RGB565 : HVS_PIXEL_FORMAT_RGBA8888,
                SCALER6_CTL0_PIXEL_FORMAT_MASK);
    ctl2 = (hvs_is_step_d0() ? 0U :
            (SCALER5_CTL2_ALPHA_MODE_FIXED << SCALER5_CTL2_ALPHA_MODE_SHIFT)) |
            (0xfffU << SCALER5_CTL2_ALPHA_SHIFT);

    write_dlist_word(word_base + 0, ctl0);
    write_dlist_word(word_base + 1,
            VC4_SET_FIELD(0, SCALER6_POS0_START_Y_MASK) |
            VC4_SET_FIELD(0, SCALER6_POS0_START_X_MASK));
    write_dlist_word(word_base + 2, ctl2);
    write_dlist_word(word_base + 3,
            VC4_SET_FIELD(mode->height - 1U, SCALER6_POS2_SRC_LINES_MASK) |
            VC4_SET_FIELD(mode->width - 1U, SCALER6_POS2_SRC_WIDTH_MASK));
    write_dlist_word(word_base + 4, 0xc0c0c0c0U);
    /* Gen6 uses PTR0/PTR1/PTR2; BCM2712 DMA is identity-mapped, no VC alias. */
    write_dlist_word(word_base + 5, 0);
    write_dlist_word(word_base + 6, fb_addr);
    write_dlist_word(word_base + 7, VC4_SET_FIELD(pitch, SCALER6_PTR2_PITCH_MASK));
    write_dlist_word(word_base + 8, SCALER6_CTL0_END);

    hvs_write(hvs_disp_lptrs_off(0),
            VC4_SET_FIELD(HVS_BOOTLOADER_DLIST_END, SCALER6_DISPX_LPTRS_HEADE_MASK));

    hvs_write(hvs_disp_ctrl0_off(0), SCALER6_DISPX_CTRL0_RESET);
    hvs_write(hvs_disp_ctrl1_off(0), hvs_read(hvs_disp_ctrl1_off(0)) & ~SCALER6_DISPX_CTRL1_INTLACE);
    hvs_write(hvs_disp_ctrl0_off(0),
            SCALER6_DISPX_CTRL0_ENB |
            VC4_SET_FIELD(mode->width - 1U, SCALER6_DISPX_CTRL0_FWIDTH_MASK) |
            VC4_SET_FIELD(mode->height - 1U, SCALER6_DISPX_CTRL0_LINES_MASK));
}

static void hdmi0_enable_output(const bcm2712_hdmi_mode_t *mode) {
    pv_write(PV_CONTROL, pv_read(PV_CONTROL) | PV_CONTROL_EN);
    hd_write(HDMI_VID_CTL,
            (hd_read(HDMI_VID_CTL) &
             ~(VC4_HD_VID_CTL_VSYNC_LOW | VC4_HD_VID_CTL_HSYNC_LOW)) |
            VC4_HD_VID_CTL_ENABLE |
            VC4_HD_VID_CTL_CLRRGB |
            VC4_HD_VID_CTL_UNDERFLOW_ENABLE |
            VC4_HD_VID_CTL_FRAME_COUNTER_RESET |
            VC4_HD_VID_CTL_BLANK_INSERT_EN |
            (mode->vsync_pos ? 0 : VC4_HD_VID_CTL_VSYNC_LOW) |
            (mode->hsync_pos ? 0 : VC4_HD_VID_CTL_HSYNC_LOW));
    hd_write(HDMI_VID_CTL, hd_read(HDMI_VID_CTL) & ~VC4_HD_VID_CTL_BLANKPIX);
    pv_write(PV_V_CONTROL, pv_read(PV_V_CONTROL) | PV_VCONTROL_VIDEN);
    hdmi_write(HDMI_SCHEDULER_CONTROL,
            hdmi_read(HDMI_SCHEDULER_CONTROL) | VC4_HDMI_SCHEDULER_CONTROL_MODE_HDMI);
}

int bcm2712_native_hdmi_supported(uint32_t w, uint32_t h, uint32_t dep) {
    bcm2712_hdmi_mode_t mode;
    return bcm2712_native_hdmi_cvt_mode(w, h, dep, 60, &mode) == 0;
}

int bcm2712_native_hdmi_init_mode(const sys_info_t *sysinfo,
        const bcm2712_hdmi_mode_t *mode,
        fbinfo_t *info) {
    uint32_t bytes_per_pixel;
    uint32_t pitch;
    uint32_t size;
    uint32_t alloc_size;
    ewokos_addr_t fb_vaddr;
    ewokos_addr_t fb_phy;
    uint32_t bus_addr;

    if (sysinfo == NULL || info == NULL) {
        return -1;
    }

    if (mode == NULL) {
        return -1;
    }
    if ((mode->depth != 16U && mode->depth != 32U) ||
            mode->width < 64U || mode->height < 64U ||
            mode->pixel_clock_hz == 0U ||
            mode->hfp == 0U || mode->hsync == 0U || mode->hbp == 0U ||
            mode->vfp == 0U || mode->vsync == 0U || mode->vbp == 0U) {
        return -1;
    }

    if (_mmio_base == 0 && mmio_map() == 0) {
        slog("native_hdmi: mmio map failed\n");
        return -1;
    }

    bytes_per_pixel = mode->depth / 8U;
    pitch = mode->width * bytes_per_pixel;
    size = pitch * mode->height;
    alloc_size = align_up(size, sysinfo->page_size == 0 ? 4096U : sysinfo->page_size);

    fb_vaddr = dma_alloc(0, alloc_size);
    if (fb_vaddr == 0) {
        slog("native_hdmi: dma alloc failed size=%u\n", alloc_size);
        return -1;
    }
    fb_phy = dma_phy_addr(0, fb_vaddr);
    if (fb_phy == 0 || fb_phy >= 0x40000000ULL) {
        slog("native_hdmi: bad dma phy=%llx\n", (unsigned long long)fb_phy);
        dma_free(0, fb_vaddr);
        return -1;
    }

        memset((void *)(uintptr_t)fb_vaddr, 0, alloc_size);
    bus_addr = (uint32_t)fb_phy;

    program_required_clocks(mode);
    hdmi0_reset();
    hdmi0_phy_init(mode);
        hdmi0_prepare_output();
    hdmi0_set_timings(mode);
    pv0_configure(mode);
    hvs0_init(mode, bus_addr, pitch);
    hdmi0_enable_output(mode);

    memset(info, 0, sizeof(*info));
    info->width = mode->width;
    info->height = mode->height;
    info->vwidth = mode->width;
    info->vheight = mode->height;
    info->depth = mode->depth;
    info->pitch = pitch;
    info->pointer = fb_vaddr;
    info->phy_base = fb_phy;
    info->bus_base = bus_addr;
    info->size = size;
    info->size_max = alloc_size;
    info->xoffset = 0;
    info->yoffset = 0;
    info->dma_id = -1;

    slog("native_hdmi: %ux%u@%u pclk=%u pitch=%u phy=%llx bus=%x\n",
            info->width, info->height, info->depth, mode->pixel_clock_hz,
            info->pitch, (unsigned long long)info->phy_base, (uint32_t)info->bus_base);
    return 0;
}

int bcm2712_native_hdmi_init(const sys_info_t *sysinfo,
        uint32_t w, uint32_t h, uint32_t dep,
        fbinfo_t *info) {
    bcm2712_hdmi_mode_t mode;

    if (bcm2712_native_hdmi_cvt_mode(w, h, dep, 60, &mode) != 0) {
        return -1;
    }

    return bcm2712_native_hdmi_init_mode(sysinfo, &mode, info);
}
