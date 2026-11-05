/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The frames of twixt,i2c-mm-gpio on a fake bus: the driver's own code, with a port (see
 * i2c_mm_gpio_port.h) whose time moves one tick per call. A target at the other end ACKs and
 * answers, and the bus writes down what went over it: S START, R repeated START, P STOP, A10w an
 * address, 01 a byte written, a0+ a0- a byte read with ACK or NACK.
 *
 * A competing reader can override the final NACK. Pinctrl models releasing SDA at handoff;
 * electrical timing between real masters still needs hardware tests.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static unsigned int fake_irq_lock(void);
static void fake_irq_unlock(unsigned int key);

#undef irq_lock
#undef irq_unlock
#define irq_lock()    fake_irq_lock()
#define irq_unlock(k) fake_irq_unlock(k)

static int target_irq_on = -1;

#undef irq_enable
#undef irq_disable
#define irq_enable(n)  (target_irq_on = 1)
#define irq_disable(n) (target_irq_on = 0)

#include "../../../drivers/i2c/i2c_mm_gpio.c"

/* No instance in the devicetree here: only the frames run */
static int (*const unused_init)(const struct device *dev) __unused = mm_gpio_init;

#define SCL         0
#define SDA         1
#define TARGET_ADDR 0x10
#define FIRST_READ  0xa0

static bool target_ready;
static bool stop_before_target;
static int restore_ret;
static int unregister_ret;
static uint32_t registrations;

static struct i2c_target_config *block_registered;

static int handoff_target_register(const struct device *dev, struct i2c_target_config *tc)
{
	registrations++;
	target_ready = restore_ret == 0;
	block_registered = restore_ret == 0 ? tc : NULL;
	return restore_ret;
}

static int handoff_target_unregister(const struct device *dev, struct i2c_target_config *tc)
{
	return tc == block_registered ? unregister_ret : -EINVAL;
}

static DEVICE_API(i2c, handoff_api) = {
	.target_register = handoff_target_register,
	.target_unregister = handoff_target_unregister,
};

DEVICE_DEFINE(handoff_i2c, "handoff_i2c", NULL, NULL, NULL, NULL, POST_KERNEL,
	      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &handoff_api);

int pinctrl_lookup_state(const struct pinctrl_dev_config *config, uint8_t id,
			 const struct pinctrl_state **state)
{
	static const struct pinctrl_state default_state = {.id = PINCTRL_STATE_DEFAULT};

	*state = &default_state;
	return 0;
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	/* The RP2040 pinctrl switches SDA to input before selecting its function. */
	stop_before_target = !target_ready;
	fake_pin_set(SDA, 1);
	return 0;
}

static const struct mm_gpio_config cfg = {
	.scl = {.pin = SCL},
	.sda = {.pin = SDA},
	.bus_free_us = 30,
	.bus_timeout_us = 1000,
	.stretch_timeout_us = 10000,
};

static struct mm_gpio_data data = {.cycles_per_us = 1, .t_low = 6, .t_high = 4, .t_high_min = 5};

/* The clock of the fake bus and what happens at a time on it */
static uint32_t ticks;
static int locks;
static uint32_t preempt_at;
static uint32_t preempt_len;
static int64_t other_master_from; /* clocks 1 bits from this tick on, -1: no other master */
static bool sda_stuck;
static uint32_t scl_stuck_from;     /* a target holds SCL low from this tick on, 0: never */
static uint32_t scl_stuck_until;    /* and lets it go at this tick, 0: never */
static uint32_t stretch_read_ticks; /* the target holds SCL this long after a read's address */
static int master_pulled_scl_in_stretch;
static int seen_scl = 1; /* the line as the fake target last saw it */
static bool other_reader_ack;
static bool other_reader_driving;

/* The lines as this master and the target drive them */
static int m_scl, m_sda, t_sda;
static bool is_read, is_addr;
static int clocks;
static uint8_t byte, next_read;
static char trace[256];

static unsigned int fake_irq_lock(void)
{
	locks++;
	return 0;
}

static void fake_irq_unlock(unsigned int key)
{
	ARG_UNUSED(key);
	locks--;
}

static int line_scl(void);
static void target_scl_rose(void);
static void target_scl_fell(void);
static bool in_frame;

static void tick(void)
{
	ticks++;
	/* The line goes high without a pin write when a stretch ends */
	if (in_frame && seen_scl != line_scl()) {
		seen_scl = line_scl();
		if (seen_scl) {
			target_scl_rose();
		} else {
			target_scl_fell();
		}
	}
	if (locks == 0 && preempt_len != 0 && ticks >= preempt_at) {
		ticks += preempt_len;
		preempt_len = 0;
	}
}

