/*
 * ev3test.cc - LEGO EV3 driver test suite (widget++ GUI).
 *
 * A multi-level, list-driven tester for every lego.ev3 vdevice driver,
 * modeled on system/xwin/apps/wDemo (a custom List subclass renders the
 * menu; selecting an entry drills in, runs an action, or goes back).
 *
 * Navigation (3 levels + back):
 *   level 0  ROOT      : Sensors / Actuators / System / Run all / Exit
 *   level 1  category  : the drivers in that category
 *   level 2  test      : a live value panel + a list of actions
 *   (Motor inserts one more level to pick output port A/B/C/D.)
 *
 * Input: the EV3 brick has no mouse/touch - its 6 buttons arrive as
 * XEVT_IM key events (UP/DOWN/LEFT/RIGHT/ENTER/ESC, see gpio_joystickd
 * + xim_none). So the window intercepts those keys to move the selection
 * and activate entries; plain mouse selection (as in wDemo) also works.
 * Navigation/activation is deferred to onTimer so the widget tree is never
 * rebuilt while an event is still being dispatched through it.
 *
 * Every driver is exercised through its structured dev_cntl() interface
 * (the fixed-width-int structs from the arch/ev3 device headers); the raw
 * ADC and joystick nodes are read with read(). Each test screen polls its
 * device on the window timer and prints the result in the live panel; each
 * action prints its outcome on the status line. Drivers that are not
 * started (gyro/color/ir/nxt-us are commented out in init.rd) simply show
 * "n/a" instead of failing.
 *
 * The EV3 LCD is 178x128 @ font 12, so text is kept short and the window
 * opens full-screen with no frame/title (use the Exit entry to quit).
 */

#include <Widget/WidgetWin.h>
#include <Widget/WidgetX.h>
#include <Widget/Label.h>
#include <Widget/List.h>
#include <Widget/Scroller.h>

#include <x++/X.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/proto.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/keydef.h>
#include <arch/ev3/motor.h>
#include <arch/ev3/led_dev.h>
#include <arch/ev3/beep_dev.h>
#include <arch/ev3/battery_dev.h>
#include <arch/ev3/sensor_dev.h>
#include <arch/ev3/i2c_dev.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

using namespace Ewok;

/* ---- device nodes (match init.rd) ---- */
#define DEV_LED     "/dev/led"
#define DEV_BEEP    "/dev/beep"
#define DEV_BATTERY "/dev/battery"
#define DEV_MOTOR   "/dev/motor"
#define DEV_TOUCH   "/dev/touch0"
#define DEV_US      "/dev/us0"
#define DEV_GYRO    "/dev/gyro0"
#define DEV_COLOR   "/dev/color0"
#define DEV_IR      "/dev/ir0"
#define DEV_NXTUS   "/dev/nxt-us0"
#define DEV_ADC     "/dev/adc0"
#define DEV_JOY     "/dev/joystick"
#define DEV_I2C     "/dev/i2c2"   /* i2cd -p 3, see init.rd */

/* LED menu presets: doStructAction() builds a led_state_t from these. */
enum {
	LEDP_OFF = 0, LEDP_GREEN, LEDP_RED, LEDP_ORANGE,
	LEDP_BLINK, LEDP_PULSE, LEDP_CYCLE
};

/* ---- menu entry operations ---- */
enum {
	OP_SUB = 1,   /* push submenu (cmd = screen id)          */
	OP_TEST,      /* open a driver test screen (cmd = id)    */
	OP_BACK,      /* pop one level                           */
	OP_EXIT,      /* quit the app                            */
	OP_RUNALL,    /* run the batch probe, show results       */
	OP_SCNTL,     /* action: dev_cntl(dev, selector, struct) */
	OP_CMD,       /* action: dev_cmd(dev, text)              */
	OP_INFO       /* display-only line (results screen)      */
};

struct MenuEntry {
	const char* label;
	int16_t op;
	int16_t cmd;          /* OP_SUB/OP_TEST: screen id; OP_SCNTL: struct cmd */
	int32_t a0, a1, a2;   /* OP_SCNTL: struct arguments                       */
	const char* text;     /* OP_CMD: text command                             */
};

#define SUB(label, scr)        { label, OP_SUB,  scr, 0, 0, 0, NULL }
#define TEST(label, scr)       { label, OP_TEST, scr, 0, 0, 0, NULL }
#define TESTP(label, scr, p)   { label, OP_TEST, scr, p, 0, 0, NULL }
#define SCNTL(label, c, x, y, z)   { label, OP_SCNTL, c, x, y, z, NULL }
#define CMD(label, text)       { label, OP_CMD, 0, 0, 0, 0, text }
#define BACK_ITEM              { ".. back", OP_BACK, 0, 0, 0, 0, NULL }
#define EXIT_ITEM              { "Exit", OP_EXIT, 0, 0, 0, 0, NULL }
#define RUNALL_ITEM            { "Run all tests", OP_RUNALL, 0, 0, 0, 0, NULL }

/* ---- screen ids ---- */
enum {
	S_ROOT = 0,
	S_SENSORS, S_ACTUATORS, S_SYSTEM, S_MOTORPORT,
	S_LED, S_BEEP, S_BATTERY, S_MOTOR, S_TOUCH, S_US,
	S_GYRO, S_COLOR, S_IR, S_ADC, S_JOY, S_I2C, S_NXTUS,
	S_RESULTS,
	S_COUNT
};

/* ---- static menu tables ---- */
static const MenuEntry menu_root[] = {
	SUB("Sensors   >", S_SENSORS),
	SUB("Actuators >", S_ACTUATORS),
	SUB("System    >", S_SYSTEM),
	RUNALL_ITEM,
	EXIT_ITEM,
};

