#ifndef UC_PMU_H
#define UC_PMU_H

/*
 * AXP223 PMIC (I2C addr 0x34, bit-banged on SDA=GPIO0/SCL=GPIO1, the
 * same bus powerd uses).  The uConsole panel is NOT powered from the
 * CM4: its VCC comes from the PMIC's ALDO2 rail ("display-vcc" in
 * devterm-pmu-overlay) with DLDO2/3/4 as companion 3.3V rails.  Linux
 * turns these on through the regulator framework at boot
 * (regulator-always-on); nothing in EwokOS did, so the panel glass was
 * simply unpowered: backlight lit (separate rail), DSI controller all
 * green, but every DCS command fell on dead silicon.
 *
 * uc_pmu_display_power() programs all five overlay rails to 3.3V and
 * enables them.  Returns 0 iff the enable bits verify by readback.
 */
int uc_pmu_display_power(void);

#endif
