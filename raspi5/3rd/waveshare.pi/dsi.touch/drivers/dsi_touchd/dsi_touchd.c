#include <ewoksys/vdevice.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/proc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <arch/bcm2712/i2c.h>
#include <arch/bcm2712/gpio.h>

/*
 * Waveshare DSI LCD touch daemon for Raspberry Pi 5 (BCM2712).
 *
 * Port of the 3rd/waveshare.pi/dsi.touch dsi_touchd, following the same
 * route the raspi5 dsi_fbdisplayd took: on the Pi5 every I2C controller
 * lives in the RP1 southbridge (Synopsys DW_apb_i2c instances), so all
 * bus access goes through arch/bcm2712/i2c.h instead of the BCM283x
 * bit-bang/BSC pair, and the few GPIO pokes (GT911 reset/int select on
 * the header bus) go through arch/bcm2712/gpio.h. The bcm2712 libs map
 * the RP1 mmio window themselves.
 *
 * Bus topology mirrors dsi_fbdisplayd (Pi5 board dts):
 *   i2c4 GPIO40/41  FPC control bus (i2c_csi_dsi, the shipping wiring):
 *                   the panel MCU (0x45) and the touch controller both
 *                   answer on it;
 *   i2c6 GPIO38/39  CAM/DISP 0 connector control bus;
 *   i2c1 GPIO2/3    40-pin header, the panel DIP switch "I2C1" position.
 * The FPC bus is probed first; once a touch controller answers, the bus
 * is latched.
 *
 * Two touch controller families are supported, exactly like the raspix
 * original:
 *   Goodix (GT911 etc.) at 0x14/0x5d, 16-bit registers — the Waveshare
 *     DSI panels carry this one, behind the panel MCU's power rails
 *     (regs 0x94/0x95), which this daemon walks up before probing;
 *   FT5x06 at 0x38, 8-bit registers.
 *
 * Events are served as 6-byte records {state, x, y} on the same stream
 * contract as qcom8gen3 goodixd: tagged multi-touch frames (header + contact
 * edges), diffed against the last emitted contact set. xtouch maps them
 * to screen coordinates and passes them to xserverd as XEVT_TOUCH_MT;
 * gesture policy and single-finger mouse compatibility live there.
 */

#define TP_POLL_MIN_US           8000u
#define TP_POLL_MAX_US          50000u
/* Every bus runs at the speed declared in its table entry, forever —
 * probing, polling and recovery included. The speed never changes at
 * runtime: failed transactions are absorbed by the driver-level
 * recovery (in-poll retry, SDA-wedge bus clear, re-probe), never by
 * stepping the bus down. Flipping the speed back and forth also
 * re-initialises the controller each time, which was itself a source
 * of dead spells.
 *
 * The FPC bus is declared at standard speed because that is its
 * physical ceiling, not a software concession: the official RPi 7"
 * panel parks an ATTINY firmware I2C slave (0x45) on it which only
 * answers at 100kHz, and the long FPC cable starves fast-mode rise
 * times — 9 probe rounds at 400kHz got not one ACK from any device on
 * the board, while dsi_fbdisplayd demonstrably talks the same ATTINY
 * at standard speed. The touch and the display panel share that bus,
 * so the touch inherits the panel's ceiling; matching dsi_fbdisplayd's
 * speed also keeps the two daemons from fighting over IC_CON. The
 * video stream never touches I2C, so this costs the display nothing. */
#define TP_STD_HZ               100000u
#define TP_FAST_HZ              400000u
/* Safety net for a lift the controller never reports: both controllers
 * normally end a touch with a zero-contact frame, but if that frame is
 * missed (GT911 ready flag never re-raises, FPC noise) the active slots
 * are released after this idle window. */
#define TP_RELEASE_TIMEOUT_MS      80
/* Release debounce: a contact that vanishes for a frame or two while the
 * finger is still down must not turn into a release+re-press pair (a
 * phantom tap downstream). A release goes out only after the contact
 * stayed absent for this whole window; one that returns inside it
 * remains pressed. Same policy as qcom8gen3 goodixd. */
#define TP_RELEASE_GRACE_MS        40
#define TP_INIT_RETRY_MS         1000
#define TP_I2C_FAIL_MAX            20
#define TP_I2C_RETRY_MAX            3
#define TP_I2C_SPLIT_DELAY_US      100u
/* one MT frame can carry header + press/move + release per contact;
 * keep room for several frames so a slow reader never starves edges */
#define TP_TOUCH_CACHE_SIZE        64u

#define GOODIX_ADDR_PRIMARY      0x14
#define GOODIX_ADDR_FALLBACK     0x5d
#define FT5X06_ADDR              0x38
#define DISPLAY_MCU_ADDR         0x45

#define GOODIX_REG_COMMAND      0x8040
#define GOODIX_REG_ID           0x8140
#define GOODIX_REG_STATUS       0x814e
#define GOODIX_REG_TRACK1       0x814f
#define GOODIX_POINT_SIZE             8
#define GOODIX_READY_FLAG       0x80
#define GOODIX_MAX_POINTS             5
#define GOODIX_RESET_GPIO           17
#define GOODIX_INT_GPIO              4

#define FT5X06_REG_TD_STATUS      0x02
#define FT5X06_REG_P1_XH          0x03
#define FT5X06_REG_P2_XH          0x09
#define FT5X06_REG_P1_XL          0x04
#define FT5X06_REG_P1_YH          0x05
#define FT5X06_REG_P1_YL          0x06
#define FT5X06_REG_ID             0xa3
#define FT5X06_REG_FW_VER         0xa6
#define FT5X06_POINT_SIZE            6
#define FT5X06_MAX_POINTS            5
#define FT5X06_EVENT_DOWN         0x00
#define FT5X06_EVENT_UP           0x01
#define FT5X06_EVENT_CONTACT      0x02

/* Multi-touch frame records on the 6-byte stream (qcom8gen3 goodixd protocol,
 * consumed by xtouch's absolute mode):
 *   state TOUCH_MT_FRAME ........ header: x = record count, y = 0
 *   state TOUCH_MT_FRAME | (act << 8) | id ... contact edge, act is
 *     TOUCH_ACT_PRESS/MOVE/RELEASE, id 0..9 (xserverd's X_TOUCH_MAX);
 *     x/y are raw panel coordinates (a release carries the contact's
 *     last known position).
 * Both controllers report exactly the active contacts per frame, so the
 * driver diffs against the last EMITTED contact set and synthesizes
 * press/move/release edges; a dropped frame (queue full) is re-derived
 * from that same set on the next controller frame - except a frame
 * carrying releases, which is never dropped: after the last finger
 * lifts the controller may send nothing more, so its room is made by
 * evicting the oldest queued records. */
