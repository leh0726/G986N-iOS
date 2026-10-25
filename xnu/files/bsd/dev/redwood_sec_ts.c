/*
 * The touch screen of the Samsung Galaxy S20+ 5G (y2q): a Samsung LSI controller of the kind
 * Samsung's kernels call "sec_ts" (firmware tsp_sec/y79a_y2.bin), at address 0x48 on the I2C bus
 * of the SoC's serial engine 5 of the first QUP wrapper, as a source of touches for
 * /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg touchscreen=1, and only on the
 * platform the loader calls "y2q": every address below belongs to this phone.
 *
 * What the chip needs, from the phone's own device tree (i2c@994000/touchscreen@48):
 *   power    its two supplies, LDO2 (1.8 V) and LDO3 (3.0 V) of the S2DOS05 display PMIC, are
 *            "boot on" for Android. Whether they still are after our UEFI is what step 2 reads:
 *            the PMIC is at 0x60 on a bus of two GPIOs (62 SDA, 63 SCL, south tile, pulled up on
 *            the board), driven by hand. The same PMIC makes the panel's supplies, so nothing is
 *            written to it but the register number a read starts with
 *   clocks   the serial engine's clock and the wrapper's four (two AHB ports, core, core 2x),
 *            by vote in the global clock controller
 *   pins     GPIO12 (SDA) and GPIO13 (SCL) to the serial engine ("qup5"), GPIO15 the interrupt
 *            line, an input (low = the chip has events); all three in the TLMM's north tile
 *   I2C      the serial engine ("GENI") in FIFO mode, polled
 *   chip     the protocol of Linux's drivers/input/touchscreen/s6sy761.c, the same family
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg
 * touch_steps=N stops after the first N:
 *   1  read how the clocks and the pins are left, and, if every clock of the serial engine is on,
 *      what the engine says it is                                        (reads only)
 *   2  read the S2DOS05: which of its outputs are on, at what voltage   (its two bus lines driven)
 *   3  switch LDO2 and LDO3 on: the two bits of its enable register, the others written as they
 *      were read; read back, then watch the touch chip's lines come up   (the first write to it)
 *   4  vote for the clocks and give the two bus pins to the serial engine (writes: GCC, TLMM)
 *   5  set the engine up for I2C and read the chip's identity            (the first transfers)
 *   6  read the panel's size, clear the chip's events and switch its sensing on
 *   7  watch the interrupt line, read the events and post the touches    (the default)
 * A finger's report: byte 0 its state (7:6, 1 press, 2 move, 3 release), its number from 1 (5:2)
 * and the kind of event (1:0, 0 = a coordinate); x is byte 1 and the high nibble of byte 3, y
 * byte 2 and the low nibble; byte 7 (5:0) how many events are still waiting.
 *
 * A read of a block whose clock is off can stop the whole SoC on this chip: the serial engine is
 * read only when the clock controller says all of its clocks run.
 *
 * Register layouts: Linux drivers/clk/qcom/gcc-sm8250.c, drivers/pinctrl/qcom/pinctrl-sm8250.c,
 * include/linux/soc/qcom/geni-se.h, drivers/i2c/busses/i2c-qcom-geni.c, and Samsung's
 * include/linux/regulator/s2dos05.h (its SM8250 kernels).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/*
 * Every line goes to the loader's log directly as well (as in redwood_buttons.c): without -v the
 * kernel's own messages reach that log only at the end of a run, and "xl log" during a run is to
 * show how far this driver has come.
 */
extern void loader_log_putc(char c);
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static vm_offset_t gcc, tlmm, tlmm_south, se;

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

/*
 * The bits of a bus driven by hand are timed by spinning on the clock. delay() may put the thread
 * to sleep until a timer wakes it, and then 5 us become most of a millisecond: an event of eight
 * bytes took a fifth of a second to read, and a tap came out as a long press.
 */
static void
spin_us(uint32_t us)
{
	uint64_t interval, deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &interval);
	deadline = mach_absolute_time() + interval;
	while (mach_absolute_time() < deadline) {
	}
}

/* ---- the global clock controller ------------------------------------------------------------------ */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_VOTE                (gcc + 0x52008)         /* the application processor's votes */
#define VOTE_QUP0_M_AHB         (1u << 6)
#define VOTE_QUP0_S_AHB         (1u << 7)
#define VOTE_QUP0_CORE          (1u << 8)
#define VOTE_QUP0_CORE_2X       (1u << 9)
#define VOTE_QUP0_SE5           (1u << 15)
#define GCC_QUP0_M_AHB_CBCR     (gcc + 0x17004)         /* bit 31: the clock is off */
#define GCC_QUP0_S_AHB_CBCR     (gcc + 0x17008)
#define GCC_QUP0_SE5_CBCR       (gcc + 0x175fc)
#define GCC_QUP0_SE5_CMD        (gcc + 0x17600)         /* the serial engine's clock source; bit 0: take the new configuration */
#define GCC_QUP0_SE5_CFG        (gcc + 0x17604)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP0_SE5_DFSR       (gcc + 0x17614)         /* bit 0: the engine chooses among preset rates itself */
#define GCC_QUP0_SE5_PERF_CFG(l) (gcc + 0x1761c + 4 * (l))
#define GCC_QUP0_SE5_PERF_M(l)  (gcc + 0x1765c + 4 * (l))
#define GCC_QUP0_SE5_PERF_N(l)  (gcc + 0x1769c + 4 * (l))
#define CLOCK_HW_CONTROL        (1u << 1)               /* in a branch: on when the hardware needs it, "off" when idle */
#define XO_HZ                   19200000
#define GCC_QUP0_CORE_CBCR      (gcc + 0x23000)
#define GCC_QUP0_CORE_2X_CBCR   (gcc + 0x23008)
#define CLOCK_OFF               (1u << 31)

