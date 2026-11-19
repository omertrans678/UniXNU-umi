#ifndef UMI_BOOT_LOG_H
#define UMI_BOOT_LOG_H

#include <stdint.h>

void umi_boot_log_putc(char c);
void umi_boot_log_line(const char *tag, const char *text);
void umi_boot_log_record_panic(uint64_t a, uint64_t b, const char *text);
void umi_boot_log_flush_snapshot(void);
void umi_boot_log_panic_flush_and_reset(void);

#endif /* UMI_BOOT_LOG_H */