#define TOUCH_MT_FRAME          0x8000u
#define TOUCH_ACT_PRESS              1u
#define TOUCH_ACT_MOVE               2u
#define TOUCH_ACT_RELEASE            3u
#define MT_MAX_CONTACTS              10u

#define WAVESHARE_REG_TP           0x94
#define WAVESHARE_REG_LCD          0x95
#define WAVESHARE_REG_PWM          0x96
#define WAVESHARE_REG_SIZE         0x97
#define WAVESHARE_REG_ID           0x98
#define WAVESHARE_REG_VERSION      0x99

#define WAVESHARE_GPIO_AVDD         0
#define WAVESHARE_GPIO_PANEL_RESET  1
#define WAVESHARE_GPIO_ENABLE       2
#define WAVESHARE_GPIO_IOVCC        4
#define WAVESHARE_GPIO_VCC          8
#define WAVESHARE_GPIO_TS_RESET     9

#define WAVESHARE_PWR_BASE \
	((uint16_t)(1u << WAVESHARE_GPIO_VCC))
#define WAVESHARE_PWR_ENABLE \
	((uint16_t)(WAVESHARE_PWR_BASE | (1u << WAVESHARE_GPIO_ENABLE)))
#define WAVESHARE_PWR_TOUCH_RAILS \
	((uint16_t)(WAVESHARE_PWR_ENABLE | (1u << WAVESHARE_GPIO_AVDD) | \
	(1u << WAVESHARE_GPIO_IOVCC)))
#define WAVESHARE_PWR_TS_RESET \
	((uint16_t)(WAVESHARE_PWR_TOUCH_RAILS | (1u << WAVESHARE_GPIO_TS_RESET)))
#define WAVESHARE_PWR_PANEL_RESET \
	((uint16_t)(WAVESHARE_PWR_TS_RESET | (1u << WAVESHARE_GPIO_PANEL_RESET)))

/* FPC control bus: i2c_csi_dsi = RP1 i2c4, GPIO40/41 */
#define DSI_I2C_BUS_FPC   4
#define DSI_I2C_FPC_SDA   40
#define DSI_I2C_FPC_SCL   41
/* CAM/DISP 0 connector control bus: RP1 i2c6, GPIO38/39 */
#define DSI_I2C_BUS_C0    6
#define DSI_I2C_C0_SDA    38
#define DSI_I2C_C0_SCL    39
/* DIP switch position "I2C1": RP1 i2c1 on the 40-pin header, GPIO2/3 */
#define DSI_I2C_BUS_DIP1  1
#define DSI_I2C1_SDA      2
#define DSI_I2C1_SCL      3

typedef enum {
	TP_OK = 0,
	TP_ERROR = 1,
	TP_NOT_RESPONSE = 2,
	TP_NO_DATA = 3
} tp_status_t;

typedef struct {
	uint8_t id;	/* controller contact id: GT911 track id / FT5x06 touch id */
	uint16_t x;
	uint16_t y;
} tp_point_t;

/* last emitted contact set, indexed by the controller's contact id */
typedef struct {
	bool active;
	uint16_t x;
	uint16_t y;
	uint64_t absent_ms; /* first frame missing this contact, 0 = present */
} mt_slot_t;

typedef struct {
	int32_t bus;	/* RP1 DW_apb_i2c controller index */
	int32_t sda;
	int32_t scl;
	const char* name;
	uint32_t hz;	/* fixed bus speed, declared once, never changed */
} dsi_i2c_bus_t;

typedef enum {
	TOUCH_KIND_NONE = 0,
	TOUCH_KIND_GOODIX = 1,
	TOUCH_KIND_FT5X06 = 2,
} touch_kind_t;

static const dsi_i2c_bus_t dsi_i2c_buses[] = {
	/* FPC: panel ATTINY is standard-speed only (see TP_STD_HZ note) */
	{DSI_I2C_BUS_FPC,  DSI_I2C_FPC_SDA, DSI_I2C_FPC_SCL, "i2c4-fpc", TP_STD_HZ},
	{DSI_I2C_BUS_C0,   DSI_I2C_C0_SDA,  DSI_I2C_C0_SCL,  "i2c6-c0",  TP_FAST_HZ},
	{DSI_I2C_BUS_DIP1, DSI_I2C1_SDA,    DSI_I2C1_SCL,    "i2c1-header", TP_FAST_HZ},
};

static tp_point_t point[GOODIX_MAX_POINTS > FT5X06_MAX_POINTS ?
	GOODIX_MAX_POINTS : FT5X06_MAX_POINTS];
static uint8_t point_nr = 0;
static uint16_t touch_data[TP_TOUCH_CACHE_SIZE][3];
static uint32_t touch_data_read = 0;
static uint32_t touch_data_write = 0;
static bool tp_ready = false;
static uint32_t poll_sleep_us = TP_POLL_MIN_US;
static uint32_t i2c_fail_count = 0;
static uint64_t tp_retry_ts = 0;
static uint32_t tp_retry_count = 0;
static mt_slot_t mt_slots[MT_MAX_CONTACTS];
static uint64_t mt_last_frame_ms = 0; /* last decoded controller frame */
static bool mt_zero_frame = true;     /* that frame carried zero contacts */
static uint8_t touch_addr = GOODIX_ADDR_PRIMARY;
static touch_kind_t touch_kind = TOUCH_KIND_NONE;
static const dsi_i2c_bus_t* active_bus = NULL;
static bool waveshare_panel_reset_tried = false;
static uint16_t waveshare_power_state = 0;
static bool waveshare_power_state_valid = false;

static tp_status_t goodix_probe_candidates(const dsi_i2c_bus_t* bus);
static tp_status_t ft5x06_probe(const dsi_i2c_bus_t* bus);

static bool touch_has_data(void) {
	return (touch_data_write - touch_data_read) > 0;
}