/* ---- the pins ------------------------------------------------------------------------------------- */

#define TLMM_NORTH_PHYS         0x0f900000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PIN_SDA                 12
#define PIN_SCL                 13
#define PIN_INT                 15      /* low = the chip has events */
#define PIN_FUNCTION_QUP5       1

/* ---- the serial engine ---------------------------------------------------------------------------- */

#define SE_PHYS                 0x00994000UL
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 3 = I2C */
#define GENI_CLK_SEL            (se + 0x07c)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define I2C_TX_TRANS_LEN        (se + 0x26c)
#define I2C_RX_TRANS_LEN        (se + 0x270)
#define I2C_SCL_COUNTERS        (se + 0x278)    /* [29:20] high, [19:10] low, [9:0] cycle, in source clocks / divider */
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation, [15:9] address, [2] no stop after it */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)
#define GENI_PROTOCOL_I2C       3

#define I2C_OP_WRITE            1u
#define I2C_OP_READ             2u
#define I2C_STOP_STRETCH        (1u << 2)       /* keep the bus: a repeated start follows */
#define M_CMD_DONE              (1u << 0)
#define M_CMD_OVERRUN           (1u << 1)
#define M_ILLEGAL_CMD           (1u << 2)
#define M_CMD_FAILURE           (1u << 3)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_GP_NACK               (1u << 10)      /* "GP IRQ 1": nobody acknowledged */
#define M_GP_BUS_PROTO          (1u << 12)
#define M_GP_ARB_LOST           (1u << 13)
#define M_I2C_ERRORS            (M_CMD_OVERRUN | M_ILLEGAL_CMD | M_CMD_FAILURE | M_GP_NACK | M_GP_BUS_PROTO | M_GP_ARB_LOST)
/* 100 kHz from the 19.2 MHz crystal: Linux's geni_i2c_clk_map */
#define I2C_100K_DIVIDER        7
#define I2C_100K_COUNTERS       (10u << 20 | 11u << 10 | 26u)
#define SEC_TS_ADDRESS          0x48

static const char *
pull_name(uint32_t cfg)
{
	static const char *const names[] = { "none", "down", "keeper", "up" };

	return names[cfg & 3];
}

static void
say_pin(const char *what, unsigned pin)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	printf("touch: GPIO%u (%s): config %#x -- function %u, pull %s, %u mA, %s; level %s\n", pin, what, cfg,
	    (cfg >> 2) & 0xf, pull_name(cfg), 2 * (((cfg >> 6) & 7) + 1), (cfg & (1u << 9)) ? "OUTPUT" : "input",
	    (io & 1) ? "high" : "low");
}

static bool
say_clock(const char *what, vm_offset_t cbcr, uint32_t vote_bit, uint32_t votes)
{
	uint32_t value = rd(cbcr);
	bool on = !(value & CLOCK_OFF);

	printf("touch: clock %s: %s (branch %#x), %svoted for by this processor\n", what, on ? "on" : "OFF", value,
	    (votes & vote_bit) ? "" : "not ");
	return on;
}

/* Step 1: what the firmware has left, read only. */
static void
step_survey(void)
{
	uint32_t votes, cfg, param, revision;
	bool all_on = true;

	printf("touch: step 1: read the clocks of serial engine 5 (QUP wrapper 0) and the pins (reads only)\n");
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	tlmm = ml_io_map(TLMM_NORTH_PHYS, TLMM_SIZE);
	votes = rd(GCC_VOTE);
	printf("touch: clock votes of this processor %#x\n", votes);
	all_on &= say_clock("wrapper 0 master AHB", GCC_QUP0_M_AHB_CBCR, VOTE_QUP0_M_AHB, votes);
	all_on &= say_clock("wrapper 0 slave AHB", GCC_QUP0_S_AHB_CBCR, VOTE_QUP0_S_AHB, votes);
	all_on &= say_clock("wrapper 0 core", GCC_QUP0_CORE_CBCR, VOTE_QUP0_CORE, votes);
	all_on &= say_clock("wrapper 0 core 2x", GCC_QUP0_CORE_2X_CBCR, VOTE_QUP0_CORE_2X, votes);
	all_on &= say_clock("serial engine 5", GCC_QUP0_SE5_CBCR, VOTE_QUP0_SE5, votes);
	cfg = rd(GCC_QUP0_SE5_CFG);
	printf("touch: serial engine 5's clock source: command %#x, config %#x (source %u, divider %u.%u)\n",
	    rd(GCC_QUP0_SE5_CMD), cfg, (cfg >> 8) & 7, ((cfg & 0x1f) + 1) / 2, ((cfg & 0x1f) + 1) % 2 ? 5 : 0);
	say_pin("SDA", PIN_SDA);
	say_pin("SCL", PIN_SCL);
	say_pin("interrupt", PIN_INT);
	if (!all_on) {
		printf("touch: not every clock of the serial engine runs: its registers are not read\n");
		return;
	}
	se = ml_io_map(SE_PHYS, SE_SIZE);
	revision = rd(GENI_FW_REVISION_RO);
	param = rd(SE_HW_PARAM_0);
	printf("touch: serial engine 5: firmware %#x (protocol %u%s), status %#x, clock config %#x, clock select %#x, "
	    "FIFO interface %s, FIFO depth %u words\n", revision, (revision >> 8) & 0xff,
	    ((revision >> 8) & 0xff) == GENI_PROTOCOL_I2C ? ", I2C" : "", rd(GENI_STATUS), rd(GENI_SER_M_CLK_CFG),
	    rd(GENI_CLK_SEL), (rd(GENI_IF_DISABLE_RO) & 1) ? "DISABLED" : "there", (param >> 16) & 0x3f);
}

