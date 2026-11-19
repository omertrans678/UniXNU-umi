// SPDX-License-Identifier: BSD-2-Clause-Patent
// umi (SM8250) CPU frequency request through the EPSS frequency domains ("qcom,cpufreq-hw-epss", umi DTB
// /soc/qcom,cpufreq-hw: freq-domain0..2 at 0x18591000/0x18592000/0x18593000, 0x1000 each). Without an OS cpufreq
// driver the clusters stay at whatever level UEFI left, which makes every CPU-bound boot step (dyld, objc, CA, the
// Turnip compiler in backboardd) slower. Same register layout as the qcom-cpufreq-hw "epss" data used by sheng's
// CpuFreqProbe: enable 0x0, domain state 0x20, dcvs ctrl 0xb0, frequency LUT 0x100 (4-byte rows), perf state 0x320.
// LUT row: bits 31:30 source, 18:16 core count (1 = turbo), 7:0 L value (multiple of 19.2 MHz; source 0 = GPLL0/2).
// Writing a perf-state index is exactly what Linux does; the EPSS firmware applies voltage for that level.
//
// umi-cpufreq=<n> (default 3): 0 off, 1 report only, 2 request the highest non-turbo level in domain 0,
// 3 the same in every enabled domain, 4 (umi-178 trial) also turns on the hardware request of domains UEFI left disabled
// (domains 1 and 2, the gold and prime clusters, read enable 0 and stay at ~1.47/1.73 GHz) before requesting their level.
#include <stdint.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>
#include <kern/misc_protos.h>
#include "UmiBootLog.h"

char native_cpufreq_row[200];
extern void IODelay(unsigned int microseconds);
volatile uint32_t native_cpufreq_stage;

static const uint64_t kDomainBase[3] = {0x18591000, 0x18592000, 0x18593000};
#define kDomainSize 0x1000
#define kRegEnable 0x0
#define kRegDomainState 0x20
#define kRegDcvsCtrl 0xb0
#define kRegFreqLut 0x100
#define kRegPerfState 0x320
#define kLutEntries 40
#define kXoKHz 19200u
#define kGpllHalfKHz 300000u

static uint32_t rd32(uintptr_t base, uint32_t reg) { return *(volatile uint32_t *)(base + reg); }
static void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
  *(volatile uint32_t *)(base + reg) = value;
  __asm__ volatile("dsb sy" ::: "memory");
}
static uint32_t lut_khz(uint32_t row) { return (row >> 30) ? kXoKHz * (row & 0xff) : kGpllHalfKHz; }

// Relative core speed without the PMU: on every CPU, count iterations of a 32-long dependent add chain for 2 ms of
// the architectural counter. Iterations scale with the core clock; the report scales them to CPU0's known level.
typedef void (*umi_broadcast_fn)(void *);
extern unsigned int cpu_broadcast_xcall_simple(boolean_t, umi_broadcast_fn, void *);
extern int cpu_number(void);
static volatile uint64_t umi_cpu_iters[8];
static volatile uint32_t umi_cpu_aff[8];
static void umi_cpu_speed(void *arg) {
  (void)arg;
  int c = cpu_number();
  if (c < 0 || c >= 8) return;
  uint64_t f = 0, t0 = 0, t = 0, n = 0, x = 0, mp = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
  if (!f) f = 19200000;
  const uint64_t ticks = f / 500;
  __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(t0));
  do {
    __asm__ volatile(
      "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\t"
      "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\t"
      "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\t"
      "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1"
      : "+r"(x));
    ++n;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
  } while (t - t0 < ticks);
  __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mp));
  umi_cpu_iters[c] = n;
  umi_cpu_aff[c] = (uint32_t)(mp & 0xffffff);
}
static uint32_t umi_cpu0_khz_saved;
static uint32_t umi_best_level[3];
static void umi_cpu_speed_report(uint32_t cpu0_khz);
// Same measurement again late in boot (auto-recovery time): how many CPUs userspace actually had.
void umi_cpufreq_late(void) {
  if (native_cpufreq_row[0]) umi_boot_log_line("CPUFREQ", native_cpufreq_row);
  // umi-179: each domain's registers as they are now (enable, current level, perf request, dcvs control, LMh/limit at 0x330..)
  char row[200]; unsigned n = 0;
  for (unsigned d = 0; d < 3 && n < sizeof(row); ++d) {
    uintptr_t va = (uintptr_t)ml_io_map(kDomainBase[d], kDomainSize);
    if (!va) continue;
    n += (unsigned)snprintf(row + n, sizeof(row) - n, " d%u:en%x st%x c%u p%x dc%x 324:%x 328:%x 330:%x 334:%x", d,
                            rd32(va, kRegEnable), rd32(va, kRegDomainState), (rd32(va, kRegDomainState) & 0xff) * kXoKHz / 1000,
                            rd32(va, kRegPerfState), rd32(va, kRegDcvsCtrl), rd32(va, 0x324), rd32(va, 0x328), rd32(va, 0x330),
                            rd32(va, 0x334));
  }
  umi_boot_log_line("CPUREG", row);
  // umi-180: request the probe's levels again now that every domain is enabled (umi-179: d1/d2 sat at perf 0).
  unsigned mode = 3; PE_parse_boot_argn("umi-cpufreq", &mode, sizeof(mode));
  if (mode >= 4) {
    n = 0;
    for (unsigned d = 1; d < 3 && n < sizeof(row); ++d) {
      uintptr_t va = (uintptr_t)ml_io_map(kDomainBase[d], kDomainSize);
      if (!va || !umi_best_level[d]) continue;
      wr32(va, kRegPerfState, umi_best_level[d]);
      if (rd32(va, kRegDcvsCtrl) & 1) for (unsigned c = 1; c < 4; ++c) wr32(va, kRegPerfState + c * 4, umi_best_level[d]);
      IODelay(2000);
      n += (unsigned)snprintf(row + n, sizeof(row) - n, " d%u:p%x st%x c%u", d, rd32(va, kRegPerfState), rd32(va, kRegDomainState),
                              (rd32(va, kRegDomainState) & 0xff) * kXoKHz / 1000);
    }
    umi_boot_log_line("CPUSET", row);
  }
  if (umi_cpu0_khz_saved) umi_cpu_speed_report(umi_cpu0_khz_saved);
}
static void umi_cpu_speed_report(uint32_t cpu0_khz) {
  umi_cpu0_khz_saved = cpu0_khz;
  for (unsigned i = 0; i < 8; ++i) { umi_cpu_iters[i] = 0; umi_cpu_aff[i] = 0; }
  unsigned ran = cpu_broadcast_xcall_simple(TRUE, umi_cpu_speed, NULL);
  char row[200];
  unsigned n = (unsigned)snprintf(row, sizeof(row), "ran%u ref%u", ran, cpu0_khz / 1000);
  for (unsigned i = 0; i < 8 && n < sizeof(row); ++i) {
    uint64_t mhz = umi_cpu_iters[0] ? (uint64_t)cpu0_khz / 1000 * umi_cpu_iters[i] / umi_cpu_iters[0] : 0;
    n += (unsigned)snprintf(row + n, sizeof(row) - n, " c%u:%x/%llu~%lluMHz", i, umi_cpu_aff[i],
                            (unsigned long long)umi_cpu_iters[i], (unsigned long long)mhz);
  }
  umi_boot_log_line("CPUMHZ", row);
}