static void touch_push(uint16_t state, uint16_t x, uint16_t y) {
	uint16_t* evt;

	if (touch_data_write - touch_data_read >= TP_TOUCH_CACHE_SIZE)
		touch_data_read++;

	evt = touch_data[touch_data_write % TP_TOUCH_CACHE_SIZE];
	evt[0] = state;
	evt[1] = x;
	evt[2] = y;
	touch_data_write++;
}

static bool mt_any_active(void) {
	uint32_t id;

	for (id = 0; id < MT_MAX_CONTACTS; id++) {
		if (mt_slots[id].active)
			return true;
	}
	return false;
}

/* Diff one decoded controller frame against the last emitted contact set
 * and queue the whole edge frame atomically: if the queue cannot hold
 * header + every record, nothing is pushed and the slot table stays
 * untouched, so the next controller frame re-derives the same edges. A
 * frame with no change at all (stationary fingers) emits nothing.
 * Releases are debounced through TP_RELEASE_GRACE_MS and, once due, must
 * go out even on a full queue. Returns true when a frame was queued. */
static bool mt_emit_frame(const tp_point_t* pts, uint8_t n) {
	uint16_t recs[MT_MAX_CONTACTS * 2][3];
	uint32_t count = 0;
	bool seen[MT_MAX_CONTACTS];
	bool changed = false;
	uint64_t now = kernel_tic_ms(0);
	uint32_t i;
	uint32_t id;

	memset(seen, 0, sizeof(seen));
	for (i = 0; i < n; i++) {
		uint16_t x = pts[i].x;
		uint16_t y = pts[i].y;
		uint32_t act;

		id = pts[i].id;
		if (id >= MT_MAX_CONTACTS || seen[id])
			continue;
		seen[id] = true;
		if (!mt_slots[id].active) {
			act = TOUCH_ACT_PRESS;
			changed = true;
		}
		else if (mt_slots[id].x != x || mt_slots[id].y != y) {
			act = TOUCH_ACT_MOVE;
			changed = true;
		}
		else {
			continue; /* unchanged contact carries no edge */
		}
		recs[count][0] = (uint16_t)(TOUCH_MT_FRAME | (act << 8) | id);
		recs[count][1] = x;
		recs[count][2] = y;
		count++;
	}
	for (id = 0; id < MT_MAX_CONTACTS; id++) {
		if (!mt_slots[id].active || seen[id])
			continue;
		if (mt_slots[id].absent_ms == 0) {
			mt_slots[id].absent_ms = now; /* start the grace window */
			continue;
		}
		if (now - mt_slots[id].absent_ms < TP_RELEASE_GRACE_MS)
			continue;
		recs[count][0] = (uint16_t)(TOUCH_MT_FRAME |
				(TOUCH_ACT_RELEASE << 8) | id);
		recs[count][1] = mt_slots[id].x;
		recs[count][2] = mt_slots[id].y;
		count++;
		changed = true;
		mt_slots[id].active = false;
		mt_slots[id].absent_ms = 0;
	}
	if (!changed || count == 0)
		return false;
	if ((touch_data_write - touch_data_read) + 1 + count > TP_TOUCH_CACHE_SIZE) {
		/* A release must never be lost: once the last finger lifts the
		 * controller may send no further frame, so a dropped release
		 * frame is never re-derived and the contact stays pressed
		 * forever downstream. Make room by evicting the oldest queued
		 * records - stale move edges whose positions the release frame
		 * supersedes anyway. Frames without a release keep the
		 * drop-and-re-derive policy. */
		bool has_release = false;
		uint32_t drop;

		for (i = 0; i < count; i++) {
			if (((recs[i][0] >> 8) & 0x3u) == TOUCH_ACT_RELEASE) {
				has_release = true;
				break;
			}
		}
		if (!has_release)
			return false;
		drop = (touch_data_write - touch_data_read) + 1 + count -
			TP_TOUCH_CACHE_SIZE;
		touch_data_read += drop;
	}
	touch_push(TOUCH_MT_FRAME, (uint16_t)count, 0);
	for (i = 0; i < count; i++)
		touch_push(recs[i][0], recs[i][1], recs[i][2]);
	/* commit the emitted set only after the frame is fully queued. A
	 * slot inside its release grace window keeps active=true (and its
	 * last position) so a returning contact continues instead of
	 * re-pressing. */
	for (id = 0; id < MT_MAX_CONTACTS; id++) {
		if (!seen[id])
			continue;
		mt_slots[id].active = true;
		mt_slots[id].absent_ms = 0;
		for (i = 0; i < n; i++) {
			if (pts[i].id == id) {
				mt_slots[id].x = pts[i].x;
				mt_slots[id].y = pts[i].y;
				break;
			}
		}
	}
	return true;
}

/* Link lost / controller reset: every active contact's lift will never
 * be reported, so release all slots immediately (no grace) and make
 * room in the queue the same way a due release frame does. */
static bool mt_release_all(void) {
	uint16_t recs[MT_MAX_CONTACTS][3];
	uint32_t count = 0;
	uint32_t id;
	uint32_t i;

	for (id = 0; id < MT_MAX_CONTACTS; id++) {
		if (!mt_slots[id].active)
			continue;
		recs[count][0] = (uint16_t)(TOUCH_MT_FRAME |
				(TOUCH_ACT_RELEASE << 8) | id);
		recs[count][1] = mt_slots[id].x;
		recs[count][2] = mt_slots[id].y;
		count++;
		mt_slots[id].active = false;
		mt_slots[id].absent_ms = 0;
	}
	if (count == 0)
		return false;
	if ((touch_data_write - touch_data_read) + 1 + count > TP_TOUCH_CACHE_SIZE)
		touch_data_read += (touch_data_write - touch_data_read) + 1 + count -
			TP_TOUCH_CACHE_SIZE;
	touch_push(TOUCH_MT_FRAME, (uint16_t)count, 0);
	for (i = 0; i < count; i++)
		touch_push(recs[i][0], recs[i][1], recs[i][2]);
	return true;
}

static int32_t dsi_i2c_select(const dsi_i2c_bus_t* bus) {
	active_bus = bus;
	if (bcm2712_i2c_init_pins(bus->bus, (uint32_t)bus->sda,
			(uint32_t)bus->scl) != 0)
		return -1;
	/*
	 * Controller init resets IC_CON back to standard speed; pin the
	 * bus straight back to its declared speed. Every select does
	 * this, so probe, recovery and normal polling all run at one
	 * fixed speed — the bus never steps.
	 */
	if (bcm2712_i2c_set_speed(bus->bus, bus->hz) != 0)
		return -1;
	usleep(2000);
	return 0;
}

