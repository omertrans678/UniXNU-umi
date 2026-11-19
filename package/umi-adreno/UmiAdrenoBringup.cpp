// SPDX-License-Identifier: BSD-2-Clause-Patent
// Full Adreno 650 (SM8250 / Xiaomi Mi 10 umi) bringup:
// Clocks, Command DB, GMU SMMU (CB1), GMU Cold Start + HFI, GX Power + RPMh DDR Vote,
// GPU SMMU (CB0 at 32 MiB+ outside GMEM), Linux v6.6 A650 hw_init + APRIV, SQE + Ring 0 + CP_ME_INIT + CP_MEM_WRITE,
// Zap Shader (PAS 13 @ 0x8681a000) + CP_SET_SECURE_MODE(0), GPU Interrupt (SPI 300 / INTID 332),
// NativeAdreno Device Publication + Kernel UserClient Self-Test (2 rounds) + AGXAccelerator (IOAcceleratorES) Registration.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
#include "GpuPowerBoard.h"
#include "A650Gmu.h"
void umi_boot_log_line(const char *tag, const char *text);

extern const uint8_t native_gpu_gmu_firmware[] __attribute__((weak_import));
extern const uint32_t native_gpu_gmu_firmware_size __attribute__((weak_import));
extern const uint8_t native_gpu_sqe_firmware[] __attribute__((weak_import));
extern const uint32_t native_gpu_sqe_firmware_size __attribute__((weak_import));
extern const uint8_t native_gpu_zap_firmware[] __attribute__((weak_import));
extern const uint32_t native_gpu_zap_firmware_size __attribute__((weak_import));
#include "AdrenoPM4.h"

extern uint64_t vm_first_phys, vm_last_phys;
boolean_t ml_set_interrupts_enabled(boolean_t);
boolean_t ml_get_interrupts_enabled(void);
boolean_t ml_at_interrupt_context(void);
int get_preemption_level(void);
IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);

void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
IOService *native_adreno_accelerator_allocate(void);
unsigned native_adreno_accelerator_instance_size(void);
kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern kmod_info_t g_kernel_kmod_info;

volatile uint64_t native_gpu_idle_sleeps, native_gpu_idle_spins;
}

#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoUserClient.h"

extern "C" {
NativeAdrenoVm *NativeAdrenoGpuContextVm();
IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
                               const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
uint32_t native_adreno_publish_stats(uint32_t *);
IOService *native_adreno_published_service();
/* Defined by the board, as rui12366B/UniXNU-gts7xlwifi package/GpuProbe.cpp does (the gts7 driver reads them). */
volatile uint32_t native_adreno_pool_stats[8];
struct NativeAdrenoMetalDiag {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];
    char reader[3][17];
    char name_reader[17];
    char starter[3][17];
    char report[64];
    char report2[64];
    uint32_t grants;
};
extern struct NativeAdrenoMetalDiag native_adreno_metal_diag;
extern volatile uint32_t native_adreno_probe_fail[5];
extern volatile uint32_t native_adreno_probe_wire[7];
struct NativeAdrenoClientText {char name[20]; char text[64];};
extern struct NativeAdrenoClientText native_adreno_client_text[4];
__attribute__((weak)) volatile uint32_t native_adreno_last_note_kind, native_adreno_last_note_code;
__attribute__((weak)) volatile uint64_t native_adreno_last_note_value;
extern volatile uint32_t native_adreno_priority_calls, native_adreno_priority_last, native_adreno_note_counts[2];   /* NativeAdrenoPublish.cpp */
extern volatile int native_gpu_addon_stage, native_gpu_addon_status, native_gpu_addon2_stage, native_gpu_addon2_status;
extern volatile int native_gpu_addon_files, native_gpu_addon_written, native_gpu_addon_same, native_gpu_addon_failed, native_gpu_addon_error;
extern volatile uint32_t native_user_exec_total __attribute__((weak_import)), native_user_exec_fail __attribute__((weak_import));
extern volatile int native_user_exec_adreno __attribute__((weak_import)), native_user_exec_bb __attribute__((weak_import)), native_user_exec_sb __attribute__((weak_import));
extern char native_user_exec_last[48] __attribute__((weak_import));
extern volatile int native_user_exec_first_err __attribute__((weak_import));
extern char native_user_exec_first_fail[64] __attribute__((weak_import));
extern volatile uint32_t native_p1_unix_cnt __attribute__((weak_import)), native_p1_mach_cnt __attribute__((weak_import));
extern volatile int32_t native_p1_last_sys __attribute__((weak_import)), native_p1_sys_ring[8] __attribute__((weak_import));
extern volatile uint64_t native_p1_last_pc __attribute__((weak_import));
extern volatile uint64_t native_p1_fcntl_fd __attribute__((weak_import)), native_p1_fcntl_cmd __attribute__((weak_import)), native_p1_fcntl_arg __attribute__((weak_import));
extern volatile uint64_t native_p1_max_ticks __attribute__((weak_import)), native_p1_total_ticks __attribute__((weak_import));
extern volatile int32_t native_p1_max_sys __attribute__((weak_import));
extern volatile int32_t native_p1_err_sys[4] __attribute__((weak_import));
extern volatile uint64_t native_p1_err_val[4] __attribute__((weak_import));
extern volatile uint32_t native_p1_err_cnt __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_accepted_count, native_legacy_swap_rejected_count, native_legacy_rej_reason __attribute__((weak_import));
extern volatile uint32_t native_t970_swapfmt[5] __attribute__((weak_import));
extern volatile uint32_t native_gop_ready, native_legacy_retirement_calls __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_offers, native_gop_gpu_completions, native_gop_gpu_failures, native_gop_gpu_abandoned, native_legacy_copy_n __attribute__((weak_import));
}

enum {
    kRowSummary, kRowGcc, kRowGpucc, kRowSmmu, kRowClk, kRowClk2, kRowGmu, kRowGmu2, kRowDbArc, kRowDbBcm, kRowLevels,
    kRowContext = kRowLevels + 3, kRowLoad, kRowHfi, kRowAfter, kRowGx, kRowGpuCtx, kRowCp, kRowCpState, kRowZap, kRowIrq,
    kRowPublish, kRowMetal, kRowLive, kRowHang, kRowHang2, kRowFaults, kRowUche, kRowTiling, kRowAddon, kRowClients, kRowSwap, kRowTest, kRowRecover, kRowInject, kRowPreempt, kRowTouch, kRowDpuSmmu, kRowDpuSmmu2, kRowDirect, kRowDirect2, kRowUc, kRowSelf, kRowLsd, kRowTScript, kRowJobs, kRowWptr, kRowStall, kRows
};

extern "C" {
char native_gpu_rows[kRows][160];
/* GPU fault recovery state (see gpuRestart/gpuBankFaultEnd below); the driver counts bank-fault retries. */
extern "C" {int native_adreno_inject_hang(void);extern volatile uint32_t native_adreno_recovery[8];}
extern "C" {uint32_t native_umi_gpu_restart[6];uint32_t native_umi_gpu_fault[13];extern volatile uint32_t native_adreno_bank_fault_retries[2];}
volatile uint32_t native_gpu_changes;
volatile uint32_t native_gpu_step;
struct gpb_result native_gpu_result;
struct gpb_gmu_clock_on native_gpu_clocks;
struct gpb_gmu_state native_gpu_gmu_state;
}

namespace {
unsigned gMode;
char gLastLoggedRows[kRows][160];

constexpr uint64_t kPhys[GPB_WINDOWS] = {
    GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE, GPB_GMU_BASE, GPB_RSCC_BASE
};
constexpr uint64_t kLen[GPB_WINDOWS] = {
    0x1000, 0x1000, GPB_GPUCC_SIZE, GPB_SMMU_GR0_SIZE, GPB_GPU_WINDOW, GPB_GMU_SIZE, GPB_RSCC_SIZE
};
OSSharedPtr<IOMemoryMap> gMaps[GPB_WINDOWS];
uintptr_t gVa[GPB_WINDOWS];

bool mapWindow(unsigned w) {
    IODeviceMemory *memory = IODeviceMemory::withRange(kPhys[w], kLen[w]);
    if (!memory) return false;
    gMaps[w] = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    gVa[w] = gMaps[w] ? static_cast<uintptr_t>(gMaps[w]->getVirtualAddress()) : 0;
    return gVa[w] != 0;
}
void delayUs(void *, unsigned us) { IODelay(us); }
int rd(void *, unsigned w, uint32_t off, uint32_t *out) {
    if (w >= GPB_WINDOWS || !gVa[w] || off >= kLen[w] || (off & 3)) return -1;
    *out = *reinterpret_cast<volatile uint32_t *>(gVa[w] + off);
    return 0;
}
bool writable(unsigned w, uint32_t off) {
    if (w == GPB_WIN_GCC_GPU && off == GPB_GCC_GPU_CFG_AHB) return gMode >= 2;
    if (w == GPB_WIN_GPUCC && (off == GPB_CC_CX_GDSCR || off == GPB_CC_AHB_CLK || off == GPB_CC_SMMU_VOTE_CLK)) return gMode >= 3;
    if (gMode < 6) return false;
    if (w == GPB_WIN_GCC_VOTE) return off == GPB_GCC_PLL_VOTE;
    if (w == GPB_WIN_GCC_GPU) return off == GPB_GCC_DDRSS_GPU_AXI || off == GPB_GCC_GPU_MEMNOC_GFX;
    if (w == GPB_WIN_GPUCC) return off == GPB_CC_GMU_RCGR || off == GPB_CC_GMU_RCGR + 4 || off == GPB_CC_CX_GMU_CLK || off == GPB_CC_CXO_CLK;
    return false;
}
int wr(void *, unsigned w, uint32_t off, uint32_t value) {
    if (w >= GPB_WINDOWS || !gVa[w] || off >= kLen[w] || (off & 3) || !writable(w, off)) return -1;
    *reinterpret_cast<volatile uint32_t *>(gVa[w] + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

void changed() {
    native_gpu_changes = native_gpu_changes + 1;
    for (unsigned r = 0; r < kRows; ++r) {
        if (native_gpu_rows[r][0] && strncmp(native_gpu_rows[r], gLastLoggedRows[r], sizeof(native_gpu_rows[0])) != 0) {
            strlcpy(gLastLoggedRows[r], native_gpu_rows[r], sizeof(gLastLoggedRows[0]));
            umi_boot_log_line("GPU", native_gpu_rows[r]);
        }
    }
}

void rowsBasic(const gpb_result &r) {
    snprintf(native_gpu_rows[kRowSummary], sizeof native_gpu_rows[0], "GPU m%u gcc=%s cc=%s smmu=%s gpu=%s io=%d cx%u clocks%u ahb%u",
             r.mode, gpb_state_text(r.gcc), gpb_state_text(r.cc), gpb_state_text(r.smmu), gpb_state_text(r.gpu), r.io_error,
             r.powered_cx, r.powered_clocks, r.enabled_ahb);
    snprintf(native_gpu_rows[kRowGcc], sizeof native_gpu_rows[0], "GCC cfg_ahb %08x>%08x memnoc %08x dvm %08x misc %08x gpll0 votes %08x",
             r.cfg_ahb, r.cfg_ahb_after, r.memnoc, r.dvm, r.misc, r.pll_vote);
    snprintf(native_gpu_rows[kRowGpucc], sizeof native_gpu_rows[0], "GPUCC cx %08x>%08x hw %08x gx %08x ahb %08x smmu %08x>%08x gmu %08x",
             r.cx_before, r.cx_gdscr, r.cx_hwctrl, r.gx_gdscr, r.cc_ahb, r.cc_smmu_before, r.cc_smmu, r.cc_gmu);
    snprintf(native_gpu_rows[kRowSmmu], sizeof native_gpu_rows[0], "SMMU cr0 %08x id %08x %08x %08x gfsr %08x smr %u first %d %08x s2cr %08x rbbm %08x",
             r.scr0, r.id0, r.id1, r.id2, r.gfsr, r.smr_valid, r.smr_first == 0xffffffffu ? -1 : int(r.smr_first), r.smr_first_val,
             r.s2cr_first_val, r.rbbm_status);
    changed();
}

alignas(8) uint8_t gCmdDb[GPB_CMD_DB_BYTES];
gpb_cmd_db_entry gDbFound[8];
int gDbRc[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
void commandDb() {
    native_gpu_step = 50;
    IODeviceMemory *memory = IODeviceMemory::withRange(GPB_CMD_DB_BASE, GPB_CMD_DB_BYTES);
    OSSharedPtr<IOMemoryMap> map;
    if (memory) {
        map = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly), OSNoRetain);
        memory->release();
    }
    if (!map) { snprintf(native_gpu_rows[kRowDbArc], sizeof native_gpu_rows[0], "CMDDB nomap"); changed(); return; }
    const volatile uint32_t *src = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    for (uint32_t i = 0; i < GPB_CMD_DB_BYTES / 4; ++i) {
        uint32_t v = src[i];
        memcpy(gCmdDb + 4 * i, &v, 4);
    }
    static const char *const names[8] = {"gfx.lvl", "mx.lvl", "cx.lvl", "gmxc.lvl", "SH0", "MC0", "ACV", "CN0"};
    gpb_cmd_db_entry *found = gDbFound;
    int *rc = gDbRc;
    for (unsigned i = 0; i < 8; ++i) rc[i] = gpb_cmd_db_find(gCmdDb, sizeof gCmdDb, names[i], &found[i]);
    for (unsigned half = 0; half < 2; ++half) {
        char *row = native_gpu_rows[kRowDbArc + half];
        const size_t cap = sizeof native_gpu_rows[0];
        int n = half ? snprintf(row, cap, "CMDDB bcm") : snprintf(row, cap, "CMDDB v%08x magic %08x", gpb_le32(gCmdDb), gpb_le32(gCmdDb + 4));
        for (unsigned i = 4 * half; i < 4 * half + 4 && n > 0 && size_t(n) < cap; ++i) {
            if (rc[i]) n += snprintf(row + n, cap - n, " %s:%d", names[i], rc[i]);
            else n += snprintf(row + n, cap - n, " %s:%x/%u", names[i], found[i].address, found[i].aux_bytes);
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        char *row = native_gpu_rows[kRowLevels + i];
        const size_t cap = sizeof native_gpu_rows[0];
        int n = snprintf(row, cap, "LVL %s", names[i]);
        for (unsigned k = 0; !rc[i] && k + 1 < found[i].aux_bytes && k < 32 && n > 0 && size_t(n) < cap; k += 2)
            n += snprintf(row + n, cap - n, " %u", gpb_le16(found[i].aux + k));
    }
    changed();
}

void gmuState(const gpb_io &io, gpb_result &r, uint32_t cx_gmu, uint32_t ahb) {
    if (!gpb_gmu_regs_readable(&r, cx_gmu, ahb)) {
        snprintf(native_gpu_rows[kRowGmu], sizeof native_gpu_rows[0], "GMU not readable (cx_gmu %08x ahb %08x)", cx_gmu, ahb);
        changed();
        return;
    }
    native_gpu_step = 60;
    gpb_gmu_state &s = native_gpu_gmu_state;
    gpb_read_gmu_state(&io, &r, &s);
    snprintf(native_gpu_rows[kRowGmu], sizeof native_gpu_rows[0], "GMU sysreset %08x fw %08x gx %08x hfi %08x gen7 %08x",
             s.sysreset, s.fw_init, s.gx_status, s.hfi_status, s.general7);
    snprintf(native_gpu_rows[kRowGmu2], sizeof native_gpu_rows[0], "GMU ao_irq %08x fence %08x rscc busy %08x status %08x",
             s.ao_irq, s.fence, s.rscc_busy, s.rscc_status);
    changed();
}

constexpr uint64_t kSmmuWindow = 0x10000, kPdcPhys = 0x0b290000, kPdcBytes = 0x10000;
constexpr unsigned kGmuBank = 1, kGmuAsid = 1, kGmuSlots[2] = {2, 3}, kGmuSids[2] = {5, 0x405};
OSSharedPtr<IOMemoryMap> gSmmuMap, gPdcMap, gPayloadMaps[5];
A650GmuDevice gDevice;
A650GmuConfig gConfig;
AdrenoHfiStartupConfig gStartup;
A650GmuStartupResult gCold;
bool gColdOk;
extern "C" {
NativeAdrenoGmuContextReport native_gpu_gmu_context;
}
void contextProgress(uint32_t step) { native_gpu_step = 80 + step; }
volatile uint8_t *gWin[A650GmuWindows];
int gmuRead(void *, unsigned w, size_t at, uint32_t *out) {
    if (w >= A650GmuWindows || !gWin[w] || (at & 3) || at + 4 > gDevice.io.bytes[w]) return 1;
    *out = *reinterpret_cast<volatile uint32_t *>(gWin[w] + at);
    return 0;
}
int gmuWrite(void *, unsigned w, size_t at, uint32_t value) {
    if (w >= A650GmuWindows || !gWin[w] || (at & 3) || at + 4 > gDevice.io.bytes[w]) return 1;
    *reinterpret_cast<volatile uint32_t *>(gWin[w] + at) = value;
    return 0;
}
int gmuBarrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); return 0; }
int gmuNow(void *, uint64_t *us) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    *us = ns / 1000;
    return 0;
}
int gmuWait(void *, unsigned us) { IODelay(us); return 0; }

