/*
 * motord - EV3 output-port (motor) driver.
 *
 * Drives the four output ports A/B/C/D. Each port combines:
 *   - one PWM channel for speed magnitude (see arch/ev3/pwm.c),
 *   - two direction GPIOs forming the H-bridge (see arch/ev3/port.c),
 *   - a tacho INT/DIR pair for position feedback, decoded from GPIO edge
 *     interrupts the way ev3dev legoev3_motor.c does it (on every INT
 *     edge: INT != DIR -> forward, INT == DIR -> backward),
 *   - the pin5 ADC line for motor detection (2400..2600 mV == nothing
 *     plugged in, ev3_ports_out.c PIN5_BALANCE_LOW/HIGH).
 *
 * Control layers:
 *   - open-loop duty (MOTOR_CMD_SET_DUTY / MOTOR_CMD_RUN);
 *   - closed-loop speed (MOTOR_CMD_RUN_AT_SPEED): PID on deg/s -> duty;
 *   - closed-loop position (MOTOR_CMD_RUN_TO_POS / MOTOR_CMD_HOLD):
 *     cascade - an outer P loop turns the position error into a speed
 *     setpoint (limited by the requested speed), fed into the speed PID.
 *
 * The control loop runs from loop_step at 200 Hz. Gains are x1000
 * fixed-point and can be re-tuned at runtime via MOTOR_CMD_SET_PID.
 *
 * All payloads are fixed-width structs from arch/ev3/motor.h; dev.cmd
 * keeps a text interface for the shell only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/proto.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <ewoksys/mmio.h>
#include <ewoksys/interrupt.h>
#include <ewoksys/kernel_tic.h>

#include <arch/ev3/gpio.h>
#include <arch/ev3/pwm.h>
#include <arch/ev3/port.h>
#include <arch/ev3/motor.h>

#define ADC_DEV        "/dev/adc0"
#define ADC_MAX_CH     16
#define DETECT_MS      200      /* motor presence poll period           */
#define PIN5_BALANCE_LOW   2400 /* mV, ev3_ports_out.c                  */
#define PIN5_BALANCE_HIGH  2600

/* Fixed-point scale for PID gains (kp/ki/kd are stored as x1000). */
#define PID_SCALE     1000
/* Control loop period in milliseconds; keep in sync with loop usleep. */
#define PID_DT_MS     5
#define PID_DT_US     (PID_DT_MS * 1000)
/* Speed is measured over a window of SPEED_WIN samples (40 ms). */
#define SPEED_WIN     8
/* Integral clamp so a stalled motor doesn't wind up forever (x1000 duty). */
#define PID_I_MAX     60000
#define PID_DUTY_MAX  100
#define SPEED_MAX     1200      /* deg/s, above any EV3 motor no-load speed */
/* Stall: driven at >= this duty with no movement for STALL_MS. */
#define STALL_DUTY    20
#define STALL_MS      300

/* Default gains, x1000. */
#define DEFAULT_KP_SPEED   120   /* 0.12 duty per deg/s error */
#define DEFAULT_KI_SPEED   600   /* per second                */
#define DEFAULT_KD_SPEED     5
#define DEFAULT_KP_POS    8000   /* 8 deg/s per count error   */

typedef struct {
    int32_t tacho_pin;
    int32_t dir_pin;
    volatile int32_t position;   /* accumulated tacho counts, IRQ owned  */
    volatile int32_t last_level; /* INT level seen at last edge          */

    int32_t present;
    int32_t duty;                /* signed -100..100 currently applied   */
    int32_t running;             /* 1 while PWM is enabled               */
    int32_t stop_action;         /* MOTOR_STOP_COAST / MOTOR_STOP_BRAKE  */
    int32_t stalled;

    int32_t mode;                /* MOTOR_MODE_*                         */
    int32_t target_speed;        /* deg/s: SPEED setpoint / POS limit    */
    int32_t target_pos;          /* counts for POS/HOLD                  */
    int32_t meas_speed;          /* filtered, deg/s                      */
    int32_t kp, ki, kd;          /* speed loop, x1000                    */
    int32_t kp_pos;              /* position -> speed, x1000             */
    int32_t integ;               /* speed integral (deg/s * ms)          */
    int32_t prev_err;

    int32_t hist[SPEED_WIN];     /* position history for speed           */
    int32_t hist_idx;
    int32_t stall_pos;
    uint64_t stall_ms;
} motor_state_t;

