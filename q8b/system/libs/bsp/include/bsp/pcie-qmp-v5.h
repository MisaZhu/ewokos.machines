/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2017, The Linux Foundation. All rights reserved.
 * SC8280XP Gen3 x1 RC QMP v5 values from Linux phy-qcom-qmp-pcie.c.
 * Reference revision: 199e665b194d42315feabf1872bcc3b56db070cf.
 * Offsets below are relative to PHY 0x01c06000. */
static const struct q8b_pcie_phy_value { uint16_t offset; uint8_t value; } q8b_pcie_phy_values[] = {
    {0x0010, 0x00}, /* QSERDES_V5_COM_SSC_EN_CENTER */
    {0x001c, 0x31}, /* QSERDES_V5_COM_SSC_PER1 */
    {0x0020, 0x01}, /* QSERDES_V5_COM_SSC_PER2 */
    {0x0024, 0xde}, /* QSERDES_V5_COM_SSC_STEP_SIZE1_MODE0 */
    {0x0028, 0x07}, /* QSERDES_V5_COM_SSC_STEP_SIZE2_MODE0 */
    {0x0030, 0x4c}, /* QSERDES_V5_COM_SSC_STEP_SIZE1_MODE1 */
    {0x0034, 0x06}, /* QSERDES_V5_COM_SSC_STEP_SIZE2_MODE1 */
    {0x0048, 0x90}, /* QSERDES_V5_COM_CLK_ENABLE1 */
    {0x0058, 0x0f}, /* QSERDES_V5_COM_PLL_IVCO */
    {0x0074, 0x06}, /* QSERDES_V5_COM_CP_CTRL_MODE0 */
    {0x0078, 0x06}, /* QSERDES_V5_COM_CP_CTRL_MODE1 */
    {0x007c, 0x16}, /* QSERDES_V5_COM_PLL_RCTRL_MODE0 */
    {0x0080, 0x16}, /* QSERDES_V5_COM_PLL_RCTRL_MODE1 */
    {0x0084, 0x36}, /* QSERDES_V5_COM_PLL_CCTRL_MODE0 */
    {0x0088, 0x36}, /* QSERDES_V5_COM_PLL_CCTRL_MODE1 */
    {0x0094, 0x08}, /* QSERDES_V5_COM_SYSCLK_EN_SEL */
    {0x00a4, 0x42}, /* QSERDES_V5_COM_LOCK_CMP_EN */
    {0x00ac, 0x0a}, /* QSERDES_V5_COM_LOCK_CMP1_MODE0 */
    {0x00b0, 0x1a}, /* QSERDES_V5_COM_LOCK_CMP2_MODE0 */
    {0x00b4, 0x14}, /* QSERDES_V5_COM_LOCK_CMP1_MODE1 */
    {0x00b8, 0x34}, /* QSERDES_V5_COM_LOCK_CMP2_MODE1 */
    {0x00bc, 0x82}, /* QSERDES_V5_COM_DEC_START_MODE0 */
    {0x00c4, 0x68}, /* QSERDES_V5_COM_DEC_START_MODE1 */
    {0x00cc, 0x55}, /* QSERDES_V5_COM_DIV_FRAC_START1_MODE0 */
    {0x00d0, 0x55}, /* QSERDES_V5_COM_DIV_FRAC_START2_MODE0 */
    {0x00d4, 0x03}, /* QSERDES_V5_COM_DIV_FRAC_START3_MODE0 */
    {0x00d8, 0xab}, /* QSERDES_V5_COM_DIV_FRAC_START1_MODE1 */
    {0x00dc, 0xaa}, /* QSERDES_V5_COM_DIV_FRAC_START2_MODE1 */
    {0x00e0, 0x02}, /* QSERDES_V5_COM_DIV_FRAC_START3_MODE1 */
    {0x010c, 0x02}, /* QSERDES_V5_COM_VCO_TUNE_MAP */
    {0x0110, 0x24}, /* QSERDES_V5_COM_VCO_TUNE1_MODE0 */
    {0x0118, 0xb4}, /* QSERDES_V5_COM_VCO_TUNE1_MODE1 */
    {0x011c, 0x03}, /* QSERDES_V5_COM_VCO_TUNE2_MODE1 */
    {0x0154, 0x34}, /* QSERDES_V5_COM_CLK_SELECT */
    {0x0158, 0x01}, /* QSERDES_V5_COM_HSCLK_SEL */
    {0x016c, 0x08}, /* QSERDES_V5_COM_CORECLK_DIV_MODE1 */
    {0x01ac, 0xb9}, /* QSERDES_V5_COM_BIN_VCOCAL_CMP_CODE1_MODE0 */
    {0x01b0, 0x1e}, /* QSERDES_V5_COM_BIN_VCOCAL_CMP_CODE2_MODE0 */
    {0x01b4, 0x94}, /* QSERDES_V5_COM_BIN_VCOCAL_CMP_CODE1_MODE1 */
    {0x01b8, 0x18}, /* QSERDES_V5_COM_BIN_VCOCAL_CMP_CODE2_MODE1 */
    {0x01bc, 0x11}, /* QSERDES_V5_COM_BIN_VCOCAL_HSCLK_SEL */
    {0x0050, 0x07}, /* QSERDES_V5_COM_SYSCLK_BUF_ENABLE */
    {0x0ee4, 0x20}, /* QSERDES_V5_TX_PI_QEC_CTRL */
    {0x0e84, 0x75}, /* QSERDES_V5_TX_LANE_MODE_1 */
    {0x0e90, 0x3f}, /* QSERDES_V5_TX_LANE_MODE_4 */
    {0x0e3c, 0x1d}, /* QSERDES_V5_TX_RES_CODE_LANE_OFFSET_TX */
    {0x0e40, 0x0c}, /* QSERDES_V5_TX_RES_CODE_LANE_OFFSET_RX */
    {0x115c, 0x7f}, /* QSERDES_V5_RX_RX_MODE_00_LOW */
    {0x1160, 0xff}, /* QSERDES_V5_RX_RX_MODE_00_HIGH */
    {0x1164, 0xbf}, /* QSERDES_V5_RX_RX_MODE_00_HIGH2 */
    {0x1168, 0x3f}, /* QSERDES_V5_RX_RX_MODE_00_HIGH3 */
    {0x116c, 0xd8}, /* QSERDES_V5_RX_RX_MODE_00_HIGH4 */
    {0x1170, 0xdc}, /* QSERDES_V5_RX_RX_MODE_01_LOW */
    {0x1174, 0xdc}, /* QSERDES_V5_RX_RX_MODE_01_HIGH */
    {0x1178, 0x5c}, /* QSERDES_V5_RX_RX_MODE_01_HIGH2 */
    {0x117c, 0x34}, /* QSERDES_V5_RX_RX_MODE_01_HIGH3 */
    {0x1180, 0xa6}, /* QSERDES_V5_RX_RX_MODE_01_HIGH4 */
    {0x10cc, 0xf0}, /* QSERDES_V5_RX_TX_ADAPT_POST_THRESH */
    {0x1190, 0x34}, /* QSERDES_V5_RX_RX_MODE_10_HIGH3 */
    {0x10d8, 0x07}, /* QSERDES_V5_RX_VGA_CAL_CNTRL2 */
    {0x10dc, 0x00}, /* QSERDES_V5_RX_GM_CAL */
    {0x104c, 0x08}, /* QSERDES_V5_RX_UCDR_SB2_THRESH1 */
    {0x1050, 0x08}, /* QSERDES_V5_RX_UCDR_SB2_THRESH2 */
    {0x1044, 0xf0}, /* QSERDES_V5_RX_UCDR_PI_CONTROLS */
    {0x11a4, 0x38}, /* QSERDES_V5_RX_DFE_CTLE_POST_CAL_OFFSET */
    {0x02dc, 0x05}, /* QPHY_V5_PCS_REFGEN_REQ_CONFIG1 */
    {0x0388, 0x77}, /* QPHY_V5_PCS_RX_SIGDET_LVL */
    {0x0398, 0x0b}, /* QPHY_V5_PCS_RATE_SLEW_CNTRL1 */
    {0x0694, 0x00}, /* QPHY_V5_PCS_PCIE_OSC_DTCT_ACTIONS */
    {0x0654, 0x00}, /* QPHY_V5_PCS_PCIE_INT_AUX_CLK_CONFIG1 */
    {0x06a8, 0x0f}, /* QPHY_V5_PCS_PCIE_EQ_CONFIG2 */
    {0x0620, 0xc1}, /* QPHY_V5_PCS_PCIE_ENDPOINT_REFCLK_DRIVE */
};