void gmuColdStart(const gpb_result &r, const gpb_gmu_clock_on &o) {
    char *ctx = native_gpu_rows[kRowContext], *load = native_gpu_rows[kRowLoad];
    const size_t cap = sizeof native_gpu_rows[0];
    native_gpu_step = 79;
    if (!&native_gpu_gmu_firmware || !&native_gpu_gmu_firmware_size || !native_gpu_gmu_firmware_size) {
        snprintf(ctx, cap, "GMUCTX no firmware linked"); changed(); return;
    }
    if (o.step != 5 || o.failed || r.smmu != GPB_DONE || r.smr_valid || gDbRc[0] || gDbRc[1] || gDbRc[2] || gDbRc[4] || gDbRc[5] ||
        gDbRc[6] || gDbRc[7] || native_gpu_gmu_state.sysreset != 1) {
        snprintf(ctx, cap, "GMUCTX skipped: clocks %u/%d smmu %s smr %u cmddb %d%d%d%d%d%d%d gmu reset %08x", o.step, o.failed,
                 gpb_state_text(r.smmu), r.smr_valid, gDbRc[0], gDbRc[1], gDbRc[2], gDbRc[4], gDbRc[5], gDbRc[6], gDbRc[7],
                 native_gpu_gmu_state.sysreset);
        changed();
        return;
    }
    IODeviceMemory *memory = IODeviceMemory::withRange(GPB_SMMU_BASE, kSmmuWindow);
    if (memory) {
        gSmmuMap = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        memory->release();
    }
    if (!gSmmuMap) { snprintf(ctx, cap, "GMUCTX nomap"); changed(); return; }
    NativeAdrenoGmuContextReport &g = native_gpu_gmu_context;
    const IOReturn status = NativeAdrenoGmuContextBringupRoutes(reinterpret_cast<volatile uint8_t *>(gSmmuMap->getVirtualAddress()),
        kSmmuWindow, kGmuBank, kGmuAsid, kGmuSlots, kGmuSids, 2, contextProgress, &g);
    snprintf(ctx, cap, "GMUCTX step %u status %x kit %u/%u routes %u id2 %x program %u/%u/%u ttbr0 %llx smr %08x s2cr %08x",
             g.step, static_cast<unsigned>(status), g.program_status, g.route_status, g.routes_verified, g.id2, g.program_phase,
             g.program_readback_valid, g.program_enabled, static_cast<unsigned long long>(g.ttbr0), g.smr, g.s2cr);
    changed();
    if (status) return;
    NativeAdrenoGmuPayload payloads[5];
    const IOReturn got = NativeAdrenoGmuContextPayloads(payloads);
    if (got) { snprintf(load, cap, "LOAD payloads %x", static_cast<unsigned>(got)); changed(); return; }
    A650GmuMemory views[5];
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(payloads[i].physical, payloads[i].bytes);
        if (m) {
            gPayloadMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gPayloadMaps[i]) { snprintf(load, cap, "LOAD nomap payload %u", i); changed(); return; }
        views[i] = {reinterpret_cast<volatile uint32_t *>(gPayloadMaps[i]->getVirtualAddress()), size_t(payloads[i].bytes), payloads[i].iova};
    }
    IODeviceMemory *pdc = IODeviceMemory::withRange(kPdcPhys, kPdcBytes);
    if (pdc) {
        gPdcMap = OSSharedPtr<IOMemoryMap>(pdc->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        pdc->release();
    }
    if (!gPdcMap || !gVa[GPB_WIN_GMU] || !gVa[GPB_WIN_RSCC]) { snprintf(load, cap, "LOAD nomap window"); changed(); return; }
    gWin[A650GmuCore] = reinterpret_cast<volatile uint8_t *>(gVa[GPB_WIN_GMU]);
    gWin[A650GmuRscc] = reinterpret_cast<volatile uint8_t *>(gVa[GPB_WIN_RSCC]);
    gWin[A650GmuPdc] = reinterpret_cast<volatile uint8_t *>(gPdcMap->getVirtualAddress());
    gDevice = {};
    gDevice.io = {nullptr, {A650_GMU_CORE_BYTES, A650_GMU_RSCC_BYTES, A650_GMU_PDC_BYTES}, gmuRead, gmuWrite, gmuBarrier};
    gDevice.services = {nullptr, gmuNow, gmuWait};
    gConfig = {A650_GMU_CHIP_ID, native_gpu_gmu_firmware, native_gpu_gmu_firmware_size, views[0], views[1], views[3], views[4], 100};
    unsigned ddrVote = 0;
    PE_parse_boot_argn("umi-gpu-bw", &ddrVote, sizeof ddrVote);
    A650GmuDdrVote = ddrVote ? 1 : 0;
    const AdrenoHfiStartupStatus built = A650GmuBuildStartup(gDbFound[0].aux, gDbFound[0].aux_bytes, gDbFound[1].aux,
        gDbFound[1].aux_bytes, gDbFound[2].aux, gDbFound[2].aux_bytes, gDbFound[5].address, gDbFound[4].address,
        gDbFound[6].address, gDbFound[7].address, &gStartup);
    if (built) { snprintf(load, cap, "LOAD vote table %u", built); changed(); return; }
    snprintf(load, cap, "LOAD running, gpu votes %08x..%08x gmu %08x", gStartup.gpu[1].vote, gStartup.gpu[7].vote, gStartup.gmu[1].vote);
    changed();
    native_gpu_step = 90;
    const A650GmuStartupLimits limits = {1000, 100, 50000, 2000000};
    const A650GmuStatus cold = A650GmuColdStart(&gDevice, &gConfig, &gStartup, &limits, &gCold);
    native_gpu_step = cold ? 98 : 97;
    const A650GmuResult &l = gCold.load;
    snprintf(load, cap, "LOAD status %u phase %u blocks %u version %08x init %08x hfi %08x ready %u%u ao %08x cm3 %08x reset %u rsc %u",
             cold, l.phase, l.blocks, l.firmware_version, l.firmware_init_result, l.hfi_status, l.firmware_ready_observed,
             l.hfi_ready_observed, l.ao_fault_status, l.cm3_fault_status, l.reset_verified, l.rsc_stop_status);
    const AdrenoHfiStartupResult &p = gCold.protocol;
    const AdrenoHfi &q = gDevice.hfi;
    snprintf(native_gpu_rows[kRowHfi], cap, "HFI status %u stage %u acked %x error %x transport %u sent %u %llu ms skipped %u/%x/%x health %u",
             gCold.protocol_status, p.stage, p.acknowledged_stages, p.firmware_error, p.transport_status, p.published_commands,
             static_cast<unsigned long long>(p.elapsed_us / 1000), q.skipped, q.skipped_header, q.skipped_ret, gCold.health_status);
    uint32_t after[5] = {};
    gmuRead(nullptr, A650GmuCore, 0x5000 * 4, &after[0]);
    gmuRead(nullptr, A650GmuCore, 0x501c * 4, &after[1]);
    gmuRead(nullptr, A650GmuCore, 0x5180 * 4, &after[2]);
    gmuRead(nullptr, A650GmuCore, 0x50d0 * 4, &after[3]);
    gmuRead(nullptr, A650GmuCore, 0x9305 * 4, &after[4]);
    snprintf(native_gpu_rows[kRowAfter], cap, "AFTER sysreset %08x init %08x hfi %08x gx %08x ao %08x", after[0], after[1], after[2],
             after[3], after[4]);
    changed();
    gColdOk = cold == A650GmuSuccess;
}

extern "C" {
A650GmuGxResult native_gpu_gx;
}
constexpr uint32_t kRbbmGbifHalt = 0x0016, kRbbmGbifHaltAck = 0x0017, kRbbmStatus = 0x0210, kGbifHalt = 0x3c45,
                   kGbifHaltAck = 0x3c46;
uint32_t gpuRead(uint32_t dword) {
    uint32_t v = 0xdeadbeef;
    rd(nullptr, GPB_WIN_GPU, dword * 4, &v);
    return v;
}

extern "C" uint64_t ml_io_map(uint64_t phys_addr, uint64_t size);
void rpmhVoteDdr() {
    unsigned vote = 1;
    PE_parse_boot_argn("umi-ddr-vote", &vote, sizeof vote);
    if (!vote) return;
    char *row = native_gpu_rows[kRowHang2];
    const size_t cap = sizeof native_gpu_rows[0];
    const uint32_t mc0 = gDbFound[5].address, sh0 = gDbFound[4].address, acv = gDbFound[6].address;
    if (!mc0 || !sh0 || !acv) { snprintf(row, cap, "DDRV no BCM addresses %x %x %x", mc0, sh0, acv); changed(); return; }
    volatile uint32_t *drv = (volatile uint32_t *)(uintptr_t)ml_io_map(0x18220000, 0x10000);
    if (!drv) { snprintf(row, cap, "DDRV no mapping"); changed(); return; }
    auto r = [&](uint32_t off) { return drv[off / 4]; };
    auto w = [&](uint32_t off, uint32_t v) { drv[off / 4] = v; __asm__ volatile("dsb sy" ::: "memory"); };
    const uint32_t config = r(0x0C);
    const uint32_t tcsCount = (config >> 12) & 0x3f, perTcs = (config >> 27) & 0x1f;
    constexpr uint32_t kTcsBase = 0xd00, kTcsStride = 672, kCmdStride = 20;
    auto tcs = [&](unsigned id, uint32_t reg) { return kTcsBase + kTcsStride * id + reg; };
    unsigned id = 0;
    for (; id < 2; ++id) if (r(tcs(id, 0x18))) break;
    if (id == 2 || tcsCount < 2 || perTcs < 3) {
        snprintf(row, cap, "DDRV refused: config %08x (tcs %u, cmds %u), status %08x %08x", config, tcsCount, perTcs, r(tcs(0, 0x18)),
                 r(tcs(1, 0x18)));
        changed();
        return;
    }
    const uint32_t top = (1u << 30) | (1u << 29) | (0x3fffu << 14) | 0x3fffu, acvOn = (1u << 30) | (1u << 29) | 0x8u;
    const uint32_t addrs[3] = {mc0, sh0, acv}, datas[3] = {top, top, acvOn};
    w(tcs(id, 0x1C), 0);
    w(kTcsBase + 0x00, r(kTcsBase + 0x00) | (1u << id));
    for (unsigned c = 0; c < 3; ++c) {
        w(tcs(id, 0x30) + kCmdStride * c, 8u | (1u << 16) | (1u << 8));
        w(tcs(id, 0x34) + kCmdStride * c, addrs[c]);
        w(tcs(id, 0x38) + kCmdStride * c, datas[c]);
    }
    w(tcs(id, 0x1C), 7);
    uint32_t control = r(tcs(id, 0x14));
    control &= ~(1u << 24); w(tcs(id, 0x14), control);
    control &= ~(1u << 16); w(tcs(id, 0x14), control);
    w(tcs(id, 0x14), 1u << 16);
    w(tcs(id, 0x14), (1u << 16) | (1u << 24));
    unsigned polls = 0;
    for (; polls < 2000 && !(r(kTcsBase + 0x04) & (1u << id)); ++polls) IODelay(5);
    uint32_t status[3];
    for (unsigned c = 0; c < 3; ++c) status[c] = r(tcs(id, 0x3C) + kCmdStride * c);
    w(kTcsBase + 0x08, 1u << id);
    w(tcs(id, 0x1C), 0);
    w(kTcsBase + 0x00, r(kTcsBase + 0x00) & ~(1u << id));
    snprintf(row, cap, "DDRV tcs %u of %u (%u cmds) polls %u cmd status %05x %05x %05x | aux mc0 %02x%02x%02x%02x w%u", id, tcsCount,
             perTcs, polls, status[0], status[1], status[2], gDbFound[5].aux_bytes > 3 ? gDbFound[5].aux[3] : 0,
             gDbFound[5].aux_bytes > 2 ? gDbFound[5].aux[2] : 0, gDbFound[5].aux_bytes > 1 ? gDbFound[5].aux[1] : 0,
             gDbFound[5].aux_bytes ? gDbFound[5].aux[0] : 0,
             gDbFound[5].aux_bytes > 5 ? gDbFound[5].aux[4] | gDbFound[5].aux[5] << 8 : 0);
    changed();
}

void gxOn() {
    unsigned perf = 7;
    PE_parse_boot_argn("umi-gpu-perf", &perf, sizeof perf);
    if (perf < 1 || perf > 7) perf = 7;
    native_gpu_step = 100;
    A650GmuGxResult &g = native_gpu_gx;
    const A650GmuStatus status = A650GmuPowerGx(&gDevice, perf, &g);
    uint32_t gdscr = 0;
    rd(nullptr, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &gdscr);
    char *row = native_gpu_rows[kRowGx];
    const size_t cap = sizeof native_gpu_rows[0];
    if (!g.gx_on) {
        snprintf(row, cap, "GX status %u perf %u vote %u/%x oob %u info %08x gx %08x>%08x gdscr %08x off", status, perf,
                 g.vote_status, g.vote_error, g.oob_polls, g.oob_info, g.gx_before, g.gx_after, gdscr);
    } else {
        native_gpu_step = 101;
        snprintf(row, cap, "GX on perf %u vote %u oob %u gx %08x>%08x gdscr %08x rbbm %08x gbif halt %x/%x rbbm halt %x/%x", perf,
                 g.vote_status, g.oob_polls, g.gx_before, g.gx_after, gdscr, gpuRead(kRbbmStatus), gpuRead(kGbifHalt),
                 gpuRead(kGbifHaltAck), gpuRead(kRbbmGbifHalt), gpuRead(kRbbmGbifHaltAck));
    }
    changed();
    rpmhVoteDdr();
}

