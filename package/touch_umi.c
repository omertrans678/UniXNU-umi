/*
 * touch_umi.c — Native UniXNU Xiaomi Mi 10 (umi / SM8250) Touchscreen Driver.
 * Drives STMicroelectronics FTS521 (0x49) capacitive touch controller.
 * Feeds multi-touch events directly to Apple iOS / Redwood backboardd via _redwood_touch_post.
 */

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long uintptr_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)

#define TLMM_PHYS       0x0f500000UL
#define TLMM_SIZE       0x300000UL

#define PIN_SDA         36
#define PIN_SCL         37
#define PIN_RESET       38
#define PIN_INT         39
#define PIN_AVDD        69

/* STMicroelectronics FTS521 definitions */
#define FTS_CHIP_ADDR            0x49

/* Official FTS Commands */
#define FTS_CMD_MS_MT_SENSE_ON   0x93
#define FTS_CMD_MS_MT_SENSE_OFF  0x92
#define FTS_CMD_FULL_INIT        0xA5
#define FTS_CMD_HW_REG_W         0xB6
#define FTS_CMD_HW_REG_R         0xB6
#define FIFO_CMD_READONE         0x85
#define IER_ADDR                 0x002C
#define IER_ENABLE               0x41

/* Event IDs (ST FTS protocol) */
#define EVENTID_NO_EVENT         0x00
#define EVENTID_ENTER_POINTER    0x03
#define EVENTID_LEAVE_POINTER    0x04
#define EVENTID_MOTION_POINTER   0x05
#define EVENTID_CONTROL_READY    0x10
#define EVENTID_STATUS_UPDATE    0x16

/* Redwood / iOS Touch Types */
#define REDWOOD_TOUCH_DOWN       1
#define REDWOOD_TOUCH_MOVE       2
#define REDWOOD_TOUCH_UP         3

/* Function pointers into XNU kernel */
#define KERN_ML_IO_MAP          0xfffffff007464228UL
#define KERN_PRINTF             0xfffffff0074c61b4UL
#define KERN_REDWOOD_TOUCH_POST 0xfffffff0074c1a08UL
#define KERN_RPMH_RESOURCE      0xfffffff0074f2168UL
#define KERN_RPMH_WRITE         0xfffffff0074f1f80UL
#define KERN_RSC_CACHE_PTR      0xfffffff007eac768UL
#define KERN_SPMI_LOCK          0xfffffff0074c5884UL
#define KERN_SPMI_UNLOCK        0xfffffff0074c5890UL

/* SPMI Arbiter v5 physical addresses (Qualcomm SM8250) */
#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL

#define PM8150B_GPIO5_ID        0x2c4
#define SPMI_STATUS_DONE        0x1

typedef struct {
    bool down;
    uint32_t x, y;
} touch_contact_t;

typedef struct {
    uintptr_t tlmm;
    touch_contact_t contacts[10];
} umi_touch_ctx_t;

/* Darwin-ABI stack-pushing printf wrapper */
static void klog(const char *fmt, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    register const char *r0 asm("x0") = fmt;
    uint64_t fn = KERN_PRINTF;
    asm volatile(
        "sub sp, sp, #48\n"
        "stp %1, %2, [sp, #0]\n"
        "stp %3, %4, [sp, #16]\n"
        "stp %5, %6, [sp, #32]\n"
        "blr %7\n"
        "add sp, sp, #48\n"
        : "+r"(r0)
        : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(fn)
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "lr", "memory"
    );
}

static inline uintptr_t tlmm_cfg(uintptr_t tlmm, uint32_t pin) {
    return tlmm + 0x1000UL * pin;
}

static inline uintptr_t tlmm_io(uintptr_t tlmm, uint32_t pin) {
    return tlmm + 0x1000UL * pin + 4;
}

static inline uint32_t rd32(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}

static inline void wr32(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}

/* Delay using ARM64 generic timer (19.2 MHz on Qualcomm) */
static void delay_us(uint32_t us) {
    uint64_t start, cur, ticks = (uint64_t)us * 192 / 10;
    asm volatile("mrs %0, cntvct_el0" : "=r"(start));
    do {
        asm volatile("mrs %0, cntvct_el0" : "=r"(cur));
    } while (cur - start < ticks);
}

static void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        delay_us(1000);
    }
}

static void gpio_cfg_input(uintptr_t tlmm, uint32_t pin, uint32_t pull) {
    wr32(tlmm_cfg(tlmm, pin), (pull & 3));
}