/* SC8280XP Gen3x2 RC table used by PCIe3A.  The source is the same
 * qcom,sc8280xp-qmp-gen3x4-pcie-phy table selected by the board's two-lane
 * configuration.  Values are kept as byte writes because the QMP register
 * interface is byte-granular. */
static const struct q8b_pcie_phy_value q8b_pcie3a_serdes_values[] = {
    {0x0010, 0x00}, {0x001c, 0x31}, {0x0020, 0x01},
    {0x0024, 0xde}, {0x0028, 0x07}, {0x0030, 0x4c},
    {0x0034, 0x06}, {0x0048, 0x90}, {0x0058, 0x0f},
    {0x0074, 0x06}, {0x0078, 0x06}, {0x007c, 0x16},
    {0x0080, 0x16}, {0x0084, 0x36}, {0x0088, 0x36},
    {0x0094, 0x08}, {0x00a4, 0x42}, {0x00ac, 0x0a},
    {0x00b0, 0x1a}, {0x00b4, 0x14}, {0x00b8, 0x34},
    {0x00bc, 0x82}, {0x00c4, 0x68}, {0x00cc, 0x55},
    {0x00d0, 0x55}, {0x00d4, 0x03}, {0x00d8, 0xab},
    {0x00dc, 0xaa}, {0x00e0, 0x02}, {0x010c, 0x02},
    {0x0110, 0x24}, {0x0118, 0xb4}, {0x011c, 0x03},
    {0x0154, 0x34}, {0x0158, 0x01}, {0x016c, 0x08},
    {0x01ac, 0xb9}, {0x01b0, 0x1e}, {0x01b4, 0x94},
    {0x01b8, 0x18}, {0x01bc, 0x11},
    /* tbls_rc: sc8280xp_qmp_gen3x2_pcie_rc_serdes_tbl */
    {0x0044, 0x14},
};