static motor_state_t _motor[MOTOR_PORT_COUNT];
static uint32_t _period = EV3_PWM_DEFAULT_PERIOD;
static int _adc_fd = -1;
static uint64_t _detect_ms = 0;

/* ---------------- tacho interrupt ---------------- */

/*
 * One handler per GPIO bank interrupt. Several ports share a bank (A/B/C
 * are all GPIO 5[x]); the ack returns the whole 32-pin group status so we
 * fan it out to every port whose INT pin fired.
 */
static interrupt_handler_t _tacho_handlers[MOTOR_PORT_COUNT];
static uint32_t            _tacho_irqs[MOTOR_PORT_COUNT];
static int                 _tacho_irq_count = 0;

static void tacho_irq(uint32_t irq, ewokos_addr_t data) {
    (void)irq;
    int32_t ref = (int32_t)data;
    uint32_t st = ev3_gpio_irq_ack(ref);
    if (st == 0)
        return;

    for (int i = 0; i < MOTOR_PORT_COUNT; i++) {
        motor_state_t* m = &_motor[i];
        if (m->tacho_pin < 0 || (m->tacho_pin / 32) != (ref / 32))
            continue;
        if ((st & (1u << (m->tacho_pin % 32))) == 0)
            continue;

        int dir = 0;
        int level = ev3_output_port_tacho(i, &dir);
        m->last_level = level;
        if (level != dir)
            m->position++;
        else
            m->position--;
    }
}

static void tacho_irq_setup(void) {
    for (int i = 0; i < MOTOR_PORT_COUNT; i++) {
        int32_t pin = _motor[i].tacho_pin;
        if (pin < 0)
            continue;
        uint32_t irq = ev3_gpio_irq_num(pin);

        int known = 0;
        for (int k = 0; k < _tacho_irq_count; k++) {
            if (_tacho_irqs[k] == irq) {
                known = 1;
                break;
            }
        }
        if (!known) {
            int k = _tacho_irq_count++;
            _tacho_irqs[k] = irq;
            _tacho_handlers[k].handler = tacho_irq;
            _tacho_handlers[k].data = (ewokos_addr_t)pin;
            sys_interrupt_setup(irq, &_tacho_handlers[k]);
        }
        ev3_gpio_irq_enable(pin, 1, 1);
    }
}

/* ---------------- helpers ---------------- */

static int parse_port(const char* s) {
    if (!s || !*s)
        return -1;
    char c = *s;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'D')
        return c - 'A';
    if (c >= '0' && c <= '3')
        return c - '0';
    return -1;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void set_stop_action(int port, int32_t action) {
    if (action == MOTOR_STOP_BRAKE || action == MOTOR_STOP_COAST)
        _motor[port].stop_action = action;
}

static void apply_duty(int port, int32_t duty) {
    motor_state_t* m = &_motor[port];
    duty = clamp(duty, -PID_DUTY_MAX, PID_DUTY_MAX);
    m->duty = duty;

    int32_t mag = duty < 0 ? -duty : duty;
    if (mag == 0) {
        ev3_pwm_enable(port, 0);
        ev3_output_port_bridge(port,
                m->stop_action == MOTOR_STOP_BRAKE ?
                EV3_MOTOR_BRAKE : EV3_MOTOR_COAST);
        m->running = 0;
        return;
    }

    ev3_output_port_bridge(port, duty > 0 ? EV3_MOTOR_FWD : EV3_MOTOR_REV);
    ev3_pwm_set_percent(port, mag);
    ev3_pwm_enable(port, 1);
    m->running = 1;
}

static void reset_pid(int port) {
    motor_state_t* m = &_motor[port];
    m->integ = 0;
    m->prev_err = 0;
    m->stalled = 0;
    m->stall_pos = m->position;
    m->stall_ms = kernel_tic_ms(0);
}