static void gpio_cfg_output(uintptr_t tlmm, uint32_t pin, bool high) {
    wr32(tlmm_io(tlmm, pin), high ? 2 : 0);
    wr32(tlmm_cfg(tlmm, pin), (1u << 9) | (2 << 6) | 3);
}

static inline bool gpio_get(uintptr_t tlmm, uint32_t pin) {
    return (rd32(tlmm_io(tlmm, pin)) & 1) != 0;
}

static inline void gpio_set(uintptr_t tlmm, uint32_t pin, bool high) {
    wr32(tlmm_io(tlmm, pin), high ? 2 : 0);
}

/* SPMI helpers for PM8150B GPIO 5 (vdd_boost_vreg) */
static inline void spmi_lock_acquire(void) {
    typedef void (*fn_t)(void);
    ((fn_t)KERN_SPMI_LOCK)();
}

static inline void spmi_lock_release(void) {
    typedef void (*fn_t)(void);
    ((fn_t)KERN_SPMI_UNLOCK)();
}

typedef uintptr_t (*ml_io_map_fn)(uint64_t phys, uint64_t size);
typedef uint32_t (*rpmh_res_fn)(const char *name, uint16_t *len, uint32_t count, uint32_t *offset);
typedef bool (*rpmh_wr_fn)(uint32_t count, const uint32_t *addrs, const uint32_t *vals);

/*
 * Accurate SPMI v5 transfer matching UmiSpmi.inc:
 * - SID 2, address 0xC444 = 0x80 (forces PM8150B GPIO5 boost output high)
 * - SID 2, address 0xC446 = 0x80 (enables GPIO5 output)
 */
static bool spmi_transfer(ml_io_map_fn io_map, uint8_t sid, uint16_t addr, uint8_t *val, bool write) {
    uintptr_t arb_core = io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
    uintptr_t arb_cnfg = io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
    if (!arb_core || !arb_cnfg) return false;

    uint16_t ppid = ((uint16_t)sid << 8) | (addr >> 8);
    int apid = -1;
    for (uint32_t n = 0; n < 512; n++) {
        uint32_t map = rd32(arb_core + 0x900 + 4 * n);
        if (map && (((map >> 8) & 0xfff) == ppid)) {
            uint32_t owner = rd32(arb_cnfg + 0x700 + 4 * n) & 7;
            if (apid < 0 || owner == 0) {
                apid = (int)n;
                if (owner == 0) break;
            }
        }
    }
    if (apid < 0) return false;

    spmi_lock_acquire();
    uintptr_t ch;
    if (write) {
        ch = io_map(ARB_CHNLS_PHYS + (uint32_t)apid * 0x10000UL, 0x1000UL);
        if (!ch) { spmi_lock_release(); return false; }
        wr32(ch + 0x10, (uint32_t)*val);
        wr32(ch + 0x00, ((uint32_t)(addr & 0xff) << 4));
    } else {
        ch = io_map(ARB_OBSERVER_PHYS + (uint32_t)apid * 0x80UL, 0x80UL);
        if (!ch) { spmi_lock_release(); return false; }
        wr32(ch + 0x00, (1u << 27) | ((uint32_t)(addr & 0xff) << 4));
    }

    bool ok = false;
    for (int t = 0; t < 2000; t++) {
        uint32_t status = rd32(ch + 0x08);
        if (status & 1) {
            if (!write) *val = (uint8_t)(rd32(ch + 0x18) & 0xff);
            ok = true;
            break;
        }
        delay_us(2);
    }
    spmi_lock_release();
    return ok;
}

static void enable_vdd_boost(ml_io_map_fn io_map) {
    uint8_t v80 = 0x80;
    // Attempt 80: Write SID 2, address 0xC444 = 0x80 (forces PM8150B GPIO5 output high)
    bool ok_out = spmi_transfer(io_map, 2, 0xc444, &v80, true);
    // Also enable output in 0xC446 = 0x80
    bool ok_en  = spmi_transfer(io_map, 2, 0xc446, &v80, true);
    delay_us(4000); // 4000 us as required by xiaomi-sm8250-common.dtsi

    uint8_t rb = 0;
    spmi_transfer(io_map, 2, 0xc444, &rb, false);
    klog("umi_touch: Attempt 80 PM8150B GPIO5 boost out=%u en=%u readback=0x%02x\n",
         (uint64_t)ok_out, (uint64_t)ok_en, (uint64_t)rb, 0, 0, 0);
}

