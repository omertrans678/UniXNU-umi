// SPDX-License-Identifier: MIT
// Observe existing state only. No device access, process traversal or policy changes.
#ifndef UMI_RUNTIME_TRACE_H
#define UMI_RUNTIME_TRACE_H
#if UMI_RUNTIME_TRACE
#include "UmiRuntimeWire.h"
extern unsigned umi_console_lock;
#include <mach/machine.h>
extern volatile uint32_t armcpu_smp_requested,armcpu_smp_running,armcpu_smp_conduit;
extern volatile uint32_t armcpu_boot_mpidr,armcpu_gic_boot[8],armcpu_gic_stride,armcpu_registered;
extern volatile uint64_t armcpu_ipi_sent,armcpu_ipi_interrupts,armcpu_ipi_selftest_sent;
extern volatile uint32_t armcpu_smp_step[8],armcpu_smp_mpidr[8],armcpu_smp_seen_mpidr[8],armcpu_smp_fail[8];
extern volatile int32_t armcpu_smp_psci_rc[8];
extern volatile uint32_t armcpu_core_timer[8],armcpu_core_sgi[8];
extern int umi_initproc_main_state(void);
extern volatile int sheng_watchdog_policy_status;
extern volatile int native_owner_data_status,native_owner_data_stage,native_owner_data_readonly;
extern volatile uint32_t native_ram_block_ready;
extern volatile uint64_t native_ram_block_reads,native_ram_block_writes,native_ram_block_failures;
extern volatile int native_mobile_graphics_load_status;
extern volatile uint32_t native_legacy_init_result,native_legacy_start_result;
extern volatile int native_legacy_surface_status;
extern volatile uint32_t native_legacy_surface_found,native_legacy_registration_calls;
extern volatile uint32_t native_legacy_gop_width,native_legacy_gop_height;
volatile uint64_t umi_runtime_trace_observation[UMI_RUNTIME_FIELDS];
static unsigned umi_runtime_trace_next;
static void umi_runtime_trace_poll(unsigned now) {
  const unsigned deadlines[]={10,30,60,90,120,180,240,300};
  unsigned sample=umi_runtime_trace_next;
  if(sample>=UMI_RUNTIME_SAMPLES || now<deadlines[sample])return;
  uint64_t v[UMI_RUNTIME_FIELDS]={2,sample+1,now,(unsigned)nprocs,
    vm_page_free_count,vm_page_active_count,vm_page_inactive_count,vm_page_wire_count,
    (uint32_t)umi_initproc_main_state(),(uint32_t)sheng_watchdog_policy_status,
    (uint32_t)native_owner_data_stage,(uint32_t)native_owner_data_status,(uint32_t)native_owner_data_readonly,
    native_ram_block_ready,BP_OPTIONAL(native_ram_block_bytes),native_ram_block_reads,native_ram_block_writes,
    native_ram_block_failures,BP_OPTIONAL(native_ram_block_resident_bytes),(uint32_t)native_mobile_graphics_load_status,
    native_legacy_stage,native_legacy_init_result,native_legacy_start_result,
    (uint32_t)native_legacy_surface_status,native_legacy_surface_found,native_legacy_registration_calls,
    native_legacy_client_calls,(uint32_t)native_legacy_client_pid,(uint32_t)native_legacy_client_status,
    native_legacy_transfer_calls,(uint32_t)native_legacy_transfer_status,native_legacy_retirement_calls,
    native_legacy_prepare_calls,native_legacy_prepare_reason,native_legacy_prepare_attempts,
    boot_progress_display_owned,native_legacy_guard_slot,process_worker_status,
    BP_OPTIONAL(boot_process_passes),BP_OPTIONAL(boot_process_busy),
    0,0,0,native_legacy_gop_bytes,native_legacy_gop_width,native_legacy_gop_height,
    BP_OPTIONAL(boot_process_seen),0};
  if(&boot_process_rows)for(unsigned i=0;i<10;++i)memcpy(v+48+i*8,boot_process_rows[i],64);
  // Do not wait on the ordinary-exit observer. An unavailable snapshot is explicit.
  if(!__atomic_exchange_n(&exit_lock,1,__ATOMIC_ACQUIRE)) {
    v[40]=exit_records.total;v[41]=exit_records.failed;v[42]=exit_records.crashes;
    memcpy(v+128,exit_records.failures,sizeof(exit_records.failures));
    __atomic_store_n(&exit_lock,0,__ATOMIC_RELEASE);v[47]=1;
  }
  // Asynchronous observations, not a coherent scheduler snapshot. Original
  // aggregate counters are advisory; per-core events and XNU's online count
  // distinguish actual SMP operation from a configured eight-core tree.
  const uint64_t smp[]={armcpu_smp_requested,armcpu_smp_running,armcpu_smp_conduit,
    armcpu_boot_mpidr,armcpu_gic_boot[3],armcpu_gic_stride,armcpu_registered,
    armcpu_ipi_sent,armcpu_ipi_interrupts,armcpu_ipi_selftest_sent,
    machine_info.logical_cpu,machine_info.logical_cpu_max,
    machine_info.physical_cpu,machine_info.physical_cpu_max,0,0};
  memcpy(v+176,smp,sizeof(smp));
  for(unsigned i=0;i<8;++i) {
    const uint64_t core[]={armcpu_smp_step[i],armcpu_smp_mpidr[i],armcpu_smp_seen_mpidr[i],
      armcpu_smp_fail[i],(uint32_t)armcpu_smp_psci_rc[i],armcpu_core_timer[i],armcpu_core_sgi[i]};
    memcpy(v+192+i*7,core,sizeof(core));
  }
  for(unsigned i=0;i<UMI_RUNTIME_FIELDS;++i)umi_runtime_trace_observation[i]=v[i];
  if(__atomic_exchange_n(&umi_console_lock,1,__ATOMIC_ACQUIRE))return;
  uint64_t daif;
  __asm__ volatile("mrs %0,daif\n\tmsr daifset,#3" : "=r"(daif) :: "memory");
  __asm__ volatile("dsb sy" ::: "memory");
  int written=umi_runtime_write((volatile uint32_t *)umi_trace_console_address,sample+1,v);
  __asm__ volatile("dsb sy\n\tmsr daif,%0" :: "r"(daif) : "memory");
  __atomic_store_n(&umi_console_lock,0,__ATOMIC_RELEASE);
  if(written)++umi_runtime_trace_next;
}
#else
static void umi_runtime_trace_poll(unsigned now) {(void)now;}
#endif
#endif