/*
 * I2C bus clear: if a transfer died mid-byte the slave may still be
 * holding SDA low waiting for clocks, which would wedge every later
 * transfer for good. Re-mux SCL/SDA as GPIOs, clock SCL nine times to
 * flush the slave's state machine, emit a STOP, then hand the pins
 * back to the controller. Open-drain is emulated by switching between
 * output-low and input-with-pull-up, so a clock-stretching slave is
 * never fought.
 */
static void dsi_i2c_bus_clear(const dsi_i2c_bus_t* bus) {
	uint32_t i;

	if (bus == NULL)
		return;

	bcm2712_gpio_init();
	bcm2712_gpio_write((uint32_t)bus->scl, false);
	bcm2712_gpio_write((uint32_t)bus->sda, false);
	bcm2712_gpio_config((uint32_t)bus->scl, GPIO_FUNC_INPUT);
	bcm2712_gpio_pull((uint32_t)bus->scl, GPIO_PULL_UP);
	bcm2712_gpio_config((uint32_t)bus->sda, GPIO_FUNC_INPUT);
	bcm2712_gpio_pull((uint32_t)bus->sda, GPIO_PULL_UP);
	usleep(10);

	for (i = 0; i < 9; i++) {
		bcm2712_gpio_config((uint32_t)bus->scl, GPIO_FUNC_OUTPUT);
		usleep(5);
		bcm2712_gpio_config((uint32_t)bus->scl, GPIO_FUNC_INPUT);
		usleep(5);
	}

	/* STOP: SDA pulled low, SCL released, then SDA released while SCL high */
	bcm2712_gpio_config((uint32_t)bus->scl, GPIO_FUNC_OUTPUT);
	bcm2712_gpio_config((uint32_t)bus->sda, GPIO_FUNC_OUTPUT);
	usleep(5);
	bcm2712_gpio_config((uint32_t)bus->scl, GPIO_FUNC_INPUT);
	usleep(5);
	bcm2712_gpio_config((uint32_t)bus->sda, GPIO_FUNC_INPUT);
	usleep(10);

	/* pins back to the DW controller, controller re-initialised */
	dsi_i2c_select(bus);
}