extern "C" {
volatile uint32_t native_adreno_readonly_imports_owned, native_adreno_written_imports_owned, native_adreno_pool_mib;
extern volatile uint32_t native_adreno_smmu_coherent_walk, native_adreno_wc_enabled;
/* The GPU level the gts7 driver records per job (NativeAdrenoSubmit.cpp, weak import): umi has no GPU DCVS yet. */
volatile uint32_t native_gpu_dcvs[15];
/* The gts7 driver weak-imports gts7xlwifi kernel-patch symbols, and this kernel's link refuses undefined weak symbols: umi has no
 * memory-outside adoption (t970-memory-outside.patch), so the adopted range is empty, and no pmap reverse lookup
 * (native_pmap_user_mappings, a fault-row diagnostic) is available, so it finds nothing. */
uint64_t vm_outside_first_phys, vm_outside_last_phys;
uint64_t *vm_outside_valid;
unsigned int native_pmap_user_mappings(ppnum_t, void **, uint64_t *, char *) { return 0; }
NativeAdrenoGmuContextReport native_gpu_context;
uint32_t native_gpu_cp[24];
}
constexpr unsigned kGpuBank = 0, kGpuAsid = 2, kGpuSlots[4] = {0, 1, 4, 5}, kGpuSids[4] = {0, 1, 0x400, 0x401};
NativeAdrenoGpuBuffer gNb[GpuBufferCount];   // the gts7 driver fills GpuBufferCount (11: two rings + preemption records)
OSSharedPtr<IOMemoryMap> gNbMaps[GpuBufferCount];
volatile uint8_t *gNbCpu[GpuBufferCount];
volatile bool gPreemptOn;   // gpu-preempt: two rings, CP_SW (RBBM_INT_0 bit 8) unmasked
volatile uint32_t gPreemptPending, gPreemptIrqs;
NativeAdrenoRing *gRing;
NativeAdrenoCpInit *gCpInit;

int naWrite(void *, uint32_t reg, uint32_t value) {
    if (reg >= GPB_GPU_WINDOW / 4 || !gVa[GPB_WIN_GPU]) return 1;
    *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPU] + reg * 4) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
int naRead(void *, uint32_t reg, uint32_t *value) {
    if (reg >= GPB_GPU_WINDOW / 4 || !gVa[GPB_WIN_GPU]) return 1;
    *value = *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPU] + reg * 4);
    return 0;
}
int naBarrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); return 0; }
int naPublish(void *, const volatile uint32_t *, size_t) { __asm__ volatile("dsb sy" ::: "memory"); return 0; }
int naAcquire(void *, volatile uint32_t *, size_t) { __asm__ volatile("dsb sy" ::: "memory"); return 0; }
uint64_t naNow(void *) { uint64_t us = 0; gmuNow(nullptr, &us); return us; }

volatile bool gIrqAttached;
volatile uint32_t gIrqCount, gIrqSeen, gIrqLast, gIrqRepeat, gIrqMasked;
int naIdle(void *) {
    if (gIrqAttached && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && get_preemption_level() == 0) {
        assert_wait_timeout(event_t(&gIrqCount), THREAD_UNINT, 100, NSEC_PER_USEC);
        thread_block(THREAD_CONTINUE_NULL);
        native_gpu_idle_sleeps = native_gpu_idle_sleeps + 1;
    } else {
        IODelay(20);
        native_gpu_idle_spins = native_gpu_idle_spins + 1;
    }
    return 0;
}
void naProgress(uint32_t step) { native_gpu_step = 110 + step; }
// umi-181: the GMU drops a CP_RB_WPTR write that lands while the GPU domain is power-collapsed and flags it in GMU_AHB_FENCE_STATUS
// (kgsl A6XX_GMU_AHB_FENCE_STATUS 0x23b13, _CLR 0x23b14; the GMU window starts 0x6a000 bytes into the GPU's), as the driver's own
// service does (NativeAdrenoGpuService::gmu_fence_status). Without this callback NativeAdrenoRing::writeWptr never re-wrote a
// dropped WPTR: umi-180 lost one submission (sub 853/852, GPU idle), the display waited 5 s on its fence and the family's
// cancel-swaps call hit an unimplemented legacy slot (panic leaf=345).
int naFenceStatus(void *, int clear, uint32_t *status) {
    if (clear) return gmuWrite(nullptr, A650GmuCore, 0x23b14u * 4u - 0x6a000u, 7u);
    return status ? gmuRead(nullptr, A650GmuCore, 0x23b13u * 4u - 0x6a000u, status) : 1;
}
NativeAdrenoRingIo gRio{nullptr, naWrite, naRead, naBarrier, nullptr, naFenceStatus};
NativeAdrenoRingCache gCache{nullptr, naPublish, naAcquire};
NativeAdrenoRingClock gClock{nullptr, naNow, naIdle};
void gw(uint32_t dword, uint32_t value) { naWrite(nullptr, dword, value); }
void gw64(uint32_t dword, uint64_t value) { gw(dword, uint32_t(value)); gw(dword + 1, uint32_t(value >> 32)); }
uint32_t smmuRead(uint32_t at) { return *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + at); }
void smmuWrite(uint32_t at, uint32_t v) {
    *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + at) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t smmuBank0() {
    const uint32_t id1 = smmuRead(0x24);
    return ((id1 >> 31) ? 0x10000u : 0x1000u) << (((id1 >> 28) & 7) + 1);
}

bool gpuContext() {
    char *row = native_gpu_rows[kRowGpuCtx];
    const size_t cap = sizeof native_gpu_rows[0];
    native_gpu_step = 110;
    native_adreno_smmu_coherent_walk = 0;
    native_adreno_wc_enabled = 1;
    native_adreno_written_imports_owned = 1;
    unsigned readOnlyOwned = 1;
    PE_parse_boot_argn("umi-gpu-ro-own", &readOnlyOwned, sizeof readOnlyOwned);
    native_adreno_readonly_imports_owned = readOnlyOwned ? 1 : 0;
    unsigned pool = 0;   // gts7xlwifi's default with its driver (the pipa-based driver used 128)
    PE_parse_boot_argn("umi-gpu-pool", &pool, sizeof pool);
    native_adreno_pool_mib = pool;
    NativeAdrenoGmuContextReport &g = native_gpu_context;
    const IOReturn ctx = NativeAdrenoGpuContextBringupRoutes(reinterpret_cast<volatile uint8_t *>(gSmmuMap->getVirtualAddress()),
        kSmmuWindow, kGpuBank, kGpuAsid, kGpuSlots, kGpuSids, 4, naProgress, &g);
    uint32_t actlr = 0;
    if (!ctx) {
        const uint32_t bank0 = smmuBank0();
        actlr = smmuRead(bank0 + 0x4);
        smmuWrite(bank0 + 0x4, actlr & ~2u);
        smmuWrite(bank0 + 0x58, 0xffffffffu);
        smmuWrite(bank0 + 0x618, 0);
        smmuWrite(bank0 + 0x7f0, 0);
        for (unsigned i = 0; i < 1000 && (smmuRead(bank0 + 0x7f4) & 1); ++i) IODelay(1);
    }
    snprintf(row, cap, "GPUCTX step %u status %x kit %u/%u routes %u program %u/%u/%u ttbr0 %llx smr %08x s2cr %08x actlr %x pool %u",
             g.step, static_cast<unsigned>(ctx), g.program_status, g.route_status, g.routes_verified, g.program_phase,
             g.program_readback_valid, g.program_enabled, static_cast<unsigned long long>(g.ttbr0), g.smr, g.s2cr, actlr, pool);
    changed();
    return ctx == kIOReturnSuccess;
}

constexpr uint32_t rdonly(uint32_t reg, uint32_t len) { return ((len & 0x3FFF) << 18) | (reg & 0x3FFFF); }
constexpr uint32_t nordwr(uint32_t reg, uint32_t len) { return (1u << 31) | rdonly(reg, len); }
constexpr uint32_t kProtect[] = {
    rdonly(0x00000, 0x04ff), rdonly(0x00501, 0x0005), rdonly(0x0050b, 0x02f4), nordwr(0x0050e, 0x0000), nordwr(0x00510, 0x0000),
    nordwr(0x00534, 0x0000), nordwr(0x00800, 0x0082), nordwr(0x008a0, 0x0008), nordwr(0x008ab, 0x0024), rdonly(0x008de, 0x00ae),
    nordwr(0x00900, 0x004d), nordwr(0x0098d, 0x0272), nordwr(0x00e00, 0x0001), nordwr(0x00e03, 0x000c), nordwr(0x03c00, 0x00c3),
    rdonly(0x03cc4, 0x1fff), nordwr(0x08630, 0x01cf), nordwr(0x08e00, 0x0000), nordwr(0x08e08, 0x0000), nordwr(0x08e50, 0x001f),
    nordwr(0x08e80, 0x027f), nordwr(0x09624, 0x01db), nordwr(0x09e60, 0x0011), nordwr(0x09e78, 0x0187), nordwr(0x0a630, 0x01cf),
    nordwr(0x0ae02, 0x0000), nordwr(0x0ae50, 0x032f), nordwr(0x0b604, 0x0000), nordwr(0x0b608, 0x0007), nordwr(0x0be02, 0x0001),
    nordwr(0x0be20, 0x17df), nordwr(0x0f000, 0x0bff), rdonly(0x0fc00, 0x1fff), nordwr(0x18400, 0x1fff), nordwr(0x1a800, 0x1fff),
    nordwr(0x1f400, 0x0443), rdonly(0x1f844, 0x007b), nordwr(0x1f887, 0x001b), nordwr(0x1f8c0, 0x0000),
};
constexpr unsigned kProtectCount = sizeof kProtect / sizeof kProtect[0];
static_assert(kProtectCount == 39, "a650_protect has 39 entries");

bool gUcheGmem16M;

constexpr uint32_t kHwcg[][2] = {
    {0x00b0, 0x02222222}, {0x00b4, 0x02222220}, {0x00b8, 0x00000080}, {0x00bc, 0x0000f3cf},
    {0x00c0, 0x02222222}, {0x00c4, 0x22222222}, {0x00c8, 0x22222222}, {0x00cc, 0x00022222},
    {0x00d0, 0x11111111}, {0x00d4, 0x11111111}, {0x00d8, 0x11111111}, {0x00dc, 0x00011111},
    {0x00e0, 0x77777777}, {0x00e4, 0x77777777}, {0x00e8, 0x77777777}, {0x00ec, 0x00077777},
    {0x00f0, 0x22222222}, {0x00f4, 0x01002222}, {0x00f8, 0x00002220}, {0x0100, 0x00040f00},
    {0x0104, 0x25222022}, {0x0105, 0x00005555}, {0x0106, 0x00000011}, {0x0107, 0x00445044},
    {0x0108, 0x04222222}, {0x0111, 0x00002222}, {0x0114, 0x00222222}, {0x0117, 0x00000002},
    {0x011b, 0x00002222}, {0x0109, 0x00004000}, {0x0112, 0x00002222}, {0x0115, 0x00000200},
    {0x011c, 0x00000000}, {0x010a, 0x00000000}, {0x0113, 0x00000000}, {0x0116, 0x04104004},
    {0x011d, 0x00000000}, {0x0120, 0x00000222}, {0x0121, 0x00000111}, {0x0122, 0x00000777},
    {0x010b, 0x22222222}, {0x0110, 0x00000004}, {0x010f, 0x00000002}, {0x0533, 0x00000182},
    {0x0044, 0x00000000}, {0x0042, 0x00000000}, {0x0118, 0x00000222}, {0x0119, 0x00000111},
    {0x011a, 0x00000555},
};
static_assert(sizeof kHwcg / sizeof kHwcg[0] == 49, "a650_hwcg has 49 entries");

void setHwcg() {
    uint32_t *n = native_gpu_cp;
    unsigned on = 1;
    PE_parse_boot_argn("umi-gpu-hwcg", &on, sizeof on);
    n[9] = gpuRead(0x00ae);
    gmuRead(nullptr, A650GmuCore, 0x80 * 4, &n[10]);
    gmuWrite(nullptr, A650GmuCore, 0x80 * 4, n[10] & ~1u);
    for (const auto &r : kHwcg) gw(r[0], on ? r[1] : 0);
    uint32_t sptp = 0;
    gmuRead(nullptr, A650GmuCore, 0x80 * 4, &sptp);
    gmuWrite(nullptr, A650GmuCore, 0x80 * 4, sptp | 1u);
    gw(0x00ae, on ? 0x8aa8aa82 : 0);
    gmuRead(nullptr, A650GmuCore, 0x80 * 4, &n[11]);
    n[12] = gpuRead(0x00ae);
}

void hwInit() {
    gw(0x3C45, 0); (void)gpuRead(0x3C45);
    gw(0x0016, 0); (void)gpuRead(0x0016);
    gw(0xF803, 0);
    gw64(0xF800, 0); gw(0xF802, 0);
    static const uint32_t addrMode[] = {0x842, 0xC01, 0x8601, 0x8E05, 0x9E01, 0xBE05, 0xA601, 0x9601, 0xE00, 0xAE01, 0xB601, 0xF810};
    for (uint32_t reg : addrMode) gw(reg, 1);
    setHwcg();
    unsigned gmem16m = 0;
    PE_parse_boot_argn("umi-gpu-gmem16m", &gmem16m, sizeof gmem16m);
    gUcheGmem16M = gmem16m != 0;
    for (uint32_t reg = 0x3C03; reg <= 0x3C06; ++reg) gw(reg, 0x00071620);
    gw(0x0011, 0x3);
    gw(0x050B, 0xffffffff);
    gw64(0xE05, 0x0001ffffffffffc0ull);
    gw64(0xE09, 0x0001fffffffff000ull);
    gw64(0xE07, 0x0001fffffffff000ull);
    if (gUcheGmem16M) { gw64(0xE0B, 0x1000000); gw64(0xE0D, 0x1000000 + 0x120000 - 1); }
    gw(0xE18, 0x804); gw(0xE17, 0x4);
    gw(0x8C2, 0x02000140); gw(0x8C1, 0x8040362c);
    gw(0x8C3, 128);
    gw(0x9E00, 0x00300200);
    gw(0x98D, 1);
    gw(0x500, 1);
    gw(0x8D0, 0);
    gw(0x8E08, 0x816); gw(0xB604, 0x6); gw(0xAE02, 0x26);
    gw(0xE01, 3u << 21);
    gw(0x001F, (1u << 30) | 0x1fffff);
    gw(0xE19, 1);
    gw(0xB608, 0); gw(0xB609, 0x3fe05ff4); gw(0xB60A, 0x3fa0ebee);
    gw(0xB60B, 0x3f5193ed); gw(0xB60C, 0x3f0243f0);
    gmuWrite(nullptr, A650GmuCore, 0x930e * 4, 0xff000000);
    uint32_t sel = 0;
    gmuRead(nullptr, A650GmuCore, 0x5041 * 4, &sel);
    gmuWrite(nullptr, A650GmuCore, 0x5041 * 4, (sel & ~0xffu) | (1u << 5));
    gmuWrite(nullptr, A650GmuCore, 0x5040 * 4, 1);
    gw(0x84F, (1u << 0) | (1u << 1) | (1u << 3));
    for (unsigned i = 0; i + 1 < kProtectCount; ++i) gw(0x850 + i, kProtect[i]);
    gw(0x850 + 47, kProtect[kProtectCount - 1]);
    unsigned apriv = 1;
    PE_parse_boot_argn("umi-gpu-apriv", &apriv, sizeof apriv);
    if (apriv) gw(0x844, (1u << 6) | (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1));
    gw(0x038, NativeAdrenoIrq::maskA6xx() | (gPreemptOn ? 1u << 8 : 0u));
}