static void motor_stop(int port, int32_t stop_action) {
    motor_state_t* m = &_motor[port];
    set_stop_action(port, stop_action);
    m->mode = MOTOR_MODE_IDLE;
    apply_duty(port, 0);
    reset_pid(port);
}

static void motor_run_duty(int port, int32_t duty, int32_t stop_action) {
    set_stop_action(port, stop_action);
    _motor[port].mode = MOTOR_MODE_DUTY;
    reset_pid(port);
    apply_duty(port, duty);
}

static void motor_run_speed(int port, int32_t speed, int32_t stop_action) {
    motor_state_t* m = &_motor[port];
    set_stop_action(port, stop_action);
    m->target_speed = clamp(speed, -SPEED_MAX, SPEED_MAX);
    m->mode = MOTOR_MODE_SPEED;
    reset_pid(port);
}

static void motor_run_to_pos(int port, int32_t target, int32_t speed_limit,
        int32_t stop_action) {
    motor_state_t* m = &_motor[port];
    set_stop_action(port, stop_action);
    m->target_pos = target;
    m->target_speed = clamp(speed_limit <= 0 ? 300 : speed_limit, 1, SPEED_MAX);
    m->mode = MOTOR_MODE_POS;
    reset_pid(port);
}

static void motor_hold(int port, int32_t stop_action) {
    motor_state_t* m = &_motor[port];
    set_stop_action(port, stop_action);
    m->target_pos = m->position;
    m->target_speed = SPEED_MAX;
    m->mode = MOTOR_MODE_HOLD;
    reset_pid(port);
}

static void motor_set_position(int port, int32_t pos) {
    motor_state_t* m = &_motor[port];
    m->position = pos;
    for (int i = 0; i < SPEED_WIN; i++)
        m->hist[i] = pos;
    if (m->mode == MOTOR_MODE_POS || m->mode == MOTOR_MODE_HOLD)
        m->target_pos = pos;
    reset_pid(port);
}

static void motor_set_pid(int port, int32_t kp, int32_t ki, int32_t kd) {
    motor_state_t* m = &_motor[port];
    m->kp = clamp(kp, 0, 100000);
    m->ki = clamp(ki, 0, 100000);
    m->kd = clamp(kd, 0, 100000);
    m->integ = 0;
}

static void fill_info(int port, motor_info_t* info) {
    motor_state_t* m = &_motor[port];
    info->present  = m->present;
    info->running  = m->running;
    info->duty     = m->duty;
    info->position = m->position;
    info->speed    = m->meas_speed;
    info->mode     = m->mode;
    info->stalled  = m->stalled;
    if (m->mode == MOTOR_MODE_SPEED)
        info->target = m->target_speed;
    else if (m->mode == MOTOR_MODE_POS || m->mode == MOTOR_MODE_HOLD)
        info->target = m->target_pos;
    else
        info->target = 0;
}

/* Dispatch one motor_cmd_t; returns 0 or -1. */
static int32_t do_command(const motor_cmd_t* c) {
    int port = c->port;
    if (port < 0 || port >= MOTOR_PORT_COUNT)
        return -1;

    switch (c->cmd) {
    case MOTOR_CMD_RUN:
        motor_run_duty(port, c->arg0, c->arg1);
        break;
    case MOTOR_CMD_SET_DUTY:
        motor_run_duty(port, c->arg0, -1);
        break;
    case MOTOR_CMD_STOP:
        motor_stop(port, c->arg0);
        break;
    case MOTOR_CMD_SET_POSITION:
        motor_set_position(port, c->arg0);
        break;
    case MOTOR_CMD_RUN_AT_SPEED:
        motor_run_speed(port, c->arg0, c->arg1);
        break;
    case MOTOR_CMD_RUN_TO_POS:
        motor_run_to_pos(port, c->arg0, c->arg1, c->arg2);
        break;
    case MOTOR_CMD_HOLD:
        motor_hold(port, c->arg0);
        break;
    case MOTOR_CMD_SET_PID:
        motor_set_pid(port, c->arg0, c->arg1, c->arg2);
        break;
    default:
        return -1;
    }
    return 0;
}