static const MenuEntry menu_sensors[] = {
	TEST("Touch    in1", S_TOUCH),
	TEST("Ultrasonic in2", S_US),
	TEST("Gyro     in?", S_GYRO),
	TEST("Color    in?", S_COLOR),
	TEST("IR       in?", S_IR),
	TEST("NXT-US   i2c", S_NXTUS),
	TEST("ADC raw", S_ADC),
	BACK_ITEM,
};

static const MenuEntry menu_actuators[] = {
	SUB("Motor  >", S_MOTORPORT),
	TEST("LED", S_LED),
	TEST("Beep", S_BEEP),
	BACK_ITEM,
};

static const MenuEntry menu_system[] = {
	TEST("Battery", S_BATTERY),
	TEST("Joystick", S_JOY),
	TEST("I2C bus", S_I2C),
	BACK_ITEM,
};

static const MenuEntry menu_motorport[] = {
	TESTP("Port A", S_MOTOR, MOTOR_PORT_A),
	TESTP("Port B", S_MOTOR, MOTOR_PORT_B),
	TESTP("Port C", S_MOTOR, MOTOR_PORT_C),
	TESTP("Port D", S_MOTOR, MOTOR_PORT_D),
	BACK_ITEM,
};

static const MenuEntry menu_led[] = {
	SCNTL("All off",     LEDP_OFF,    0, 0, 0),
	SCNTL("Green both",  LEDP_GREEN,  0, 0, 0),
	SCNTL("Red both",    LEDP_RED,    0, 0, 0),
	SCNTL("Orange both", LEDP_ORANGE, 0, 0, 0),
	SCNTL("Blink green", LEDP_BLINK,  0, 0, 0),
	SCNTL("Pulse green", LEDP_PULSE,  0, 0, 0),
	SCNTL("Cycle all",   LEDP_CYCLE,  0, 0, 0),
	BACK_ITEM,
};

static const MenuEntry menu_beep[] = {
	SCNTL("Beep 1k 300ms", BEEP_CMD_BEEP,   1000, 300, 0),
	SCNTL("Tone 440",      BEEP_CMD_TONE,   440,  0,   0),
	SCNTL("Tone 880",      BEEP_CMD_TONE,   880,  0,   0),
	SCNTL("Stop",          BEEP_CMD_STOP,   0,    0,   0),
	SCNTL("Melody 1",      BEEP_CMD_MELODY, 0,    0,   1),
	SCNTL("Melody 3",      BEEP_CMD_MELODY, 0,    0,   3),
	BACK_ITEM,
};

static const MenuEntry menu_battery[] = {
	SCNTL("Set low 6800mV", 0, 6800, 0, 0),
	SCNTL("Set low 7000mV", 0, 7000, 0, 0),
	BACK_ITEM,
};

static const MenuEntry menu_motor[] = {
	SCNTL("Run +50",     MOTOR_CMD_RUN,          50,  MOTOR_STOP_COAST, 0),
	SCNTL("Run -50",     MOTOR_CMD_RUN,          -50, MOTOR_STOP_COAST, 0),
	SCNTL("Speed 180/s", MOTOR_CMD_RUN_AT_SPEED, 180, MOTOR_STOP_COAST, 0),
	SCNTL("Hold",        MOTOR_CMD_HOLD,         MOTOR_STOP_BRAKE, 0, 0),
	SCNTL("Stop brake",  MOTOR_CMD_STOP,         MOTOR_STOP_BRAKE, 0, 0),
	SCNTL("Stop coast",  MOTOR_CMD_STOP,         MOTOR_STOP_COAST, 0, 0),
	SCNTL("Zero pos",    MOTOR_CMD_SET_POSITION, 0,   0, 0),
	BACK_ITEM,
};

static const MenuEntry menu_touch[] = { BACK_ITEM };

/* sensor screens: SCNTL cmd = EV3_SENSOR_CMD_*, a0 = mode */
static const MenuEntry menu_us[] = {
	SCNTL("Mode CM",     EV3_SENSOR_CMD_SET_MODE, US_MODE_DIST_CM, 0, 0),
	SCNTL("Mode IN",     EV3_SENSOR_CMD_SET_MODE, US_MODE_DIST_IN, 0, 0),
	SCNTL("Mode LISTEN", EV3_SENSOR_CMD_SET_MODE, US_MODE_LISTEN,  0, 0),
	BACK_ITEM,
};

static const MenuEntry menu_gyro[] = {
	SCNTL("Mode ANGLE", EV3_SENSOR_CMD_SET_MODE, GYRO_MODE_ANG,  0, 0),
	SCNTL("Mode RATE",  EV3_SENSOR_CMD_SET_MODE, GYRO_MODE_RATE, 0, 0),
	SCNTL("Mode G&A",   EV3_SENSOR_CMD_SET_MODE, GYRO_MODE_GA,   0, 0),
	SCNTL("Reset",      EV3_SENSOR_CMD_RESET,    0, 0, 0),
	BACK_ITEM,
};

static const MenuEntry menu_color[] = {
	SCNTL("Mode REFLECT", EV3_SENSOR_CMD_SET_MODE, COLOR_MODE_REFLECT, 0, 0),
	SCNTL("Mode AMBIENT", EV3_SENSOR_CMD_SET_MODE, COLOR_MODE_AMBIENT, 0, 0),
	SCNTL("Mode COLOR",   EV3_SENSOR_CMD_SET_MODE, COLOR_MODE_COLOR,   0, 0),
	SCNTL("Mode RGB-RAW", EV3_SENSOR_CMD_SET_MODE, COLOR_MODE_RGB_RAW, 0, 0),
	BACK_ITEM,
};