static uint32_t dsi_i2c_write_reg16(uint8_t addr, uint16_t reg,
		const uint8_t* data, uint16_t len) {
	uint8_t buf[258];
	uint16_t i;

	if (active_bus == NULL || len > sizeof(buf) - 2)
		return 1;
	buf[0] = (uint8_t)(reg >> 8);
	buf[1] = (uint8_t)(reg & 0xff);
	for (i = 0; i < len; i++)
		buf[i + 2] = data[i];
	return bcm2712_i2c_write(active_bus->bus, addr, buf, len + 2) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_reg16(uint8_t addr, uint16_t reg,
		uint8_t* data, uint16_t len) {
	uint8_t reg_buf[2];

	if (active_bus == NULL)
		return 1;
	reg_buf[0] = (uint8_t)(reg >> 8);
	reg_buf[1] = (uint8_t)(reg & 0xff);
	return bcm2712_i2c_write_read(active_bus->bus, addr,
			reg_buf, sizeof(reg_buf), data, len) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_reg16_split(uint8_t addr, uint16_t reg,
		uint8_t* data, uint16_t len) {
	uint8_t reg_buf[2];

	if (active_bus == NULL)
		return 1;
	reg_buf[0] = (uint8_t)(reg >> 8);
	reg_buf[1] = (uint8_t)(reg & 0xff);
	if (bcm2712_i2c_write(active_bus->bus, addr, reg_buf, sizeof(reg_buf)) != 0)
		return 1;
	usleep(TP_I2C_SPLIT_DELAY_US);
	return bcm2712_i2c_read(active_bus->bus, addr, data, len) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_write_reg8(uint8_t addr, uint8_t reg, uint8_t value) {
	uint8_t buf[2];

	if (active_bus == NULL)
		return 1;
	buf[0] = reg;
	buf[1] = value;
	return bcm2712_i2c_write(active_bus->bus, addr, buf, sizeof(buf)) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_reg8(uint8_t addr, uint8_t reg, uint8_t* value) {
	if (active_bus == NULL)
		return 1;
	return bcm2712_i2c_write_read(active_bus->bus, addr,
			&reg, 1, value, 1) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_reg8_split(uint8_t addr, uint8_t reg, uint8_t* value) {
	if (active_bus == NULL)
		return 1;
	if (bcm2712_i2c_write(active_bus->bus, addr, &reg, 1) != 0)
		return 1;
	usleep(TP_I2C_SPLIT_DELAY_US);
	return bcm2712_i2c_read(active_bus->bus, addr, value, 1) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_regs8(uint8_t addr, uint8_t reg,
		uint8_t* data, uint16_t len) {
	if (len == 0)
		return 0;
	if (active_bus == NULL)
		return 1;
	return bcm2712_i2c_write_read(active_bus->bus, addr,
			&reg, 1, data, len) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_regs8_split(uint8_t addr, uint8_t reg,
		uint8_t* data, uint16_t len) {
	if (active_bus == NULL)
		return 1;
	if (bcm2712_i2c_write(active_bus->bus, addr, &reg, 1) != 0)
		return 1;
	usleep(TP_I2C_SPLIT_DELAY_US);
	return bcm2712_i2c_read(active_bus->bus, addr, data, len) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_reg8_retry(uint8_t addr, uint8_t reg, uint8_t* value) {
	uint32_t attempt;

	for (attempt = 0; attempt < TP_I2C_RETRY_MAX; attempt++) {
		if (dsi_i2c_read_reg8(addr, reg, value) == 0)
			return 0;
		if (dsi_i2c_read_reg8_split(addr, reg, value) == 0)
			return 0;
		if (active_bus == NULL)
			break;
		usleep(2000);
		/* a failed transaction can leave the slave holding SDA low;
		 * re-initialising the controller alone will not release the
		 * wire — clock a bus clear (which re-selects the bus too) */
		dsi_i2c_bus_clear(active_bus);
		usleep(2000);
	}
	return 1;
}

static uint32_t dsi_i2c_read_regs8_retry(uint8_t addr, uint8_t reg,
		uint8_t* data, uint16_t len) {
	uint32_t attempt;

	for (attempt = 0; attempt < TP_I2C_RETRY_MAX; attempt++) {
		if (dsi_i2c_read_regs8(addr, reg, data, len) == 0)
			return 0;
		if (dsi_i2c_read_regs8_split(addr, reg, data, len) == 0)
			return 0;
		if (active_bus == NULL)
			break;
		usleep(2000);
		/* a failed transaction can leave the slave holding SDA low;
		 * re-initialising the controller alone will not release the
		 * wire — clock a bus clear (which re-selects the bus too) */
		dsi_i2c_bus_clear(active_bus);
		usleep(2000);
	}
	return 1;
}

static uint32_t dsi_i2c_probe_addr_only(uint8_t addr) {
	uint8_t dummy;

	if (active_bus == NULL)
		return 1;
	/* the DW controller has no zero-length transfer: a 1-byte read
	 * returns 0 exactly when the address phase ACKs */
	return bcm2712_i2c_read(active_bus->bus, addr, &dummy, 1) == 0 ? 0 : 1;
}

static bool waveshare_mcu_bus(const dsi_i2c_bus_t* bus) {
	if (bus == NULL)
		return false;
	return bus->bus == DSI_I2C_BUS_FPC &&
		bus->sda == DSI_I2C_FPC_SDA && bus->scl == DSI_I2C_FPC_SCL;
}

static bool waveshare_mcu_present(const dsi_i2c_bus_t* bus) {
	if (!waveshare_mcu_bus(bus))
		return false;
	return dsi_i2c_probe_addr_only(DISPLAY_MCU_ADDR) == 0;
}

/*
 * Positive chip identification, not just an address ACK: the 0x45 slot
 * is shared by two incompatible parts — the Waveshare panel MCU
 * (registers 0x94+) and the official RPi 7" panel's ATTINY (registers
 * 0x80-0x91). The ATTINY NAKs the register phase of a 0x98 read, the
 * Waveshare MCU answers it.
 */
static bool waveshare_mcu_identified(void) {
	uint8_t id = 0;

	return dsi_i2c_read_reg8_retry(DISPLAY_MCU_ADDR,
			WAVESHARE_REG_ID, &id) == 0;
}

static int32_t waveshare_mcu_write_power(uint16_t state, uint32_t settle_ms, const char* tag) {
	(void)tag;

	if (dsi_i2c_write_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_TP,
			(uint8_t)(state >> 8)) != 0)
		return -1;
	if (dsi_i2c_write_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_LCD,
			(uint8_t)(state & 0xff)) != 0)
		return -1;
	usleep(settle_ms * 1000u);
	return 0;
}

static int32_t waveshare_mcu_sync_power(void) {
	uint8_t tp = 0;
	uint8_t lcd = 0;

	if (dsi_i2c_read_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_TP, &tp) != 0)
		return -1;
	if (dsi_i2c_read_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_LCD, &lcd) != 0)
		return -1;

	waveshare_power_state = (uint16_t)(((uint16_t)tp << 8) | lcd);
	waveshare_power_state_valid = true;
	return 0;
}

static int32_t waveshare_mcu_set_gpio(uint8_t gpio, bool level,
		uint32_t settle_ms, const char* tag) {
	uint16_t bit = (uint16_t)(1u << gpio);

	if (!waveshare_power_state_valid && waveshare_mcu_sync_power() != 0)
		return -1;

	if (level)
		waveshare_power_state |= bit;
	else
		waveshare_power_state &= (uint16_t)~bit;

	if (waveshare_mcu_write_power(waveshare_power_state, settle_ms, tag) != 0)
		return -1;
	return 0;
}

static tp_status_t waveshare_probe_goodix(const dsi_i2c_bus_t* bus) {
	(void)bus;
	return goodix_probe_candidates(bus);
}

static tp_status_t ft5x06_probe(const dsi_i2c_bus_t* bus) {
	uint8_t chip_id = 0xff;
	uint8_t fw_ver = 0xff;

	(void)bus;

	if (dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_ID, &chip_id) != 0) {
		if (dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_FW_VER, &fw_ver) != 0)
			return TP_NOT_RESPONSE;
	}
	else {
		(void)dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_FW_VER, &fw_ver);
	}

	touch_addr = FT5X06_ADDR;
	touch_kind = TOUCH_KIND_FT5X06;
	return TP_OK;
}

static tp_status_t waveshare_mcu_probe_touch_sequence(const dsi_i2c_bus_t* bus) {
	if (!waveshare_mcu_present(bus))
		return TP_NOT_RESPONSE;
	/*
	 * Run the power walk only against a positively identified
	 * Waveshare MCU: the walk writes registers 0x94/0x95, and against
	 * the ATTINY those bytes land on its own register space and
	 * power-cycle the panel out from under dsi_fbdisplayd — wedging
	 * the touch controller and glitching the display. An ATTINY panel
	 * falls through to the direct GT911 probe: fbdisplayd has already
	 * walked its rails and released TP reset.
	 */
	if (!waveshare_mcu_identified())
		return TP_NOT_RESPONSE;

	if (waveshare_mcu_sync_power() != 0)
		return TP_NOT_RESPONSE;

	if (waveshare_probe_goodix(bus) == TP_OK)
		return TP_OK;

	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_VCC, true, 20u, "vcc") != 0)
		return TP_NOT_RESPONSE;
	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_ENABLE, true, 20u, "enable") != 0)
		return TP_NOT_RESPONSE;
	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_AVDD, true, 40u, "avdd") != 0)
		return TP_NOT_RESPONSE;
	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_IOVCC, true, 60u, "iovcc") != 0)
		return TP_NOT_RESPONSE;
	if (waveshare_probe_goodix(bus) == TP_OK)
		return TP_OK;

	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, true, 20u,
			"touch-assert") != 0)
		return TP_NOT_RESPONSE;
	if (waveshare_probe_goodix(bus) == TP_OK)
		return TP_OK;

	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, false, 120u,
			"touch-release") != 0)
		return TP_NOT_RESPONSE;
	if (waveshare_probe_goodix(bus) == TP_OK)
		return TP_OK;

	if (!waveshare_panel_reset_tried) {
		waveshare_panel_reset_tried = true;
		if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_PANEL_RESET, true, 20u,
				"panel-assert") != 0)
			return TP_NOT_RESPONSE;
		if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_PANEL_RESET, false, 180u,
				"panel-release") != 0)
			return TP_NOT_RESPONSE;
		if (waveshare_probe_goodix(bus) == TP_OK)
			return TP_OK;

		if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, true, 20u,
				"touch-cycle-assert") != 0)
			return TP_NOT_RESPONSE;
		if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, false, 120u,
				"touch-cycle-release") != 0)
			return TP_NOT_RESPONSE;
		if (waveshare_probe_goodix(bus) == TP_OK)
			return TP_OK;
	}

	return TP_NOT_RESPONSE;
}

