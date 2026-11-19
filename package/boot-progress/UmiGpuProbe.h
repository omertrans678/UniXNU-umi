// SPDX-License-Identifier: MIT
// Opt-in hardware probe & full bringup for umi SM8250 Adreno 650 GPU/GMU/SMMU/GPUCC/cmd-db.
#ifndef UMI_GPU_PROBE_H
#define UMI_GPU_PROBE_H

#include <stdint.h>
#include <stddef.h>
#include <pexpert/pexpert.h>
#include "UmiBootLog.h"

extern void umi_adreno_bringup(unsigned mode);

static void umi_gpu_probe(void) {
  unsigned enabled_probe = 0;
  if (!PE_parse_boot_argn("umi-gp", &enabled_probe, sizeof(enabled_probe)) || !enabled_probe)
    return;
  umi_adreno_bringup(enabled_probe);
}

#endif /* UMI_GPU_PROBE_H */
