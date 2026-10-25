/*
 * The Galaxy S20+'s battery: the MAX77705 (dt: i2c@980000, serial engine 0 of QUP wrapper 0, max77705@66),
 * whose fuel gauge answers at 0x36 and whose charger at 0x69. Its UEFI has no charger driver of its own
 * (ChargerExDxe is the null one), so xnuloader's battery=1 has nothing to share here; the chip is read
 * directly instead, over its two pins driven by hand (GPIO28 SDA, GPIO29 SCL, the TLMM's north tile,
 * pulled up on the board), as the touch screen's bus is (redwood_sec_ts.c). Reads only: nothing of the
 * charger's configuration is written.
 *
 * Every five seconds: the fuel gauge's state of charge (RepSOC), cell voltage, current, temperature and
 * capacities (ModelGauge m5 registers, as Samsung's max77705_fuelgauge.c reads them, with its 10 mOhm
 * sense resistor), and the charger's input and charging details; all of it to redwood_battery_report(),
 * which iOS's power source and the status bar are fed from.
 *
 * Boot-arg max77705=1 on the y2q (AUTOBOOT_ARGS has it); max77705_log=1: the raw registers of every read.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/redwood_input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define TLMM_NORTH_PHYS         0x0f900000UL
#define TLMM_SIZE               0x100000UL
#define PIN_SDA                 28
#define PIN_SCL                 29
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_FUNCTION            (0xfu << 2)
#define CFG_OUTPUT_ENABLE       (1u << 9)
#define HALF_BIT_US             5               /* 100 kHz */
#define STRETCH_US              1000

#define FG_ADDRESS              0x36
#define FG_REPCAP               0x05            /* 0.5 mAh a step (5 uVh over 10 mOhm) */
#define FG_REPSOC               0x06            /* 1/256 % */
#define FG_TEMP                 0x08            /* 1/256 degree, signed */
#define FG_VCELL                0x09            /* 78.125 uV */
#define FG_CURRENT              0x0a            /* 156.25 uA, signed, more than 0 while charging */
#define FG_FULLCAPREP           0x10
#define FG_DESIGNCAP            0x18
#define CHG_ADDRESS             0x69
#define CHG_DETAILS_00          0xb3            /* [6:5] the USB input: 3 = valid */
#define CHG_DETAILS_01          0xb4            /* [3:0] 0 precharge, 1 fast (CC), 2 fast (CV), 3 top-off, 4 done */
#define POLL_MS                 5000

static vm_offset_t tlmm;
static uint32_t cfg_sda, cfg_scl;               /* the pins as inputs, function 0 */
static uint32_t max_log;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* (as in redwood_sec_ts.c: delay() sleeps, and a bit of 5 us became a millisecond) */
static void
spin_us(uint32_t us)
{
	uint64_t interval, deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &interval);
	deadline = mach_absolute_time() + interval;
	while (mach_absolute_time() < deadline) {
	}
}

/* Open drain: low is the pin an output at 0, high is the pin let go (the board pulls it up). */
static void
line(unsigned pin, uint32_t cfg, bool high)
{
	if (high) {
		wr(GPIO_CFG(pin), cfg & ~CFG_OUTPUT_ENABLE);
	} else {
		wr(GPIO_IO(pin), 0);
		wr(GPIO_CFG(pin), cfg | CFG_OUTPUT_ENABLE);
	}
	spin_us(HALF_BIT_US);
}