/*
 * GT911 address-select reset on the header bus: a bare Goodix on the
 * 40-pin header latches 0x14 vs 0x5d from the INT level at reset
 * release. Only meaningful on i2c1 (GPIO2/3), where GPIO17/GPIO4 are
 * free header pins; the FPC-panel Goodix is reset through the panel
 * MCU's TS_RESET bit instead.
 */
static void goodix_reset_select(const dsi_i2c_bus_t* bus, uint8_t addr) {
	bool int_level;

	if (bus == NULL)
		return;
	if (bus->sda != DSI_I2C1_SDA || bus->scl != DSI_I2C1_SCL)
		return;
	if (addr != GOODIX_ADDR_PRIMARY && addr != GOODIX_ADDR_FALLBACK)
		return;

	int_level = (addr == GOODIX_ADDR_FALLBACK);

	bcm2712_gpio_init();
	bcm2712_gpio_config(GOODIX_RESET_GPIO, GPIO_FUNC_OUTPUT);
	bcm2712_gpio_config(GOODIX_INT_GPIO, GPIO_FUNC_OUTPUT);
	bcm2712_gpio_pull(GOODIX_INT_GPIO, GPIO_PULL_NONE);

	bcm2712_gpio_write(GOODIX_INT_GPIO, int_level);

	bcm2712_gpio_write(GOODIX_RESET_GPIO, false);
	usleep(20000);
	bcm2712_gpio_write(GOODIX_RESET_GPIO, true);
	usleep(60000);

	bcm2712_gpio_config(GOODIX_INT_GPIO, GPIO_FUNC_INPUT);
	bcm2712_gpio_pull(GOODIX_INT_GPIO, GPIO_PULL_UP);
	usleep(20000);
}

static bool goodix_id_valid(const uint8_t* id_buf) {
	if ((id_buf[0] == 0 && id_buf[1] == 0 &&
			id_buf[2] == 0 && id_buf[3] == 0) ||
			(id_buf[0] == 0xff && id_buf[1] == 0xff &&
			 id_buf[2] == 0xff && id_buf[3] == 0xff))
		return false;
	return true;
}

static tp_status_t goodix_write_reg(uint16_t reg, const uint8_t* data, uint16_t len) {
	return dsi_i2c_write_reg16(touch_addr, reg, data, len) == 0 ?
		TP_OK : TP_NOT_RESPONSE;
}

static tp_status_t goodix_read_reg(uint16_t reg, uint8_t* data, uint16_t len) {
	if (dsi_i2c_read_reg16(touch_addr, reg, data, len) == 0)
		return TP_OK;
	if (waveshare_mcu_bus(active_bus) &&
			dsi_i2c_read_reg16_split(touch_addr, reg, data, len) == 0)
		return TP_OK;
	return TP_NOT_RESPONSE;
}

static tp_status_t goodix_set_command(uint8_t value) {
	return goodix_write_reg(GOODIX_REG_COMMAND, &value, 1);
}

static tp_status_t goodix_probe_addr(uint8_t addr, uint8_t* id_buf) {
	uint32_t attempt;

	touch_addr = addr;
	touch_kind = TOUCH_KIND_GOODIX;
	/* the ID read is the first contact with a controller that may
	 * still be coming out of reset: a single NAK must not sink the
	 * whole candidate — retry with a bus clear between attempts */
	for (attempt = 0; attempt < TP_I2C_RETRY_MAX; attempt++) {
		if (goodix_read_reg(GOODIX_REG_ID, id_buf, 4) == TP_OK)
			return TP_OK;
		if (active_bus == NULL)
			break;
		usleep(2000);
		dsi_i2c_bus_clear(active_bus);
		usleep(2000);
	}
	return TP_NOT_RESPONSE;
}

static tp_status_t goodix_probe_candidates(const dsi_i2c_bus_t* bus) {
	static const uint8_t candidates_default[] = {
		GOODIX_ADDR_PRIMARY,
		GOODIX_ADDR_FALLBACK,
	};
	static const uint8_t candidates_waveshare[] = {
		GOODIX_ADDR_FALLBACK,
		GOODIX_ADDR_PRIMARY,
	};
	uint8_t id_buf[4];
	const uint8_t* candidates;
	uint32_t candidate_nr;
	uint32_t addr_i;

	if (waveshare_mcu_bus(bus)) {
		candidates = candidates_waveshare;
		candidate_nr = sizeof(candidates_waveshare);
	}
	else {
		candidates = candidates_default;
		candidate_nr = sizeof(candidates_default);
	}

	for (addr_i = 0; addr_i < candidate_nr; addr_i++) {
		goodix_reset_select(bus, candidates[addr_i]);
		memset(id_buf, 0, sizeof(id_buf));
		if (goodix_probe_addr(candidates[addr_i], id_buf) != TP_OK)
			continue;
		if (!goodix_id_valid(id_buf))
			continue;

		goodix_set_command(0x00);
		return TP_OK;
	}

	return TP_NOT_RESPONSE;
}

static tp_status_t goodix_init(void) {
	uint32_t bus_i;

	for (bus_i = 0; bus_i < sizeof(dsi_i2c_buses) / sizeof(dsi_i2c_buses[0]); bus_i++) {
		if (dsi_i2c_select(&dsi_i2c_buses[bus_i]) != 0)
			continue;
		usleep(20000);

		if (ft5x06_probe(&dsi_i2c_buses[bus_i]) == TP_OK)
			return TP_OK;
		if (waveshare_mcu_probe_touch_sequence(&dsi_i2c_buses[bus_i]) == TP_OK)
			return TP_OK;
		else if (goodix_probe_candidates(&dsi_i2c_buses[bus_i]) == TP_OK)
			return TP_OK;

	}

	return TP_NOT_RESPONSE;
}