void ucheRow() {
    auto r64 = [](uint32_t dword) { return uint64_t(gpuRead(dword)) | uint64_t(gpuRead(dword + 1)) << 32; };
    char line[sizeof native_gpu_rows[0]];
    snprintf(line, sizeof line, "UCHE m%x w%llx t%llx/%llx g%llx-%llx 0e12 %x pf %x | nc %x %x %x %x",
             gpuRead(0xE01), (unsigned long long)r64(0xE05), (unsigned long long)r64(0xE07), (unsigned long long)r64(0xE09),
             (unsigned long long)r64(0xE0B), (unsigned long long)r64(0xE0D), gpuRead(0xE12), gpuRead(0xE19), gpuRead(0x8E08),
             gpuRead(0xB604), gpuRead(0xAE02), gpuRead(0x534));
    if (strncmp(line, native_gpu_rows[kRowUche], sizeof line)) {
        strlcpy(native_gpu_rows[kRowUche], line, sizeof native_gpu_rows[0]);
        changed();
    }
}

extern "C" volatile uint64_t native_adreno_submit_phase[7] __attribute__((weak_import));
extern "C" volatile uint64_t native_adreno_cache_stats[5] __attribute__((weak_import));
extern "C" volatile uint32_t native_adreno_wc_distrusted __attribute__((weak_import));
extern "C" volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak_import));
void submitRow() {
    if (!&native_adreno_submit_phase) return;
    const uint64_t n = native_adreno_submit_phase[6];
    if (!n) return;
    uint64_t us[6];
    for (unsigned i = 0; i < 6; ++i) {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(native_adreno_submit_phase[i], &ns);
        us[i] = ns / 1000 / n;
    }
    char line[sizeof native_gpu_rows[0]];
    uint64_t maintained_ns = 0;
    if (&native_adreno_cache_stats) absolutetime_to_nanoseconds(native_adreno_cache_stats[0], &maintained_ns);
    snprintf(line, sizeof line, "SUBT n%llu us check %llu sweep %llu wc %llu pub %llu tlb %llu ring %llu | mt %llu %lluMB %llums d%u r%u",
             (unsigned long long)n, (unsigned long long)us[0], (unsigned long long)us[1], (unsigned long long)us[2],
             (unsigned long long)us[3], (unsigned long long)us[4], (unsigned long long)us[5],
             &native_adreno_cache_stats ? (unsigned long long)native_adreno_cache_stats[3] : 0ull,
             &native_adreno_cache_stats ? (unsigned long long)(native_adreno_cache_stats[1] >> 20) : 0ull,
             (unsigned long long)(maintained_ns / 1000000), &native_adreno_wc_distrusted ? native_adreno_wc_distrusted : 0u,
             &native_adreno_wc_audit ? native_adreno_wc_audit[5] : 0u);
    if (strncmp(line, native_gpu_rows[kRowTiling], sizeof line)) {
        strlcpy(native_gpu_rows[kRowTiling], line, sizeof native_gpu_rows[0]);
        changed();
    }
}

bool cpStart() {
    uint32_t *n = native_gpu_cp;
    char *row = native_gpu_rows[kRowCp], *state = native_gpu_rows[kRowCpState];
    const size_t cap = sizeof native_gpu_rows[0];
    auto report = [&](const char *what) {
        snprintf(row, cap, "CP %s step %u sqe %u/%u ring %x init %x fence %x/%u write %x marker %08x | hwcg %x>%x sptp %x>%x", what, n[0],
                 n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9], n[12], n[10], n[11]);
        const uint32_t bank0 = smmuBank0();
        snprintf(state, cap, "CPSTATE rbbm %08x rptr %x wptr %x hwfault %x cpint %x int0 %08x cb0 fsr %08x far %08x%08x",
                 gpuRead(0x210), gpuRead(0x806), gpuRead(0x807), gpuRead(0x821), gpuRead(0x823), gpuRead(0x201),
                 smmuRead(bank0 + 0x58), smmuRead(bank0 + 0x64), smmuRead(bank0 + 0x60));
        changed();
    };
    native_gpu_step = 120;
    n[0] = 1;
    if (NativeAdrenoGpuContextBuffers(gNb)) { report("no buffers"); return false; }
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(gNb[i].physical, gNb[i].bytes);
        if (m) {
            gNbMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gNbMaps[i]) { report("nomap"); return false; }
        gNbCpu[i] = reinterpret_cast<volatile uint8_t *>(gNbMaps[i]->getVirtualAddress());
    }
    n[0] = 2;
    if (!&native_gpu_sqe_firmware || !&native_gpu_sqe_firmware_size || native_gpu_sqe_firmware_size < 8 ||
        native_gpu_sqe_firmware_size - 4 > gNb[0].bytes) { report("no sqe"); return false; }
    const uint32_t sqeBytes = native_gpu_sqe_firmware_size - 4;
    n[1] = sqeBytes;
    for (uint32_t i = 0; i < sqeBytes; ++i) gNbCpu[0][i] = native_gpu_sqe_firmware[4 + i];
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint32_t i = 0; i < sqeBytes && !n[2]; ++i) if (gNbCpu[0][i] != native_gpu_sqe_firmware[4 + i]) n[2] = i + 1;
    if (n[2]) { report("sqe readback"); return false; }
    n[0] = 3; native_gpu_step = 121;
    hwInit();
    ucheRow();
    gw64(0x830, gNb[0].iova);
    n[0] = 4; native_gpu_step = 122;
    gRing = new NativeAdrenoRing;
    gCpInit = new NativeAdrenoCpInit;
    if (!gRing || !gCpInit || gRing->setGeneration(6)) { report("no ring"); return false; }
    n[3] = uint32_t(gRing->attach(&gRio, &gCache, const_cast<uint8_t *>(gNbCpu[1]), gNb[1].bytes, gNb[1].iova,
                                  const_cast<uint8_t *>(gNbCpu[2]), gNb[2].bytes, gNb[2].iova));
    if (n[3]) { report("ring attach"); return false; }
    gw(0x808, 1);
    IODelay(100);
    {
        gw(0x808, 3);
        IODelay(20);
        auto read_icache = [&](uint32_t idx) -> uint32_t {
            gw(0x908, (1u << 28) | idx);
            return gpuRead(0x909);
        };
        uint32_t u00 = read_icache(0x00), u01 = read_icache(0x01), u3f = read_icache(0x3f);
        uint32_t u40 = read_icache(0x40), u41 = read_icache(0x41), u7f = read_icache(0x7f), u80 = read_icache(0x80);
        gw(0x808, 1);
        IODelay(20);
        char icacheMsg[160];
        snprintf(icacheMsg, sizeof icacheMsg,
                 "SQE_ICACHE iova=0x%llx pa=0x%llx u00=%08x u01=%08x u3f=%08x u40=%08x u41=%08x u7f=%08x u80=%08x",
                 (unsigned long long)gNb[0].iova, (unsigned long long)gNb[0].physical, u00, u01, u3f, u40, u41, u7f, u80);
        umi_boot_log_line("GPU", icacheMsg);
    }
    n[0] = 5; native_gpu_step = 123;
    NativeAdrenoCpInitConfig config{gNb[0].iova, 0, 1};
    config.generation = 6;
    uint32_t fence = 0;
    n[4] = uint32_t(gCpInit->run(&gRio, gRing, &config, &fence));
    if (n[4]) { report("cp init"); return false; }
    n[5] = uint32_t(gRing->wait(fence, &gClock, 200000, &n[6]));
    if (n[5]) { report("cp init fence"); return false; }
    n[0] = 6; native_gpu_step = 124;
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gNbCpu[4]);
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t words[4] = {};
    if (AdrenoPM4Type7(0x3d, 3, &words[0]) != AdrenoPM4Success) { report("pm4"); return false; }
    words[1] = uint32_t(gNb[4].iova); words[2] = uint32_t(gNb[4].iova >> 32); words[3] = 0xA6500C0D;
    if (gRing->submit(words, 4, &fence)) { report("submit"); return false; }
    uint32_t seen = 0;
    n[7] = uint32_t(gRing->wait(fence, &gClock, 200000, &seen));
    n[8] = scratch[0];
    if (n[7] || n[8] != 0xA6500C0D) { report("mem write"); return false; }
    n[0] = 7;
    report("ok");
    return true;
}

struct ScmRes { int64_t a0; uint64_t a1; };
uint32_t gScmResumes;
ScmRes scmCall(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3 = 0) {
    const boolean_t was = ml_set_interrupts_enabled(FALSE);
    ScmRes out{-1000, 0};
    uint64_t first = fn, a6 = 0;
    gScmResumes = 0;
    for (unsigned resume = 0, busy = 0; resume < 10000; gScmResumes = resume) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = arginfo, x2 asm("x2") = p0, x3 asm("x3") = p1;
        register uint64_t x4 asm("x4") = p2, x5 asm("x5") = p3, x6 asm("x6") = a6, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) { first = 1; a6 = x6; ++resume; continue; }
        if (int64_t(x0) == -12 && busy++ < 20) { first = fn; a6 = 0; IODelay(30000); continue; }
        out = ScmRes{int64_t(x0), x1};
        break;
    }
    ml_set_interrupts_enabled(was);
    return out;
}
constexpr uint64_t kScmPil(uint32_t cmd) { return 0x42000000u | (0x02u << 8) | cmd; }
constexpr uint32_t kGpuPasId = 13;
constexpr uint64_t kZapPhys = 0x8681a000, kZapBytes = 0x2000;
uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
void cleanToPoc(const void *p, size_t len) {
    uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const size_t line = 4u << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1); a < reinterpret_cast<uintptr_t>(p) + len; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t msSince(uint64_t start) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    return uint32_t(ns / 1000000);
}
extern "C" {
int32_t native_gpu_zap[16];
}
IOBufferMemoryDescriptor *gZapMeta;
void zapRow(const char *what) {
    const int32_t *z = native_gpu_zap;
    snprintf(native_gpu_rows[kRowZap], sizeof native_gpu_rows[0],
             "ZAP %s step %d init %x/%x mem %x/%x auth %x/%x resumes %d ms %d/%d/%d phdrs %d reloc %d span %x secure %d meta %x000",
             what, z[0], z[1], z[2], z[3], z[4], z[5], z[6], z[7], z[8], z[9], z[10], z[11], z[12], z[13], z[14], z[15]);
    changed();
}
bool zapLoad() {
    int32_t *z = native_gpu_zap;
    auto stop = [&](int why) { z[0] = why; zapRow("stopped"); return false; };
    native_gpu_step = 130;
    z[0] = 1;
    if (kZapPhys + kZapBytes > vm_first_phys && kZapPhys < vm_last_phys) return stop(-20);
    if (!&native_gpu_zap_firmware || !&native_gpu_zap_firmware_size) return stop(-21);
    const uint8_t *fw = native_gpu_zap_firmware;
    const uint64_t size = native_gpu_zap_firmware_size;
    if (size < 52 || fw[0] != 0x7f || fw[1] != 'E' || fw[2] != 'L' || fw[3] != 'F' || fw[4] != 1) return stop(-1);
    const uint32_t phoff = le32(fw + 28);
    const unsigned phent = le16(fw + 42), phnum = le16(fw + 44);
    z[11] = int32_t(phnum);
    if (phent != 32 || phnum < 2 || phnum > 16 || phoff + uint64_t(phnum) * 32 > size) return stop(-2);
    auto ph = [&](unsigned i, unsigned field) { return le32(fw + phoff + i * 32 + field * 4); };
    if (ph(0, 0) == 1) return stop(-3);
    unsigned hash = 0;
    for (unsigned i = 1; i < phnum && !hash; ++i) if (((ph(i, 6) >> 24) & 7) == 2) hash = i;
    if (!hash) return stop(-4);
    uint64_t lo = ~0ull, hi = 0;
    bool relocate = false;
    for (unsigned i = 0; i < phnum; ++i) {
        if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;
        if (ph(i, 6) & (1u << 27)) relocate = true;
        lo = lo < ph(i, 3) ? lo : ph(i, 3);
        const uint64_t end = (uint64_t(ph(i, 3)) + ph(i, 5) + 0xfff) & ~0xfffull;
        hi = hi > end ? hi : end;
    }
    z[12] = relocate; z[13] = int32_t(hi - lo);
    if (lo >= hi || hi - lo > kZapBytes) return stop(-5);
    const uint64_t ehdr = ph(0, 4), hashBytes = ph(hash, 4), hashOff = ph(hash, 1);
    if (ehdr > size || hashOff + hashBytes > size || ehdr + hashBytes > 0x4000) return stop(-6);
    z[0] = 2;
    const size_t metaBytes = size_t((ehdr + hashBytes + 0xfff) & ~0xfffull);
    gZapMeta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        metaBytes, 0xFFFFFFF000ULL);
    if (!gZapMeta || gZapMeta->prepare() != kIOReturnSuccess) return stop(-7);
    uint8_t *meta = static_cast<uint8_t *>(gZapMeta->getBytesNoCopy());
    const uint64_t metaPhys = gZapMeta->getPhysicalSegment(0, nullptr);
    z[15] = int32_t(metaPhys >> 12);
    if (!meta || !metaPhys) return stop(-8);
    for (uint64_t i = 0; i < ehdr; ++i) meta[i] = fw[i];
    for (uint64_t i = 0; i < hashBytes; ++i) meta[ehdr + i] = fw[hashOff + i];
    cleanToPoc(meta, metaBytes);
    z[0] = 3; native_gpu_step = 131;
    uint64_t start = mach_absolute_time();
    ScmRes r = scmCall(kScmPil(0x01), 2 | (2u << 6), kGpuPasId, metaPhys, 0);
    z[1] = int32_t(r.a0); z[2] = int32_t(r.a1); z[8] = int32_t(msSince(start)); z[7] = int32_t(gScmResumes);
    if (r.a0 || r.a1) return stop(3);
    if (relocate) {
        z[0] = 4; native_gpu_step = 132;
        start = mach_absolute_time();
        r = scmCall(kScmPil(0x02), 3, kGpuPasId, kZapPhys, hi - lo);
        z[3] = int32_t(r.a0); z[4] = int32_t(r.a1); z[9] = int32_t(msSince(start));
        if (r.a0 || r.a1) return stop(4);
    }
    z[0] = 5; native_gpu_step = 133;
    {
        IODeviceMemory *m = IODeviceMemory::withRange(kZapPhys, kZapBytes);
        OSSharedPtr<IOMemoryMap> map;
        if (m) { map = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain); m->release(); }
        if (!map) return stop(-9);
        volatile uint8_t *region = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
        const uint64_t base = relocate ? lo : kZapPhys;
        for (unsigned i = 0; i < phnum; ++i) {
            if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;
            const uint64_t at = uint64_t(ph(i, 3)) - base, file = ph(i, 4), mem = ph(i, 5), off = ph(i, 1);
            if (ph(i, 3) < base || at + mem > kZapBytes || file > mem || off + file > size) return stop(-10);
            for (uint64_t b = 0; b < file; ++b) region[at + b] = fw[off + b];
            for (uint64_t b = file; b < mem; ++b) region[at + b] = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
    z[0] = 6; native_gpu_step = 134;
    start = mach_absolute_time();
    r = scmCall(kScmPil(0x05), 1, kGpuPasId, 0, 0);
    z[5] = int32_t(r.a0); z[6] = int32_t(r.a1); z[10] = int32_t(msSince(start));
    if (r.a0 || r.a1) return stop(6);
    z[0] = 7;
    return true;
}

bool leaveSecure() {
    if (!zapLoad()) {
        gw(0xF400, 0);
    }
    native_gpu_step = 135;
    uint32_t words[2] = {}, fence = 0, seen = 0;
    if (AdrenoPM4Type7(0x66, 1, &words[0]) != AdrenoPM4Success || gRing->submit(words, 2, &fence)) { zapRow("submit"); return false; }
    native_gpu_zap[14] = int32_t(gRing->wait(fence, &gClock, 200000, &seen));
    zapRow(native_gpu_zap[14] ? "secure exit timeout" : "ok");
    return native_gpu_zap[14] == 0;
}

