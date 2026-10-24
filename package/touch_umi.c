/*
 * touch_umi.c — Native UniXNU Xiaomi Mi 10 (umi / SM8250) Touchscreen Driver.
 * STMicroelectronics FTS521 / FTM5 capacitive touch controller.
 * Bitbanged Open-Drain I2C bus on GPIO36 (SDA) / GPIO37 (SCL).
 * Feeds multi-touch events directly to Apple iOS / Redwood via _redwood_touch_post.
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

#define FTS_SLAVE_ADDR  0x49

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

typedef struct {
    bool down;
    uint32_t x, y;
} touch_contact_t;

typedef struct {
    uintptr_t tlmm;
    uint32_t sda_ctl;
    uint32_t scl_ctl;
    touch_contact_t contacts[10];
} umi_touch_ctx_t;

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
    asm volatile("dsb sy" ::: "memory");
}

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

static inline bool gpio_get(uintptr_t tlmm, uint32_t pin) {
    return (rd32(tlmm_io(tlmm, pin)) & 1) != 0;
}

/* Open-drain line control: Output register is always 0 (LOW).
 * Released (1) -> OE = 0 (input, external pullup pulls line HIGH).
 * Driven   (0) -> OE = 1 (output enables low drive).
 */
static inline void line_sda(umi_touch_ctx_t *ctx, bool released) {
    wr32(tlmm_cfg(ctx->tlmm, PIN_SDA), ctx->sda_ctl | (released ? 0 : (1u << 9)));
}

static inline void line_scl(umi_touch_ctx_t *ctx, bool released) {
    wr32(tlmm_cfg(ctx->tlmm, PIN_SCL), ctx->scl_ctl | (released ? 0 : (1u << 9)));
}

#define HALF_CYCLE_US 5

static void clock_low(umi_touch_ctx_t *ctx) {
    line_scl(ctx, false);
    delay_us(HALF_CYCLE_US);
}

static bool clock_high(umi_touch_ctx_t *ctx) {
    line_scl(ctx, true);
    for (uint32_t w = 0; !gpio_get(ctx->tlmm, PIN_SCL); w++) {
        if (w >= 1000) return false; // Clock stretch timeout
        delay_us(1);
    }
    delay_us(HALF_CYCLE_US);
    return true;
}

static bool i2c_start(umi_touch_ctx_t *ctx) {
    line_sda(ctx, true);
    delay_us(HALF_CYCLE_US);
    if (!clock_high(ctx)) return false;
    line_sda(ctx, false);
    delay_us(HALF_CYCLE_US);
    clock_low(ctx);
    return true;
}

static void i2c_stop(umi_touch_ctx_t *ctx) {
    clock_low(ctx);
    line_sda(ctx, false);
    delay_us(HALF_CYCLE_US);
    clock_high(ctx);
    line_sda(ctx, true);
    delay_us(HALF_CYCLE_US);
}

static bool i2c_write_byte(umi_touch_ctx_t *ctx, uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        line_sda(ctx, ((byte >> i) & 1) != 0);
        delay_us(HALF_CYCLE_US);
        if (!clock_high(ctx)) return false;
        clock_low(ctx);
    }
    line_sda(ctx, true); // Release for ACK
    delay_us(HALF_CYCLE_US);
    if (!clock_high(ctx)) return false;
    bool ack = !gpio_get(ctx->tlmm, PIN_SDA);
    clock_low(ctx);
    return ack;
}

static bool i2c_read_byte(umi_touch_ctx_t *ctx, uint8_t *val, bool last) {
    *val = 0;
    line_sda(ctx, true);
    for (int i = 7; i >= 0; i--) {
        if (!clock_high(ctx)) return false;
        if (gpio_get(ctx->tlmm, PIN_SDA)) *val |= (1 << i);
        clock_low(ctx);
    }
    line_sda(ctx, last); // ACK (low) if not last, NACK (high) if last
    delay_us(HALF_CYCLE_US);
    if (!clock_high(ctx)) return false;
    clock_low(ctx);
    line_sda(ctx, true);
    return true;
}