/* ---- the S2DOS05, on two GPIOs ------------------------------------------------------------------ */

#define TLMM_SOUTH_PHYS         0x0f500000UL
#define BB_CFG(n)               (bb->tlmm + 0x1000UL * (n))
#define BB_IO(n)                (bb->tlmm + 0x1000UL * (n) + 4)
#define PIN_PMIC_SDA            62
#define PIN_PMIC_SCL            63
#define CFG_OUTPUT_ENABLE       (1u << 9)
#define IO_OUT                  (1u << 1)
#define S2DOS05_ADDRESS         0x60
#define S2DOS05_REG_DEV_ID      0x00
#define S2DOS05_REG_EN          0x03    /* bit 0 LDO1, 1 LDO2, 2 LDO3, 3 LDO4, 4 buck */
#define S2DOS05_REG_LDO1_CFG    0x04    /* [6:0] voltage, [7] fast discharge */
#define S2DOS05_READ_COUNT      10      /* 0x00 to 0x09: identity, status, enables, voltages (not 0x11, which a read clears) */
#define HALF_BIT_US             5       /* 100 kHz */
#define STRETCH_US              1000    /* how long a device may hold the clock low */

/*
 * The lines are open drain, as on any I2C bus: a line is pulled low by making its pin an output
 * at 0, and let go by making it an input again -- the board's pull-up makes it high. Nothing else
 * of the pins' configuration changes.
 */
struct bitbang {
	vm_offset_t     tlmm;           /* the TLMM tile of both pins */
	unsigned        sda, scl;
	uint32_t        cfg_sda, cfg_scl;       /* their configuration, as inputs */
	uint8_t         address;
};
static struct bitbang pmic_bus = { .sda = PIN_PMIC_SDA, .scl = PIN_PMIC_SCL, .address = S2DOS05_ADDRESS };
static struct bitbang touch_bus = { .sda = PIN_SDA, .scl = PIN_SCL, .address = 0x48 };
static struct bitbang *bb = &pmic_bus;  /* the bus the functions below drive */

static void
line(unsigned pin, uint32_t cfg, bool high)
{
	if (high) {
		wr(BB_CFG(pin), cfg & ~CFG_OUTPUT_ENABLE);
	} else {
		wr(BB_IO(pin), 0);
		wr(BB_CFG(pin), cfg | CFG_OUTPUT_ENABLE);
	}
	spin_us(HALF_BIT_US);
}

static bool
level(unsigned pin)
{
	return rd(BB_IO(pin)) & 1;
}

static void sda(bool high) { line(bb->sda, bb->cfg_sda, high); }

/* Let go of the clock and wait for it to be high: a device may hold it low while it gets ready. */
static bool
scl_release(void)
{
	line(bb->scl, bb->cfg_scl, true);
	for (unsigned waited = 0; !level(bb->scl); waited++) {
		if (waited >= STRETCH_US) {
			return false;
		}
		spin_us(1);
	}
	return true;
}

static void scl_low(void) { line(bb->scl, bb->cfg_scl, false); }

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

/*
 * A bus left in the middle of a read -- a run that ended while a transfer was going on, the chip
 * still powered -- has the device holding SDA low for the bit it was sending. As Linux's
 * i2c_generic_scl_recovery(): clock it out, at most nine times, then a stop. True if the bus is
 * idle (both lines high) after that.
 */
static bool
bb_recover(const char *name)
{
	unsigned pulses = 0;

	if (level(bb->sda) && level(bb->scl)) {
		return true;
	}
	if (!level(bb->scl)) {
		printf("touch: %s: the clock line is held low: not driven\n", name);
		return false;
	}
	while (!level(bb->sda) && pulses < 9) {
		scl_low();
		scl_release();
		pulses++;
	}
	bus_stop();
	printf("touch: %s: SDA was held low; %u clock pulses and a stop: SDA %s, SCL %s now\n", name, pulses,
	    level(bb->sda) ? "high" : "low", level(bb->scl) ? "high" : "low");
	return level(bb->sda) && level(bb->scl);
}

/* One byte out; true if the device acknowledged it. */
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
	ack = !level(bb->sda);
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
		byte |= (uint8_t)(level(bb->sda) << bit);
		scl_low();
	}
	sda(last);      /* acknowledge all but the last */
	scl_release();
	scl_low();
	sda(true);
	return byte;
}