static const MenuEntry menu_ir[] = {
	SCNTL("Mode PROX",   EV3_SENSOR_CMD_SET_MODE, IR_MODE_PROX,   0, 0),
	SCNTL("Mode SEEK",   EV3_SENSOR_CMD_SET_MODE, IR_MODE_SEEK,   0, 0),
	SCNTL("Mode REMOTE", EV3_SENSOR_CMD_SET_MODE, IR_MODE_REMOTE, 0, 0),
	BACK_ITEM,
};

static const MenuEntry menu_nxtus[] = {
	SCNTL("Mode off",    EV3_SENSOR_CMD_SET_MODE, NXTUS_MODE_OFF,        0, 0),
	SCNTL("Mode single", EV3_SENSOR_CMD_SET_MODE, NXTUS_MODE_SINGLE,     0, 0),
	SCNTL("Mode cont",   EV3_SENSOR_CMD_SET_MODE, NXTUS_MODE_CONTINUOUS, 0, 0),
	BACK_ITEM,
};

static const MenuEntry menu_adc[] = { BACK_ITEM };

static const MenuEntry menu_joy[] = { BACK_ITEM };

/* i2c screen: SCNTL a0 = bus clock in Hz (I2C_CNTL_SPEED) */
static const MenuEntry menu_i2c[] = {
	CMD("Scan bus", "scan"),
	SCNTL("Speed 9.6k",  0, 9600,   0, 0),
	SCNTL("Speed 100k",  0, 100000, 0, 0),
	BACK_ITEM,
};

#define NELEM(a) ((int)(sizeof(a)/sizeof((a)[0])))

static const MenuEntry* screenEntries(int scr, int* count) {
	switch (scr) {
	case S_ROOT:       *count = NELEM(menu_root);       return menu_root;
	case S_SENSORS:    *count = NELEM(menu_sensors);    return menu_sensors;
	case S_ACTUATORS:  *count = NELEM(menu_actuators);  return menu_actuators;
	case S_SYSTEM:     *count = NELEM(menu_system);     return menu_system;
	case S_MOTORPORT:  *count = NELEM(menu_motorport);  return menu_motorport;
	case S_LED:        *count = NELEM(menu_led);        return menu_led;
	case S_BEEP:       *count = NELEM(menu_beep);       return menu_beep;
	case S_BATTERY:    *count = NELEM(menu_battery);    return menu_battery;
	case S_MOTOR:      *count = NELEM(menu_motor);      return menu_motor;
	case S_TOUCH:      *count = NELEM(menu_touch);      return menu_touch;
	case S_US:         *count = NELEM(menu_us);         return menu_us;
	case S_GYRO:       *count = NELEM(menu_gyro);       return menu_gyro;
	case S_COLOR:      *count = NELEM(menu_color);      return menu_color;
	case S_IR:         *count = NELEM(menu_ir);         return menu_ir;
	case S_ADC:        *count = NELEM(menu_adc);        return menu_adc;
	case S_JOY:        *count = NELEM(menu_joy);        return menu_joy;
	case S_I2C:        *count = NELEM(menu_i2c);        return menu_i2c;
	case S_NXTUS:      *count = NELEM(menu_nxtus);      return menu_nxtus;
	default:           *count = 0;                      return NULL;
	}
}

static const char* screenDev(int scr) {
	switch (scr) {
	case S_LED:     return DEV_LED;
	case S_BEEP:    return DEV_BEEP;
	case S_BATTERY: return DEV_BATTERY;
	case S_MOTOR:   return DEV_MOTOR;
	case S_TOUCH:   return DEV_TOUCH;
	case S_US:      return DEV_US;
	case S_GYRO:    return DEV_GYRO;
	case S_COLOR:   return DEV_COLOR;
	case S_IR:      return DEV_IR;
	case S_ADC:     return DEV_ADC;
	case S_JOY:     return DEV_JOY;
	case S_I2C:     return DEV_I2C;
	case S_NXTUS:   return DEV_NXTUS;
	default:        return NULL;
	}
}

static int screenLiveLines(int scr) {
	switch (scr) {
	case S_LED:     return 1;
	case S_BEEP:    return 1;
	case S_BATTERY: return 2;
	case S_MOTOR:   return 3;
	case S_TOUCH:   return 1;
	case S_US:      return 2;
	case S_GYRO:    return 2;
	case S_COLOR:   return 2;
	case S_IR:      return 2;
	case S_ADC:     return 4;
	case S_JOY:     return 1;
	case S_NXTUS:   return 1;
	case S_I2C:     return 1;
	default:        return 0;
	}
}

static const char* screenTitle(int scr) {
	switch (scr) {
	case S_ROOT:      return "EV3 driver test";
	case S_SENSORS:   return "Sensors";
	case S_ACTUATORS: return "Actuators";
	case S_SYSTEM:    return "System";
	case S_MOTORPORT: return "Motor port";
	case S_LED:       return "LED";
	case S_BEEP:      return "Beep";
	case S_BATTERY:   return "Battery";
	case S_TOUCH:     return "Touch in1";
	case S_US:        return "Ultrasonic in2";
	case S_GYRO:      return "Gyro";
	case S_COLOR:     return "Color";
	case S_IR:        return "IR";
	case S_ADC:       return "ADC raw";
	case S_JOY:       return "Joystick";
	case S_I2C:       return "I2C bus";
	case S_NXTUS:     return "NXT-US i2c";
	case S_RESULTS:   return "Results";
	default:          return "?";
	}
}

