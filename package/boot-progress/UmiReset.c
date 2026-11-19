/*
 * umi PM8150 primary PON / Qualcomm PS_HOLD reset & shutdown for XNU.
 * Ported from uefi-umi UmiResetSystem.c and UmiSpmi.c.
 */
#include "UmiReset.h"
#include <stddef.h>

extern uintptr_t ml_io_map(uint64_t phys_addr, uint64_t size);

static uintptr_t g_ps_hold_va;
static uintptr_t g_imem_va;
static uintptr_t g_spmi_wr_va;
static uintptr_t g_spmi_rd_va;
static unsigned g_spmi_rd_off;
static int g_spmi_owner = -1;
static int g_reset_ready;

static inline uint32_t mmio_r32(uintptr_t addr) {
  uint32_t v = *(volatile uint32_t *)addr;
  __asm__ volatile("dmb ld" ::: "memory");
  return v;
}

static inline void mmio_w32(uintptr_t addr, uint32_t val) {
  __asm__ volatile("dmb st" ::: "memory");
  *(volatile uint32_t *)addr = val;
}

static void udelay(uint64_t us) {
  uint64_t freq = 0, start = 0, cur = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  if (!freq) freq = 19200000ULL;
  uint64_t ticks = (freq * us) / 1000000ULL;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
  do {
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cur));
  } while ((cur - start) < ticks);
}

static int scm_call(uint32_t fn, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t *res) {
  register uint64_t x0 __asm__("x0") = fn;
  register uint64_t x1 __asm__("x1") = arginfo;
  register uint64_t x2 __asm__("x2") = a;
  register uint64_t x3 __asm__("x3") = b;
  register uint64_t x4 __asm__("x4") = 0;
  register uint64_t x5 __asm__("x5") = 0;
  register uint64_t x6 __asm__("x6") = 0;
  for (unsigned retry = 0; retry < 32; ++retry) {
    __asm__ volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6)
                     :
                     : "memory");
    if (x0 != 1) break; /* SCM_INTERRUPTED */
    x0 = fn;
  }
  if (res) *res = x1;
  return x0 == 0;
}

static int scm_avail(uint32_t mask, uint32_t fn) {
  uint64_t res = 0;
  return scm_call(0x02000601U | mask, 1, fn, 0, &res) && (res != 0);
}

static int spmi_xfer(uint16_t addr, uint8_t *val, int write) {
  if (!val) return 0;
  if (write && g_spmi_owner != 0) return 0;
  uintptr_t ch = write ? g_spmi_wr_va : (g_spmi_rd_va + g_spmi_rd_off);
  if (!ch) return 0;
  if (write) mmio_w32(ch + 0x10, *val);
  mmio_w32(ch, (write ? 0U : (1U << 27)) | ((uint32_t)(addr & 0xFFU) << 4));
  for (unsigned i = 0; i < 1000; ++i) {
    uint32_t st = mmio_r32(ch + 0x08);
    if (st & 1U) {
      if (st & (2U | 4U | 8U)) return 0;
      if (!write) *val = (uint8_t)mmio_r32(ch + 0x18);
      return 1;
    }
    udelay(1);
  }
  return 0;
}

static int pon_write_verify(uint16_t addr, uint8_t val) {
  uint8_t actual = 0;
  if (!spmi_xfer(addr, &val, 1)) return 0;
  if (!spmi_xfer(addr, &actual, 0)) return 0;
  return actual == val;
}