static bool
level(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void sda(bool high) { line(PIN_SDA, cfg_sda, high); }
static void scl_low(void) { line(PIN_SCL, cfg_scl, false); }

static bool
scl_release(void)
{
	line(PIN_SCL, cfg_scl, true);
	for (unsigned waited = 0; !level(PIN_SCL); waited++) {
		if (waited >= STRETCH_US) {
			return false;
		}
		spin_us(1);
	}
	return true;
}

static void
bus_start(void)
{
	sda(true);
	scl_release();
	sda(false);
	scl_low();
}

static void
bus_stop(void)
{
	sda(false);
	scl_release();
	sda(true);
}

/* A device left holding SDA low in the middle of a byte: clock it out, then a stop (Linux's recovery). */
static bool
bus_recover(void)
{
	unsigned pulses = 0;

	if (level(PIN_SDA) && level(PIN_SCL)) {
		return true;
	}
	if (!level(PIN_SCL)) {
		return false;
	}
	while (!level(PIN_SDA) && pulses < 9) {
		scl_low();
		scl_release();
		pulses++;
	}
	bus_stop();
	return level(PIN_SDA) && level(PIN_SCL);
}

static bool
bus_put(uint8_t byte)
{
	bool ack;

	for (int bit = 7; bit >= 0; bit--) {
		sda((byte >> bit) & 1);
		if (!scl_release()) {
			return false;
		}
		scl_low();
	}
	sda(true);
	if (!scl_release()) {
		return false;
	}
	ack = !level(PIN_SDA);
	scl_low();
	return ack;
}

static uint8_t
bus_get(bool last)
{
	uint8_t byte = 0;

	sda(true);
	for (int bit = 7; bit >= 0; bit--) {
		scl_release();
		byte |= (uint8_t)(level(PIN_SDA) << bit);
		scl_low();
	}
	sda(last);
	scl_release();
	scl_low();
	sda(true);
	return byte;
}

/* Read `count` bytes from register `reg` on of the device at `address`; NULL, or what went wrong. */
static const char *
max_read(uint8_t address, uint8_t reg, uint8_t *bytes, unsigned count)
{
	const char *problem = NULL;

	if (!bus_recover()) {
		return "the bus is held";
	}
	bus_start();
	if (!bus_put(address << 1)) {
		problem = "no acknowledge of its address";
	} else if (!bus_put(reg)) {
		problem = "no acknowledge of the register number";
	} else {
		bus_start();
		if (!bus_put(address << 1 | 1)) {
			problem = "no acknowledge of its address for the read";
		} else {
			for (unsigned i = 0; i < count; i++) {
				bytes[i] = bus_get(i + 1 == count);
			}
		}
	}
	bus_stop();
	return problem;
}

/* A sixteen-bit register of the fuel gauge (low byte first), or false. */
static bool
fg_word(uint8_t reg, uint16_t *value)
{
	uint8_t b[2];
	const char *problem = max_read(FG_ADDRESS, reg, b, 2);

	if (problem != NULL) {
		if (max_log) {
			printf("max77705: fuel gauge register %#x: %s\n", reg, problem);
		}
		return false;
	}
	*value = (uint16_t)(b[0] | b[1] << 8);
	return true;
}

static uint64_t reads, failures;

static void
max_poll(void)
{
	struct redwood_battery_state b = { .size = sizeof(b) };
	uint16_t soc, vcell, current, temp, repcap, fullcap, designcap;
	uint8_t details[2] = { 0, 0 };
	bool charger_read, input, full;
	unsigned charging;

	reads++;
	if (!fg_word(FG_REPSOC, &soc) || !fg_word(FG_VCELL, &vcell) || !fg_word(FG_CURRENT, &current)) {
		failures++;
		return;
	}
	charger_read = max_read(CHG_ADDRESS, CHG_DETAILS_00, details, 2) == NULL;
	b.percent_hundredths = (int32_t)((uint32_t)soc * 100 / 256);
	b.voltage_uv = (int32_t)((uint32_t)vcell * 625 / 8);
	b.current_ua = (int32_t)(int16_t)current * 15625 / 100;
	b.temperature_hundredths = 2500;
	if (fg_word(FG_TEMP, &temp)) {
		int32_t t = (int32_t)(int16_t)temp * 100 / 256;

		if (t > -2000 && t < 8000) {    /* (a gauge without its thermistor says nonsense: 25 then) */
			b.temperature_hundredths = t;
		}
	}
	if (fg_word(FG_REPCAP, &repcap)) {
		b.charge_uah = (int32_t)repcap * 500;
	}
	if (fg_word(FG_FULLCAPREP, &fullcap)) {
		b.full_uah = (int32_t)fullcap * 500;
	}
	if (fg_word(FG_DESIGNCAP, &designcap)) {
		b.design_uah = (int32_t)designcap * 500;
	}
	b.present = vcell != 0;
	input = charger_read && ((details[0] >> 5) & 3) == 3;
	charging = details[1] & 0xf;
	full = charger_read && charging == 4;
	b.external = input;
	b.status = !input ? 2 : full ? 4 : charging <= 3 ? 1 : 3;
	b.usb_type = input ? 2 : 0;
	if (max_log) {
		printf("max77705: soc %#x vcell %#x current %#x temp %#x; charger %s details %#x %#x\n", soc, vcell, current,
		    temp, charger_read ? "read" : "NOT read", details[0], details[1]);
	}
	redwood_battery_report(&b);
}

static void
max_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned tick = 0;; tick++) {
		max_poll();
		if (tick % 12 == 0 && failures != 0) {
			printf("max77705: %llu reads, %llu failed\n", reads, failures);
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
}

void
redwood_max77705_init(void)
{
	uint32_t enabled = 0;
	uint16_t soc = 0, vcell = 0;
	uint8_t details[2] = { 0, 0 };
	thread_t thread;

	if (!redwood_platform_is("y2q") || !PE_parse_boot_argn("max77705", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("max77705_log", &max_log, sizeof(max_log));
	tlmm = ml_io_map(TLMM_NORTH_PHYS, TLMM_SIZE);
	printf("max77705: GPIO%u config %#x level %u, GPIO%u config %#x level %u (as the firmware left them)\n", PIN_SDA,
	    rd(GPIO_CFG(PIN_SDA)), rd(GPIO_IO(PIN_SDA)) & 1, PIN_SCL, rd(GPIO_CFG(PIN_SCL)), rd(GPIO_IO(PIN_SCL)) & 1);
	/* the pins from the serial engine (function qup0) to plain GPIOs, as inputs: the lines let go */
	cfg_sda = rd(GPIO_CFG(PIN_SDA)) & ~(CFG_FUNCTION | CFG_OUTPUT_ENABLE);
	cfg_scl = rd(GPIO_CFG(PIN_SCL)) & ~(CFG_FUNCTION | CFG_OUTPUT_ENABLE);
	wr(GPIO_CFG(PIN_SDA), cfg_sda);
	wr(GPIO_CFG(PIN_SCL), cfg_scl);
	spin_us(100);
	if (!fg_word(FG_REPSOC, &soc) || !fg_word(FG_VCELL, &vcell)) {
		printf("max77705: the fuel gauge (%#x) does not answer: SDA %s, SCL %s; no battery readings\n", FG_ADDRESS,
		    level(PIN_SDA) ? "high" : "low", level(PIN_SCL) ? "high" : "low");
		return;
	}
	(void)max_read(CHG_ADDRESS, CHG_DETAILS_00, details, 2);
	printf("max77705: the fuel gauge answers: %u.%02u%%, %u mV; charger details %#x %#x; polling every %u s\n",
	    soc / 256, (soc % 256) * 100 / 256, (uint32_t)vcell * 625 / 8000, details[0], details[1], POLL_MS / 1000);
	if (kernel_thread_start(max_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