uint32_t fake_now(void)
{
	tick();
	return ticks;
}

static int line_scl(void)
{
	bool other_low = other_master_from >= 0 && ticks >= other_master_from &&
			 (ticks - other_master_from) % 10 < 5;

	bool stretched = scl_stuck_from != 0 && ticks >= scl_stuck_from &&
			 (scl_stuck_until == 0 || ticks < scl_stuck_until);

	return m_scl && !other_low && !stretched;
}

static int line_sda(void)
{
	return m_sda && t_sda && !sda_stuck && !other_reader_driving;
}

int fake_pin_get(int pin)
{
	int v = pin == SCL ? line_scl() : line_sda();

	tick();
	return v;
}

static void note(const char *text)
{
	strncat(trace, text, sizeof(trace) - strlen(trace) - 1);
	strncat(trace, " ", sizeof(trace) - strlen(trace) - 1);
}

static void target_scl_rose(void)
{
	char text[8];

	clocks++;
	if (clocks <= 8) {
		byte = (byte << 1) | line_sda();
		return;
	}
	if (is_read && !is_addr) {
		snprintf(text, sizeof(text), "%02x%c", next_read++, line_sda() ? '-' : '+');
		note(text);
	}
}

static void target_scl_fell(void)
{
	char text[8];

	if (clocks == 8) {
		if (is_addr) {
			is_read = byte & 1;
			snprintf(text, sizeof(text), "A%02x%c", byte >> 1, is_read ? 'r' : 'w');
			note(text);
		} else if (!is_read) {
			snprintf(text, sizeof(text), "%02x", byte);
			note(text);
		}
		if (is_addr && (byte >> 1) != TARGET_ADDR) {
			t_sda = 1; /* not for it */
		} else if (is_addr || !is_read) {
			t_sda = 0; /* ACK */
		} else {
			t_sda = 1; /* the master's ACK or NACK */
			other_reader_driving = other_reader_ack;
		}
		return;
	}
	if (clocks == 9) {
		other_reader_driving = false;
		if (is_addr && is_read && stretch_read_ticks != 0) {
			scl_stuck_from = ticks;
			scl_stuck_until = ticks + stretch_read_ticks;
		}
		clocks = 0;
		byte = 0;
		is_addr = false;
	}
	t_sda = is_read && !is_addr ? (next_read >> (7 - clocks)) & 1 : 1;
}

void fake_pin_set(int pin, int v)
{
	int scl = line_scl();
	int sda = line_sda();

	if (pin == SCL) {
		if (!v && m_scl && scl_stuck_from != 0 && ticks >= scl_stuck_from &&
		    (scl_stuck_until == 0 || ticks < scl_stuck_until)) {
			master_pulled_scl_in_stretch++;
		}
		m_scl = v;
	} else {
		m_sda = v;
	}
	if (scl && sda && !line_sda()) {
		note(in_frame ? "R" : "S");
		in_frame = true;
		is_addr = true;
		is_read = false;
		clocks = 0;
		byte = 0;
		t_sda = 1;
	} else if (scl && !sda && line_sda()) {
		note("P");
		in_frame = false;
	} else if (in_frame && !scl && line_scl()) {
		target_scl_rose();
	} else if (in_frame && scl && !line_scl()) {
		target_scl_fell();
	}
	seen_scl = line_scl();
	tick();
}

static void bus_reset(void *fixture)
{
	ARG_UNUSED(fixture);
	ticks = 1000;
	locks = 0;
	preempt_len = 0;
	other_master_from = -1;
	sda_stuck = false;
	scl_stuck_from = 0;
	scl_stuck_until = 0;
	stretch_read_ticks = 0;
	master_pulled_scl_in_stretch = 0;
	seen_scl = 1;
	other_reader_ack = false;
	other_reader_driving = false;
	target_ready = false;
	stop_before_target = false;
	restore_ret = 0;
	unregister_ret = 0;
	registrations = 0;
	m_scl = m_sda = t_sda = 1;
	in_frame = false;
	next_read = FIRST_READ;
	trace[0] = '\0';
}

static void run(struct i2c_msg *msgs, uint8_t num)
{
	zassert_ok(msgs_run(&data, &cfg, msgs, num, TARGET_ADDR));
	release(&cfg); /* pins_give(): ends the STOP */
	zassert_equal(locks, 0);
}