static bool fts_write(umi_touch_ctx_t *ctx, const uint8_t *wbuf, uint32_t wlen) {
    if (!i2c_start(ctx)) { i2c_stop(ctx); return false; }
    if (!i2c_write_byte(ctx, (FTS_SLAVE_ADDR << 1) | 0)) { i2c_stop(ctx); return false; }
    for (uint32_t i = 0; i < wlen; i++) {
        if (!i2c_write_byte(ctx, wbuf[i])) { i2c_stop(ctx); return false; }
    }
    i2c_stop(ctx);
    return true;
}

static bool fts_write_read(umi_touch_ctx_t *ctx, const uint8_t *wbuf, uint32_t wlen, uint8_t *rbuf, uint32_t rlen) {
    if (!i2c_start(ctx)) { i2c_stop(ctx); return false; }
    if (!i2c_write_byte(ctx, (FTS_SLAVE_ADDR << 1) | 0)) { i2c_stop(ctx); return false; }
    for (uint32_t i = 0; i < wlen; i++) {
        if (!i2c_write_byte(ctx, wbuf[i])) { i2c_stop(ctx); return false; }
    }
    // Repeated start
    if (!i2c_start(ctx)) { i2c_stop(ctx); return false; }
    if (!i2c_write_byte(ctx, (FTS_SLAVE_ADDR << 1) | 1)) { i2c_stop(ctx); return false; }
    for (uint32_t i = 0; i < rlen; i++) {
        if (!i2c_read_byte(ctx, &rbuf[i], i + 1 == rlen)) { i2c_stop(ctx); return false; }
    }
    i2c_stop(ctx);
    return true;
}

static int fts_read_fifo(umi_touch_ctx_t *ctx, uint8_t *ev) {
    uint8_t cmd86 = 0x86;
    for (int i = 0; i < 8; i++) ev[i] = 0;
    return fts_write_read(ctx, &cmd86, 1, ev, 8) ? 0 : -1;
}

static void post_touch(uint32_t type, uint32_t id, uint32_t x, uint32_t y) {
    typedef void (*fn_t)(uint64_t type, uint64_t id, uint64_t x, uint64_t y);
    ((fn_t)KERN_REDWOOD_TOUCH_POST)((uint64_t)type, (uint64_t)id, (uint64_t)x, (uint64_t)y);
}

typedef uintptr_t (*ml_io_map_fn)(uint64_t phys, uint64_t size);
typedef uint32_t (*rpmh_res_fn)(const char *name, uint16_t *len, uint32_t count, uint32_t *offset);
typedef bool (*rpmh_wr_fn)(uint32_t count, const uint32_t *addrs, const uint32_t *vals);

static void spmi_enable_boost(ml_io_map_fn io_map) {
    uintptr_t arb_core = io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
    uintptr_t arb_cnfg = io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
    if (!arb_core || !arb_cnfg) return;

    uint16_t ppid = ((uint16_t)2 << 8) | (0xc444 >> 8);
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
    if (apid < 0) return;

    ((void (*)(void))KERN_SPMI_LOCK)();
    uintptr_t ch = io_map(ARB_CHNLS_PHYS + (uint32_t)apid * 0x10000UL, 0x1000UL);
    if (ch) {
        // C444 = 0x80 (drive high)
        wr32(ch + 0x10, 0x80);
        wr32(ch + 0x00, ((uint32_t)(0x44) << 4));
        for (int t = 0; t < 2000 && !(rd32(ch + 0x08) & 1); t++) delay_us(2);

        // C446 = 0x80 (output enable)
        wr32(ch + 0x10, 0x80);
        wr32(ch + 0x00, ((uint32_t)(0x46) << 4));
        for (int t = 0; t < 2000 && !(rd32(ch + 0x08) & 1); t++) delay_us(2);
    }
    ((void (*)(void))KERN_SPMI_UNLOCK)();
    delay_us(4000);
}