void gpuIrq(void *) {
    const uint32_t status = gpuRead(0x201);
    if (status) gw(0x37, status);
    gIrqRepeat = (status == gIrqLast && !(status & (1u << 20))) ? gIrqRepeat + 1 : 0;
    gIrqLast = status;
    gIrqSeen = gIrqSeen | status;
    gIrqCount = gIrqCount + 1;
    if (status & (1u << 8)) { gPreemptIrqs = gPreemptIrqs + 1; gPreemptPending = 1; thread_wakeup(event_t(&gPreemptPending)); }
    if (gIrqRepeat > 100000) { gw(0x38, 0); gIrqMasked = 1; }
    thread_wakeup(event_t(&gIrqCount));
}

bool gpuInterrupt() {
    native_gpu_step = 140;
    uint32_t readback = 0;
    uint64_t router = ~0ull;
    const IOReturn attached = armcpu_spi_attach(32 + 300, false, gpuIrq, nullptr, &readback, &router);
    gIrqAttached = attached == kIOReturnSuccess;
    IOSleep(20);
    const uint32_t first = gIrqCount;
    uint32_t nop = 0, fence = 0, seen = 0;
    const bool fenced = AdrenoPM4Type7(0x10, 0, &nop) == AdrenoPM4Success && !gRing->submit(&nop, 1, &fence) &&
                        !gRing->wait(fence, &gClock, 200000, &seen);
    IOSleep(20);
    snprintf(native_gpu_rows[kRowIrq], sizeof native_gpu_rows[0],
             "IRQ attach %x readback %x router %llx count %u>%u nop fence %u seen %08x status now %08x masked %u",
             static_cast<unsigned>(attached), readback, static_cast<unsigned long long>(router), first, gIrqCount, fenced, gIrqSeen,
             gpuRead(0x201), gIrqMasked);
    changed();
    return gIrqAttached && fenced;
}

NativeAdrenoAbiDeviceInfo kDeviceInfo = {
    0x06050002u, 0x120000u, 0, 0x1fffffffff000ull, 0x20000000ull, 0x60000000ull,
    16, 6, 1, 4, NativeAdrenoDeviceClientIova, 0
};
NativeAdrenoSubmit *gSubmit;
NativeAdrenoTimer *gTimer;
NativeAdrenoTimeIo gTimerIo{nullptr, naRead};
extern "C" {
uint32_t native_gpu_publish[16];
}
void publishRow(const char *what) {
    const uint32_t *p = native_gpu_publish;
    snprintf(native_gpu_rows[kRowPublish], sizeof native_gpu_rows[0],
             "PUBLISH %s submit %x timer %x publish %x | uc %u/%u first %x at %u cpumap %x | rounds %d %d",
             what, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], int32_t(p[8]), int32_t(p[9]));
    changed();
}

extern "C" bool native_adreno_preempt_irq();
extern "C" {extern volatile uint32_t umi_job_latency[8];}
extern "C" {extern volatile uint32_t native_adreno_wptr_fence[4];}
extern "C" int native_adreno_kick_if_stalled(uint32_t out[4]);
extern "C" {extern volatile uint32_t native_legacy_swap_timeouts __attribute__((weak));}
extern "C" {extern volatile uint32_t umi_sys_hist_cnt[560] __attribute__((weak)); extern volatile uint32_t umi_touch_script[4] __attribute__((weak));}   // sleh.c: the lsd syscall profile
extern "C" {extern volatile uint32_t native_adreno_cp_aperture[4];}
// qcom_scm_set_gpu_smmu_aperture (qcom_scm.c; arm-smmu-qcom.c calls it for the GPU's context bank): lets the CP program the bank's
// TTBR0 itself, which the driver's table mode 2 (and with it the second ring's work) needs. [0] availability, [1] the call's x0.
void cpAperture() {
    constexpr uint64_t kSip = 0x42000000u;                       // SMC64 standard call, owner SIP, as kScmPil
    const uint64_t setId = (0x0cu << 8) | 0x1bu;                 // QCOM_SCM_SVC_MP, QCOM_SCM_MP_CP_SMMU_APERTURE_ID
    const ScmRes avail = scmCall(kSip | (0x06u << 8) | 0x01u, 1, (0x02u << 24) | setId, 0, 0);   // QCOM_SCM_INFO_IS_CALL_AVAIL
    native_adreno_cp_aperture[0] = avail.a0 == 0 ? uint32_t(avail.a1) : 0x80000000u | uint32_t(-avail.a0);
    if (avail.a0 || !avail.a1) return;
    const ScmRes r = scmCall(kSip | setId, 4, 0xffff0000u | (0u << 8) | (kGpuBank & 0xffu), 0xffffffffu, 0xffffffffu, 0xffffffffu);
    native_adreno_cp_aperture[1] = uint32_t(r.a0);
}
extern "C" char boot_progress_ext[8][48];   // the touch driver's FTS/TOUCH/HID rows (UmiFtsTouch.cpp)
extern "C" {extern volatile uint32_t native_t970_submit_fail[5];}
NativeAdrenoRing *gRing1;
uint32_t gPreemptSetup[4];   // [0] mode, [1] failing step, [2] status, [3] done
int preemptWrite(void *, uint32_t reg, uint32_t value) { gw(reg, value); return 0; }
int preemptRead(void *, uint32_t reg, uint32_t *value) { *value = gpuRead(reg); return 0; }
int preemptBarrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); return 0; }
// The driver names GMU registers by a6xx_gmu.xml dword (GPU-relative, 0x1f8c4); the GMU window starts 0x6a000 bytes into the GPU's.
int preemptGmuWrite(void *, uint32_t dword, uint32_t value) {
    return dword * 4 < 0x6a000 ? 1 : gmuWrite(nullptr, A650GmuCore, dword * 4 - 0x6a000, value);
}
void preemptThread(void *, wait_result_t) {
    for (;;) {
        if (!gPreemptPending) {
            assert_wait_timeout(event_t(&gPreemptPending), THREAD_UNINT, 2, NSEC_PER_MSEC);
            thread_block(THREAD_CONTINUE_NULL);
        }
        gPreemptPending = 0;
        (void)native_adreno_preempt_irq();
    }
}
// gpu-preempt=1|2: the second ring and the CP switch between them (NativeAdrenoSubmit::setPreemption), before publication.
bool preemptSetup() {
    uint32_t *r = gPreemptSetup;
    unsigned mode = 0;
    PE_parse_boot_argn("gpu-preempt", &mode, sizeof mode);
    r[0] = mode;
    if (!mode) return true;
    auto fail = [&](uint32_t step, IOReturn status) { r[1] = step; r[2] = uint32_t(status); return false; };
    for (unsigned i = 5; i < GpuBufferCount; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(gNb[i].physical, gNb[i].bytes);
        if (m) {
            gNbMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gNbMaps[i]) return fail(1, kIOReturnNoMemory);
        gNbCpu[i] = reinterpret_cast<volatile uint8_t *>(gNbMaps[i]->getVirtualAddress());
    }
    gRing1 = new NativeAdrenoRing;
    if (!gRing1 || gRing1->setGeneration(6)) return fail(2, kIOReturnNoMemory);
    IOReturn st = gRing1->attachSecondary(&gRio, &gCache, const_cast<uint8_t *>(gNbCpu[9]), gNb[9].bytes, gNb[9].iova,
                                          const_cast<uint8_t *>(gNbCpu[10]), gNb[10].bytes, gNb[10].iova);
    if (st) return fail(3, st);
    if ((st = gRing->setPreempt(gNb[5].iova, gNb[7].iova, 0))) return fail(4, st);
    if ((st = gRing1->setPreempt(gNb[6].iova, gNb[8].iova, 0))) return fail(5, st);
    NativeAdrenoSubmit::PreemptSetup setup;
    setup.io = {nullptr, preemptWrite, preemptRead, preemptBarrier, nullptr, preemptGmuWrite};
    for (unsigned i = 0; i < NativeAdrenoPreemptRingCount; ++i) {
        setup.pages.record[i] = gNb[5 + i].iova;
        setup.pages.smmu_info[i] = gNb[7 + i].iova;
        setup.pages.record_cpu[i] = const_cast<uint8_t *>(gNbCpu[5 + i]);
        setup.pages.smmu_cpu[i] = const_cast<uint8_t *>(gNbCpu[7 + i]);
    }
    setup.now = naNow;
    setup.mode = mode >= 2 ? 2 : 1;
    thread_t th = nullptr;
    if (kernel_thread_start(preemptThread, nullptr, &th) != KERN_SUCCESS) return fail(6, kIOReturnNoResources);
    thread_deallocate(th);
    gPreemptOn = true;
    gw(0x038, NativeAdrenoIrq::maskA6xx() | (1u << 8));
    if ((st = gSubmit->setPreemption(gRing, gRing1, setup, true))) {
        gPreemptOn = false;
        gw(0x038, NativeAdrenoIrq::maskA6xx());
        return fail(7, st);
    }
    r[3] = 1;
    return true;
}

bool publish() {
    uint32_t *p = native_gpu_publish;
    native_gpu_step = 150;
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    gSubmit = new NativeAdrenoSubmit;
    gTimer = new NativeAdrenoTimer;
    if (!vm || !gSubmit || !gTimer) { publishRow("no objects"); return false; }
    kDeviceInfo.gmem_base = gUcheGmem16M ? 0x1000000 : 0;
    // As rui12366B/UniXNU-gts7xlwifi publish(): per-process address spaces, each user connection mapping from GpuClientVaStart
    // (512 MiB, above the gts7 driver's kernel and preemption buffers) to 2^vaBits in its own table. GPU addresses above 4 GiB hang
    // the A650 (gts7xlwifi v72, v77), so the window ends at 4 GiB (umi-gpu-va-bits, 32 by default, 32..48, capped by the table).
    unsigned vaBits = 32;
    PE_parse_boot_argn("umi-gpu-va-bits", &vaBits, sizeof vaBits);
    NativeAdrenoGpuContext *ctx = NativeAdrenoGpuContextObject();
    const unsigned tableBits = ctx ? ctx->inputBits() : 0;
    if (vaBits > tableBits) vaBits = tableBits;
    if (vaBits < 32 || vaBits > 48) { publishRow("no client window"); return false; }
    kDeviceInfo.va_start = GpuClientVaStart;
    kDeviceInfo.va_size = (uint64_t(1) << vaBits) - kDeviceInfo.va_start;
    p[0] = uint32_t(gSubmit->attach(vm, gRing));
    if (!p[0]) (void)preemptSetup();   // a failed switch setup keeps the first ring alone
    // umi-173: the CP aperture (table mode 2: the CP switches tables in the ring) no longer depends on gpu-preempt. In mode 1 every
    // switch between processes waited for an idle GPU on the CPU (JOBS: 1036 of 5800 jobs 8-16 ms, display waits avg 7.3 ms).
    // umi-gpu-aperture=0 keeps the CPU's switch.
    {
        unsigned aperture = 1;
        PE_parse_boot_argn("umi-gpu-aperture", &aperture, sizeof aperture);
        if (aperture || gPreemptSetup[0]) cpAperture();   // before publication: its table mode reads the result
    }
    p[1] = uint32_t(gTimer->attach(&gTimerIo, NativeAdrenoTimeCp));
    p[2] = uint32_t(native_adreno_publish(vm, gSubmit, gRing, &gClock, p[1] ? nullptr : gTimer, &kDeviceInfo, vaBits,
                                          native_gpu_context.output_bits));
    p[10] = vaBits | tableBits << 8;
    publishRow(p[0] || p[2] ? "failed" : "ok");
    return !p[0] && !p[2];
}

IOReturn ucCall(IOUserClient *uc, uint32_t method, const void *in, void *out) {
    IOExternalMethodArguments a{};
    a.version = kIOExternalMethodArgumentsCurrentVersion;
    a.selector = method;
    const size_t is = NativeAdrenoUserClient::inputSize(method), os = NativeAdrenoUserClient::outputSize(method);
    a.structureInput = is ? in : nullptr;
    a.structureInputSize = uint32_t(is);
    a.structureOutput = os ? out : nullptr;
    a.structureOutputSize = uint32_t(os);
    return uc->externalMethod(method, &a, nullptr, nullptr, nullptr);
}
int ucRound(IOService *svc) {
    uint32_t *p = native_gpu_publish;
    auto step = [&](IOReturn status) {
        const uint32_t at = p[4]++;
        if (!status) ++p[3];
        else if (!p[5]) { p[5] = uint32_t(status); p[6] = at; }
        snprintf(native_gpu_rows[kRowUc], sizeof native_gpu_rows[0], "UCSTEP round%u at%u st%x ok%u first%x@%u", p[8] ? 2u : 1u, at,
                 uint32_t(status), p[3], p[5], p[6]);
        changed();
        return !status;
    };
    constexpr UInt32 kPluginOpenType = 0x4147;
    IOUserClient *uc = nullptr;
    if (!step(svc->newUserClient(kernel_task, nullptr, kPluginOpenType, nullptr, &uc)) || !uc) return -1;
    IOUserClient *second = nullptr;
    step(svc->newUserClient(kernel_task, nullptr, kPluginOpenType, nullptr, &second) == kIOReturnBusy ? kIOReturnSuccess
                                                                                                       : kIOReturnError);
    if (second) second->release();
    NativeAdrenoAbiParams params{};
    step(ucCall(uc, NativeAdrenoMethodQuery, nullptr, &params));
    NativeAdrenoAbiDeviceInfo info{};
    step(ucCall(uc, NativeAdrenoMethodGetDeviceInfo, nullptr, &info));
    step(info.chip_id == kDeviceInfo.chip_id && info.gmem_bytes == kDeviceInfo.gmem_bytes ? kIOReturnSuccess : kIOReturnError);
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib{}, tgt{};
    int matched = -1;
    IOMemoryDescriptor *desc[2] = {};
    IOMemoryMap *maps[2] = {};
    if (step(ucCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &ib)) && step(ucCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &tgt))) {
        const uint64_t ibIova = 0x20004000, tgtIova = 0x20005000;
        NativeAdrenoAbiBoMap mapIb{ib.handle, ibIova, 1, 0, 2, 0}, mapTgt{tgt.handle, tgtIova, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        NativeAdrenoAbiBoCpuMap cmIb{ib.handle}, cmTgt{tgt.handle};
        NativeAdrenoAbiBoCpuMapReply crIb{}, crTgt{};
        if (step(ucCall(uc, NativeAdrenoMethodBoMap, &mapIb, &r)) && step(ucCall(uc, NativeAdrenoMethodBoMap, &mapTgt, &r)) &&
            step(ucCall(uc, NativeAdrenoMethodBoCpuMap, &cmIb, &crIb)) && step(ucCall(uc, NativeAdrenoMethodBoCpuMap, &cmTgt, &crTgt))) {
            IOOptionBits opt[2] = {};
            const uint32_t tokens[2] = {crIb.token, crTgt.token};
            bool mapped = true;
            for (int i = 0; i < 2; ++i) {
                const IOReturn status = uc->clientMemoryForType(tokens[i], &opt[i], &desc[i]);
                if (!p[7]) p[7] = uint32_t(status);
                if (status || !desc[i]) { mapped = false; continue; }
                maps[i] = desc[i]->createMappingInTask(kernel_task, 0, opt[i]);
                if (!maps[i]) { mapped = false; if (!p[7]) p[7] = 0xbad; }
            }
            IOMemoryDescriptor *none = nullptr;
            IOOptionBits o = 0;
            step(uc->clientMemoryForType(0, &o, &none) != kIOReturnSuccess && !none ? kIOReturnSuccess : kIOReturnError);
            step(mapped ? kIOReturnSuccess : kIOReturnNoMemory);
            if (mapped) {
                volatile uint32_t *iw = reinterpret_cast<volatile uint32_t *>(maps[0]->getVirtualAddress());
                volatile uint32_t *tw = reinterpret_cast<volatile uint32_t *>(maps[1]->getVirtualAddress());
                for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
                const uint32_t values[3] = {0x0badc0deu, 0xfeed0001u, 0x600d0003u};
                uint32_t words[12];
                bool built = true;
                for (unsigned i = 0; i < 3; ++i) {
                    built = built && AdrenoPM4Type7(0x3d, 3, &words[i * 4]) == AdrenoPM4Success;
                    words[i * 4 + 1] = uint32_t(tgtIova + i * 4); words[i * 4 + 2] = uint32_t(tgtIova >> 32);
                    words[i * 4 + 3] = values[i];
                }
                for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
                __asm__ volatile("dsb sy" ::: "memory");
                // umi-150: on the stack (3 KiB with 128 IB slots) it overflowed the 16 KiB kernel stack inside the submit's mappings.
                auto *submit = static_cast<NativeAdrenoAbiSubmit *>(IOMallocZero(sizeof(NativeAdrenoAbiSubmit)));
                if (submit) { submit->buffer_count = 1; submit->buffers[0] = {ib.handle, ibIova, sizeof words}; }
                NativeAdrenoAbiSubmitReply sr{};
                const bool submitted = step(built && submit ? kIOReturnSuccess : kIOReturnInternalError) &&
                                       step(ucCall(uc, NativeAdrenoMethodSubmit, submit, &sr));
                if (submit) IOFree(submit, sizeof(NativeAdrenoAbiSubmit));
                if (submitted) {
                    NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
                    if (step(ucCall(uc, NativeAdrenoMethodWaitFence, &wait, nullptr))) {
                        __asm__ volatile("dsb sy" ::: "memory");
                        matched = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
                    }
                }
                NativeAdrenoAbiTimestamp ts{};
                step(ucCall(uc, NativeAdrenoMethodGetTimestamp, nullptr, &ts));
            }
        }
    }
    for (auto *m : maps) if (m) m->release();
    for (auto *d : desc) if (d) d->release();
    step(uc->clientClose());
    uc->release();
    return matched;
}