/* Robust Bitbanged Open-Drain I2C */
#define I2C_DELAY_US 8

static inline void i2c_scl_low(uintptr_t tlmm) {
    gpio_set(tlmm, PIN_SCL, false);
    gpio_cfg_output(tlmm, PIN_SCL, false);
    delay_us(I2C_DELAY_US);
}

static inline void i2c_scl_high(uintptr_t tlmm) {
    gpio_set(tlmm, PIN_SCL, true);
    gpio_cfg_output(tlmm, PIN_SCL, true);
    delay_us(I2C_DELAY_US);
}

static inline void i2c_sda_low(uintptr_t tlmm) {
    gpio_set(tlmm, PIN_SDA, false);
    gpio_cfg_output(tlmm, PIN_SDA, false);
    delay_us(I2C_DELAY_US);
}

static inline void i2c_sda_release(uintptr_t tlmm) {
    gpio_cfg_input(tlmm, PIN_SDA, 3);
    delay_us(I2C_DELAY_US);
}

static void i2c_start(uintptr_t tlmm) {
    i2c_sda_release(tlmm);
    delay_us(I2C_DELAY_US);
    i2c_scl_high(tlmm);
    delay_us(I2C_DELAY_US);
    i2c_sda_low(tlmm);
    delay_us(I2C_DELAY_US);
    i2c_scl_low(tlmm);
    delay_us(I2C_DELAY_US);
}

static void i2c_stop(uintptr_t tlmm) {
    i2c_sda_low(tlmm);
    delay_us(I2C_DELAY_US);
    i2c_scl_high(tlmm);
    delay_us(I2C_DELAY_US);
    i2c_sda_release(tlmm);
    delay_us(I2C_DELAY_US);
}

static bool i2c_write_byte(uintptr_t tlmm, uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        if ((byte >> i) & 1) i2c_sda_release(tlmm);
        else i2c_sda_low(tlmm);
        i2c_scl_high(tlmm);
        i2c_scl_low(tlmm);
    }
    i2c_sda_release(tlmm);
    i2c_scl_high(tlmm);
    bool ack = (gpio_get(tlmm, PIN_SDA) == 0);
    i2c_scl_low(tlmm);
    return ack;
}

static uint8_t i2c_read_byte(uintptr_t tlmm, bool ack) {
    i2c_sda_release(tlmm);
    uint8_t byte = 0;
    for (int i = 7; i >= 0; i--) {
        i2c_scl_high(tlmm);
        if (gpio_get(tlmm, PIN_SDA)) byte |= (1 << i);
        i2c_scl_low(tlmm);
    }
    if (ack) i2c_sda_low(tlmm);
    else i2c_sda_release(tlmm);
    i2c_scl_high(tlmm);
    i2c_scl_low(tlmm);
    i2c_sda_release(tlmm);
    return byte;
}

static void i2c_recover_bus(uintptr_t tlmm) {
    i2c_sda_release(tlmm);
    for (int i = 0; i < 9; i++) {
        i2c_scl_high(tlmm);
        delay_us(I2C_DELAY_US);
        i2c_scl_low(tlmm);
        delay_us(I2C_DELAY_US);
    }
    i2c_stop(tlmm);
}

static bool i2c_probe_addr(uintptr_t tlmm, uint8_t addr7) {
    i2c_start(tlmm);
    bool ack = i2c_write_byte(tlmm, (uint8_t)(addr7 << 1));
    i2c_stop(tlmm);
    return ack;
}

static bool fts_i2c_write(uintptr_t tlmm, uint8_t dev, const uint8_t *buf, uint32_t len) {
    i2c_start(tlmm);
    if (!i2c_write_byte(tlmm, (uint8_t)(dev << 1))) {
        i2c_stop(tlmm);
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (!i2c_write_byte(tlmm, buf[i])) {
            i2c_stop(tlmm);
            return false;
        }
    }
    i2c_stop(tlmm);
    return true;
}