ZTEST(twixt_gpio_bus, test_write_stop_write)
{
	uint8_t a = 1, b = 2;
	struct i2c_msg msgs[] = {
		{.buf = &a, .len = 1, .flags = I2C_MSG_WRITE | I2C_MSG_STOP},
		{.buf = &b, .len = 1, .flags = I2C_MSG_WRITE | I2C_MSG_STOP},
	};

	run(msgs, ARRAY_SIZE(msgs));
	zassert_str_equal(trace, "S A10w 01 P S A10w 02 P ");
}

/* After a STOP the START is fresh, whatever the flag says */
ZTEST(twixt_gpio_bus, test_write_stop_write_restart)
{
	uint8_t a = 1, b = 2;
	struct i2c_msg msgs[] = {
		{.buf = &a, .len = 1, .flags = I2C_MSG_WRITE | I2C_MSG_STOP},
		{.buf = &b, .len = 1, .flags = I2C_MSG_WRITE | I2C_MSG_RESTART | I2C_MSG_STOP},
	};

	run(msgs, ARRAY_SIZE(msgs));
	zassert_str_equal(trace, "S A10w 01 P S A10w 02 P ");
}

ZTEST(twixt_gpio_bus, test_write_restart_read)
{
	uint8_t a = 1, in;
	struct i2c_msg msgs[] = {
		{.buf = &a, .len = 1, .flags = I2C_MSG_WRITE},
		{.buf = &in, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP},
	};

	run(msgs, ARRAY_SIZE(msgs));
	zassert_str_equal(trace, "S A10w 01 R A10r a0- P ");
	zassert_equal(in, FIRST_READ);
}

ZTEST(twixt_gpio_bus, test_read_restart_write)
{
	uint8_t in, b = 2;
	struct i2c_msg msgs[] = {
		{.buf = &in, .len = 1, .flags = I2C_MSG_READ},
		{.buf = &b, .len = 1, .flags = I2C_MSG_WRITE | I2C_MSG_RESTART | I2C_MSG_STOP},
	};

	run(msgs, ARRAY_SIZE(msgs));
	zassert_str_equal(trace, "S A10r a0- R A10w 02 P ");
}

ZTEST(twixt_gpio_bus, test_read_restart_read)
{
	uint8_t a, b;
	struct i2c_msg msgs[] = {
		{.buf = &a, .len = 1, .flags = I2C_MSG_READ},
		{.buf = &b, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP},
	};

	run(msgs, ARRAY_SIZE(msgs));
	zassert_str_equal(trace, "S A10r a0- R A10r a1- P ");
}

ZTEST(twixt_gpio_bus, test_read_continues)
{
	uint8_t a, b;
	struct i2c_msg msgs[] = {
		{.buf = &a, .len = 1, .flags = I2C_MSG_READ},
		{.buf = &b, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_STOP},
	};

	run(msgs, ARRAY_SIZE(msgs));
	zassert_str_equal(trace, "S A10r a0+ a1- P ");
	zassert_equal(a, FIRST_READ);
	zassert_equal(b, FIRST_READ + 1);
}

ZTEST(twixt_gpio_bus, test_start_idle_bus)
{
	zassert_ok(bus_free(&data, &cfg));
	zassert_ok(start(&data, &cfg));
	zassert_str_equal(trace, "S ");
}

/* A frame that bus_free() cannot see: it began while this master was elsewhere */
ZTEST(twixt_gpio_bus, test_start_after_preemption)
{
	for (uint32_t off = 0; off < 40; off++) {
		bus_reset(NULL);
		other_master_from = 1020;
		preempt_at = 1010 + off / 4;
		preempt_len = 100 + off;

		int ret = bus_free(&data, &cfg);

		if (ret == 0) {
			ret = start(&data, &cfg);
		}
		zassert_equal(ret, -EBUSY, "START in another master's frame, offset %u", off);
		zassert_str_equal(trace, "");
	}
}

/* A frame that begins after bus_free(), during pins_take() */
ZTEST(twixt_gpio_bus, test_start_after_pins_take)
{
	for (uint32_t off = 0; off < 40; off++) {
		bus_reset(NULL);
		other_master_from = 1045;
		zassert_ok(bus_free(&data, &cfg));
		ticks += 20 + off;
		zassert_equal(start(&data, &cfg), -EBUSY, "START in another frame, offset %u", off);
		zassert_str_equal(trace, "");
	}
}

ZTEST(twixt_gpio_bus, test_bus_busy)
{
	other_master_from = 0;
	zassert_equal(bus_free(&data, &cfg), -EBUSY);
}