/* Read `count` registers from `reg` on: the register number written, then a repeated start to read. */
static const char *
bb_read(uint8_t reg, uint8_t *bytes, unsigned count)
{
	const char *problem = NULL;

	bus_start();
	if (!bus_put(bb->address << 1)) {
		problem = "no acknowledge of its address";
	} else if (!bus_put(reg)) {
		problem = "no acknowledge of the register number";
	} else {
		bus_start();
		if (!bus_put(bb->address << 1 | 1)) {
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

/* Write one register. */
static const char *
bb_write(uint8_t reg, uint8_t value)
{
	const char *problem = NULL;

	bus_start();
	if (!bus_put(bb->address << 1)) {
		problem = "no acknowledge of its address";
	} else if (!bus_put(reg)) {
		problem = "no acknowledge of the register number";
	} else if (!bus_put(value)) {
		problem = "no acknowledge of the value";
	}
	bus_stop();
	return problem;
}

/* A command alone: the address, one byte, stop. */
static const char *
bb_command(uint8_t command)
{
	const char *problem = NULL;

	bus_start();
	if (!bus_put(bb->address << 1)) {
		problem = "no acknowledge of its address";
	} else if (!bus_put(command)) {
		problem = "no acknowledge of the command";
	}
	bus_stop();
	return problem;
}

static const char *
pmic_read(uint8_t reg, uint8_t *bytes, unsigned count)
{
	bb = &pmic_bus;
	return bb_read(reg, bytes, count);
}

static const char *
pmic_write(uint8_t reg, uint8_t value)
{
	bb = &pmic_bus;
	return bb_write(reg, value);
}

/* LDO voltage from its configuration: LDO1 and LDO2 from 1.5 V, LDO3 and LDO4 from 2.7 V, 25 mV a step */
static unsigned
ldo_millivolts(unsigned ldo, uint8_t cfg)
{
	return (ldo <= 2 ? 1500 : 2700) + 25 * (cfg & 0x7f);
}

/* Step 2: what the display PMIC says, read only. */
static bool
step_pmic(void)
{
	uint8_t r[S2DOS05_READ_COUNT];
	const char *problem;
	uint32_t function_sda, function_scl;

	printf("touch: step 2: read the S2DOS05 (address %#x on GPIO%u/GPIO%u), registers 0x00 to 0x%02x\n", S2DOS05_ADDRESS,
	    PIN_PMIC_SDA, PIN_PMIC_SCL, S2DOS05_READ_COUNT - 1);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_SIZE);
	pmic_bus.tlmm = tlmm_south;
	bb = &pmic_bus;
	pmic_bus.cfg_sda = rd(BB_CFG(PIN_PMIC_SDA));
	pmic_bus.cfg_scl = rd(BB_CFG(PIN_PMIC_SCL));
	function_sda = (pmic_bus.cfg_sda >> 2) & 0xf;
	function_scl = (pmic_bus.cfg_scl >> 2) & 0xf;
	printf("touch: the PMIC's bus: SDA config %#x (function %u, pull %s, %s, level %s), SCL config %#x (function %u, pull %s, "
	    "%s, level %s)\n", pmic_bus.cfg_sda, function_sda, pull_name(pmic_bus.cfg_sda),
	    (pmic_bus.cfg_sda & CFG_OUTPUT_ENABLE) ? "OUTPUT" : "input", level(PIN_PMIC_SDA) ? "high" : "low", pmic_bus.cfg_scl,
	    function_scl, pull_name(pmic_bus.cfg_scl), (pmic_bus.cfg_scl & CFG_OUTPUT_ENABLE) ? "OUTPUT" : "input",
	    level(PIN_PMIC_SCL) ? "high" : "low");
	if (function_sda != 0 || function_scl != 0) {
		printf("touch: those pins are not plain GPIOs: somebody else has them, left alone\n");
		return false;
	}
	/* (as inputs, so that nothing is driven before the bus is seen idle) */
	pmic_bus.cfg_sda &= ~CFG_OUTPUT_ENABLE;
	pmic_bus.cfg_scl &= ~CFG_OUTPUT_ENABLE;
	wr(BB_CFG(PIN_PMIC_SDA), pmic_bus.cfg_sda);
	wr(BB_CFG(PIN_PMIC_SCL), pmic_bus.cfg_scl);
	delay(20);
	if (!bb_recover("the PMIC's bus")) {
		printf("touch: the bus is not idle (SDA %s, SCL %s): nothing more is driven on it\n", level(PIN_PMIC_SDA) ? "high" : "low",
		    level(PIN_PMIC_SCL) ? "high" : "low");
		return false;
	}
	problem = pmic_read(S2DOS05_REG_DEV_ID, r, S2DOS05_READ_COUNT);
	wr(BB_CFG(PIN_PMIC_SDA), pmic_bus.cfg_sda);
	wr(BB_CFG(PIN_PMIC_SCL), pmic_bus.cfg_scl);
	if (problem != NULL) {
		printf("touch: S2DOS05: %s\n", problem);
		return false;
	}
	printf("touch: S2DOS05: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x (id, topsys, status, enable, LDO1-4, buck, buck vout)\n",
	    r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9]);
	for (unsigned ldo = 1; ldo <= 4; ldo++) {
		uint8_t cfg = r[S2DOS05_REG_LDO1_CFG + ldo - 1];

		printf("touch: S2DOS05 LDO%u: %s, %u mV%s\n", ldo, (r[S2DOS05_REG_EN] & (1u << (ldo - 1))) ? "ON" : "off",
		    ldo_millivolts(ldo, cfg), ldo == 2 ? " (the touch chip's 1.8 V)" : ldo == 3 ? " (the touch chip's 3.0 V)" : "");
	}
	printf("touch: S2DOS05 buck: %s\n", (r[S2DOS05_REG_EN] & (1u << 4)) ? "ON" : "off");
	return true;
}

#define S2DOS05_EN_TOUCH        ((1u << 1) | (1u << 2))  /* LDO2 and LDO3 */

/* The touch chip's three lines, as step 1 reads them: high once its I/O supply is up. */
static void
say_touch_lines(const char *when)
{
	printf("touch: %s: SDA %s, SCL %s, interrupt %s\n", when, (rd(GPIO_IO(PIN_SDA)) & 1) ? "high" : "low",
	    (rd(GPIO_IO(PIN_SCL)) & 1) ? "high" : "low", (rd(GPIO_IO(PIN_INT)) & 1) ? "high" : "low");
}

/* Step 3: the touch chip's two supplies on, and nothing else changed. */
static bool
step_power(void)
{
	static const unsigned after_ms[] = { 1, 10, 50, 100, 300 };
	uint8_t en, now[S2DOS05_READ_COUNT];
	const char *problem;
	unsigned waited = 0;

	printf("touch: step 3: switch the S2DOS05's LDO2 and LDO3 on\n");
	problem = pmic_read(S2DOS05_REG_EN, &en, 1);
	if (problem != NULL) {
		printf("touch: S2DOS05: reading the enable register: %s\n", problem);
		return false;
	}
	if ((en & S2DOS05_EN_TOUCH) == S2DOS05_EN_TOUCH) {
		printf("touch: S2DOS05: enable register %#x: both are on already\n", en);
	} else {
		printf("touch: S2DOS05: enable register %#x; next: write %#x to it (LDO2 and LDO3 added, the rest as read)\n", en,
		    en | S2DOS05_EN_TOUCH);
		say_touch_lines("before");
		problem = pmic_write(S2DOS05_REG_EN, en | S2DOS05_EN_TOUCH);
		if (problem != NULL) {
			printf("touch: S2DOS05: writing the enable register: %s\n", problem);
			return false;
		}
	}
	/* (Samsung's driver waits 50 us for an LDO to come up) */
	delay(1000);
	problem = pmic_read(S2DOS05_REG_DEV_ID, now, S2DOS05_READ_COUNT);
	if (problem != NULL) {
		printf("touch: S2DOS05: reading back: %s\n", problem);
		return false;
	}
	printf("touch: S2DOS05 read back: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; LDO2 %s, LDO3 %s, the others %s\n",
	    now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9],
	    (now[S2DOS05_REG_EN] & (1u << 1)) ? "ON" : "off", (now[S2DOS05_REG_EN] & (1u << 2)) ? "ON" : "off",
	    (now[S2DOS05_REG_EN] & ~S2DOS05_EN_TOUCH) == (en & ~S2DOS05_EN_TOUCH) ? "as they were" : "CHANGED");
	for (unsigned i = 0; i < sizeof(after_ms) / sizeof(after_ms[0]); i++) {
		delay((after_ms[i] - waited) * 1000);
		waited = after_ms[i];
		char when[24];
		snprintf(when, sizeof(when), "%u ms after", after_ms[i]);
		say_touch_lines(when);
	}
	return (now[S2DOS05_REG_EN] & S2DOS05_EN_TOUCH) == S2DOS05_EN_TOUCH;
}