static int fts_i2c_write_read(uintptr_t tlmm, uint8_t dev, const uint8_t *wbuf, uint32_t wlen, uint8_t *rbuf, uint32_t rlen) {
    i2c_start(tlmm);
    if (!i2c_write_byte(tlmm, (uint8_t)(dev << 1))) {
        i2c_stop(tlmm);
        return -1;
    }
    for (uint32_t i = 0; i < wlen; i++) {
        if (!i2c_write_byte(tlmm, wbuf[i])) {
            i2c_stop(tlmm);
            return -2;
        }
    }

    // Repeated Start: NO INTERVENING STOP (as required by FTS521 ftsIO.c)
    i2c_sda_release(tlmm);
    delay_us(I2C_DELAY_US);
    i2c_scl_high(tlmm);
    delay_us(I2C_DELAY_US);
    i2c_sda_low(tlmm);
    delay_us(I2C_DELAY_US);
    i2c_scl_low(tlmm);
    delay_us(I2C_DELAY_US);

    if (i2c_write_byte(tlmm, (uint8_t)((dev << 1) | 1))) {
        for (uint32_t i = 0; i < rlen; i++) {
            rbuf[i] = i2c_read_byte(tlmm, i < rlen - 1);
        }
        i2c_stop(tlmm);
        return 0;
    }

    i2c_stop(tlmm);
    return -3;
}

static int fts_read_event(uintptr_t tlmm, uint8_t dev, uint8_t *ev) {
    uint8_t cmd86 = 0x86; // FIFO_CMD_READALL (as used by UmiFtsTouch.cpp)
    for (int b = 0; b < 8; b++) ev[b] = 0;
    return fts_i2c_write_read(tlmm, dev, &cmd86, 1, ev, 8);
}

static void post_touch(uint32_t type, uint32_t id, uint32_t x, uint32_t y) {
    typedef void (*fn_t)(uint64_t type, uint64_t id, uint64_t x, uint64_t y);
    ((fn_t)KERN_REDWOOD_TOUCH_POST)((uint64_t)type, (uint64_t)id, (uint64_t)x, (uint64_t)y);
}

static void fts_init_sensor(uintptr_t tlmm, uint8_t dev) {
    klog("umi_touch: initializing ST FTS521 sensor at 0x%02x...\n", (uint64_t)dev, 0, 0, 0, 0, 0);

    // Drain initial events
    uint8_t ev[8];
    for (int i = 0; i < 16; i++) {
        if (fts_read_event(tlmm, dev, ev) < 0 || ev[0] == EVENTID_NO_EVENT || ev[0] == 0xff) break;
        delay_us(2000);
    }

    // Official Attempt 80 Sensing Sequence (UmiFtsSensing.h):
    const uint8_t cmd_on[3]      = { 0xa0, 0x00, 0x01 };
    const uint8_t cmd_reports[6] = { 0xa2, 0x03, 0x00, 0x00, 0x00, 0x03 };
    const uint8_t cmd_off[3]     = { 0xa0, 0x00, 0x00 };

    bool s1 = fts_i2c_write(tlmm, dev, cmd_on, 3);
    delay_ms(10);
    bool s2 = fts_i2c_write(tlmm, dev, cmd_reports, 6);
    delay_ms(10);
    bool s3 = fts_i2c_write(tlmm, dev, cmd_off, 3);
    delay_ms(50); // WAIT_AFTER_SENSEOFF = 50 ms
    bool s4 = fts_i2c_write(tlmm, dev, cmd_on, 3);
    delay_ms(10);

    // Also send FTS_CMD_MS_MT_SENSE_ON (0x93)
    uint8_t sense_on = 0x93;
    fts_i2c_write(tlmm, dev, &sense_on, 1);
    delay_ms(10);

    // Enable Interrupts (IER_ADDR = 0x002C, IER_ENABLE = 0x41)
    uint8_t ier_cmd[4] = { FTS_CMD_HW_REG_W, 0x00, 0x2C, IER_ENABLE };
    bool ok_ier = fts_i2c_write(tlmm, dev, ier_cmd, 4);
    delay_ms(10);

    klog("umi_touch: Sensing cmds -> %u %u %u %u IER=%u\n",
         (uint64_t)s1, (uint64_t)s2, (uint64_t)s3, (uint64_t)s4, (uint64_t)ok_ier, 0);
}