/*
 * dev_cntl helper for the struct protocol: reply is <status int>[struct].
 * Returns true when the daemon answered with status 0 (and, if out != NULL,
 * the payload was copied).
 */
static bool cntlGetStruct(const char* dev, int cmd, void* out, int outsz) {
	proto_t ret;
	PF->init(&ret);
	int r = dev_cntl(dev, cmd, NULL, &ret);
	bool ok = (r == 0 && proto_read_int(&ret) == 0);
	if (ok && out != NULL)
		ok = (proto_read_to(&ret, out, outsz) == outsz);
	PF->clear(&ret);
	return ok;
}

static bool sensorGet(const char* dev, ev3_sensor_data_t* d) {
	memset(d, 0, sizeof(*d));
	return cntlGetStruct(dev, EV3_SENSOR_CNTL_GET_DATA, d, sizeof(*d));
}

class Ev3TestWin;

/*
 * Menu list: renders MenuEntry labels, highlights the selection, and
 * defers activation to the window (so the tree is never rebuilt from
 * inside the event dispatch that is iterating it).
 */
class NavList: public List {
	const MenuEntry* items;
	int count;
	bool guard;
	Ev3TestWin* win;
protected:
	void drawItem(graph_t* g, XTheme* theme, int32_t index, const grect_t& r) {
		if (index < 0 || index >= count)
			return;
		uint32_t fg = theme->basic.fgColor;
		if (index == itemSelected) {
			graph_fill_rect(g, r.x, r.y, r.w, r.h, theme->basic.selectBGColor);
			fg = theme->basic.selectColor;
		}
		/* center the text in the row so big fonts are not clipped */
		int fh = (int)theme->basic.fontSize;
		int y = r.y + (r.h > fh ? (r.h - fh) / 2 : 0);
		graph_draw_text_font(g, r.x + 2, y, items[index].label,
				theme->getFont(), theme->basic.fontSize, fg);
	}
	void onSelect(int sel);   /* defined after Ev3TestWin */
public:
	NavList(Ev3TestWin* w) {
		win = w; items = NULL; count = 0; guard = false;
		setItemMargin(1);
	}
	void setEntries(const MenuEntry* e, int n) {
		items = e; count = n; setItemNum((uint32_t)n);
	}
	int getCount() { return count; }
	/* move the highlight without firing the action */
	void selectGuarded(int i) {
		guard = true;
		select(i);
		guard = false;
	}
};

class Ev3TestWin: public WidgetWin {
public:
	static const int MAX_PATH = 8;
	static const int MAX_LIVE = 4;
	static const int MAX_RESULTS = 20;
	static const int RLABEL = 24;

	int path[MAX_PATH];
	int pathLen;
	int motorPort;
	int fontH;          /* theme font size; all row heights derive from it */

	Label* titleLabel;
	Label* statusLabel;
	Label* liveLabels[MAX_LIVE];
	int liveCount;
	NavList* list;
	int rawFd;
	char titleBuf[24];

	/* deferred navigation request (executed in onTimer) */
	int pendingOp;      /* 0 none, 1 activate, 2 back */
	int pendingSel;

	/* results screen storage */
	char resultLabels[MAX_RESULTS][RLABEL];
	MenuEntry dynEntries[MAX_RESULTS];
	int dynCount;

	Ev3TestWin() {
		pathLen = 1;
		path[0] = S_ROOT;
		motorPort = MOTOR_PORT_A;
		fontH = 12;
		titleLabel = statusLabel = NULL;
		list = NULL;
		liveCount = 0;
		rawFd = -1;
		pendingOp = 0;
		pendingSel = 0;
		dynCount = 0;
		for (int i = 0; i < MAX_LIVE; i++)
			liveLabels[i] = NULL;
	}

	~Ev3TestWin() {
		if (rawFd >= 0)
			::close(rawFd);
	}

	int cur() { return path[pathLen - 1]; }

	void setFontSize(int fs) {
		fontH = (fs > 0) ? fs : 12;
	}

	const MenuEntry* currentEntries(int* count) {
		if (cur() == S_RESULTS) {
			*count = dynCount;
			return dynEntries;
		}
		return screenEntries(cur(), count);
	}

	const char* titleFor(int s) {
		if (s == S_MOTOR) {
			snprintf(titleBuf, sizeof(titleBuf), "Motor %c", 'A' + motorPort);
			return titleBuf;
		}
		return screenTitle(s);
	}

	void setLive(int i, const char* s) {
		if (i >= 0 && i < liveCount && liveLabels[i] != NULL)
			liveLabels[i]->setLabel(s);
	}

	void setStatus(const char* s) {
		if (statusLabel != NULL)
			statusLabel->setLabel(s);
	}

