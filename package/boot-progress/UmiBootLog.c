/*
 * Persistent panic & console log writer for umi (last 32 MiB of userdata).
 * Zero heap allocations, zero locks on panic path.
 */
#include "UmiBootLog.h"
#include "UmiReset.h"
#include <stddef.h>
#include <stdint.h>

extern int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
extern char *debug_buf_base __attribute__((weak_import));
extern unsigned int debug_buf_size __attribute__((weak_import));
extern char boot_progress_panic_text[256];
extern void boot_progress_emergency_banner(const char *tag, uint64_t a, uint64_t b);

extern int native_ufs_log_ready(void) __attribute__((weak_import));
extern int native_ufs_log_write_panic(uint64_t rel_block, uint32_t blocks, const void *src) __attribute__((weak_import));

#define LOG_RING_SIZE 65536U
#define DEBUG_MAX_SIZE 65536U
#define STAGE_BUF_SIZE (4096U + LOG_RING_SIZE + DEBUG_MAX_SIZE)

struct umi_log_header {
  char magic[8];           /* "UMILOG01" */
  uint32_t version;        /* 1 */
  uint32_t header_bytes;   /* 4096 */
  uint64_t boot_ticks;
  uint64_t panic_ticks;
  uint64_t panic_a;
  uint64_t panic_b;
  uint32_t ring_bytes;
  uint32_t debug_bytes;
  uint32_t total_ring_written;
  uint32_t ufs_ready;
  char panic_text[256];
  uint8_t reserved[4096 - 8 - 4 - 4 - 8 * 4 - 4 * 4 - 256];
};
_Static_assert(sizeof(struct umi_log_header) == 4096, "umi_log_header must be 4 KiB");

static char g_ring[LOG_RING_SIZE];
static volatile uint32_t g_ring_pos;
static volatile uint64_t g_panic_a;
static volatile uint64_t g_panic_b;
static volatile int g_panic_done;
static uint8_t g_stage_buf[STAGE_BUF_SIZE] __attribute__((aligned(4096)));

static void delay_ms(uint64_t ms) {
  uint64_t freq = 0, start = 0, cur = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  if (!freq) freq = 19200000ULL;
  uint64_t ticks = (freq * ms) / 1000ULL;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
  do {
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cur));
  } while ((cur - start) < ticks);
}

void umi_boot_log_putc(char c) {
  uint32_t pos = __atomic_fetch_add(&g_ring_pos, 1, __ATOMIC_RELAXED);
  g_ring[pos % LOG_RING_SIZE] = c;
}

void umi_boot_log_line(const char *tag, const char *text) {
  if (tag) {
    for (const char *p = tag; *p; ++p) umi_boot_log_putc(*p);
    umi_boot_log_putc(':');
    umi_boot_log_putc(' ');
  }
  if (text) {
    for (const char *p = text; *p; ++p) umi_boot_log_putc(*p);
  }
  umi_boot_log_putc('\n');
}

void umi_boot_log_record_panic(uint64_t a, uint64_t b, const char *text) {
  if (!g_panic_a) g_panic_a = a;
  if (!g_panic_b) g_panic_b = b;
  if (text && text[0]) {
    umi_boot_log_line("PANIC", text);
  }
}

static int umi_boot_log_write_stage(void) {
  for (size_t i = 0; i < STAGE_BUF_SIZE; ++i) g_stage_buf[i] = 0;

  struct umi_log_header *hdr = (struct umi_log_header *)g_stage_buf;
  const char magic[8] = {'U','M','I','L','O','G','0','1'};
  for (unsigned i = 0; i < 8; ++i) hdr->magic[i] = magic[i];
  hdr->version = 1;
  hdr->header_bytes = 4096;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(hdr->panic_ticks));
  hdr->panic_a = g_panic_a;
  hdr->panic_b = g_panic_b;

  uint32_t total_written = __atomic_load_n(&g_ring_pos, __ATOMIC_RELAXED);
  hdr->total_ring_written = total_written;
  uint32_t ring_len = total_written < LOG_RING_SIZE ? total_written : LOG_RING_SIZE;
  hdr->ring_bytes = ring_len;
  uint8_t *ring_dst = g_stage_buf + 4096U;
  if (total_written <= LOG_RING_SIZE) {
    for (uint32_t i = 0; i < ring_len; ++i) ring_dst[i] = (uint8_t)g_ring[i];
  } else {
    uint32_t start = total_written % LOG_RING_SIZE;
    for (uint32_t i = 0; i < LOG_RING_SIZE; ++i) {
      ring_dst[i] = (uint8_t)g_ring[(start + i) % LOG_RING_SIZE];
    }
  }

  uint32_t dbg_len = 0;
  if (&debug_buf_base && debug_buf_base) {
    uint32_t max_dbg = (&debug_buf_size && debug_buf_size) ? debug_buf_size : DEBUG_MAX_SIZE;
    if (max_dbg > DEBUG_MAX_SIZE) max_dbg = DEBUG_MAX_SIZE;
    uint8_t *dbg_dst = g_stage_buf + 4096U + LOG_RING_SIZE;
    for (uint32_t i = 0; i < max_dbg; ++i) {
      char ch = debug_buf_base[i];
      if (!ch) break;
      dbg_dst[i] = (uint8_t)ch;
      dbg_len = i + 1;
    }
  }
  hdr->debug_bytes = dbg_len;

  for (unsigned i = 0; i < 255 && boot_progress_panic_text[i]; ++i) {
    hdr->panic_text[i] = boot_progress_panic_text[i];
  }

  int saved = 0;
  if (&native_ufs_log_ready && native_ufs_log_ready && native_ufs_log_ready() &&
      &native_ufs_log_write_panic && native_ufs_log_write_panic) {
    hdr->ufs_ready = 1;
    uint32_t total_bytes = 4096U + LOG_RING_SIZE + DEBUG_MAX_SIZE;
    uint32_t blocks = (total_bytes + 4095U) / 4096U;
    saved = native_ufs_log_write_panic(0, blocks, g_stage_buf);
  }

  return saved;
}

void umi_boot_log_flush_snapshot(void) {
  if (__atomic_load_n(&g_panic_done, __ATOMIC_ACQUIRE)) return;
  umi_boot_log_write_stage();
}

void umi_boot_log_panic_flush_and_reset(void) {
  if (__atomic_exchange_n(&g_panic_done, 1, __ATOMIC_ACQ_REL)) return;

  unsigned reset_mode = 2; /* 0 = force shutdown, 1 = cold reset, 2 = fastboot (default), 9 = stay */
  uint32_t arg = 0;
  if (PE_parse_boot_argn("umi-panic-reset", &arg, sizeof(arg))) {
    reset_mode = arg;
  }

  int saved = umi_boot_log_write_stage();
  struct umi_log_header *hdr = (struct umi_log_header *)g_stage_buf;

  boot_progress_emergency_banner(saved ? "LOG_SAVED" : "UFS_NA",
                                 (uint64_t)hdr->ring_bytes,
                                 (uint64_t)hdr->debug_bytes);

  if (reset_mode == 9) return;
  delay_ms(1500);
  umi_reset_board(reset_mode);
}