/* ---------------- vdevice callbacks ---------------- */

static int motor_dev_cntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;

    motor_cmd_t c;
    memset(&c, 0, sizeof(c));

    switch (cmd) {
    case MOTOR_CNTL_COMMAND: {
        proto_read_to(in, &c, sizeof(c));
        int32_t res = do_command(&c);
        PF->clear(ret)->addi(ret, res);
        return res;
    }
    case MOTOR_CNTL_GET_INFO: {
        proto_read_to(in, &c, sizeof(c));
        if (c.port < 0 || c.port >= MOTOR_PORT_COUNT) {
            PF->clear(ret)->addi(ret, -1);
            return -1;
        }
        motor_info_t info;
        fill_info(c.port, &info);
        PF->clear(ret)->addi(ret, 0)->add(ret, &info, sizeof(info));
        return 0;
    }
    case MOTOR_CNTL_GET_ALL: {
        motor_info_t all[MOTOR_PORT_COUNT];
        for (int i = 0; i < MOTOR_PORT_COUNT; i++)
            fill_info(i, &all[i]);
        PF->clear(ret)->addi(ret, 0)->add(ret, all, sizeof(all));
        return 0;
    }
    default:
        break;
    }
    PF->clear(ret)->addi(ret, -1);
    return -1;
}

static int motor_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (buf == NULL || size < (int)sizeof(motor_cmd_t))
        return -1;

    /* accept a batch of commands in one write */
    int n = size / (int)sizeof(motor_cmd_t);
    const motor_cmd_t* c = (const motor_cmd_t*)buf;
    for (int i = 0; i < n; i++) {
        if (do_command(&c[i]) != 0)
            return -1;
    }
    return n * (int)sizeof(motor_cmd_t);
}

static int motor_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)p; (void)offset;

    motor_info_t all[MOTOR_PORT_COUNT];
    for (int i = 0; i < MOTOR_PORT_COUNT; i++)
        fill_info(i, &all[i]);

    if (size > (int)sizeof(all))
        size = sizeof(all);
    memcpy(buf, all, size);
    return size;
}

/* ---------------- text command interface (shell debugging) ---------------- */