void __attribute__((section(".text.umi_touch_thread"))) umi_touch_thread(void *arg) {
    (void)arg;
    klog("umi_touch: starting pure UniXNU Xiaomi Mi 10 touchscreen driver v3.5 (Attempt 80)\n", 0, 0, 0, 0, 0, 0);

    umi_touch_ctx_t ctx;
    for (int f = 0; f < 10; f++) {
        ctx.contacts[f].down = false;
        ctx.contacts[f].x = 0;
        ctx.contacts[f].y = 0;
    }

    ctx.tlmm = ((ml_io_map_fn)KERN_ML_IO_MAP)(TLMM_PHYS, TLMM_SIZE);
    if (!ctx.tlmm) {
        klog("umi_touch: failed to map TLMM!\n", 0, 0, 0, 0, 0, 0);
        return;
    }
    klog("umi_touch: TLMM mapped at virtual 0x%016lx\n", (uint64_t)ctx.tlmm, 0, 0, 0, 0, 0);

    // 1. Hold reset LOW
    gpio_cfg_output(ctx.tlmm, PIN_RESET, false);

    // 2. Power up pm8150a_l1 (ldoc1 @ 1.8V VDD) via RPMh
    uintptr_t *rsc_cache = (uintptr_t *)KERN_RSC_CACHE_PTR;
    if (!*rsc_cache) {
        *rsc_cache = ((ml_io_map_fn)KERN_ML_IO_MAP)(0x18220000UL, 0x10000UL);
    }

    rpmh_res_fn rpmh_res = (rpmh_res_fn)KERN_RPMH_RESOURCE;
    rpmh_wr_fn  rpmh_wr  = (rpmh_wr_fn)KERN_RPMH_WRITE;

    uint16_t rlen = 0;
    uint32_t roff = 0;
    uint32_t ldoc1_addr = rpmh_res("ldoc1", &rlen, 1, &roff);
    if (!ldoc1_addr) ldoc1_addr = rpmh_res("ldoc2", &rlen, 1, &roff);

    if (ldoc1_addr) {
        uint32_t addrs[3] = { ldoc1_addr, ldoc1_addr + 4, ldoc1_addr + 8 };
        uint32_t vals[3]  = { 1800, 1, 7 }; // 1800mV, Enable=1, Mode=7 (PWM)
        bool ok = rpmh_wr(3, addrs, vals);
        klog("umi_touch: RPMh ldoc1 1800mV enabled -> %u\n", (uint64_t)ok, 0, 0, 0, 0, 0);
    }
    delay_ms(15); // fts_enable_reg: VDD 15 ms before AVDD

    // 3. Attempt 80: Enable PM8150B GPIO5 Boost (0xC444 = 0x80)
    enable_vdd_boost((ml_io_map_fn)KERN_ML_IO_MAP);

    // 4. Enable AVDD (GPIO 69)
    gpio_cfg_output(ctx.tlmm, PIN_AVDD, true);
    delay_ms(10);

    // 5. Native Reset Sequence: HIGH, LOW for 10 ms, HIGH
    gpio_cfg_output(ctx.tlmm, PIN_RESET, true);
    delay_ms(5);
    gpio_cfg_output(ctx.tlmm, PIN_RESET, false);
    delay_ms(10);
    gpio_cfg_output(ctx.tlmm, PIN_RESET, true);
    delay_ms(150); // wait for chip boot and ready

    // 5. Configure INT pin (GPIO 39) with pull-up
    gpio_cfg_input(ctx.tlmm, PIN_INT, 3);

    // 6. Configure I2C pins (GPIO 36=SDA, GPIO 37=SCL)
    i2c_sda_release(ctx.tlmm);
    i2c_scl_high(ctx.tlmm);
    delay_ms(10);
    i2c_recover_bus(ctx.tlmm);

    uint8_t found_addr = 0;
    if (i2c_probe_addr(ctx.tlmm, FTS_CHIP_ADDR)) {
        found_addr = FTS_CHIP_ADDR;
        klog("umi_touch: ST FTS verified at 0x%02x\n", found_addr, 0, 0, 0, 0, 0);
    } else {
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            if (i2c_probe_addr(ctx.tlmm, a)) {
                klog("umi_touch: scan found ACK at 0x%02x\n", a, 0, 0, 0, 0, 0);
                if (a == 0x49) { found_addr = a; break; }
            }
        }
    }

    if (!found_addr) found_addr = FTS_CHIP_ADDR;

    // 7. Full ST FTS521 Sensor Initialization
    fts_init_sensor(ctx.tlmm, found_addr);

    uint32_t loop_count = 0;
    uint32_t total_touches = 0;

    while (1) {
        loop_count++;
        uint32_t int_val = gpio_get(ctx.tlmm, PIN_INT);

        uint8_t ev[8] = {0};
        int events_drained = 0;

        // Drain event FIFO if INT pin is LOW (0) or on periodic poll
        while (events_drained < 16) {
            int rcode = fts_read_event(ctx.tlmm, found_addr, ev);
            if (rcode < 0 || ev[0] == EVENTID_NO_EVENT) break;

            events_drained++;
            uint8_t evt_id = ev[0];
            uint8_t kind   = ev[0] >> 4;
            uint8_t touch_id = ev[1] >> 4;
            if (touch_id >= 10) touch_id = ev[1] & 0x0F;

            klog("umi_touch: RAW EVENT [%02x %02x %02x %02x %02x %02x]\n",
                 (uint64_t)ev[0], (uint64_t)ev[1], (uint64_t)ev[2],
                 (uint64_t)ev[3], (uint64_t)ev[4], (uint64_t)ev[5]);

            if (kind == 1 || kind == 2) {
                // Official STMicro FTS coordinate unpacking (UmiFtsCore.h):
                uint32_t rx = (((uint32_t)ev[3] & 0x0F) << 8) | (uint32_t)ev[2];
                uint32_t ry = ((uint32_t)ev[4] << 4) | ((uint32_t)ev[3] >> 4);

                if (rx >= 1080) rx = 1079;
                if (ry >= 2340) ry = 2339;

                uint32_t nx = (rx * 65535) / 1079;
                uint32_t ny = (ry * 65535) / 2339;

                total_touches++;
                klog("umi_touch: TOUCH pos=(%u,%u) norm=(%u,%u) kind=%u id=%u\n",
                     (uint64_t)rx, (uint64_t)ry, (uint64_t)nx, (uint64_t)ny,
                     (uint64_t)kind, (uint64_t)touch_id);

                if (touch_id < 10) {
                    uint32_t type = (kind == 1) ? REDWOOD_TOUCH_DOWN : REDWOOD_TOUCH_MOVE;
                    post_touch(type, touch_id, nx, ny);
                    ctx.contacts[touch_id].down = true;
                    ctx.contacts[touch_id].x = nx;
                    ctx.contacts[touch_id].y = ny;
                }
            } else if (kind == 3) {
                klog("umi_touch: TOUCH UP id=%u touches=%u\n", (uint64_t)touch_id, (uint64_t)total_touches, 0, 0, 0, 0);

                if (touch_id < 10 && ctx.contacts[touch_id].down) {
                    post_touch(REDWOOD_TOUCH_UP, touch_id, ctx.contacts[touch_id].x, ctx.contacts[touch_id].y);
                    ctx.contacts[touch_id].down = false;
                } else {
                    for (int f = 0; f < 10; f++) {
                        if (ctx.contacts[f].down) {
                            post_touch(REDWOOD_TOUCH_UP, f, ctx.contacts[f].x, ctx.contacts[f].y);
                            ctx.contacts[f].down = false;
                        }
                    }
                }
            } else if (evt_id == EVENTID_CONTROL_READY) {
                klog("umi_touch: CONTROLLER_READY received! Re-enabling scan...\n", 0, 0, 0, 0, 0, 0);
                fts_init_sensor(ctx.tlmm, found_addr);
            }

            if ((ev[7] & 0x1F) == 0) break;
        }

        if (events_drained > 0) {
            uint8_t ier_cmd[4] = { FTS_CMD_HW_REG_W, 0x00, 0x2C, IER_ENABLE };
            fts_i2c_write(ctx.tlmm, found_addr, ier_cmd, 4);
        }

        // Heartbeat log every ~2.5s (300 * 8.3ms)
        if ((loop_count % 300) == 1) {
            uint32_t sda_val = gpio_get(ctx.tlmm, PIN_SDA);
            uint32_t scl_val = gpio_get(ctx.tlmm, PIN_SCL);
            uint32_t hb = loop_count / 300;
            uint8_t dev_ev[8] = {0};
            int last_rc = fts_read_event(ctx.tlmm, found_addr, dev_ev);
            klog("umi_touch: [hb #%u] int=%u sda=%u scl=%u rc=%lld ev0=%02x ev1=%02x\n",
                 (uint64_t)hb, (uint64_t)int_val, (uint64_t)sda_val,
                 (uint64_t)scl_val, (uint64_t)(long long)last_rc, (uint64_t)dev_ev[0]);
        }

        delay_us(8333); // 120 Hz
    }
}