	/* ---- build the widget tree for the current screen ---- */
	void render() {
		if (rawFd >= 0) {
			::close(rawFd);
			rawFd = -1;
		}
		root->clear();
		titleLabel = statusLabel = NULL;
		list = NULL;
		liveCount = 0;
		for (int i = 0; i < MAX_LIVE; i++)
			liveLabels[i] = NULL;

		int s = cur();
		int count = 0;
		const MenuEntry* entries = currentEntries(&count);

		titleLabel = new Label(titleFor(s));
		titleLabel->fix(0, fontH + 2);
		root->add(titleLabel);

		int ll = screenLiveLines(s);
		if (ll > MAX_LIVE)
			ll = MAX_LIVE;
		for (int i = 0; i < ll; i++) {
			Label* L = new Label("");
			L->fix(0, fontH + 1);
			root->add(L);
			liveLabels[i] = L;
		}
		liveCount = ll;

		statusLabel = new Label("OK=sel  LT=back");
		statusLabel->fix(0, fontH + 1);
		root->add(statusLabel);

		Container* row = new Container();
		row->setType(Container::HORIZONTAL);
		root->add(row);

		list = new NavList(this);
		list->setEntries(entries, count);
		list->setItemSize(fontH + 3);
		row->add(list);

		Scroller* sr = new Scroller();
		sr->fix(6, 0);
		row->add(sr);
		list->setScrollerV(sr);
		list->selectGuarded(0);

		if (s == S_ADC)
			rawFd = ::open(DEV_ADC, O_RDONLY | O_NONBLOCK);
		else if (s == S_JOY)
			rawFd = ::open(DEV_JOY, O_RDONLY | O_NONBLOCK);

		root->focus(list);

		if (getCWin() != NULL)
			poll();
	}

	void push(int scr) {
		if (pathLen < MAX_PATH)
			path[pathLen++] = scr;
		render();
	}

	void pop() {
		if (pathLen > 1)
			pathLen--;
		render();
	}

	/* called by NavList (mouse) and by the key handler; deferred to onTimer */
	void requestActivate(int sel) {
		pendingSel = sel;
		pendingOp = 1;
	}
	void requestBack() {
		pendingOp = 2;
	}

	void moveSel(int delta) {
		if (list == NULL)
			return;
		int c = list->getCount();
		if (c <= 0)
			return;
		int i = list->getSelected();
		if (i < 0)
			i = 0;
		i = (i + delta + c) % c;
		list->selectGuarded(i);
	}

	void activateSel() {
		if (list == NULL)
			return;
		requestActivate(list->getSelected());
	}

	void handleSelect(int sel) {
		int count = 0;
		const MenuEntry* entries = currentEntries(&count);
		if (entries == NULL || sel < 0 || sel >= count)
			return;
		const MenuEntry& e = entries[sel];
		switch (e.op) {
		case OP_SUB:    push(e.cmd); break;
		case OP_TEST:
			if (e.cmd == S_MOTOR)
				motorPort = e.a0;
			push(e.cmd);
			break;
		case OP_BACK:   pop(); break;
		case OP_EXIT:   close(); break;
		case OP_RUNALL: runAll(); break;
		case OP_SCNTL:
		case OP_CMD:    doAction(e); break;
		default: break;
		}
	}

	/* format a dev_cntl action result onto the status line */
	static void actionStatus(char* s, int sn, int r, int st) {
		if (r != 0)
			snprintf(s, sn, "no driver");
		else if (st == 0)
			snprintf(s, sn, "OK");
		else
			snprintf(s, sn, "err %d", st);
	}

	/*
	 * Struct-payload actions: build the daemon's fixed-width command struct
	 * and send it with the matching dev_cntl selector (see the arch/ev3
	 * device headers). The reply is a status int.
	 */
	void doStructAction(const MenuEntry& e, char* s, int sn) {
		proto_t in, out;
		int r = -1;
		const char* dev = screenDev(cur());
		switch (cur()) {
		case S_LED: {
			led_state_t ls;
			memset(&ls, 0, sizeof(ls));
			switch (e.cmd) {
			case LEDP_GREEN:  ls.left_green = 1; ls.right_green = 1; break;
			case LEDP_RED:    ls.left_red = 1; ls.right_red = 1; break;
			case LEDP_ORANGE: ls.left_green = 1; ls.left_red = 1;
			                  ls.right_green = 1; ls.right_red = 1; break;
			case LEDP_BLINK:  ls.left_green = 1; ls.right_green = 1;
			                  ls.blink_ms = 500; break;
			case LEDP_PULSE:  ls.pattern = LED_PATTERN_GREEN_PULSE; break;
			case LEDP_CYCLE:  ls.pattern = LED_PATTERN_CYCLE; break;
			default: break;   /* LEDP_OFF: all fields zero */
			}
			PF->init(&in); PF->add(&in, &ls, sizeof(ls)); PF->init(&out);
			r = dev_cntl(DEV_LED, LED_CNTL_SET, &in, &out);
		} break;
		case S_BEEP: {
			beep_cmd_t c;
			memset(&c, 0, sizeof(c));
			c.cmd = e.cmd; c.freq_hz = e.a0; c.duration_ms = e.a1;
			c.volume = 0; c.melody = e.a2;
			PF->init(&in); PF->add(&in, &c, sizeof(c)); PF->init(&out);
			r = dev_cntl(DEV_BEEP, BEEP_CNTL_PLAY, &in, &out);
		} break;
		case S_BATTERY: {
			battery_cmd_t c;
			memset(&c, 0, sizeof(c));
			c.cmd = BATTERY_CMD_SET_THRESH; c.value = e.a0;
			PF->init(&in); PF->add(&in, &c, sizeof(c)); PF->init(&out);
			r = dev_cntl(DEV_BATTERY, BATTERY_CNTL_SET_THRESH, &in, &out);
		} break;
		case S_MOTOR: {
			motor_cmd_t c;
			memset(&c, 0, sizeof(c));
			c.cmd = e.cmd; c.port = motorPort;
			c.arg0 = e.a0; c.arg1 = e.a1; c.arg2 = e.a2;
			PF->init(&in); PF->add(&in, &c, sizeof(c)); PF->init(&out);
			r = dev_cntl(DEV_MOTOR, MOTOR_CNTL_COMMAND, &in, &out);
		} break;
		case S_US: case S_GYRO: case S_COLOR: case S_IR: case S_NXTUS: {
			ev3_sensor_cmd_t c;
			memset(&c, 0, sizeof(c));
			c.cmd = e.cmd; c.arg0 = e.a0; c.arg1 = e.a1; c.arg2 = e.a2;
			PF->init(&in); PF->add(&in, &c, sizeof(c)); PF->init(&out);
			r = dev_cntl(dev, EV3_SENSOR_CNTL_COMMAND, &in, &out);
		} break;
		case S_I2C: {
			i2c_bus_info_t bi;
			memset(&bi, 0, sizeof(bi));
			bi.hz = e.a0;
			PF->init(&in); PF->add(&in, &bi, sizeof(bi)); PF->init(&out);
			r = dev_cntl(DEV_I2C, I2C_CNTL_SPEED, &in, &out);
		} break;
		default:
			snprintf(s, sn, "n/a");
			return;
		}
		PF->clear(&in);
		int status = (r == 0) ? proto_read_int(&out) : -1;
		PF->clear(&out);
		actionStatus(s, sn, r, status);
	}