void umi_cpufreq_probe(void) {
  unsigned mode = 3;
  PE_parse_boot_argn("umi-cpufreq", &mode, sizeof(mode));
  if (!mode || native_cpufreq_stage) return;
  native_cpufreq_stage = 1;
  char *out = native_cpufreq_row;
  unsigned n = (unsigned)snprintf(out, sizeof(native_cpufreq_row), "m%u", mode);
  uint32_t cpu0_khz = 0;   // domain 0 (CPU0-3) level after this probe: the speed report's reference
  for (unsigned d = 0; d < 3; ++d) {
    uintptr_t va = (uintptr_t)ml_io_map(kDomainBase[d], kDomainSize);
    if (!va) { n += (unsigned)snprintf(out + n, sizeof(native_cpufreq_row) - n, " d%u:nomap", d); continue; }
    uint32_t enable = rd32(va, kRegEnable), state = rd32(va, kRegDomainState) & 0xff, perf = rd32(va, kRegPerfState);
    uint32_t dcvs = rd32(va, kRegDcvsCtrl);
    // Last distinct, non-turbo row before the table ends (two equal frequencies end it).
    int best = -1; uint32_t best_khz = 0, prev = 0;
    for (unsigned i = 0; i < kLutEntries; ++i) {
      uint32_t row = rd32(va, kRegFreqLut + i * 4);
      uint32_t khz = lut_khz(row);
      int turbo = ((row >> 16) & 7) == 1;
      if (i > 0 && khz == prev) break;
      if (!turbo && khz >= best_khz) { best = (int)i; best_khz = khz; }
      prev = khz;
    }
    int set = 0;
    if (mode >= 4 && !(enable & 1) && best >= 0) {
      wr32(va, kRegEnable, enable | 1);
      // umi-180: the readback right after the write is 2 (bit 0 not yet set); by late boot all domains read 3. Wait for it.
      for (unsigned w = 0; w < 100 && !(rd32(va, kRegEnable) & 1); ++w) IODelay(100);
      enable = rd32(va, kRegEnable);
      n += (unsigned)snprintf(out + n, sizeof(native_cpufreq_row) - n, " d%u:en>%x", d, enable);
    }
    if (mode >= 2 && (d == 0 || mode >= 3) && ((enable & 1) || mode >= 4) && best >= 0) {
      wr32(va, kRegPerfState, (uint32_t)best);
      if (dcvs & 1) for (unsigned c = 1; c < 4; ++c) wr32(va, kRegPerfState + c * 4, (uint32_t)best);
      set = 1;
    }
    if (best >= 0) umi_best_level[d] = (uint32_t)best;
    if (d == 0) cpu0_khz = set ? best_khz : state * kXoKHz;
    if (set) { for (unsigned w = 0; w < 100 && (rd32(va, kRegDomainState) & 0xff) * kXoKHz < best_khz; ++w) IODelay(100); }
    n += (unsigned)snprintf(out + n, sizeof(native_cpufreq_row) - n, " d%u:e%u c%u p%u m%d@%u%s>%u",
                            d, enable & 1, state * kXoKHz / 1000, perf, best, best_khz / 1000, set ? " SET" : "",
                            (rd32(va, kRegDomainState) & 0xff) * kXoKHz / 1000);
  }
  native_cpufreq_stage = 2;
  umi_boot_log_line("CPUFREQ", native_cpufreq_row);
  if (cpu0_khz) umi_cpu_speed_report(cpu0_khz);
}