/* ---- step 4: the serial engine's clocks and pins --------------------------------------------------- */

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	/* SM8250's parents of these sources: 0 the crystal, 1 GPLL0 (600 MHz), 6 GPLL0 even (300 MHz) */
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static uint32_t se_clock_select;
static bool bitbang;            /* the touch bus driven by hand (the engine has no FIFO interface) */

/* a branch runs: not "off", or under hardware control (which reads as off while nothing asks for it) */
static bool
branch_runs(vm_offset_t cbcr)
{
	uint32_t value = rd(cbcr);

	return !(value & CLOCK_OFF) || (value & CLOCK_HW_CONTROL);
}

static bool
step_clocks_pins(void)
{
	printf("touch: step 4: vote for serial engine 5's clocks and give GPIO12/GPIO13 to it\n");
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_M_AHB | VOTE_QUP0_S_AHB | VOTE_QUP0_CORE | VOTE_QUP0_CORE_2X);
	if (rd(GCC_QUP0_SE5_DFSR) & 1) {
		/* preset rates: the I2C timings below are for the 19.2 MHz crystal, so that one */
		bool found = false;

		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP0_SE5_PERF_CFG(level)), rd(GCC_QUP0_SE5_PERF_M(level)),
			    rd(GCC_QUP0_SE5_PERF_N(level)));

			printf("touch: preset rate %u: %u Hz\n", level, hz);
			if (hz == XO_HZ && !found) {
				found = true;
				se_clock_select = level;
			}
		}
		if (!found) {
			printf("touch: no preset rate is the crystal's: the engine is left alone\n");
			return false;
		}
		printf("touch: the engine chooses its rate itself (DFS): preset %u, 19.2 MHz\n", se_clock_select);
	} else {
		wr(GCC_QUP0_SE5_CFG, (rd(GCC_QUP0_SE5_CFG) & ~0x371fu) | 1);   /* the crystal, undivided */
		wr(GCC_QUP0_SE5_CMD, rd(GCC_QUP0_SE5_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP0_SE5_CMD) & 1); i++) {
			delay(10);
		}
		if (rd(GCC_QUP0_SE5_CMD) & 1) {
			printf("touch: the engine's clock source does not take its configuration (%#x)\n", rd(GCC_QUP0_SE5_CMD));
			return false;
		}
		se_clock_select = 0;
		printf("touch: the engine's clock source set to the crystal (config %#x)\n", rd(GCC_QUP0_SE5_CFG));
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_SE5);
	for (int i = 0; !(branch_runs(GCC_QUP0_M_AHB_CBCR) && branch_runs(GCC_QUP0_S_AHB_CBCR) && branch_runs(GCC_QUP0_SE5_CBCR)); i++) {
		if (i >= 1000) {
			printf("touch: the clocks stay off: master AHB %#x, slave AHB %#x, engine %#x\n", rd(GCC_QUP0_M_AHB_CBCR),
			    rd(GCC_QUP0_S_AHB_CBCR), rd(GCC_QUP0_SE5_CBCR));
			return false;
		}
		delay(10);
	}
	printf("touch: clocks: votes %#x; master AHB %#x, slave AHB %#x, engine %#x\n", rd(GCC_VOTE), rd(GCC_QUP0_M_AHB_CBCR),
	    rd(GCC_QUP0_S_AHB_CBCR), rd(GCC_QUP0_SE5_CBCR));

	/* the device tree's qupv3_se5_tsp_i2c_active: function qup5, 2 mA, no pull (the board pulls up); the interrupt an input */
	wr(GPIO_CFG(PIN_SDA), PIN_FUNCTION_QUP5 << 2);
	wr(GPIO_CFG(PIN_SCL), PIN_FUNCTION_QUP5 << 2);
	wr(GPIO_CFG(PIN_INT), 0);
	say_pin("SDA", PIN_SDA);
	say_pin("SCL", PIN_SCL);
	say_pin("interrupt", PIN_INT);

	se = ml_io_map(SE_PHYS, SE_SIZE);
	uint32_t revision = rd(GENI_FW_REVISION_RO);
	printf("touch: serial engine 5: firmware %#x (protocol %u), FIFO interface %s, FIFO depth %u words\n", revision,
	    (revision >> 8) & 0xff, (rd(GENI_IF_DISABLE_RO) & 1) ? "DISABLED" : "there", (rd(SE_HW_PARAM_0) >> 16) & 0x3f);
	if (((revision >> 8) & 0xff) != GENI_PROTOCOL_I2C) {
		printf("touch: the engine's firmware is not I2C's: left alone\n");
		return false;
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		/*
		 * The firmware has given this engine to the GSI DMA engine alone (Linux then talks to it
		 * through GPI DMA). Without that, the bus is driven by hand on the same two pins, as the
		 * PMIC's is: the engine's clock is let go again and the pins become plain GPIOs.
		 */
		printf("touch: the engine's FIFO interface is disabled (GSI DMA only): the bus is driven by hand instead\n");
		wr(GCC_VOTE, rd(GCC_VOTE) & ~VOTE_QUP0_SE5);
		touch_bus.tlmm = tlmm;
		touch_bus.cfg_sda = 0;  /* function 0 (GPIO), no pull, 2 mA, input */
		touch_bus.cfg_scl = 0;
		wr(GPIO_CFG(PIN_SDA), touch_bus.cfg_sda);
		wr(GPIO_CFG(PIN_SCL), touch_bus.cfg_scl);
		delay(20);
		say_pin("SDA", PIN_SDA);
		say_pin("SCL", PIN_SCL);
		bb = &touch_bus;
		if (!bb_recover("the touch bus")) {
			printf("touch: the touch bus is not idle: nothing more is driven on it\n");
			return false;
		}
		bitbang = true;
	}
	return true;
}