	void doAction(const MenuEntry& e) {
		const char* dev = screenDev(cur());
		if (dev == NULL)
			return;
		char s[48];
		if (e.op == OP_SCNTL) {
			doStructAction(e, s, sizeof(s));
		} else { /* OP_CMD */
			char* ret = dev_cmd(dev, e.text);
			if (ret == NULL)
				snprintf(s, sizeof(s), "no driver");
			else {
				/* first line of the reply, newline stripped */
				char* nl = strchr(ret, '\n');
				if (nl) *nl = '\0';
				snprintf(s, sizeof(s), "%.40s", ret);
				free(ret);
			}
		}
		setStatus(s);
	}

	/* ---- Run-all batch probe ---- */
	void addResult(const char* name, int present, int ok) {
		if (dynCount >= MAX_RESULTS - 2)
			return;
		char tmp[RLABEL];
		snprintf(tmp, sizeof(tmp), "%s", name);
		int n = (int)strlen(tmp);
		while (n < 9 && n < RLABEL - 4)
			tmp[n++] = ' ';
		tmp[n] = '\0';
		const char* st = !present ? "NA" : (ok ? "OK" : "ERR");
		snprintf(resultLabels[dynCount], RLABEL, "%s%s", tmp, st);

		MenuEntry& m = dynEntries[dynCount];
		m.label = resultLabels[dynCount];
		m.op = OP_INFO;
		m.cmd = 0;
		m.a0 = m.a1 = m.a2 = 0;
		m.text = NULL;
		dynCount++;
	}

	/* raw: read() the node instead of dev_cntl */
	void probeRaw(const char* name, const char* dev) {
		int pid = dev_get_pid(dev);
		if (pid <= 0) {
			addResult(name, 0, 0);
			return;
		}
		int ok = 0;
		int fd = ::open(dev, O_RDONLY | O_NONBLOCK);
		if (fd >= 0) {
			char buf[64];
			int n = read(fd, buf, sizeof(buf));
			::close(fd);
			ok = (n >= 0);
		}
		addResult(name, 1, ok);
	}

	/* probe a struct-returning GET: the first reply item is the status int */
	void probeGet(const char* name, const char* dev, int selector) {
		int pid = dev_get_pid(dev);
		if (pid <= 0) {
			addResult(name, 0, 0);
			return;
		}
		addResult(name, 1, cntlGetStruct(dev, selector, NULL, 0) ? 1 : 0);
	}

	void runAll() {
		dynCount = 0;
		probeGet("LED",     DEV_LED,     LED_CNTL_GET);
		probeGet("Beep",    DEV_BEEP,    BEEP_CNTL_GET);
		probeGet("Battery", DEV_BATTERY, BATTERY_CNTL_GET);
		probeGet("Motor",   DEV_MOTOR,   MOTOR_CNTL_GET_ALL);
		probeGet("Touch",   DEV_TOUCH,   EV3_SENSOR_CNTL_GET_DATA);
		probeGet("US",      DEV_US,      EV3_SENSOR_CNTL_GET_DATA);
		probeGet("Gyro",    DEV_GYRO,    EV3_SENSOR_CNTL_GET_DATA);
		probeGet("Color",   DEV_COLOR,   EV3_SENSOR_CNTL_GET_DATA);
		probeGet("IR",      DEV_IR,      EV3_SENSOR_CNTL_GET_DATA);
		probeGet("NXT-US",  DEV_NXTUS,   EV3_SENSOR_CNTL_GET_DATA);
		probeGet("I2C",     DEV_I2C,     I2C_CNTL_INFO);
		probeRaw("ADC",     DEV_ADC);
		probeRaw("Joy",     DEV_JOY);

		/* trailing commands: re-run + back */
		MenuEntry& rr = dynEntries[dynCount];
		rr.label = "Re-run"; rr.op = OP_RUNALL; rr.cmd = 0;
		rr.a0 = rr.a1 = rr.a2 = 0; rr.text = NULL;
		dynCount++;
		MenuEntry& bb = dynEntries[dynCount];
		bb.label = ".. back"; bb.op = OP_BACK; bb.cmd = 0;
		bb.a0 = bb.a1 = bb.a2 = 0; bb.text = NULL;
		dynCount++;

		push(S_RESULTS);
	}