void __attribute__((section(".text.umi_touch_thread"))) umi_touch_thread(void *arg) {
    (void)arg;
    klog("umi_touch: starting pure UniXNU FTS521 driver (open-drain)\n", 0, 0, 0, 0, 0, 0);

    umi_touch_ctx_t ctx;
    for (int f = 0; f < 10; f++) {
        ctx.contacts[f].down = false;
        ctx.contacts[f].x = 0;
        ctx.contacts[f].y = 0;
    }

    ctx.tlmm = ((ml_io_map_fn)KERN_ML_IO_MAP)(TLMM_PHYS, TLMM_SIZE);
    if (!ctx.tlmm) return;

    // 1. Hold reset LOW
    wr32(tlmm_io(ctx.tlmm, PIN_RESET), 0);
    wr32(tlmm_cfg(ctx.tlmm, PIN_RESET), (1u << 9) | (2 << 6) | 3);

    // 2. Power up pm8150a_l1 (1.8V VDD) via RPMh
    uintptr_t *rsc_cache = (uintptr_t *)KERN_RSC_CACHE_PTR;
    if (!*rsc_cache) *rsc_cache = ((ml_io_map_fn)KERN_ML_IO_MAP)(0x18220000UL, 0x10000UL);
    rpmh_res_fn rpmh_res = (rpmh_res_fn)KERN_RPMH_RESOURCE;
    rpmh_wr_fn  rpmh_wr  = (rpmh_wr_fn)KERN_RPMH_WRITE;
    uint16_t rlen = 0; uint32_t roff = 0;
    uint32_t ldoc1_addr = rpmh_res("ldoc1", &rlen, 1, &roff);
    if (!ldoc1_addr) ldoc1_addr = rpmh_res("ldoc2", &rlen, 1, &roff);
    if (ldoc1_addr) {
        uint32_t addrs[3] = { ldoc1_addr, ldoc1_addr + 4, ldoc1_addr + 8 };
        uint32_t vals[3]  = { 1800, 1, 7 };
        rpmh_wr(3, addrs, vals);
    }
    delay_ms(15);

    // 3. Enable PM8150B GPIO5 Boost (VDD boost)
    spmi_enable_boost((ml_io_map_fn)KERN_ML_IO_MAP);

    // 4. Enable AVDD (GPIO 69)
    wr32(tlmm_io(ctx.tlmm, PIN_AVDD), 2);
    wr32(tlmm_cfg(ctx.tlmm, PIN_AVDD), (1u << 9) | (2 << 6) | 3);
    delay_ms(10);

    // 5. Hardware Reset: HIGH, LOW 10ms, HIGH, wait 150ms
    wr32(tlmm_io(ctx.tlmm, PIN_RESET), 2);
    delay_ms(5);
    wr32(tlmm_io(ctx.tlmm, PIN_RESET), 0);
    delay_ms(10);
    wr32(tlmm_io(ctx.tlmm, PIN_RESET), 2);
    delay_ms(150);

    // 6. Configure INT pin (GPIO 39) as input with pull-up
    wr32(tlmm_cfg(ctx.tlmm, PIN_INT), 3 | (3 << 6)); // pull-up, 8mA

    // 7. Setup open-drain I2C pins (GPIO 36 SDA, GPIO 37 SCL)
    ctx.sda_ctl = rd32(tlmm_cfg(ctx.tlmm, PIN_SDA)) & ~(0x3cu | (1u << 9) | 3u | (7u << 6));
    ctx.scl_ctl = rd32(tlmm_cfg(ctx.tlmm, PIN_SCL)) & ~(0x3cu | (1u << 9) | 3u | (7u << 6));
    if (ctx.sda_ctl & (1u << 11)) ctx.sda_ctl |= (1u << 12);
    if (ctx.scl_ctl & (1u << 11)) ctx.scl_ctl |= (1u << 12);

    wr32(tlmm_io(ctx.tlmm, PIN_SDA), 0);
    wr32(tlmm_io(ctx.tlmm, PIN_SCL), 0);
    line_sda(&ctx, true);
    line_scl(&ctx, true);
    delay_ms(10);

    // 8. Drain boot FIFO
    uint8_t ev[8];
    for (int i = 0; i < 16; i++) {
        if (fts_read_fifo(&ctx, ev) < 0 || ev[0] == 0) break;
        delay_us(2000);
    }

    // 9. Official ST FTS521 Sensing Startup Sequence (from UmiFtsSensing.h)
    const uint8_t cmd_on[3]      = { 0xa0, 0x00, 0x01 };
    const uint8_t cmd_reports[6] = { 0xa2, 0x03, 0x00, 0x00, 0x00, 0x03 };
    const uint8_t cmd_off[3]     = { 0xa0, 0x00, 0x00 };

    bool s1 = fts_write(&ctx, cmd_on, 3);
    delay_ms(10);
    bool s2 = fts_write(&ctx, cmd_reports, 6);
    delay_ms(10);
    bool s3 = fts_write(&ctx, cmd_off, 3);
    delay_ms(50);
    bool s4 = fts_write(&ctx, cmd_on, 3);
    delay_ms(10);

    klog("umi_touch: Sensing started (s1=%u s2=%u s3=%u s4=%u)\n",
         (uint64_t)s1, (uint64_t)s2, (uint64_t)s3, (uint64_t)s4, 0, 0);

    uint32_t loop_count = 0;
    uint32_t total_points = 0;

    while (1) {
        loop_count++;
        bool irq = gpio_get(ctx.tlmm, PIN_INT); // 0 = touch active / event pending

        // Read FIFO when INT is LOW (event ready) or periodically every 100ms
        if (!irq || (loop_count % 12) == 0) {
            for (int drained = 0; drained < 8; drained++) {
                if (fts_read_fifo(&ctx, ev) < 0 || ev[0] == 0) break;

                uint8_t kind = ev[0] >> 4;
                uint8_t touch_id = ev[1] >> 4;

                if (kind == 1 || kind == 2) {
                    // Coordinate unpacking
                    uint32_t rx = (((uint32_t)ev[3] & 0x0F) << 8) | (uint32_t)ev[2];
                    uint32_t ry = ((uint32_t)ev[4] << 4) | ((uint32_t)ev[3] >> 4);
                    if (rx > 1079) rx = 1079;
                    if (ry > 2339) ry = 2339;

                    uint32_t nx = (rx * 65535) / 1079;
                    uint32_t ny = (ry * 65535) / 2339;

                    if (touch_id < 10) {
                        uint32_t type = (kind == 1) ? REDWOOD_TOUCH_DOWN : REDWOOD_TOUCH_MOVE;
                        post_touch(type, touch_id, nx, ny);
                        ctx.contacts[touch_id].down = true;
                        ctx.contacts[touch_id].x = nx;
                        ctx.contacts[touch_id].y = ny;
                        total_points++;
                        klog("umi_touch: TOUCH %u id=%u at (%u, %u)\n",
                             (uint64_t)kind, (uint64_t)touch_id, (uint64_t)rx, (uint64_t)ry, 0, 0);
                    }
                } else if (kind == 3) {
                    // Touch UP
                    if (touch_id < 10 && ctx.contacts[touch_id].down) {
                        post_touch(REDWOOD_TOUCH_UP, touch_id, ctx.contacts[touch_id].x, ctx.contacts[touch_id].y);
                        ctx.contacts[touch_id].down = false;
                        klog("umi_touch: TOUCH UP id=%u\n", (uint64_t)touch_id, 0, 0, 0, 0, 0);
                    } else {
                        for (int f = 0; f < 10; f++) {
                            if (ctx.contacts[f].down) {
                                post_touch(REDWOOD_TOUCH_UP, f, ctx.contacts[f].x, ctx.contacts[f].y);
                                ctx.contacts[f].down = false;
                            }
                        }
                    }
                }

                if ((ev[7] & 0x1f) == 0) break;
            }
        }

        // Heartbeat every ~2.5s (300 * 8.3ms)
        if ((loop_count % 300) == 1) {
            uint32_t sda_val = gpio_get(ctx.tlmm, PIN_SDA);
            uint32_t scl_val = gpio_get(ctx.tlmm, PIN_SCL);
            uint32_t hb = loop_count / 300;
            klog("umi_touch: [hb #%u] int=%u sda=%u scl=%u pts=%u\n",
                 (uint64_t)hb, (uint64_t)irq, (uint64_t)sda_val, (uint64_t)scl_val, (uint64_t)total_points, 0);
        }

        delay_us(8333); // 120 Hz
    }
}