static tp_status_t goodix_read_touch(tp_point_t* pts, uint8_t* nr) {
	uint8_t status;
	uint8_t buf[GOODIX_POINT_SIZE * GOODIX_MAX_POINTS + 1];
	uint8_t i;

	if (goodix_read_reg(GOODIX_REG_STATUS, &status, 1) != TP_OK)
		return TP_NOT_RESPONSE;
	if ((status & GOODIX_READY_FLAG) == 0)
		return TP_NO_DATA;

	*nr = status & 0x0f;
	if (*nr > GOODIX_MAX_POINTS)
		*nr = GOODIX_MAX_POINTS;

	/*
	 * The point records are contiguous: one bulk read instead of one
	 * transaction per point. Every transaction costs an address phase,
	 * a start/stop on the wire and a controller disable/enable cycle
	 * over PCIe MMIO — with five points that is five times the bus and
	 * link load of a single burst, for the same coordinate bytes.
	 *
	 * Each record is led by its track id (0x814f + i*8, stable across
	 * the contact's life), then x/y low-high pairs — the same layout
	 * the Linux goodix driver parses. Start the burst one byte before
	 * the first coordinate block so every record's id comes with it.
	 */
	if (*nr > 0 &&
			goodix_read_reg(GOODIX_REG_TRACK1, buf,
				(uint16_t)(*nr * GOODIX_POINT_SIZE + 1)) != TP_OK)
		return TP_NOT_RESPONSE;
	for (i = 0; i < *nr; i++) {
		const uint8_t* p = buf + 1 + i * GOODIX_POINT_SIZE;
		pts[i].id = (uint8_t)(buf[i * GOODIX_POINT_SIZE] & 0x0f);
		pts[i].x = (uint16_t)((p[1] << 8) | p[0]);
		pts[i].y = (uint16_t)((p[3] << 8) | p[2]);
	}

	/*
	 * The buffer-status clear must not be fire-and-forget: if it is
	 * lost, the GT911 keeps the ready flag raised with the same stale
	 * coordinates and never refreshes, and the slot diff then sees no
	 * change and emits nothing — the touch looks frozen from then on.
	 * Retry the clear, and report a persistent failure so the poll
	 * loop's recovery path can kick in.
	 */
	status = 0;
	for (i = 0; i < TP_I2C_RETRY_MAX; i++) {
		if (goodix_write_reg(GOODIX_REG_STATUS, &status, 1) == TP_OK)
			return TP_OK;
		usleep(2000);
	}
	return TP_NOT_RESPONSE;
}

static tp_status_t ft5x06_read_touch(tp_point_t* pts, uint8_t* nr) {
	uint8_t buf[5];
	/* P2..Pn records when more than one finger is reported */
	uint8_t rest[FT5X06_POINT_SIZE * (FT5X06_MAX_POINTS - 1)];
	uint8_t count;
	uint8_t event;
	uint8_t i;

	*nr = 0;

	if (dsi_i2c_read_regs8_retry(FT5X06_ADDR, FT5X06_REG_TD_STATUS,
			buf, sizeof(buf)) != 0) {
		if (dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_TD_STATUS, &buf[0]) != 0 ||
				dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_XH, &buf[1]) != 0 ||
				dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_XL, &buf[2]) != 0 ||
				dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_YH, &buf[3]) != 0 ||
				dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_YL, &buf[4]) != 0)
			return TP_NOT_RESPONSE;
	}

	/*
	 * Garbage frame: a 0xff TD_STATUS is the data phase clocking in
	 * all-ones (controller mid-boot or a NAK'd read), not a real
	 * 15-touch report — drop it instead of parsing phantom points.
	 */
	if (buf[0] == 0xff)
		return TP_NO_DATA;

	count = (uint8_t)(buf[0] & 0x0f);
	if (count > FT5X06_MAX_POINTS)
		count = FT5X06_MAX_POINTS;
	if (count == 0)
		return TP_OK;

	/*
	 * Every reported point register must be consumed: this controller
	 * latches its report frame while extra points sit unread —
	 * TD_STATUS freezes at count>=2, P1 repeats forever, the release
	 * never arrives and the touch service dies. Read P2..Pn in one
	 * burst and parse them alongside P1.
	 */
	if (count > 1) {
		if (dsi_i2c_read_regs8_retry(FT5X06_ADDR, FT5X06_REG_P2_XH,
				rest, (uint16_t)((count - 1) * FT5X06_POINT_SIZE)) != 0)
			return TP_NOT_RESPONSE;
	}

	/* P1: XH = event<<6 | x-high, YH = touch-id<<4 | y-high */
	event = (uint8_t)(buf[1] >> 6);
	if (event == FT5X06_EVENT_DOWN || event == FT5X06_EVENT_CONTACT) {
		pts[*nr].id = (uint8_t)((buf[3] >> 4) & 0x0f);
		pts[*nr].x = (uint16_t)(((uint16_t)(buf[1] & 0x0f) << 8) | buf[2]);
		pts[*nr].y = (uint16_t)(((uint16_t)(buf[3] & 0x0f) << 8) | buf[4]);
		(*nr)++;
	}
	/* P2..Pn: same 6-byte record shape. UP/reserved entries are lifts,
	 * not active contacts — leaving them out of the set lets the slot
	 * diff release them through the grace window. */
	for (i = 1; i < count; i++) {
		const uint8_t* p = rest + (i - 1) * FT5X06_POINT_SIZE;

		event = (uint8_t)(p[0] >> 6);
		if (event != FT5X06_EVENT_DOWN && event != FT5X06_EVENT_CONTACT)
			continue;
		pts[*nr].id = (uint8_t)((p[2] >> 4) & 0x0f);
		pts[*nr].x = (uint16_t)(((uint16_t)(p[0] & 0x0f) << 8) | p[1]);
		pts[*nr].y = (uint16_t)(((uint16_t)(p[2] & 0x0f) << 8) | p[3]);
		(*nr)++;
	}
	return TP_OK;
}