	/* ---- live polling of the current test screen ---- */
	void poll() {
		int s = cur();
		char b[64];
		switch (s) {
		case S_LED: {
			led_state_t ls;
			memset(&ls, 0, sizeof(ls));
			proto_t out; PF->init(&out);
			int r = dev_cntl(DEV_LED, LED_CNTL_GET, NULL, &out);
			int ok = (r == 0 && proto_read_int(&out) == 0);
			if (ok)
				proto_read_to(&out, &ls, sizeof(ls));
			PF->clear(&out);
			if (!ok) {
				setLive(0, "led n/a");
			} else {
				snprintf(b, sizeof(b), "LG%d LR%d RG%d RR%d p%d",
						ls.left_green, ls.left_red, ls.right_green,
						ls.right_red, ls.pattern);
				setLive(0, b);
			}
		} break;
		case S_BEEP: {
			beep_state_t bs;
			memset(&bs, 0, sizeof(bs));
			proto_t out; PF->init(&out);
			int r = dev_cntl(DEV_BEEP, BEEP_CNTL_GET, NULL, &out);
			int ok = (r == 0 && proto_read_int(&out) == 0);
			if (ok)
				proto_read_to(&out, &bs, sizeof(bs));
			PF->clear(&out);
			if (ok)
				snprintf(b, sizeof(b), "play:%d %dHz rem:%d",
						bs.playing, bs.freq_hz, bs.remain_ms);
			else
				snprintf(b, sizeof(b), "beep n/a");
			setLive(0, b);
		} break;
		case S_BATTERY: {
			battery_info_t bi;
			memset(&bi, 0, sizeof(bi));
			proto_t out; PF->init(&out);
			int r = dev_cntl(DEV_BATTERY, BATTERY_CNTL_GET, NULL, &out);
			int ok = (r == 0 && proto_read_int(&out) == 0);
			if (ok)
				proto_read_to(&out, &bi, sizeof(bi));
			PF->clear(&out);
			if (ok) {
				snprintf(b, sizeof(b), "%d mV  %d mA", bi.voltage_mv, bi.current_ma);
				setLive(0, b);
				snprintf(b, sizeof(b), "%d%% low:%d rech:%d",
						bi.percent, bi.low, bi.rechargeable);
				setLive(1, b);
			} else {
				setLive(0, "battery n/a");
				setLive(1, "");
			}
		} break;
		case S_MOTOR: {
			motor_cmd_t c;
			memset(&c, 0, sizeof(c));
			c.port = motorPort;
			motor_info_t mi;
			memset(&mi, 0, sizeof(mi));
			proto_t in, out;
			PF->init(&in); PF->add(&in, &c, sizeof(c)); PF->init(&out);
			int r = dev_cntl(DEV_MOTOR, MOTOR_CNTL_GET_INFO, &in, &out);
			PF->clear(&in);
			if (r == 0 && proto_read_int(&out) == 0) {
				proto_read_to(&out, &mi, sizeof(mi));
				snprintf(b, sizeof(b), "pres:%d run:%d duty:%d",
						mi.present, mi.running, mi.duty);
				setLive(0, b);
				snprintf(b, sizeof(b), "pos:%d spd:%d", mi.position, mi.speed);
				setLive(1, b);
				snprintf(b, sizeof(b), "mode:%d tgt:%d stall:%d",
						mi.mode, mi.target, mi.stalled);
				setLive(2, b);
			} else {
				setLive(0, "motor n/a");
				setLive(1, "");
				setLive(2, "");
			}
			PF->clear(&out);
		} break;
		case S_TOUCH: {
			ev3_sensor_data_t d;
			memset(&d, 0, sizeof(d));
			proto_t out; PF->init(&out);
			int r = dev_cntl(DEV_TOUCH, EV3_SENSOR_CNTL_GET_DATA, NULL, &out);
			int ok = (r == 0 && proto_read_int(&out) == 0);
			if (ok)
				proto_read_to(&out, &d, sizeof(d));
			PF->clear(&out);
			if (ok)
				snprintf(b, sizeof(b), "%s  %d mV",
						d.value[0] ? "PRESS" : "release", d.raw_mv);
			else
				snprintf(b, sizeof(b), "touch n/a");
			setLive(0, b);
		} break;
		case S_US: {
			ev3_sensor_data_t d;
			if (sensorGet(DEV_US, &d)) {
				snprintf(b, sizeof(b), "%d %s conn:%d", d.value[0],
						d.mode == US_MODE_DIST_IN ? "0.1in" : "mm", d.connected);
				setLive(0, b);
				snprintf(b, sizeof(b), "type:%d mode:%d err:%d", d.type, d.mode, d.errors);
				setLive(1, b);
			} else {
				setLive(0, "us n/a");
				setLive(1, "(not started)");
			}
		} break;
		case S_GYRO: {
			ev3_sensor_data_t d;
			if (sensorGet(DEV_GYRO, &d)) {
				if (d.mode == GYRO_MODE_GA)
					snprintf(b, sizeof(b), "angle:%d rate:%d", d.value[0], d.value[1]);
				else if (d.mode == GYRO_MODE_ANG)
					snprintf(b, sizeof(b), "angle:%d", d.value[0]);
				else
					snprintf(b, sizeof(b), "rate:%d", d.value[0]);
				setLive(0, b);
				snprintf(b, sizeof(b), "mode:%d conn:%d err:%d", d.mode, d.connected, d.errors);
				setLive(1, b);
			} else {
				setLive(0, "gyro n/a");
				setLive(1, "(not started)");
			}
		} break;
		case S_COLOR: {
			ev3_sensor_data_t d;
			if (sensorGet(DEV_COLOR, &d)) {
				if (d.mode == COLOR_MODE_RGB_RAW)
					snprintf(b, sizeof(b), "rgb %d,%d,%d", d.value[0], d.value[1], d.value[2]);
				else
					snprintf(b, sizeof(b), "val:%d", d.value[0]);
				setLive(0, b);
				snprintf(b, sizeof(b), "mode:%d conn:%d err:%d", d.mode, d.connected, d.errors);
				setLive(1, b);
			} else {
				setLive(0, "color n/a");
				setLive(1, "(not started)");
			}
		} break;
		case S_IR: {
			ev3_sensor_data_t d;
			if (sensorGet(DEV_IR, &d)) {
				if (d.mode == IR_MODE_REMOTE)
					snprintf(b, sizeof(b), "rem %d %d %d %d",
							d.value[0], d.value[1], d.value[2], d.value[3]);
				else if (d.mode == IR_MODE_SEEK)
					snprintf(b, sizeof(b), "seek h:%d d:%d", d.value[0], d.value[1]);
				else
					snprintf(b, sizeof(b), "prox:%d", d.value[0]);
				setLive(0, b);
				snprintf(b, sizeof(b), "mode:%d conn:%d err:%d", d.mode, d.connected, d.errors);
				setLive(1, b);
			} else {
				setLive(0, "ir n/a");
				setLive(1, "(not started)");
			}
		} break;
		case S_NXTUS: {
			ev3_sensor_data_t d;
			if (sensorGet(DEV_NXTUS, &d))
				snprintf(b, sizeof(b), "conn:%d cm:%d mode:%d",
						d.connected, d.value[0], d.mode);
			else
				snprintf(b, sizeof(b), "nxtus n/a");
			setLive(0, b);
		} break;
		case S_I2C: {
			i2c_bus_info_t bi;
			memset(&bi, 0, sizeof(bi));
			if (cntlGetStruct(DEV_I2C, I2C_CNTL_INFO, &bi, sizeof(bi)))
				snprintf(b, sizeof(b), "in%d %dHz x:%d nak:%d",
						bi.port + 1, bi.hz, bi.xfers, bi.nacks);
			else
				snprintf(b, sizeof(b), "i2c n/a");
			setLive(0, b);
		} break;
		case S_ADC: {
			if (rawFd < 0) {
				setLive(0, "adc n/a");
				break;
			}
			uint16_t ch[16];
			int n = read(rawFd, ch, sizeof(ch));
			if (n < (int)sizeof(ch)) {
				setLive(0, "adc no data");
				break;
			}
			for (int row = 0; row < 4; row++) {
				int base = row * 4;
				snprintf(b, sizeof(b), "%d:%4d%4d%4d%4d", base,
						ch[base], ch[base + 1], ch[base + 2], ch[base + 3]);
				setLive(row, b);
			}
		} break;
		case S_JOY: {
			if (rawFd < 0) {
				setLive(0, "joy n/a");
				break;
			}
			uint8_t keys[8];
			int n = read(rawFd, keys, sizeof(keys));
			if (n <= 0) {
				setLive(0, "keys: -");
				break;
			}
			char line[28];
			int off = snprintf(line, sizeof(line), "keys:");
			for (int i = 0; i < n && off < (int)sizeof(line) - 5; i++) {
				const char* nm = "?";
				switch (keys[i]) {
				case KEY_UP:    nm = "UP"; break;
				case KEY_DOWN:  nm = "DN"; break;
				case KEY_LEFT:  nm = "LT"; break;
				case KEY_RIGHT: nm = "RT"; break;
				case KEY_ENTER: nm = "OK"; break;
				case KEY_ESC:  nm = "ESC"; break;
				default: break;
				}
				off += snprintf(line + off, sizeof(line) - off, " %s", nm);
			}
			setLive(0, line);
		} break;
		default:
			break;
		}
	}

