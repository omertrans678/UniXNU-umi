#ifndef UMI_RESET_H
#define UMI_RESET_H

#include <stdint.h>

/* Pre-map SPMI, PS_HOLD, and IMEM pages while ml_io_map is safe. */
void umi_reset_init(void);

/*
 * Force shutdown (mode=0), normal cold reset (mode=1), or fastboot reset (mode=2).
 * Safe to call from panic context (zero allocations, zero locks, pre-mapped MMIO).
 */
void umi_reset_board(unsigned mode);

#endif /* UMI_RESET_H */