static char* motor_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev; (void)from_pid; (void)p;
    char* buf = (char*)malloc(512);
    if (!buf) return NULL;

    const char* usage =
        "usage: run|duty|stop|speed|pos|hold|goto|pid|info|reset <A-D> [args]\n";

    if (argc <= 0 || argv[0] == NULL) {
        snprintf(buf, 512, "%s", usage);
        return buf;
    }

    const char* op = argv[0];
    int port = (argc > 1) ? parse_port(argv[1]) : -1;
    motor_state_t* m = (port >= 0) ? &_motor[port] : NULL;

    if (strcmp(op, "info") == 0) {
        int len = 0;
        for (int i = 0; i < MOTOR_PORT_COUNT; i++) {
            if (port >= 0 && port != i) continue;
            motor_info_t info;
            fill_info(i, &info);
            len += snprintf(buf+len, 512-len,
                    "%c: present=%d run=%d duty=%d pos=%d spd=%d mode=%d tgt=%d stall=%d\n",
                    'A'+i, info.present, info.running, info.duty, info.position,
                    info.speed, info.mode, info.target, info.stalled);
        }
        return buf;
    }

    if (m == NULL) {
        snprintf(buf, 512, "%s", usage);
        return buf;
    }

    if (strcmp(op, "run") == 0 || strcmp(op, "duty") == 0) {
        int32_t duty = (argc > 2) ? atoi(argv[2]) : 50;
        motor_run_duty(port, duty, -1);
        snprintf(buf, 512, "port %c duty %d\n", 'A'+port, m->duty);
    } else if (strcmp(op, "stop") == 0) {
        int32_t action = (argc > 2 && strcmp(argv[2], "brake") == 0) ?
                MOTOR_STOP_BRAKE : MOTOR_STOP_COAST;
        motor_stop(port, action);
        snprintf(buf, 512, "port %c stopped (%s)\n", 'A'+port,
                action == MOTOR_STOP_BRAKE ? "brake" : "coast");
    } else if (strcmp(op, "reset") == 0) {
        motor_set_position(port, 0);
        snprintf(buf, 512, "port %c position reset\n", 'A'+port);
    } else if (strcmp(op, "speed") == 0) {
        if (argc > 2) {
            motor_run_speed(port, atoi(argv[2]), -1);
            snprintf(buf, 512, "port %c speed target=%d deg/s\n",
                    'A'+port, m->target_speed);
        } else {
            snprintf(buf, 512, "port %c speed=%d deg/s (mode=%d)\n",
                    'A'+port, m->meas_speed, m->mode);
        }
    } else if (strcmp(op, "pos") == 0) {
        snprintf(buf, 512, "port %c position %d speed %d\n",
                'A'+port, m->position, m->meas_speed);
    } else if (strcmp(op, "hold") == 0) {
        motor_hold(port, -1);
        snprintf(buf, 512, "port %c hold at %d\n", 'A'+port, m->target_pos);
    } else if (strcmp(op, "goto") == 0 && argc > 2) {
        int32_t tgt = atoi(argv[2]);
        int32_t sp  = (argc > 3) ? atoi(argv[3]) : 300;
        motor_run_to_pos(port, tgt, sp, -1);
        snprintf(buf, 512, "port %c goto %d (limit %d deg/s)\n",
                'A'+port, tgt, m->target_speed);
    } else if (strcmp(op, "pid") == 0 && argc > 4) {
        motor_set_pid(port, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
        snprintf(buf, 512, "port %c pid kp=%d ki=%d kd=%d (x1000)\n",
                'A'+port, m->kp, m->ki, m->kd);
    } else {
        snprintf(buf, 512, "%s", usage);
    }
    return buf;
}

/* ---------------- control loop ---------------- */

static void detect_step(uint64_t now) {
    if (_adc_fd < 0 || now - _detect_ms < DETECT_MS)
        return;
    _detect_ms = now;

    uint16_t adc[ADC_MAX_CH];
    int n = read(_adc_fd, adc, sizeof(adc));
    if (n < (int)sizeof(adc))
        return;

    for (int i = 0; i < MOTOR_PORT_COUNT; i++) {
        int ch = ev3_output_port_adc_channel(i);
        if (ch < 0 || ch >= ADC_MAX_CH)
            continue;
        int mv = adc[ch];
        int present = !(mv >= PIN5_BALANCE_LOW && mv <= PIN5_BALANCE_HIGH);
        if (_motor[i].present && !present && _motor[i].mode != MOTOR_MODE_IDLE)
            motor_stop(i, MOTOR_STOP_COAST);   /* unplugged while running */
        _motor[i].present = present;
    }
}

/*
 * Speed PID: setpoint deg/s -> duty percent. Called every PID_DT_MS.
 */
static int32_t speed_pid(motor_state_t* m, int32_t setpoint) {
    int32_t err = setpoint - m->meas_speed;

    m->integ += err * PID_DT_MS;
    m->integ = clamp(m->integ, -PID_I_MAX, PID_I_MAX);
    int32_t deriv = (err - m->prev_err) * 1000 / PID_DT_MS;
    m->prev_err = err;

    int32_t p_term = (m->kp * err) / PID_SCALE;
    int32_t i_term = (m->ki * (m->integ / 1000)) / PID_SCALE;
    int32_t d_term = (m->kd * (deriv / 1000)) / PID_SCALE;
    return clamp(p_term + i_term + d_term, -PID_DUTY_MAX, PID_DUTY_MAX);
}