	/* ---- brick-button (IM key) navigation ---- */
	void onEvent(xevent_t* ev) {
		if (ev->type == XEVT_IM && ev->state == XIM_STATE_PRESS) {
			int k = ev->value.im.key_code;
			if (k == KEY_UP)        { moveSel(-1); return; }
			else if (k == KEY_DOWN) { moveSel(1);  return; }
			else if (k == KEY_ENTER || k == KEY_RIGHT) { activateSel(); return; }
			else if (k == KEY_LEFT || k == KEY_ESC) { requestBack(); return; }
		}
		WidgetWin::onEvent(ev);
	}

	void onTimer(uint32_t timerFPS, uint32_t timerSteps) {
		(void)timerFPS;
		(void)timerSteps;
		if (pendingOp != 0) {
			int op = pendingOp, sel = pendingSel;
			pendingOp = 0;
			if (op == 1)
				handleSelect(sel);
			else if (op == 2)
				pop();
			return;   /* render() already polled the new screen */
		}
		/* every tick: the daemons cache the latest sample, polling is a
		 * cheap dev_cntl and keeps the live panel in step with the sensor */
		poll();
	}
};

/* NavList needs the complete Ev3TestWin type for the deferred activate. */
void NavList::onSelect(int sel) {
	if (guard || win == NULL)
		return;
	win->requestActivate(sel);
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	X x;
	Ev3TestWin win;

	/* row heights follow the theme font so big fonts are not clipped */
	x_theme_t th;
	if (x.getTheme(&th) == 0)
		win.setFontSize((int)th.fontSize);

	RootWidget* root = new RootWidget();
	win.setRoot(root);
	root->setType(Container::VERTICAL);
	root->setAlpha(false);

	win.render();
	win.open(&x, 0, 0, 0, 178, 128, "ev3test",
			XWIN_STYLE_NO_FRAME | XWIN_STYLE_NO_TITLE);
	win.setTimer(30);

	widgetXRun(&x, &win);
	return 0;
}