static tp_status_t tp_read_touch(tp_point_t* pts, uint8_t* nr) {
	if (touch_kind == TOUCH_KIND_FT5X06)
		return ft5x06_read_touch(pts, nr);
	return goodix_read_touch(pts, nr);
}

static int tp_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, off_t offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)offset;
	(void)p;

	if (!touch_has_data())
		return VFS_ERR_RETRY;
	if (size < 6)
		return -1;

	memcpy(buf, touch_data[touch_data_read % TP_TOUCH_CACHE_SIZE], 6);
	memset(touch_data[touch_data_read % TP_TOUCH_CACHE_SIZE], 0,
			sizeof(touch_data[0]));
	touch_data_read++;
	return 6;
}

static uint32_t tp_check_poll_events(vdevice_t* dev, int fd, int from_pid,
		fsinfo_t* node, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)p;

	return touch_has_data() ? VFS_EVT_RD : 0;
}

static int tp_loop(vdevice_t* dev, void* p) {
	tp_status_t ret;
	bool need_wakeup = false;
	uint64_t now_ms = kernel_tic_ms(0);

	(void)p;

	if (!tp_ready) {
		if (now_ms >= tp_retry_ts) {
			tp_retry_count++;
			if (goodix_init() == TP_OK) {
				tp_ready = true;
				i2c_fail_count = 0;
				poll_sleep_us = TP_POLL_MIN_US;
				/* the controller may have been power-cycled by the
				 * probe: start from an empty contact set */
				memset(mt_slots, 0, sizeof(mt_slots));
				mt_last_frame_ms = now_ms;
				mt_zero_frame = true;
				slog("dsi_touchd: touch on bus=%s addr=0x%02x kind=%s %ukHz\n",
						active_bus == NULL ? "?" : active_bus->name,
						touch_addr,
						touch_kind == TOUCH_KIND_FT5X06 ? "ft5x06" : "goodix",
						active_bus == NULL ? 0u : active_bus->hz / 1000u);
			}
			else {
				/* probe failures are otherwise silent; log every few
				 * rounds so a dead bus is diagnosable from the boot
				 * log */
				if ((tp_retry_count % 5) == 1)
					slog("dsi_touchd: no touch on %u buses (round %u)\n",
							(uint32_t)(sizeof(dsi_i2c_buses) / sizeof(dsi_i2c_buses[0])),
							tp_retry_count);
				tp_retry_ts = now_ms + TP_INIT_RETRY_MS;
			}
		}

		usleep(TP_POLL_MAX_US);
		return 0;
	}

	point_nr = 0;
	ret = tp_read_touch(point, &point_nr);

	if (ret == TP_NOT_RESPONSE) {
		/*
		 * A single failed transaction (the GT911 NAKs while it is
		 * busy, FPC noise next to the DSI lanes) must not burn the
		 * link budget: retry inside the poll and only count a real
		 * streak of failures.
		 */
		uint8_t retry;
		for (retry = 1; retry < TP_I2C_RETRY_MAX; retry++) {
			usleep(2000);
			ret = tp_read_touch(point, &point_nr);
			if (ret != TP_NOT_RESPONSE)
				break;
		}
	}

	if (ret == TP_NOT_RESPONSE) {
		i2c_fail_count++;
		if (i2c_fail_count >= TP_I2C_FAIL_MAX) {
			i2c_fail_count = 0;
			need_wakeup = mt_release_all() || need_wakeup;
			/* a transfer that died mid-byte can leave the slave
			 * holding SDA low, wedging every later transfer; clock
			 * the bus clear before re-probing (the re-select inside
			 * re-pins the bus's declared speed) */
			dsi_i2c_bus_clear(active_bus);
			tp_ready = false;
			tp_retry_ts = now_ms;
			slog("dsi_touchd: link_lost bus=%s addr=0x%02x kind=%s\n",
					active_bus == NULL ? "?" : active_bus->name,
					touch_addr,
					touch_kind == TOUCH_KIND_FT5X06 ? "ft5x06" : "goodix");
		}
	}
	else if (ret == TP_OK || ret == TP_NO_DATA) {
		i2c_fail_count = 0;
	}

	if (ret == TP_OK) {
		/* a decoded frame (even zero contacts) is the controller's
		 * current truth: diff it against the emitted set */
		mt_last_frame_ms = now_ms;
		mt_zero_frame = (point_nr == 0);
		if (mt_emit_frame(point, point_nr))
			need_wakeup = true;
	}

	/* Advance the release grace window while contacts are still marked
	 * active but no contact frame carries them: after an explicit
	 * zero-contact frame feed empty diffs every tick (the controller
	 * sends nothing more once the last finger lifts); after a missed
	 * final frame wait out the idle window first, then do the same.
	 * A no-op once no slot is active. */
	if (mt_any_active() &&
			(mt_zero_frame ||
			 now_ms - mt_last_frame_ms >= TP_RELEASE_TIMEOUT_MS)) {
		if (mt_emit_frame(NULL, 0))
			need_wakeup = true;
	}

	if (need_wakeup) {
		poll_sleep_us = TP_POLL_MIN_US;
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	}
	else if (mt_any_active()) {
		poll_sleep_us = TP_POLL_MIN_US;
	}
	else if (poll_sleep_us < TP_POLL_MAX_US) {
		poll_sleep_us <<= 1;
		if (poll_sleep_us > TP_POLL_MAX_US)
			poll_sleep_us = TP_POLL_MAX_US;
	}

	usleep(poll_sleep_us);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = "/dev/touch0";
	vdevice_t dev;

	if (argc > 1)
		mnt_point = argv[1];

	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.desc, "dsi_touch");
	dev.read = tp_read;
	dev.loop_step = tp_loop;
	dev.check_poll_events = tp_check_poll_events;

	slog("dsi_touchd: starting, mount=%s\n", mnt_point);

	/*
	 * multi_task is mandatory here: in single-task mode the kernel's
	 * sys_usleep takes the "schedule, do not really sleep" fast path
	 * for the main context while a blocked client (xtouch reading
	 * /dev/touch0) keeps an ipc task queued. Every usleep in the poll
	 * loop — including all the 20-180ms rail-settle and reset waits
	 * inside the probe sequence — then collapses to nothing: the loop
	 * burns a whole core, and the GT911 power-up timing never holds,
	 * so probing fails forever.
	 */
	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, true);
	return 0;
}