/* ---- step 5: I2C through the serial engine, and the chip's identity ------------------------------- */

static uint32_t fifo_depth;

static void
geni_i2c_init(void)
{
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables (the general purpose ones say NACK and bus errors) */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc07fff);
	/* 8 bits, four bytes in a FIFO word, as Linux's geni_se_config_packing(8, 4, true) */
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, I2C_100K_DIVIDER << 4 | 1);
	wr(I2C_SCL_COUNTERS, I2C_100K_COUNTERS);
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		delay(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			delay(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* Wait for the command to end; NULL, or what went wrong. */
static const char *
geni_wait(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint64_t deadline;
	uint32_t status = 0;

	clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		status = rd(GENI_M_IRQ_STATUS);
		if (rx != NULL) {
			uint32_t fifo = rd(GENI_RX_FIFO_STATUS), words = fifo & 0x01ffffff, bytes = words * 4;

			if ((fifo & 0x80000000u) && ((fifo >> 28) & 7) && ((fifo >> 28) & 7) < 4) {
				bytes -= 4 - ((fifo >> 28) & 7);
			}
			for (uint32_t i = 0; i < words; i++) {
				uint32_t word = rd(GENI_RX_FIFO);

				for (int j = 0; j < 4 && bytes; j++, bytes--) {
					if (*received < length) {
						rx[(*received)++] = (uint8_t)(word >> (8 * j));
					}
				}
			}
		}
		if (status & M_I2C_ERRORS) {
			wr(GENI_M_IRQ_CLEAR, status);
			geni_recover();
			return (status & M_GP_NACK) ? "no acknowledge (NACK)" : (status & M_GP_ARB_LOST) ? "arbitration lost" :
			       (status & M_GP_BUS_PROTO) ? "bus protocol error" : "the engine reports an error";
		}
		if (status & M_CMD_DONE) {
			wr(GENI_M_IRQ_CLEAR, status);
			return NULL;
		}
	}
	geni_recover();
	return "no end of the command in 100 ms";
}

/* Write the command byte, then (repeated start) read `length` bytes. NULL, or what went wrong. */
static const char *
sec_ts_read(uint8_t command, uint8_t *rx, uint32_t length)
{
	const char *problem;
	uint32_t received = 0;

	if (bitbang) {
		bb = &touch_bus;
		return bb_read(command, rx, length);
	}
	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 100) {
			geni_recover();
			break;
		}
		delay(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(I2C_TX_TRANS_LEN, 1);
	wr(GENI_M_CMD0, I2C_OP_WRITE << 27 | SEC_TS_ADDRESS << 9 | I2C_STOP_STRETCH);
	wr(GENI_TX_FIFO, command);
	problem = geni_wait(NULL, 0, NULL);
	if (problem != NULL) {
		return problem;
	}
	wr(I2C_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, I2C_OP_READ << 27 | SEC_TS_ADDRESS << 9);
	problem = geni_wait(rx, length, &received);
	if (problem == NULL && received != length) {
		return "fewer bytes than asked for";
	}
	return problem;
}

#define SEC_TS_DEVICE_ID        0x52    /* 3 bytes */
#define SEC_TS_BOOT_STATUS      0x55    /* 0x10 boot loader, 0x20 application */
#define SEC_TS_READ_ONE_EVENT   0x60    /* 8 bytes */
#define SEC_TS_EVENT_SIZE       8

static bool
step_identity(void)
{
	uint8_t id[3], boot, event[SEC_TS_EVENT_SIZE];
	const char *problem;

	if (bitbang) {
		printf("touch: step 5: read the chip (address %#x) over the bus driven by hand, 100 kHz\n", SEC_TS_ADDRESS);
	} else {
		printf("touch: step 5: set the engine up for I2C at 100 kHz and read the chip (address %#x)\n", SEC_TS_ADDRESS);
		geni_i2c_init();
		printf("touch: engine: clock config %#x, SCL counters %#x, status %#x\n", rd(GENI_SER_M_CLK_CFG), rd(I2C_SCL_COUNTERS),
		    rd(GENI_STATUS));
	}
	problem = sec_ts_read(SEC_TS_DEVICE_ID, id, sizeof(id));
	if (problem != NULL) {
		printf("touch: reading the device identity (%#x): %s\n", SEC_TS_DEVICE_ID, problem);
		return false;
	}
	printf("touch: device identity %02x %02x %02x\n", id[0], id[1], id[2]);
	problem = sec_ts_read(SEC_TS_BOOT_STATUS, &boot, 1);
	if (problem != NULL) {
		printf("touch: reading the boot status (%#x): %s\n", SEC_TS_BOOT_STATUS, problem);
		return false;
	}
	printf("touch: boot status %#x (%s)\n", boot, boot == 0x20 ? "the application runs" : boot == 0x10 ? "BOOT LOADER only" : "?");
	problem = sec_ts_read(SEC_TS_READ_ONE_EVENT, event, sizeof(event));
	if (problem != NULL) {
		printf("touch: reading an event (%#x): %s\n", SEC_TS_READ_ONE_EVENT, problem);
		return false;
	}
	printf("touch: first event %02x %02x %02x %02x %02x %02x %02x %02x; the interrupt line is now %s\n", event[0], event[1],
	    event[2], event[3], event[4], event[5], event[6], event[7], (rd(GPIO_IO(PIN_INT)) & 1) ? "high" : "low");
	return boot == 0x20;
}

/* A command byte alone. */
static const char *
sec_ts_command(uint8_t command)
{
	if (bitbang) {
		bb = &touch_bus;
		return bb_command(command);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(I2C_TX_TRANS_LEN, 1);
	wr(GENI_M_CMD0, I2C_OP_WRITE << 27 | SEC_TS_ADDRESS << 9);
	wr(GENI_TX_FIFO, command);
	return geni_wait(NULL, 0, NULL);
}

/* ---- steps 6 and 7: sensing, and the touches ------------------------------------------------------ */

#define SEC_TS_SENSE_ON         0x10
#define SEC_TS_PANEL_INFO       0x23    /* 11 bytes: [0..1] x range, [2..3] y range (big endian) */
#define SEC_TS_READ_ALL_EVENT   0x61    /* the waiting events, 8 bytes each */
#define SEC_TS_CLEAR_EVENTS     0x62
#define SEC_TS_EVENT_COUNT      32
#define SEC_TS_FINGERS          10
#define POLL_US                 8000    /* 125 times a second, as the redwood driver */

static uint32_t max_x, max_y;

static bool
step_sense(void)
{
	uint8_t panel[11];
	const char *problem;

	printf("touch: step 6: read the panel's size, clear the chip's events and switch sensing on\n");
	problem = sec_ts_read(SEC_TS_PANEL_INFO, panel, sizeof(panel));
	if (problem != NULL) {
		printf("touch: reading the panel information (%#x): %s\n", SEC_TS_PANEL_INFO, problem);
		return false;
	}
	max_x = (uint32_t)panel[0] << 8 | panel[1];
	max_y = (uint32_t)panel[2] << 8 | panel[3];
	printf("touch: panel information %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x: x up to %u, y up to %u\n",
	    panel[0], panel[1], panel[2], panel[3], panel[4], panel[5], panel[6], panel[7], panel[8], panel[9], panel[10],
	    max_x, max_y);
	if (max_x < 100 || max_y < 100) {
		max_x = max_y = 4096;   /* the device tree's sec,max_coords */
		printf("touch: ... which does not look like a size: 4096 x 4096 instead (the device tree's)\n");
	}
	problem = sec_ts_command(SEC_TS_CLEAR_EVENTS);
	if (problem == NULL) {
		delay(10000);
		problem = sec_ts_command(SEC_TS_SENSE_ON);
	}
	if (problem != NULL) {
		printf("touch: clearing the events or switching sensing on: %s\n", problem);
		return false;
	}
	delay(10000);
	printf("touch: sensing is on; the interrupt line is %s\n", (rd(GPIO_IO(PIN_INT)) & 1) ? "high" : "low");
	return true;
}

static struct {
	bool down;
	uint32_t x, y;
	uint64_t since;         /* when it landed */
	unsigned moves;
} finger[SEC_TS_FINGERS];
/*
 * boot-arg touch_log=1: a line for each touch and for the first events. Off by default: under -v
 * a line is drawn on the screen, and that takes long enough (the whole framebuffer scrolls) for a
 * tap to be read late and taken for a long press.
 */
static uint32_t touch_log;
static unsigned said_events, said_touches;
static uint64_t touches_down, events_read, read_errors, reads, read_ns, read_longest_ns;

static void
handle_event(const uint8_t *e)
{
	unsigned kind = e[0] & 3, state = e[0] >> 6, id = (e[0] >> 2) & 0xf;
	uint32_t x, y;

	events_read++;
	if (touch_log && said_events < 40) {
		said_events++;
		printf("touch: event %02x %02x %02x %02x %02x %02x %02x %02x\n", e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
	}
	if (kind != 0 || id == 0 || id > SEC_TS_FINGERS) {
		return;         /* a status or vendor event, or no finger */
	}
	id--;
	x = (uint32_t)e[1] << 4 | e[3] >> 4;
	y = (uint32_t)e[2] << 4 | (e[3] & 0xf);
	/* to the display's 0..65535, left/top to right/bottom */
	x = x >= max_x ? 65535 : x * 65535 / max_x;
	y = y >= max_y ? 65535 : y * 65535 / max_y;
	if (state == 1 || state == 2) {
		bool landed = !finger[id].down;

		finger[id].down = true;
		finger[id].x = x;
		finger[id].y = y;
		if (landed) {
			touches_down++;
			finger[id].since = mach_absolute_time();
			finger[id].moves = 0;
		} else {
			finger[id].moves++;
		}
		redwood_touch_post(landed ? REDWOOD_TOUCH_DOWN : REDWOOD_TOUCH_MOVE, id, x, y);
		if (touch_log && landed && said_touches < 200) {
			said_touches++;
			printf("touch: finger %u DOWN at %u, %u (of 65535); event %02x %02x %02x %02x %02x %02x %02x %02x\n", id, x, y,
			    e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
		}
	} else if (state == 3 && finger[id].down) {
		uint64_t held;

		finger[id].down = false;
		redwood_touch_post(REDWOOD_TOUCH_UP, id, finger[id].x, finger[id].y);
		absolutetime_to_nanoseconds(mach_absolute_time() - finger[id].since, &held);
		if (touch_log && said_touches < 200) {
			said_touches++;
			printf("touch: finger %u up at %u, %u after %llu ms and %u moves; event %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    id, finger[id].x, finger[id].y, held / NSEC_PER_MSEC, finger[id].moves, e[0], e[1], e[2], e[3], e[4], e[5],
			    e[6], e[7]);
		}
	}
}

/* Whether any finger is down, as far as the events have said. */
static bool
any_finger_down(void)
{
	for (unsigned i = 0; i < SEC_TS_FINGERS; i++) {
		if (finger[i].down) {
			return true;
		}
	}
	return false;
}

/*
 * Step 7: whenever the interrupt line is low, read what is waiting -- and while a finger is down,
 * every time whatever the line says. A lift was missed now and then (seen with touch_trace=1:
 * a quick tap's landing, then nothing until the next touch, which came as a move of the same
 * finger), so that iOS held the finger down and made a long press of the tap: the line says so
 * for less than the 8 ms between two looks, it seems. Asked when nothing waits, the chip
 * answers an event of zeroes.
 */
static void
step_watch(void)
{
	static uint8_t events[SEC_TS_EVENT_COUNT * SEC_TS_EVENT_SIZE];
	unsigned left, ticks = 0;
	const char *problem;

	printf("touch: step 7: watching the interrupt line every %u us, posting the touches\n", POLL_US);
	for (;;) {
		if (!(rd(GPIO_IO(PIN_INT)) & 1) || any_finger_down()) {
			uint64_t start = mach_absolute_time(), taken;

			problem = sec_ts_read(SEC_TS_READ_ONE_EVENT, events, SEC_TS_EVENT_SIZE);
			if (problem == NULL && events[0] != 0) {
				/*
				 * How many more wait: the low five bits (Linux's sec_ts_event_coordinate:
				 * left_event:5, max_energy:1, ttype_1_0:2). Read as six, the max_energy bit
				 * that this chip sets in nearly every event made it 32 -- 31 events asked for
				 * that were not there, and now and then a lift lost with them.
				 */
				left = events[7] & 0x1f;
				if (left > SEC_TS_EVENT_COUNT - 1) {
					left = SEC_TS_EVENT_COUNT - 1;
				}
				if (left != 0) {
					problem = sec_ts_read(SEC_TS_READ_ALL_EVENT, events + SEC_TS_EVENT_SIZE, left * SEC_TS_EVENT_SIZE);
				}
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - start, &taken);
			reads++;
			read_ns += taken;
			if (taken > read_longest_ns) {
				read_longest_ns = taken;
			}
			if (reads == 10) {
				printf("touch: the first 10 reads of events took %llu us each (the longest %llu)\n", read_ns / reads / 1000,
				    read_longest_ns / 1000);
			}
			if (problem == NULL && events[0] != 0) {
				for (unsigned i = 0; problem == NULL && i <= left && events[i * SEC_TS_EVENT_SIZE] != 0; i++) {
					handle_event(events + i * SEC_TS_EVENT_SIZE);
				}
			}
			if (problem != NULL && read_errors++ < 8) {
				printf("touch: reading events: %s\n", problem);
			}
		}
		/* once in a while a line about what has happened */
		if (++ticks % (60 * 1000000 / POLL_US) == 10 * 1000000 / POLL_US) {
			printf("touch: so far %llu events, %llu touches, %llu failed reads; a read takes %llu us (the longest %llu)\n",
			    events_read, touches_down, read_errors, reads ? read_ns / reads / 1000 : 0, read_longest_ns / 1000);
		}
		delay(POLL_US);
	}
}

static void
sec_ts_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter;
	unsigned done = 1;

	step_survey();
	if (steps >= 2 && step_pmic()) {
		done = 2;
		if (steps >= 3 && step_power()) {
			done = 3;
			if (steps >= 4 && step_clocks_pins()) {
				done = 4;
				if (steps >= 5 && step_identity()) {
					done = 5;
					if (steps >= 6 && step_sense()) {
						done = 6;
						if (steps >= 7) {
							step_watch();
						}
					}
				}
			}
		}
	}
	printf("touch: stopping here (touch_steps=%u, %u done)\n", steps, done);
	thread_terminate(current_thread());
}

void
redwood_sec_ts_init(void)
{
	uint32_t enabled = 0, steps = 7;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (!redwood_platform_is("y2q")) {
		return;
	}
	PE_parse_boot_argn("touch_steps", &steps, sizeof(steps));
	PE_parse_boot_argn("touch_log", &touch_log, sizeof(touch_log));
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(sec_ts_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