ZTEST(twixt_gpio_bus, test_stretch_timeout)
{
	uint8_t a = 1;
	struct i2c_msg msg = {.buf = &a, .len = 1, .flags = I2C_MSG_WRITE | I2C_MSG_STOP};

	scl_stuck_from = ticks + 100;
	zassert_equal(msgs_run(&data, &cfg, &msg, 1, TARGET_ADDR), -ETIMEDOUT);
	zassert_equal(locks, 0);
}

/*
 * A long stretch before the read. If this master pulled SCL low to let interrupts run, a target
 * letting go just before that would see a clock this master does not count: its read is one bit
 * off, 00 ff reads as 01. The line is the target's until it lets go.
 */
ZTEST(twixt_gpio_bus, test_stretch_no_clock_of_its_own)
{
	uint8_t in;
	struct i2c_msg msg = {.buf = &in, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_STOP};

	stretch_read_ticks = 1500;
	zassert_ok(msgs_run(&data, &cfg, &msg, 1, TARGET_ADDR));
	release(&cfg);
	zassert_equal(master_pulled_scl_in_stretch, 0, "SCL pulled low during the stretch");
	zassert_equal(in, FIRST_READ);
}

ZTEST(twixt_gpio_bus, test_address_nack)
{
	uint8_t a = 1;
	struct i2c_msg msg = {.buf = &a, .len = 1, .flags = I2C_MSG_WRITE | I2C_MSG_STOP};

	zassert_equal(msgs_run(&data, &cfg, &msg, 1, TARGET_ADDR + 1), -ENXIO);
	zassert_equal(locks, 0);
}

ZTEST(twixt_gpio_bus, test_bus_stuck)
{
	sda_stuck = true;
	zassert_equal(bus_free(&data, &cfg), -EIO);
}

ZTEST_SUITE(twixt_gpio_bus, NULL, NULL, bus_reset, NULL, NULL);

ZTEST(twixt_gpio_bus, test_read_nack_loses_arbitration)
{
	uint8_t val;
	struct i2c_msg msg = {.buf = &val, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_STOP};

	other_reader_ack = true;
	zassert_equal(msgs_run(&data, &cfg, &msg, 1, TARGET_ADDR), -EAGAIN);
	release(&cfg);
	zassert_is_null(strchr(trace, 'P'));
	zassert_equal(m_scl, 1);
	zassert_equal(m_sda, 1);
	zassert_equal(locks, 0);
}

ZTEST(twixt_gpio_bus, test_target_ready_before_stop)
{
	static const struct pinctrl_dev_config pcfg;
	struct i2c_target_config tc = {0};
	struct mm_gpio_config handoff = cfg;
	struct mm_gpio_data d = data;

	handoff.target = DEVICE_GET(handoff_i2c);
	handoff.pcfg = &pcfg;
	d.target_cfg = &tc;
	zassert_ok(start(&d, &handoff));
	stop_begin(&d, &handoff);
	zassert_str_equal(trace, "S ");
	pins_give(&d, &handoff);
	zassert_true(target_ready);
	zassert_false(stop_before_target, "pinctrl released SDA before target registration");
	zassert_str_equal(trace, "S P ");
	zassert_equal(target_irq_on, 1, "the interrupt must be back with the target");
}

/* The block is not in target mode if it refuses: its interrupt must stay off, or it may never end
 */
ZTEST(twixt_gpio_bus, test_target_irq_stays_off_without_target)
{
	static const struct pinctrl_dev_config pcfg;
	struct i2c_target_config tc = {0};
	struct mm_gpio_config handoff = cfg;
	struct mm_gpio_data d = data;

	handoff.target = DEVICE_GET(handoff_i2c);
	handoff.pcfg = &pcfg;
	d.target_cfg = &tc;
	target_irq_on = 0;
	restore_ret = -EIO;
	pins_give(&d, &handoff);
	zassert_equal(target_irq_on, 0);
}

ZTEST(twixt_gpio_bus, test_busy_target_handoff_keeps_active_target)
{
	struct i2c_target_config tc = {0};
	struct mm_gpio_config handoff = cfg;
	struct mm_gpio_data d = data;
	struct device dev = {.config = &handoff, .data = &d};
	uint8_t byte = 0;
	struct i2c_msg msg = {.buf = &byte, .len = 1, .flags = I2C_MSG_STOP};

	handoff.target = DEVICE_GET(handoff_i2c);
	d.target_cfg = &tc;
	block_registered = &tc;
	target_ready = true;
	target_irq_on = 1;
	unregister_ret = -EBUSY;
	k_mutex_init(&d.lock);
	zassert_equal(mm_gpio_transfer(&dev, &msg, 1, TARGET_ADDR), -EBUSY);
	zassert_equal(mm_gpio_recover_bus(&dev), -EBUSY);
	zassert_equal(registrations, 0, "a refused handoff must not reset the target");
	zassert_equal(target_irq_on, 1);
	zassert_equal(block_registered, &tc);
	zassert_str_equal(trace, "");
}