bool selfTest() {
    unsigned on = 1;   // umi-150: the "hang" was a kernel stack overflow (submit struct on the stack), fixed in ucRound; =0 skips it
    PE_parse_boot_argn("umi-gpu-selftest", &on, sizeof on);
    if (!on) return true;
    native_gpu_step = 155;
    IOService *svc = native_adreno_published_service();
    if (!svc) { publishRow("no service"); return false; }
    // umi-161: even with the submit struct on the heap, the bring-up thread's deep stack plus the user-client path (3.5 KiB
    // mailbox buffers, 3 KiB IB copies, memory-descriptor mapping) still overflowed 16 KiB when an IRQ landed at the bottom.
    // Both rounds run on a fresh kernel thread, whose stack starts empty; the bring-up waits for it (10 s at most).
    static IOService *gSelfSvc;
    static volatile uint32_t gSelfDone;
    gSelfSvc = svc; gSelfDone = 0;
    thread_t th = nullptr;
    const kern_return_t kr = kernel_thread_start([](void *, wait_result_t) {
        native_gpu_publish[8] = uint32_t(ucRound(gSelfSvc));
        native_gpu_step = 156;
        native_gpu_publish[9] = uint32_t(ucRound(gSelfSvc));
        __asm__ volatile("dmb ish" ::: "memory");
        gSelfDone = 1;
    }, nullptr, &th);
    if (kr != KERN_SUCCESS) { publishRow("no self test thread"); return false; }
    thread_deallocate(th);
    for (unsigned i = 0; i < 1000 && !gSelfDone; ++i) IOSleep(10);
    if (!gSelfDone) { publishRow("self test timeout"); return false; }
    const bool ok = native_gpu_publish[8] == 4 && native_gpu_publish[9] == 4;
    publishRow(ok ? "ok" : "self test failed");
    return ok;
}

bool metal() {
    char *row = native_gpu_rows[kRowMetal];
    const size_t cap = sizeof native_gpu_rows[0];
    unsigned stage = 0;
    auto stop = [&](IOReturn why) {
        snprintf(row, cap, "METAL stopped at %u: %x", stage, static_cast<unsigned>(why));
        changed();
        return false;
    };
    native_gpu_step = 160;
    stage = 1;
    const kern_return_t kext = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (kext) return stop(kext);
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) return stop(kIOReturnNotFound);
    stage = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return stop(kIOReturnNoResources);
    native_adreno_accelerator_static_init();
    const IOReturn loaded = OSMetaClass::postModLoad(handle);
    if (loaded) return stop(loaded);
    if (native_adreno_accelerator_instance_size() != 136) return stop(kIOReturnInvalid);
    stage = 3;
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator) return stop(kIOReturnNoMemory);
    if (!accelerator->init()) { accelerator->release(); return stop(kIOReturnError); }
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("NativeSandboxGrantProcess", "backboardd,iconservicesagent,Preferences,SpringBoard,PosterBoard");
    accelerator->setProperty("NativeSandboxGrantEntitlement", "com.apple.security.ts.opengl-or-metal");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    char turnip[64] = "nocb";
    PE_parse_boot_arg_str("umi-turnip-debug", turnip, sizeof turnip);
    accelerator->setProperty("NativeTurnipDebug", turnip);
    char withhold[64] = "apple-families";
    PE_parse_boot_arg_str("umi-metal-withhold", withhold, sizeof withhold);
    if (withhold[0] && strcmp(withhold, "none"))
        accelerator->setProperty("NativeMetalWithhold", withhold);
    stage = 4;
    if (!accelerator->attach(IOService::getPlatform())) { accelerator->release(); return stop(kIOReturnNotAttached); }
    accelerator->registerService();
    snprintf(row, cap, "METAL ok: accelerator registered (IOGPUFamily loaded, class size 136), Turnip options %s", turnip);
    changed();
    return true;
}

void hangRows(unsigned t) {
    static unsigned firstT;
    static uint64_t firstIb1, firstIb2;
    static uint32_t firstRem1, firstRem2;
    static bool seen;
    const uint64_t ib1 = gpuRead(0x928) | uint64_t(gpuRead(0x929)) << 32, ib2 = gpuRead(0x92b) | uint64_t(gpuRead(0x92c)) << 32;
    const uint32_t rem1 = gpuRead(0x92a), rem2 = gpuRead(0x92d);
    if (!seen) { seen = true; firstT = t; firstIb1 = ib1; firstIb2 = ib2; firstRem1 = rem1; firstRem2 = rem2; }
    snprintf(native_gpu_rows[kRowHang], sizeof native_gpu_rows[0],
        "HANG st%08x %x/%x/%x r%x/%x | ib1 %llx+%x ib2 %llx+%x | hw%x cpi%x prot%x", gpuRead(0x210), gpuRead(0x211),
        gpuRead(0x212), gpuRead(0x213), gpuRead(0x806), gpuRead(0x807), (unsigned long long)ib1, rem1, (unsigned long long)ib2, rem2,
        gpuRead(0x821), gpuRead(0x823), gpuRead(0x824));
    gw(0x908, 0);
    const uint32_t pc = gpuRead(0x909), op = gpuRead(0x909);
    const uint32_t bank0 = smmuBank0();
    snprintf(native_gpu_rows[kRowHang2], sizeof native_gpu_rows[0],
        "HANG sqe pc%x op%08x | fsr%x far%08x%08x fsynr%x | first t%u ib1 %llx+%x ib2 %llx+%x", pc, op, smmuRead(bank0 + 0x58),
        smmuRead(bank0 + 0x64), smmuRead(bank0 + 0x60), smmuRead(bank0 + 0x68), firstT, (unsigned long long)firstIb1, firstRem1,
        (unsigned long long)firstIb2, firstRem2);
    changed();
}

// umi direct scanout groundwork: the apps SMMU (0x15000000) routes of the display (stream IDs 0x800..0x8ff, kona's MDSS range) and
// the context bank they use, plus the display driver's direct-scanout probe (native_t970_direct).
extern "C" {extern volatile uint32_t native_t970_direct[11] __attribute__((weak)); extern volatile uint32_t native_legacy_direct[14] __attribute__((weak));
           extern volatile uint32_t native_umi_dpu[8]; extern volatile uint32_t native_ufs_dpu_state;
           extern volatile uint32_t native_legacy_swapstat[16] __attribute__((weak)); extern volatile uint32_t native_t970_swapfmt[5] __attribute__((weak));}
void dpuSmmuRows(unsigned t) {
    static volatile uint32_t *apps = nullptr;
    static uint32_t pageBytes = 0, pages = 0;
    if (!apps) {
        volatile uint32_t *g0 = (volatile uint32_t *)(uintptr_t)ml_io_map(0x15000000, 0x1000);
        if (!g0) return;
        const uint32_t id1 = g0[0x24 / 4];
        pageBytes = (id1 >> 31) ? 0x10000u : 0x1000u;
        pages = 1u << (((id1 >> 28) & 7) + 1);
        apps = (volatile uint32_t *)(uintptr_t)ml_io_map(0x15000000, 2ull * pages * pageBytes);
        if (!apps) return;
    }
    const uint32_t id0 = apps[0x20 / 4], id1 = apps[0x24 / 4], count = id0 & 0xff, banks = id1 & 0xff;
    unsigned matches = 0, first = ~0u;
    uint32_t smr[3] = {}, s2cr[3] = {}, idx[3] = {};
    for (unsigned i = 0; i < count && i < 128; ++i) {
        const uint32_t r = apps[(0x800 + 4 * i) / 4];
        if (!(r & 0x80000000u)) continue;
        const uint32_t id = r & 0x7fff;
        if (id < 0x800 || id > 0x8ff) continue;
        if (matches < 3) { smr[matches] = r; s2cr[matches] = apps[(0xc00 + 4 * i) / 4]; idx[matches] = i; }
        if (first == ~0u) first = i;
        ++matches;
    }
    char line[sizeof native_gpu_rows[0]];
    const uint32_t cb = matches ? (s2cr[0] & 0xff) : 0;
    const volatile uint32_t *c = apps + (pages * pageBytes + cb * pageBytes) / 4;
    snprintf(line, sizeof line, "DPUSMMU n%u/%u b%u pg%x | %u:%x/%x %u:%x/%x %u:%x/%x | t%u", matches, count, banks, pageBytes,
             idx[0], smr[0], s2cr[0], idx[1], smr[1], s2cr[1], idx[2], smr[2], s2cr[2], t);
    strlcpy(native_gpu_rows[kRowDpuSmmu], line, sizeof native_gpu_rows[0]);
    snprintf(line, sizeof line, "DPUCB cb%u sctlr %x tcr %x tcr2 %x ttbr0 %x%08x mair %x cbar %x cba2r %x fsr %x | t%u", cb, c[0], c[0x30 / 4],
             c[0x10 / 4], c[0x24 / 4], c[0x20 / 4], c[0x38 / 4], apps[(0x1000 * (pageBytes / 0x1000) + 4 * cb) / 4],
             apps[(0x1000 * (pageBytes / 0x1000) + 0x800 + 4 * cb) / 4], c[0x58 / 4], t);
    strlcpy(native_gpu_rows[kRowDpuSmmu2], line, sizeof native_gpu_rows[0]);
    if (&native_t970_direct && &native_legacy_direct) {
        const volatile uint32_t *d = native_t970_direct, *l = native_legacy_direct;
        snprintf(line, sizeof line, "DIRECT st%u pipes%x ctl%u flush%x boot%x stride%x src %x ufs%x | dpu%x cb%u sc%x m%u f%u | flips%u map%u fail%u back%u | t%u",
                 d[0], d[1], d[2], d[3], d[4], d[5], d[6], native_ufs_dpu_state, native_umi_dpu[0], native_umi_dpu[2], native_umi_dpu[5],
                 native_umi_dpu[3], native_umi_dpu[7], l[0], l[1], l[2], l[7], t);
        strlcpy(native_gpu_rows[kRowDirect], line, sizeof native_gpu_rows[0]);
    }
    if (&native_legacy_swapstat && &native_t970_swapfmt && &native_legacy_direct) {
        const volatile uint32_t *w = native_legacy_swapstat, *f = native_t970_swapfmt, *l = native_legacy_direct;
        snprintf(line, sizeof line, "DIRECT2 val%u kick%u ref%u/%u/%u lat%u cpy%u flip%u latch%u ret%u | fmt%x xr%u | l50 %u max%u w%u/%u | t%u",
                 w[0], w[1], w[3], w[4], w[5], w[6], w[7], w[9], w[10], w[11], f[0], f[1], l[3], l[4], l[9], l[13], t);
        strlcpy(native_gpu_rows[kRowDirect2], line, sizeof native_gpu_rows[0]);
    }
    changed();
}

