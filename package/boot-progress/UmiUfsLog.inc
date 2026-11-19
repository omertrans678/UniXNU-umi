// Diagnostic log region: last 32 MiB (8192 x 4 KiB blocks) of userdata.
// Bounds-checked to the reserved 32 MiB tail only.
//
// Serialization: every other command path (hostRead4k / hostWrite4k / hostSyncCache) owns slot 0, the UCD
// and the DMA data area under gHost.lock. The log writer must do the same, otherwise it races an in-flight
// root read: it overwrites the DMA buffer and hostClearUcd() then sees DB != 0 and poisons the host for good
// (the "UMILOG01 still shows the previous boot" symptom). The auto-recovery flush runs from the ordinary
// boot_progress kernel thread, so it can block on the lock. Only from a context that cannot block (interrupt
// / interrupts masked, e.g. a real panic with the other CPUs stopped) do we fall back to lockless doorbell
// polling.
extern "C" boolean_t ml_at_interrupt_context(void);
extern "C" boolean_t ml_get_interrupts_enabled(void);

extern "C" int native_ufs_log_ready(void)
{
  return (gHost.ready && !gHost.poisoned && gStoreGeometry.valid &&
          (gStoreGeometry.last - gStoreGeometry.first + 1) > 8192ULL) ? 1 : 0;
}

// Last failure reason of the log writer (inspect from a debugger / memory dump).
// 1 args/not ready, 2 lock timeout, 3 doorbell busy, 4 write failed, 5 poisoned under lock.
static volatile uint32_t native_ufs_log_last_error = 0;

static int native_ufs_log_write_locked(uint64_t rel_block, uint32_t blocks, const void *src)
{
  if (!native_ufs_log_ready()) { native_ufs_log_last_error = 5; return 0; }
  uint64_t log_first = gStoreGeometry.last + 1ULL - 8192ULL;
  while (blocks) {
    uint32_t chunk = blocks > (kDataMax / 4096U) ? static_cast<uint32_t>(kDataMax / 4096U) : blocks;
    uint64_t lba = log_first + rel_block;
    // Under the lock DB is normally already 0; keep a bounded wait for the lockless fallback.
    for (uint32_t wait = 0; wait < 20000 && read32(gHost.hc, 0x58) != 0; ++wait) {
      IODelay(100);
    }
    if (read32(gHost.hc, 0x58) != 0) { native_ufs_log_last_error = 3; return 0; }
    memcpy(gHost.virt + kDataOffset, src, chunk * 4096U);
    uint8_t cdb[10] = { 0x2a, 8, uint8_t(lba >> 24), uint8_t(lba >> 16),
                        uint8_t(lba >> 8), uint8_t(lba), 0, uint8_t(chunk >> 8), uint8_t(chunk), 0 };
    uint32_t scsi = 0;
    if (hostScsiDir(0, cdb, 10, 1, chunk * 4096U, 2000000, &scsi) != kIOReturnSuccess) {
      native_ufs_log_last_error = 4;
      return 0;
    }
    src = static_cast<const uint8_t *>(src) + chunk * 4096ULL;
    rel_block += chunk;
    blocks -= chunk;
  }
  uint8_t sync_cdb[10] = { 0x35 };
  uint32_t scsi = 0;
  hostScsiDir(0, sync_cdb, 10, 0, 0, 5000000, &scsi);
  native_ufs_log_last_error = 0;
  return 1;
}

// Read-only link diagnostics: UIC DME_GET of the UniPro attributes that set throughput, plus the
// number of transfer requests issued so far. out[0..5] = PA_RXGEAR, PA_TXGEAR, PA_HSSERIES,
// PA_PWRMODE, PA_ACTIVERXDATALANES, PA_ACTIVETXDATALANES (0xffffffff = not read), out[6] = commands.
// Blocking context only (takes gHost.lock). Returns the number of attributes read.
static bool native_ufs_dme_get(uint16_t attr, uint32_t *value)
{
  if (!(read32(gHost.hc, 0x30) & (1U << 3))) return false;       // HCS.UCRDY
  hostWrite(0x20, 1U << 10);                                       // clear stale IS.UCCS only
  hostWrite(0x94, uint32_t(attr) << 16);                           // UICCMDARG1: attribute, selector 0
  hostWrite(0x98, 0);
  hostWrite(0x9c, 0);
  hostWrite(0x90, 0x01);                                           // UICCMD: DME_GET
  for (unsigned wait = 0; wait < 10000; ++wait) {                  // <= ~100 ms
    if (read32(gHost.hc, 0x20) & (1U << 10)) {
      hostWrite(0x20, 1U << 10);
      if (read32(gHost.hc, 0x98) & 0xff) return false;             // UIC config result code
      *value = read32(gHost.hc, 0x9c);
      return true;
    }
    IODelay(10);
  }
  return false;
}

extern "C" int native_ufs_link_info(uint32_t *out, unsigned count)
{
  static const uint16_t attrs[6] = { 0x1583, 0x1568, 0x156a, 0x1571, 0x1580, 0x1560 };
  if (!out || count < 7) return -1;
  for (unsigned i = 0; i < 7; ++i) out[i] = 0xffffffffU;
  if (!gHost.lock || !gHost.ready || gHost.poisoned || ml_at_interrupt_context()) return -1;
  IOLockLock(gHost.lock);
  int got = 0;
  if (read32(gHost.hc, 0x58) == 0) {
    for (unsigned i = 0; i < 6; ++i) if (native_ufs_dme_get(attrs[i], &out[i])) ++got;
  }
  out[6] = gRootDiagnostic.command;
  IOLockUnlock(gHost.lock);
  return got;
}

extern "C" int native_ufs_log_write_panic(uint64_t rel_block, uint32_t blocks, const void *src)
{
  if (!native_ufs_log_ready() || !src || !blocks || rel_block >= 8192ULL || blocks > 8192ULL - rel_block) {
    native_ufs_log_last_error = 1;
    return 0;
  }
  bool can_block = gHost.lock && !ml_at_interrupt_context() && ml_get_interrupts_enabled();
  if (!can_block) {
    return native_ufs_log_write_locked(rel_block, blocks, src);
  }
  // Root commands time out after at most 10 s (SYNCHRONIZE CACHE); wait a little longer than that.
  bool locked = false;
  for (uint32_t wait = 0; wait < 12000; ++wait) {
    if (IOLockTryLock(gHost.lock)) { locked = true; break; }
    IODelay(1000);
  }
  if (!locked) {
    // The holder never finished a bounded command: the host is wedged. Do not touch the UCD.
    native_ufs_log_last_error = 2;
    return 0;
  }
  int saved = native_ufs_log_write_locked(rel_block, blocks, src);
  IOLockUnlock(gHost.lock);
  return saved;
}