/* However long this master is preempted, a frame in the meantime begins the idle time anew */
ZTEST(twixt_gpio_bus, test_bus_free_after_preemption)
{
	for (uint32_t off = 0; off < 40; off++) {
		bus_reset(NULL);
		other_master_from = 1020;
		preempt_at = 1010 + off / 4;
		preempt_len = 100 + off;
		zassert_equal(bus_free(&data, &cfg), -EBUSY, "free in a frame, offset %u", off);
	}
}

/* A short interrupt on an idle bus only delays: a poll gap below t_low keeps the idle time */
ZTEST(twixt_gpio_bus, test_bus_free_idle_short_gap)
{
	preempt_at = 1010;
	preempt_len = 3;
	zassert_ok(bus_free(&data, &cfg));
}

static int user_buf_writes, user_stops, user_bytes, user_reads;

static void user_buf_write_received(struct i2c_target_config *tc, uint8_t *buf, uint32_t len)
{
	user_buf_writes++;
}

static int user_buf_read_requested(struct i2c_target_config *tc, uint8_t **buf, uint32_t *len)
{
	user_reads++;
	return 0;
}

static int user_stop(struct i2c_target_config *tc)
{
	user_stops++;
	return 0;
}

static int user_write_received(struct i2c_target_config *tc, uint8_t val)
{
	user_bytes += val;
	return 0;
}

static const struct i2c_target_callbacks user_callbacks = {
	.buf_write_received = user_buf_write_received,
	.buf_read_requested = user_buf_read_requested,
	.write_received = user_write_received,
	.stop = user_stop,
};

ZTEST(twixt_gpio_bus, test_target_write_end)
{
	struct mm_gpio_config handoff = cfg;
	struct mm_gpio_data d = data;
	struct i2c_target_config user = {.address = 0x10, .callbacks = &user_callbacks};
	struct device dev = {.config = &handoff, .data = &d};
	const struct i2c_target_callbacks *cb;
	uint8_t byte_in = 0;
	uint8_t *buf;
	uint32_t len;

	user_buf_writes = user_stops = user_bytes = user_reads = 0;
	handoff.target = DEVICE_GET(handoff_i2c);
	k_mutex_init(&d.lock);
	zassert_ok(mm_gpio_target_register(&dev, &user));
	zassert_not_equal(block_registered, &user, "the block must call this driver");
	zassert_equal(block_registered->address, user.address);
	cb = block_registered->callbacks;

	/* After a STOP both lines stay high: the request is abandoned */
	m_scl = 1;
	cb->buf_write_received(block_registered, &byte_in, 1);
	zassert_equal(user_buf_writes, 1);
	zassert_equal(user_stops, 1);

	/* Before a read after a repeated START the block holds SCL low: the request stays */
	m_scl = 0;
	cb->buf_write_received(block_registered, &byte_in, 1);
	zassert_equal(user_buf_writes, 2);
	zassert_equal(user_stops, 1);

	/* The last clock of the read's address may still be high when the write ends */
	m_scl = 1;
	other_master_from = ticks + 4;
	cb->buf_write_received(block_registered, &byte_in, 1);
	zassert_equal(user_buf_writes, 3);
	zassert_equal(user_stops, 1);
	other_master_from = -1;

	zassert_ok(cb->buf_read_requested(block_registered, &buf, &len));
	zassert_equal(user_reads, 1);
	zassert_ok(cb->write_received(block_registered, 7));
	zassert_equal(user_bytes, 7);
	zassert_ok(cb->stop(block_registered));
	zassert_equal(user_stops, 2);

	zassert_equal(mm_gpio_target_register(&dev, &(struct i2c_target_config){0}), -EBUSY);
	zassert_equal(mm_gpio_target_unregister(&dev, &(struct i2c_target_config){0}), -EINVAL);
	target_irq_on = 1;
	zassert_ok(mm_gpio_target_unregister(&dev, &user));
	zassert_equal(target_irq_on, 0, "no target, no interrupt");
	zassert_is_null(d.target_cfg);
	zassert_is_null(d.user_cfg);
}