void heartbeatThread(void *, wait_result_t) {
    uint32_t lastRptr = ~0u, lastIb1 = ~0u, lastRem1 = ~0u;
    uint64_t lastBusy = 0, lastNs = 0;
    for (unsigned t = 0;; t += 2) {
        uint32_t lo = 0, hi = 0, hi2 = 0;
        gmuRead(nullptr, A650GmuCore, 0x5045 * 4, &hi);
        gmuRead(nullptr, A650GmuCore, 0x5044 * 4, &lo);
        gmuRead(nullptr, A650GmuCore, 0x5045 * 4, &hi2);
        if (hi2 != hi) gmuRead(nullptr, A650GmuCore, 0x5044 * 4, &lo);
        const uint64_t busy = uint64_t(hi2) << 32 | lo;
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        const uint32_t permille = lastNs && ns > lastNs && busy >= lastBusy
            ? uint32_t((busy - lastBusy) * 1000000000ull / 19200000ull * 1000 / (ns - lastNs)) : 0;
        lastBusy = busy; lastNs = ns;
        const NativeAdrenoSubmitStats st = gSubmit->stats();
        const uint32_t rptr = gpuRead(0x806), ib1 = gpuRead(0x928), rem1 = gpuRead(0x92a);
        if (st.submissions != st.completed && rptr == lastRptr && ib1 == lastIb1 && rem1 == lastRem1) hangRows(t);
        lastRptr = rptr; lastIb1 = ib1; lastRem1 = rem1;
        uint32_t closed = 0;
        const uint32_t opened = native_adreno_publish_stats(&closed);
        char line[sizeof native_gpu_rows[0]];
        snprintf(line, sizeof line,
            "LIVE irq%u last%x seen%x m%u | sub %u/%u t%u p%u | uc %u/%u | pool %u/%u/%u fb%u sc%u | busy %u",
            gIrqCount, gIrqLast, gIrqSeen, gIrqMasked, st.submissions, st.completed, st.timeouts, st.poisoned_jobs, opened, closed,
            native_adreno_pool_stats[3], native_adreno_pool_stats[4], native_adreno_pool_stats[0], native_adreno_pool_stats[2],
            native_adreno_pool_stats[5], permille);
        static uint32_t lastSub = ~0u, lastOpened = ~0u, lastIrqMasked = ~0u;
        if (st.submissions != lastSub || opened != lastOpened || gIrqMasked != lastIrqMasked || t % 30 == 0) {
            strlcpy(native_gpu_rows[kRowLive], line, sizeof native_gpu_rows[0]);
            lastSub = st.submissions; lastOpened = opened; lastIrqMasked = gIrqMasked;
            changed();
        }
        {
            const uint32_t *rs = native_umi_gpu_restart, *f = native_umi_gpu_fault;
            snprintf(line, sizeof line,
                "RECOVER hang%u rs %u/%u step%u st%x rbbm%x gbif%x | flt %u fsr%x far%x%08x syn%x/%x cp%x asid%u t%u | ret %u/%u",
                st.hangs, rs[4], rs[0], rs[1], rs[2], rs[3], rs[5], f[0], f[1], f[3], f[2], f[4], f[5], f[6], f[7], f[8],
                native_adreno_bank_fault_retries[0], native_adreno_bank_fault_retries[1]);
            if (strncmp(native_gpu_rows[kRowRecover], line, sizeof line)) {
                strlcpy(native_gpu_rows[kRowRecover], line, sizeof native_gpu_rows[0]);
                changed();
            }
        }
        {
            // umi recovery test: umi-gpu-inject=N stalls the CP once every N submissions, three times at most.
            static unsigned every = ~0u, done = 0, nextAt = 0;
            static int lastStatus = 0;
            if (every == ~0u) {every = 0; PE_parse_boot_argn("umi-gpu-inject", &every, sizeof every); nextAt = every;}
            if (every && done < 3 && st.submissions >= nextAt) {
                lastStatus = native_adreno_inject_hang();
                ++done; nextAt = st.submissions + every;
            }
            if (every) {
                const volatile uint32_t *a = native_adreno_recovery;
                const uint32_t *rs = native_umi_gpu_restart;
                snprintf(line, sizeof line,
                         "INJECT n%u st%x nx%u sub%u/%u h%u to%u rp%x | rec h%u rs%u f%u fen%u ret%x rep%u g%u | brd %u/%u s%u st%x | t%u",
                         done, lastStatus, nextAt, st.submissions, st.completed, st.hangs, st.timeouts, rptr, a[0], a[1], a[2], a[3], a[4], a[5], a[6],
                         rs[0], rs[4], rs[1], rs[2], t);
                if (t % 10 == 0 || strncmp(native_gpu_rows[kRowInject], line, 60)) {
                    strlcpy(native_gpu_rows[kRowInject], line, sizeof native_gpu_rows[0]);
                    changed();
                }
            }
        }
        {
            // umi touch: the FTS/TOUCH/HID overlay rows in the ring log too, every 10 s (they are otherwise only drawn on screen).
            snprintf(line, sizeof line, "TOUCH %.46s | %.40s | %.40s | t%u", boot_progress_ext[4], boot_progress_ext[5] + 6,
                     boot_progress_ext[6], t);
            if (t % 10 == 0) { strlcpy(native_gpu_rows[kRowTouch], line, sizeof native_gpu_rows[0]); changed(); }
            // umi lsd: its syscall profile (sleh.c umi_sys_hist_cnt, target = lsd) over time, to see whether a normal boot is
            // still writing the LaunchServices store when umi-ar resets (unix numbers: write 4, open 5, msync 65, fsync 95,
            // rename 128, pwrite 154, ftruncate 201, write/open/fsync_nocancel 397/398/408, openat 463, renameatx_np 488).
            if (t % 10 == 0 && &umi_touch_script && umi_touch_script[0]) {
                snprintf(native_gpu_rows[kRowTScript], sizeof native_gpu_rows[0], "TSCRIPT steps %u frames %u last %ums home %u | t%u",
                         umi_touch_script[0], umi_touch_script[1], umi_touch_script[2], umi_touch_script[3], t);
                changed();
            }
            if (t % 10 == 0) {
                const volatile uint32_t *j = umi_job_latency;
                const volatile uint32_t *l = &native_legacy_direct ? native_legacy_direct : nullptr;
                snprintf(native_gpu_rows[kRowJobs], sizeof native_gpu_rows[0],
                         "JOBS tm%u/%x <4 %u <8 %u <16 %u <33 %u <100 %u >=100 %u max %ums | dpwait n%u avg%uus max%uus 8ms%u 250ms%u | t%u",
                         native_adreno_cp_aperture[3], native_adreno_cp_aperture[2], j[0], j[1], j[2], j[3], j[4], j[5], j[6] / 1000, l ? l[9] : 0, l && l[9] ? l[10] / l[9] : 0,
                         l ? l[11] : 0, l ? l[13] : 0, l ? l[12] : 0, t);
                changed();
            }
            if (t % 10 == 0) {
                snprintf(native_gpu_rows[kRowWptr], sizeof native_gpu_rows[0], "WPTR writes %u dropped %u retries %u lost %u | swap timeouts %u | t%u",
                         native_adreno_wptr_fence[0], native_adreno_wptr_fence[1], native_adreno_wptr_fence[2], native_adreno_wptr_fence[3],
                         &native_legacy_swap_timeouts ? native_legacy_swap_timeouts : 0u, t);
                changed();
            }
            {
                // umi-184: every heartbeat (~1 s): a first-ring fence that has not moved since the previous beat while work is pending and the
                // GPU is idle gets its write pointer again (the display otherwise waits 5 s and the family cancels the swap).
                static uint32_t kicks = 0, last[4] = {};
                uint32_t info[4] = {};
                if (native_adreno_kick_if_stalled(info)) {
                    ++kicks; for (unsigned k = 0; k < 4; ++k) last[k] = info[k];
                    snprintf(native_gpu_rows[kRowStall], sizeof native_gpu_rows[0], "STALL kicks %u | fence %u newest %u rptr %x wptr %x | t%u",
                             kicks, last[0], last[1], last[2], last[3], t);
                    changed();
                }
            }
            if (t % 10 == 0 && &umi_sys_hist_cnt) {
                const volatile uint32_t *h = umi_sys_hist_cnt;
                uint32_t total = 0;
                for (unsigned i = 0; i < 560; ++i) total += h[i];
                snprintf(native_gpu_rows[kRowLsd], sizeof native_gpu_rows[0],
                         "LSD calls %u | w %u pw %u wn %u | open %u/%u/%u | fsync %u/%u msync %u trunc %u ren %u/%u | t%u",
                         total, h[4], h[154], h[397], h[5], h[398], h[463], h[95], h[408], h[65], h[201], h[128], h[488], t);
                changed();
            }
            if (t % 20 == 0) {
                const uint32_t *pp = native_gpu_publish;
                snprintf(native_gpu_rows[kRowSelf], sizeof native_gpu_rows[0], "SELFTEST rounds %d %d | steps %u/%u first %x@%u cpumap %x | t%u",
                         int32_t(pp[8]), int32_t(pp[9]), pp[3], pp[4], pp[5], pp[6], pp[7], t);
                dpuSmmuRows(t);
            }
        }
        if (gPreemptSetup[0]) {
            NativeAdrenoPreemptStats ps{};
            uint32_t loaded = 0, cntl = 0, dead = 0, rescues = 0;
            bool inFlight = false;
            if (gSubmit) (void)gSubmit->preemptStats(&ps, &loaded, &inFlight, &cntl, &dead, &rescues);
            snprintf(line, sizeof line, "PREEMPT m%u ok%u f%u/%x on%u | trg %u done %u skip %u flt %u | sf%u ap%x/%x/%x/%u | r%u fl%u c%x d%u res%u irq%u | sub%u/%u h%u to%u rs%u | t%u",
                     gPreemptSetup[0], gPreemptSetup[3], gPreemptSetup[1], gPreemptSetup[2], gSubmit ? gSubmit->preemption() : 0,
                     ps.triggers, ps.completed, ps.skipped, ps.faulted, native_t970_submit_fail[0], native_adreno_cp_aperture[0],
                     native_adreno_cp_aperture[1], native_adreno_cp_aperture[2], native_adreno_cp_aperture[3],
                     loaded, inFlight, cntl, dead, rescues, gPreemptIrqs, st.submissions, st.completed, st.hangs, st.timeouts,
                     native_umi_gpu_restart[0], t);
            if (t % 10 == 0 || strncmp(native_gpu_rows[kRowPreempt], line, 60)) {
                strlcpy(native_gpu_rows[kRowPreempt], line, sizeof native_gpu_rows[0]);
                changed();
            }
        }
        const uint32_t bank0 = smmuBank0();
        snprintf(line, sizeof line, "FAULTS cr0 %x gfsr %x syn %x/%x/%x | cb0 fsr %x syn %x far %x%08x", smmuRead(0x0), smmuRead(0x48),
                 smmuRead(0x50), smmuRead(0x54), smmuRead(0x58), smmuRead(bank0 + 0x58), smmuRead(bank0 + 0x68),
                 smmuRead(bank0 + 0x64), smmuRead(bank0 + 0x60));
        if (strncmp(line, native_gpu_rows[kRowFaults], sizeof line)) {
            strlcpy(native_gpu_rows[kRowFaults], line, sizeof native_gpu_rows[0]);
            changed();
        }
        snprintf(line, sizeof line, "GPUADDON ext %d/%d dmn %d/%d | svc u%u max(%d:%llums) tot%llums fd%llu e%u(%d:%llu,%d:%llu,%d:%llu,%d:%llu) s(%d,%d,%d,%d)",
                 native_gpu_addon_stage, native_gpu_addon_status,
                 native_gpu_addon2_stage, native_gpu_addon2_status,
                 &native_p1_unix_cnt ? native_p1_unix_cnt : 0,
                 &native_p1_max_sys ? native_p1_max_sys : 0,
                 (unsigned long long)((&native_p1_max_ticks ? native_p1_max_ticks : 0) / 19200ull),
                 (unsigned long long)((&native_p1_total_ticks ? native_p1_total_ticks : 0) / 19200ull),
                 (unsigned long long)(&native_p1_fcntl_fd ? native_p1_fcntl_fd : 0),
                 &native_p1_err_cnt ? native_p1_err_cnt : 0,
                 &native_p1_err_sys ? native_p1_err_sys[0] : 0,
                 (unsigned long long)(&native_p1_err_val ? native_p1_err_val[0] : 0),
                 &native_p1_err_sys ? native_p1_err_sys[1] : 0,
                 (unsigned long long)(&native_p1_err_val ? native_p1_err_val[1] : 0),
                 &native_p1_err_sys ? native_p1_err_sys[2] : 0,
                 (unsigned long long)(&native_p1_err_val ? native_p1_err_val[2] : 0),
                 &native_p1_err_sys ? native_p1_err_sys[3] : 0,
                 (unsigned long long)(&native_p1_err_val ? native_p1_err_val[3] : 0),
                 &native_p1_sys_ring ? native_p1_sys_ring[4] : 0,
                 &native_p1_sys_ring ? native_p1_sys_ring[5] : 0,
                 &native_p1_sys_ring ? native_p1_sys_ring[6] : 0,
                 &native_p1_sys_ring ? native_p1_sys_ring[7] : 0);
        if (strncmp(line, native_gpu_rows[kRowAddon], sizeof line)) {
            strlcpy(native_gpu_rows[kRowAddon], line, sizeof native_gpu_rows[0]);
            changed();
        }
        const NativeAdrenoMetalDiag &md = native_adreno_metal_diag;
        snprintf(line, sizeof line,
                 "GPUMETAL op %u/%u gr %u bsy %u f %u(%x) tnl %u prp %u chk %x/%x/%x note %u:%u:%llx n%u/%u pri %u:%x | %s | %s",
                 md.opens, md.closes, md.grants, md.busy, md.foreign_opens, md.foreign_type, md.tunnel_calls,
                 md.property_reads, md.chk_type, md.chk_macf, md.chk_res,
                 native_adreno_last_note_kind, native_adreno_last_note_code,
                 (unsigned long long)native_adreno_last_note_value,
                 native_adreno_note_counts[0], native_adreno_note_counts[1], native_adreno_priority_calls, native_adreno_priority_last,
                 md.opener[0][0] ? md.opener[0] : "-", md.report[0] ? md.report : "-");
        if (strncmp(line, native_gpu_rows[kRowClients], sizeof line)) {
            strlcpy(native_gpu_rows[kRowClients], line, sizeof native_gpu_rows[0]);
            changed();
        }
        snprintf(line, sizeof line,
                 "GPUSWAP acc %u rej %u(%u) fmt %08x(%u/%u/%u/%u) gop %u offer %llu/%llu fail %llu abn %llu cpy %llu ret %u",
                 &native_legacy_swap_accepted_count ? native_legacy_swap_accepted_count : 0,
                 &native_legacy_swap_rejected_count ? native_legacy_swap_rejected_count : 0,
                 &native_legacy_rej_reason ? native_legacy_rej_reason : 0,
                 &native_t970_swapfmt ? native_t970_swapfmt[0] : 0,
                 &native_t970_swapfmt ? native_t970_swapfmt[1] : 0,
                 &native_t970_swapfmt ? native_t970_swapfmt[2] : 0,
                 &native_t970_swapfmt ? native_t970_swapfmt[3] : 0,
                 &native_t970_swapfmt ? native_t970_swapfmt[4] : 0,
                 &native_gop_ready ? native_gop_ready : 0,
                 (unsigned long long)(&native_gop_gpu_offers ? native_gop_gpu_offers : 0),
                 (unsigned long long)(&native_gop_gpu_completions ? native_gop_gpu_completions : 0),
                 (unsigned long long)(&native_gop_gpu_failures ? native_gop_gpu_failures : 0),
                 (unsigned long long)(&native_gop_gpu_abandoned ? native_gop_gpu_abandoned : 0),
                 (unsigned long long)(&native_legacy_copy_n ? native_legacy_copy_n : 0),
                 &native_legacy_retirement_calls ? native_legacy_retirement_calls : 0);
        if (strncmp(line, native_gpu_rows[kRowSwap], sizeof line)) {
            strlcpy(native_gpu_rows[kRowSwap], line, sizeof native_gpu_rows[0]);
            changed();
        }
        // Docs step 3.4 (known image): adrenoprobed's Turnip smoke test reports "T s<stage> r<VkResult> v<fill> c<compute>
        // g<triangle px>/<stray px> ..." into report2 (64x64 RGBA8, blue clear + orange triangle = 512 px expected).
        {
            const NativeAdrenoMetalDiag &mt = native_adreno_metal_diag;
            const char *probeText = "-";   // the probe's Turnip line goes to its own client row, not report2
            for (const auto &entry : native_adreno_client_text)
                if (!strncmp(entry.name, "adrenoprobed", 12) && entry.text[0]) probeText = entry.text;
            snprintf(line, sizeof line, "GPUTEST %s | rep %.24s | fail m%u s%x | map %x%08x sub h%u %x%08x b%u q%u",
                     probeText, mt.report[0] ? mt.report : "-",
                     native_adreno_probe_fail[1], native_adreno_probe_fail[2],
                     native_adreno_probe_wire[1], native_adreno_probe_wire[0], native_adreno_probe_wire[2], native_adreno_probe_wire[4],
                     native_adreno_probe_wire[3], native_adreno_probe_wire[5], native_adreno_probe_wire[6]);
            if (strncmp(line, native_gpu_rows[kRowTest], sizeof line)) {
                strlcpy(native_gpu_rows[kRowTest], line, sizeof native_gpu_rows[0]);
                changed();
            }
        }
        ucheRow();
        submitRow();
        IOSleep(250);
    }
}
} // namespace