void umi_reset_init(void) {
  if (g_reset_ready) return;
  g_ps_hold_va = ml_io_map(0x0C264000ULL, 0x1000);
  g_imem_va = ml_io_map(0x146BF000ULL, 0x1000);
  uintptr_t spmi_core = ml_io_map(0x0C440000ULL, 0x2000);
  uintptr_t spmi_owner = ml_io_map(0x0C40A000ULL, 0x2000);
  if (!g_ps_hold_va || !g_imem_va || !spmi_core || !spmi_owner) return;
  if (mmio_r32(spmi_core) != 0x50010000U) return;

  /* Find APID for Sid=0, PON PPID=0x008 (covers 0x0801..0x088F). */
  unsigned apid = 512;
  int sel_owner = 7;
  for (unsigned idx = 0; idx < 512; ++idx) {
    uint32_t map = mmio_r32(spmi_core + 0x0900 + idx * 4);
    if (!map || (((map >> 8) & 0xFFFU) != 0x008U)) continue;
    int owner = (int)(mmio_r32(spmi_owner + 0x0700 + idx * 4) & 7U);
    if (apid == 512 || owner == 0) {
      apid = idx;
      sel_owner = owner;
    }
  }
  if (apid == 512) return;
  g_spmi_owner = sel_owner;
  uint64_t wr_phys = 0x0C600000ULL + (uint64_t)apid * 0x10000ULL;
  uint64_t rd_phys = 0x0E600000ULL + (uint64_t)apid * 0x80ULL;
  g_spmi_wr_va = ml_io_map(wr_phys, 0x1000);
  g_spmi_rd_va = ml_io_map(rd_phys & ~0xFFFULL, 0x1000);
  g_spmi_rd_off = (unsigned)(rd_phys & 0xFFFULL);
  if (g_spmi_wr_va && g_spmi_rd_va) {
    g_reset_ready = 1;
  }
}

void umi_reset_board(unsigned mode) {
  if (!g_reset_ready) return;
  uint8_t type = (mode == 0) ? 4 : 7; /* 4=Shutdown, 7=Cold reset */
  uint32_t imem_reason = (mode == 2) ? 0x77665502U : 0x77665501U;
  uint8_t pon_reason = (mode == 2) ? 0x02U : 0x40U; /* 0x02=recovery(TWRP), 0x40=normal */

  __asm__ volatile("msr daifset, #0xf" ::: "memory");

  uint8_t rev = 0, subtype = 0, ctl = 0, en = 0, spare = 0;
  if (!spmi_xfer(0x0801, &rev, 0) ||
      !spmi_xfer(0x0805, &subtype, 0) ||
      !spmi_xfer(0x085A, &ctl, 0) ||
      !spmi_xfer(0x085B, &en, 0) ||
      !spmi_xfer(0x088F, &spare, 0)) {
    return;
  }
  uint32_t ps_hold = mmio_r32(g_ps_hold_va);
  if (subtype != 4 || ps_hold != 1) return;

  uint64_t res = 0;
  uint32_t mask = 0;
  if (scm_call(0xC2000601U, 1, 0x82000601U, 0, &res) && res != 0) {
    mask = 0x40000000U;
  } else if (scm_call(0x82000601U, 1, 0x82000601U, 0, &res) && res != 0) {
    mask = 0;
  } else {
    return;
  }
  if (!scm_avail(mask, 0x02000902U)) return;
  int native_dload = scm_avail(mask, 0x02000110U);
  if (!native_dload && !scm_avail(mask, 0x02000502U)) return;
  int sdi = scm_avail(mask, 0x02000109U);

  if (!pon_write_verify(0x085B, en & (uint8_t)~0x80U)) goto rollback;
  udelay(500);
  if (!pon_write_verify(0x085A, (ctl & 0xF0U) | type)) goto rollback;
  if (!pon_write_verify(0x085B, en | 0x80U)) goto rollback;
  if (!pon_write_verify(0x088F, (spare & 0x01U) | pon_reason)) goto rollback;

  if (native_dload) {
    if (!scm_call(0x82000110U | mask, 2, 0, 0, &res)) goto rollback;
  } else {
    if (!scm_call(0x82000502U | mask, 2, 0x01FD3000U, 0, &res)) goto rollback;
  }
  if (sdi && !scm_call(0x82000109U | mask, 2, 1, 0, &res)) goto rollback;

  mmio_w32(g_imem_va + 0x65CU, imem_reason);
  __asm__ volatile("dsb sy" ::: "memory");
  scm_call(0x82000902U | mask, 1, 0, 0, &res);
  mmio_w32(g_ps_hold_va, 0);
  for (;;) {
    __asm__ volatile("wfe");
  }

rollback:
  pon_write_verify(0x085B, en & (uint8_t)~0x80U);
  udelay(500);
  pon_write_verify(0x085A, ctl);
  pon_write_verify(0x088F, spare);
  pon_write_verify(0x085B, en);
}