static void control_step(int port, uint64_t now) {
    motor_state_t* m = &_motor[port];

    /* Speed over the last SPEED_WIN samples, smoothed with a 1-pole IIR. */
    int32_t pos = m->position;
    int32_t old = m->hist[m->hist_idx];
    m->hist[m->hist_idx] = pos;
    m->hist_idx = (m->hist_idx + 1) % SPEED_WIN;
    int32_t inst = (pos - old) * 1000 / (PID_DT_MS * SPEED_WIN);
    m->meas_speed = (m->meas_speed * 3 + inst) / 4;

    /* Stall detection while power is applied. */
    if (m->running && (m->duty >= STALL_DUTY || m->duty <= -STALL_DUTY)) {
        if (pos != m->stall_pos) {
            m->stall_pos = pos;
            m->stall_ms = now;
            m->stalled = 0;
        } else if (now - m->stall_ms >= STALL_MS) {
            m->stalled = 1;
        }
    } else {
        m->stall_pos = pos;
        m->stall_ms = now;
        m->stalled = 0;
    }

    if (m->mode == MOTOR_MODE_IDLE || m->mode == MOTOR_MODE_DUTY)
        return;

    int32_t speed_sp = 0;
    if (m->mode == MOTOR_MODE_SPEED) {
        speed_sp = m->target_speed;
    } else {
        /* Cascade: position error -> speed setpoint, limited. */
        int32_t perr = m->target_pos - pos;
        speed_sp = (m->kp_pos * perr) / PID_SCALE;
        speed_sp = clamp(speed_sp, -m->target_speed, m->target_speed);

        if (m->mode == MOTOR_MODE_POS && perr > -2 && perr < 2 &&
            m->meas_speed > -30 && m->meas_speed < 30) {
            /* Arrived: keep regulating around the target. */
            m->mode = MOTOR_MODE_HOLD;
            m->target_speed = SPEED_MAX;
        }
        if (m->mode == MOTOR_MODE_HOLD && perr == 0 &&
            m->meas_speed == 0 && m->integ == 0) {
            apply_duty(port, 0);          /* rest against brake/coast */
            return;
        }
    }

    apply_duty(port, speed_pid(m, speed_sp));
}

static int motor_loop(vdevice_t* dev, void* p) {
    (void)dev; (void)p;

    uint64_t now = kernel_tic_ms(0);

    ipc_disable();
    detect_step(now);
    for (int i = 0; i < MOTOR_PORT_COUNT; i++)
        control_step(i, now);
    ipc_enable();

    usleep(PID_DT_US);
    return 0;
}

/* ---------------- main ---------------- */

static int doargs(int argc, char* argv[]) {
    int c;
    while ((c = getopt(argc, argv, "p:")) != -1) {
        if (c == 'p')
            _period = (uint32_t)atoi(optarg);
    }
    return optind;
}

int main(int argc, char** argv) {
    _period = EV3_PWM_DEFAULT_PERIOD;
    int argind = doargs(argc, argv);
    const char* mnt_point = "/dev/motor";
    if (argind < argc)
        mnt_point = argv[argind];

    ev3_gpio_init();
    ev3_pwm_init();

    memset(_motor, 0, sizeof(_motor));
    for (int i = 0; i < MOTOR_PORT_COUNT; i++) {
        motor_state_t* m = &_motor[i];
        int tp = -1, dp = -1;
        ev3_output_port_init(i);
        ev3_output_port_tacho_pins(i, &tp, &dp);
        m->tacho_pin = tp;
        m->dir_pin = dp;
        m->kp = DEFAULT_KP_SPEED;
        m->ki = DEFAULT_KI_SPEED;
        m->kd = DEFAULT_KD_SPEED;
        m->kp_pos = DEFAULT_KP_POS;
        m->stop_action = MOTOR_STOP_COAST;
        ev3_pwm_set_period(i, _period);
        motor_stop(i, MOTOR_STOP_COAST);
    }

    tacho_irq_setup();

    _adc_fd = open(ADC_DEV, O_RDONLY | O_NONBLOCK);
    _detect_ms = 0;

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "motord");
    dev.dev_cntl = motor_dev_cntl;
    dev.cmd = motor_cmd;
    dev.read = motor_read;
    dev.write = motor_write;
    dev.loop_step = motor_loop;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

    if (_adc_fd >= 0)
        close(_adc_fd);
    return 0;
}