// GPU fault recovery, as rui12366B/UniXNU-gts7xlwifi package/GpuProbe.cpp (gpufault1..6, its T970-GPU-FAULT-FREEZE.md) does for the
// same SM8250 GPU: the driver's hangcheck ends a fault latched in the GPU context bank (else ArmSmmu4KObserveOwnedContext poisons
// every VM in turn) and restarts the GPU, then replays. Without these hooks the hang is counted but the CP stays stopped.
// umi-gpu-recover=0 leaves them out (the previous behaviour).
extern "C" {
extern int (*native_adreno_bank_fault_end)(unsigned bank);
extern int (*native_adreno_dram_read32)(uint64_t physical, uint32_t *value);
void native_adreno_set_restart(NativeAdrenoRestart restart, void *cookie);
}
namespace {
uint32_t uptimeSeconds() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return uint32_t(ns / 1000000000ull);
}
// [0] faults ended, [1] FSR, [2..5] FAR lo/hi, FSYNR0/1, [6] CP status (0xE19), [7] ASID, [8] time; [9..12] the first fault's FAR, ASID, time.
int gpuBankFaultEnd(unsigned bank) {
    if (bank != kGpuBank || !gSmmuMap) return 0;
    const uint32_t cb = smmuBank0();
    const uint32_t fsr = smmuRead(cb + 0x58);
    // The fault bits (arm-smmu.h ARM_SMMU_CB_FSR_FAULT); 0x400/0x600 is the context's AArch64 format, not a fault.
    if (!(fsr & 0xc00001feu)) return 0;
    uint32_t *f = native_umi_gpu_fault;
    const uint32_t asid = smmuRead(cb + 0x24) >> 16, t = uptimeSeconds();
    f[1] = fsr; f[2] = smmuRead(cb + 0x60); f[3] = smmuRead(cb + 0x64); f[4] = smmuRead(cb + 0x68); f[5] = smmuRead(cb + 0x6c);
    f[6] = gpuRead(0xE19); f[7] = asid; f[8] = t;
    if (!f[0]) {f[9] = f[2]; f[10] = f[3]; f[11] = asid; f[12] = t;}
    f[0] = f[0] + 1;
    if (fsr & (1u << 30)) smmuWrite(cb + 0x08, 1);   // CB_RESUME TnR: the stalled transaction ends with an error
    smmuWrite(cb + 0x58, fsr);
    return 1;
}
// [0] restarts asked, [1] failing step (0 none), [2] its status, [3] RBBM_STATUS after the reset, [4] completed, [5] GBIF drain status.
IOReturn gpuRestart(void *, NativeAdrenoRing *ring, uint32_t completed) {
    uint32_t *r = native_umi_gpu_restart;
    r[0] = r[0] + 1;
    auto fail = [&](uint32_t step, IOReturn status) {r[1] = step; r[2] = uint32_t(status); return status ? status : kIOReturnIOError;};
    if (!ring || !gCpInit) return fail(1, kIOReturnNotReady);
    // umi two rings: the restart brings the CP up on the first ring, which must take its write pointer again (a ring that is not
    // current never gives CP_RB_WPTR), whichever ring the CP was running when it hung.
    if (gRing1) { ring->setCurrent(true); gRing1->setCurrent(false); }
    (void)gpuBankFaultEnd(kGpuBank);
    r[5] = uint32_t(ring->busHalt(true, 1000000));
    gw(0x0043, 1); (void)gpuRead(0x0043);                               // RBBM_SW_RESET_CMD
    IODelay(100);
    gw(0x0043, 0); (void)gpuRead(0x0043);
    IODelay(1);
    r[3] = gpuRead(0x210);
    hwInit();                                                           // releases both GBIF halts first
    if (gPreemptOn) { gw(0x8e50, 1); gw(0x8a1, 0); gw(0x8a2, 0); }    // a6xx_preempt_hw_init after the reset
    gIrqRepeat = 0; gIrqMasked = 0; gIrqLast = 0;                       // hwInit unmasked the line again
    gw64(0x830, gNb[0].iova);                                           // CP_SQE_INSTR_BASE
    IOReturn status = ring->restart(completed);
    if (status) return fail(2, status);
    gw(0x808, 1);                                                       // CP_SQE_CNTL
    NativeAdrenoCpInitConfig config{gNb[0].iova, 0, 1};
    config.generation = 6;
    NativeAdrenoCpInit init;
    uint32_t fence = 0;
    status = init.run(&gRio, ring, &config, &fence, false);
    if (status) return fail(3, status);
    status = ring->idle(&gClock, 1000000);
    if (status) return fail(4, status);
    if (native_gpu_zap[0] == 7) {                                       // the zap shader was loaded: leave secure mode again
        uint32_t words[2] = {};
        if (AdrenoPM4Type7(0x66, 1, &words[0]) != AdrenoPM4Success) return fail(5, kIOReturnInternalError);
        status = ring->submitUnfenced(words, 2);
        if (!status) status = ring->idle(&gClock, 1000000);
        if (status) return fail(5, status);
    }
    (void)gpuBankFaultEnd(kGpuBank);
    r[1] = 0; r[2] = 0; r[4] = r[4] + 1;
    return kIOReturnSuccess;
}
// UniXNU-alioth's reader: one page mapped uncached for the read, then unmapped (a page a pmap still maps cannot be freed).
int dramRead32(uint64_t physical, uint32_t *value) {
    if (!value) return -1;
    const uint64_t page = physical & ~uint64_t(0xfff);
    IODeviceMemory *memory = IODeviceMemory::withRange(page, 0x1000);
    if (!memory) return -1;
    IOMemoryMap *map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (!map) return -1;
    *value = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress())[(physical - page) / 4];
    map->release();
    return 0;
}
}  // namespace

extern "C" void umi_adreno_bringup(unsigned mode) {
    if (gMode || !mode) return;
    gMode = (mode == 1) ? 1 : 12;
    {
        unsigned recover = 1;
        PE_parse_boot_argn("umi-gpu-recover", &recover, sizeof recover);
        if (recover) {
            native_adreno_dram_read32 = dramRead32;
            native_adreno_bank_fault_end = gpuBankFaultEnd;
            native_adreno_set_restart(gpuRestart, nullptr);
        }
    }
    native_gpu_step = 1;
    unsigned have = 0;
    for (unsigned w = 0; w < GPB_WINDOWS; ++w) if (mapWindow(w)) have |= 1u << w;
    const gpb_io io = {nullptr, rd, wr, delayUs};
    gpb_result &r = native_gpu_result;
    native_gpu_step = 10;
    gpb_run(&io, gMode, have, &r);
    rowsBasic(r);
    if (gMode < 5) { native_gpu_step = 99; return; }
    commandDb();
    uint32_t cx_gmu = r.cc_gmu, ahb = r.cc_ahb;
    if (gMode >= 6) {
        native_gpu_step = 70;
        gpb_gmu_clock_on &o = native_gpu_clocks;
        gpb_gmu_clocks_on(&io, &r, &o);
        snprintf(native_gpu_rows[kRowClk], sizeof native_gpu_rows[0],
                 "CLK step %u failed %d polls %u/%u gpll0 %08x rcg %08x cfg %08x cx_gmu %08x",
                 o.step, o.failed, o.rcg_polls, o.branch_polls, o.vote, o.gmu_cmd, o.gmu_cfg, o.cx_gmu);
        snprintf(native_gpu_rows[kRowClk2], sizeof native_gpu_rows[0], "CLK ahb %08x cxo %08x axi %08x memnoc %08x pll1 %08x",
                 o.ahb, o.cxo, o.axi, o.memnoc, o.pll1_mode);
        changed();
        cx_gmu = o.cx_gmu; ahb = o.ahb;
    }
    gmuState(io, r, cx_gmu, ahb);
    if (gMode >= 7) gmuColdStart(r, native_gpu_clocks);
    if (gMode >= 8 && gColdOk) gxOn();
    const bool cp = gMode >= 9 && native_gpu_gx.gx_on && gpuContext() && cpStart();
    const bool irq = gMode >= 10 && cp && leaveSecure() && gpuInterrupt();
    if (gMode >= 11 && irq && publish()) {
        if (selfTest() && gMode >= 12) metal();
        native_gpu_step = 99;
        thread_t thread = nullptr;
        if (kernel_thread_start(heartbeatThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
        return;
    }
    native_gpu_step = 99;
}

extern "C" void umi_adreno_gmu_test(uintptr_t, uintptr_t) {}

extern "C" __attribute__((weak)) int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) {
    return -1;
}

// umi direct scanout (dpu-scanout=2, unixnu.com/porting/display): the display's context bank on the apps SMMU (SMR 0x820/0x402 -> CB 4 on
// this phone, found at run time) gets a 32-bit stage 1 table: 2..4 GiB identity in 1 GiB blocks (the boot framebuffer and every DRAM
// address the firmware gave the pipes keep working), 1..2 GiB a window of kDpuSlots x 32 MiB for the compositor's surfaces. Built on the
// first map call, so the display driver's start order does not matter; any failure returns 0 and the display keeps the copy.
extern "C" {
volatile uint32_t native_ufs_dpu_state = 0x80000000u;   // bit 31: the window can be asked for; low byte: the bank, once set up
volatile uint32_t native_umi_dpu[8];                      // [0] step (0 not tried, 0xff ready), [1] status, [2] bank, [3] maps, [4] pages last, [5] sctlr, [6] tcr2, [7] fails
}
namespace {
constexpr unsigned kDpuSlots = 4, kDpuSlotL2 = 16;            // 16 x 2 MiB = 32 MiB, the display driver's 8192-page limit
constexpr uint32_t kDpuWindow = 0x40000000u;
IOBufferMemoryDescriptor *gDpuTables;
volatile uint64_t *gDpuL1, *gDpuL2, *gDpuL3;                   // L3: kDpuSlots * kDpuSlotL2 tables of 512 entries
uint64_t gDpuL1Pa, gDpuL2Pa, gDpuL3Pa;
volatile uint32_t *gApps, *gDpuCb;
constexpr uint64_t kDpuPage = (1ull << 10) | (1ull << 6) | 3;   // AF, AP unprivileged RW, AttrIndx 0 (MAIR0 0x44 normal NC), page
constexpr uint64_t kDpuBlock = (1ull << 10) | (1ull << 6) | 1;  // the same as a 1 GiB block
void dpuSync() {
    __asm__ volatile("dsb sy" ::: "memory");
    gDpuCb[0x618 / 4] = 0;                                     // CB_TLBIALL
    gDpuCb[0x7f0 / 4] = 0;                                     // CB_TLBSYNC
    for (unsigned i = 0; i < 100000 && (gDpuCb[0x7f4 / 4] & 1); ++i) IODelay(1);
}
bool dpuSetup() {
    volatile uint32_t *u = native_umi_dpu;
    auto fail = [&](uint32_t step) { u[0] = step; return false; };
    volatile uint32_t *g0 = (volatile uint32_t *)(uintptr_t)ml_io_map(0x15000000, 0x1000);
    if (!g0) return fail(1);
    const uint32_t id1 = g0[0x24 / 4];
    const uint32_t pageBytes = (id1 >> 31) ? 0x10000u : 0x1000u, pages = 1u << (((id1 >> 28) & 7) + 1);
    gApps = (volatile uint32_t *)(uintptr_t)ml_io_map(0x15000000, 2ull * pages * pageBytes);
    if (!gApps) return fail(2);
    const uint32_t count = gApps[0x20 / 4] & 0xff;
    unsigned cb = ~0u;
    for (unsigned i = 0; i < count && i < 128; ++i) {
        const uint32_t r = gApps[(0x800 + 4 * i) / 4], mask = (r >> 16) & 0x7fff;
        if (!(r & 0x80000000u) || ((r ^ 0x820u) & 0x7fff & ~mask)) continue;
        const uint32_t s2 = gApps[(0xc00 + 4 * i) / 4];
        if ((s2 >> 16) & 3) return fail(3);                    // not a translation route
        cb = s2 & 0xff; break;
    }
    if (cb == ~0u) return fail(4);
    u[2] = cb;
    if (((gApps[(pageBytes + 4 * cb) / 4] >> 16) & 3) != 1 || !(gApps[(pageBytes + 0x800 + 4 * cb) / 4] & 1)) return fail(5);   // S1+bypass, AArch64
    gDpuCb = gApps + (pages * pageBytes + cb * pageBytes) / 4;
    if (gDpuCb[0] & 1) return fail(6);                         // translation already on: someone else's table
    const size_t bytes = (2 + kDpuSlots * kDpuSlotL2) * 4096;
    gDpuTables = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache,
                                                                  bytes, 0x000000fffffff000ull);
    if (!gDpuTables || gDpuTables->prepare() != kIOReturnSuccess) return fail(7);
    uint8_t *va = static_cast<uint8_t *>(gDpuTables->getBytesNoCopy());
    IOByteCount len = 0;
    const uint64_t pa = gDpuTables->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
    if (!va || !pa || len < bytes) return fail(8);
    // umi-147 panicked in bzero here (unaligned data abort on the uncached mapping): aligned 64-bit stores only.
    for (size_t i = 0; i < bytes / 8; ++i) reinterpret_cast<volatile uint64_t *>(va)[i] = 0;
    gDpuL1 = reinterpret_cast<volatile uint64_t *>(va); gDpuL1Pa = pa;
    gDpuL2 = reinterpret_cast<volatile uint64_t *>(va + 4096); gDpuL2Pa = pa + 4096;
    gDpuL3 = reinterpret_cast<volatile uint64_t *>(va + 8192); gDpuL3Pa = pa + 8192;
    gDpuL1[1] = gDpuL2Pa | 3;
    gDpuL1[2] = 0x80000000ull | kDpuBlock;
    gDpuL1[3] = 0xc0000000ull | kDpuBlock;
    for (unsigned i = 0; i < kDpuSlots * kDpuSlotL2; ++i) gDpuL2[i] = (gDpuL3Pa + 4096ull * i) | 3;
    __asm__ volatile("dsb sy" ::: "memory");
    gDpuCb[0x38 / 4] = 0x44; gDpuCb[0x3c / 4] = 0;            // MAIR0/1: attr 0 normal non-cacheable
    gDpuCb[0x30 / 4] = (1u << 23) | 32;                        // TCR: EPD1, 4K granule, non-cacheable walks, T0SZ 32
    gDpuCb[0x10 / 4] = (gDpuCb[0x10 / 4] & ~(7u << 15)) | (7u << 15);   // TCR2 SEP 7 (the guide: SEP 0 faults bit-31 addresses)
    gDpuCb[0x20 / 4] = uint32_t(gDpuL1Pa); gDpuCb[0x24 / 4] = uint32_t(gDpuL1Pa >> 32);
    dpuSync();
    gDpuCb[0] = 0x67;                                          // SCTLR: M, TRE, AFE, CFRE, CFIE (no stall on fault)
    __asm__ volatile("dsb sy" ::: "memory");
    u[5] = gDpuCb[0]; u[6] = gDpuCb[0x10 / 4];
    if (!(gDpuCb[0] & 1) || gDpuCb[0x20 / 4] != uint32_t(gDpuL1Pa)) { gDpuCb[0] = 0xe0; return fail(9); }
    dpuSync();
    native_ufs_dpu_state = 0x80000000u | 0x100u | cb;
    u[0] = 0xff;
    return true;
}
}  // namespace
extern "C" uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count) {
    volatile uint32_t *u = native_umi_dpu;
    if (!u[0] && !dpuSetup()) { u[7] = u[7] + 1; return 0; }
    if (u[0] != 0xff || slot >= kDpuSlots || !pages || !count || count > kDpuSlotL2 * 512) { u[7] = u[7] + 1; return 0; }
    volatile uint64_t *l3 = gDpuL3 + size_t(slot) * kDpuSlotL2 * 512;
    for (unsigned i = 0; i < kDpuSlotL2 * 512; ++i) {
        if (i < count && (pages[i] & 0xfff || pages[i] >> 40)) { u[7] = u[7] + 1; return 0; }
        l3[i] = i < count ? (pages[i] | kDpuPage) : 0;
    }
    dpuSync();
    u[3] = u[3] + 1; u[4] = count;
    return kDpuWindow + slot * kDpuSlotL2 * 0x200000u;
}