static const struct q8b_pcie_phy_value q8b_pcie3a_tx_values[] = {
    {0x0ee4, 0x02}, /* lane 1 PI_QEC_CTRL */
    {0x0e84, 0xd5}, {0x0e90, 0x3f},
    {0x0e3c, 0x11}, {0x0e40, 0x0c},
};

static const struct q8b_pcie_phy_value q8b_pcie3a_tx2_values[] = {
    {0x0ee4, 0x04}, /* lane 2 PI_QEC_CTRL */
    {0x0e84, 0xd5}, {0x0e90, 0x3f},
    {0x0e3c, 0x11}, {0x0e40, 0x0c},
};

static const struct q8b_pcie_phy_value q8b_pcie3a_rx_values[] = {
    {0x115c, 0x7f}, {0x1160, 0xff}, {0x1164, 0x7f},
    {0x1168, 0x34}, {0x116c, 0xd8}, {0x1170, 0xdc},
    {0x1174, 0xdc}, {0x1178, 0x5c}, {0x117c, 0x34},
    {0x1180, 0xa6}, {0x1190, 0x34}, {0x10d8, 0x0f},
    {0x10dc, 0x00}, {0x104c, 0x08}, {0x1050, 0x08},
    {0x1044, 0xf0}, {0x11a4, 0x38},
};

static const struct q8b_pcie_phy_value q8b_pcie3a_pcs_values[] = {
    {0x02dc, 0x05}, {0x0388, 0x88}, {0x0398, 0x0b},
    {0x03e4, 0x0f}, /* QPHY_V5_PCS_EQ_CONFIG3 */
};

static const struct q8b_pcie_phy_value q8b_pcie3a_pcs_misc_values[] = {
    {0x060c, 0x1d}, /* QPHY_V5_PCS_PCIE_POWER_STATE_CONFIG2 */
    {0x0614, 0x07}, /* QPHY_V5_PCS_PCIE_POWER_STATE_CONFIG4 */
    {0x0620, 0xc1}, /* QPHY_V5_PCS_PCIE_ENDPOINT_REFCLK_DRIVE */
    {0x0694, 0x00}, /* QPHY_V5_PCS_PCIE_OSC_DTCT_ACTIONS */
};
