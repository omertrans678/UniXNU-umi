// SPDX-License-Identifier: BSD-2-Clause-Patent
// umi-176: concurrent READ(10) on UTP transfer request slots 1..kUmiFastSlots.
//
// Why: every root and Data read was one synchronous command on slot 0 under gHost.lock (~0.55 ms per <=128 KiB, ~235 MB/s);
// app launches fault on many threads at once and all of them queued behind one command. UFSHCI lets the host ring several
// doorbell bits; the device works on them concurrently.
//
// How: each fast slot owns a 4 KiB UCD (command UPIU, response UPIU at kRspOffset, PRDT at kPrdtOffset) and a kDataMax bounce
// buffer in one separate contiguous DMA allocation. Its 32-byte UTRD lives in the transfer request list at gHost.virt + 32 * slot
// (slot 0's own UTRD stays at offset 0; the list's other entries were unused). A command is built and its doorbell bit rung under
// gHost.lock; completion (the bit clearing) is polled without the lock, so other threads can submit on free slots meanwhile. The
// response is checked like hostScsiDir does (type 0x21, task tag = slot, LUN, SCSI status, no residual). Slot-0 commands
// (writes, cache sync, queries, the legacy read) wait in umiWaitFastIdle() until no fast command is in flight, because slot 0
// treats any set doorbell bit as a fault. A timeout or failure poisons the transport exactly like slot 0 does.
// umi-ufs-qd=<0..8> (default 8) sets the number of fast slots; 0 keeps every read on slot 0.

constexpr unsigned kUmiFastMaxSlots = 8;   // umi-182: 4 were always full (122k slot waits per interactive boot); slots 2..16
constexpr uint32_t kUmiFastStride = 4096 + kDataMax;
static IOBufferMemoryDescriptor *gUmiFastDma = nullptr;
static uint8_t *gUmiFastVirt = nullptr;
static uint64_t gUmiFastPhys = 0;
static int gUmiFastSlots = -1;            // -1 not set up, 0 off
static uint32_t gUmiFastBusy = 0;         // bit n: UTRD slot n in use (slots 2, 4, 6, 8: one 64-byte cache line each)
extern "C" {
// [0] fast commands, [1] most in flight at once, [2] reads that fell back to slot 0, [3] waits for a free slot,
// [4] failed fast commands, [5] setup result (1 ready, 2 off, 3 allocation, 4 outside owned RAM), [6] total poll us, [7] longest us
volatile uint64_t native_ufs_fast[8];
// The first failed fast command: [0] OCS, [1] rsp[0] | rsp[1] << 8 | rsp[2] << 16 | rsp[3] << 24, [2] SCSI status, [3] residual
volatile uint32_t native_ufs_fast_err[4];
}

static void umiFastSetup (void)
{
  if (gUmiFastSlots >= 0) return;
  unsigned qd = kUmiFastMaxSlots;
  PE_parse_boot_argn ("umi-ufs-qd", &qd, sizeof (qd));
  if (qd > kUmiFastMaxSlots) qd = kUmiFastMaxSlots;
  if (!qd) { gUmiFastSlots = 0; native_ufs_fast[5] = 2; return; }
  const uint64_t bytes = uint64_t (qd) * kUmiFastStride;
  gUmiFastDma = IOBufferMemoryDescriptor::inTaskWithOptions (kernel_task,
    IOOptionBits (kIODirectionInOut) | IOOptionBits (kIOMemoryPhysicallyContiguous) | IOOptionBits (kIOMemoryMapperNone), bytes, 4096);
  if (!gUmiFastDma) { gUmiFastSlots = 0; native_ufs_fast[5] = 3; return; }
  gUmiFastVirt = static_cast<uint8_t *> (gUmiFastDma->getBytesNoCopy ());
  IOByteCount contiguous = 0;
  gUmiFastPhys = gUmiFastDma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  // The same rule the slot-0 allocation obeys: DMA only into XNU-owned conventional RAM, below 2^36 like that allocation's checks.
  if (!gUmiFastVirt || (gUmiFastPhys & 4095) || contiguous < bytes || (gUmiFastPhys + bytes) > (1ULL << 36) ||
      !umiDmaInOwnedMemory (gUmiFastPhys, bytes, gPhysBase, gPhysSize)) {
    gUmiFastDma->release (); gUmiFastDma = nullptr; gUmiFastSlots = 0; native_ufs_fast[5] = 4; return;
  }
  bzero (gUmiFastVirt, bytes);
  cacheCivac (gUmiFastVirt, bytes);
  gUmiFastSlots = static_cast<int> (qd);
  native_ufs_fast[5] = 1;
}

static uint64_t umiFastNowUs (void)
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds (mach_absolute_time (), &ns);
  return ns / 1000;
}

static IOReturn umiFastRead4k (uint8_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
  if (!gHost.ready || !dst || !blocks || blocks > kDataMax / 4096 || lba > 0xffffffffULL - (blocks - 1)) {
    return hostRead4k (lun, lba, blocks, dst);
  }
  IOLockLock (gHost.lock);
  umiFastSetup ();
  if (gUmiFastSlots <= 0 || gHost.poisoned) {
    IOLockUnlock (gHost.lock);
    native_ufs_fast[2] = native_ufs_fast[2] + 1;
    return hostRead4k (lun, lba, blocks, dst);
  }
  // umi-177: UTRDs are 32 bytes and a cache line is 64, so neighbouring slots shared a line and cleaning one slot's descriptor
  // wrote a stale copy over the OCS the controller had just stored in its neighbour (umi-176: 156-334 failed commands per boot).
  // Only even slots are used, each alone in its line (slot 1, sharing slot 0's line, is never used).
  uint32_t all = 0;
  for (int i = 0; i < gUmiFastSlots; ++i) all |= 1U << (2 * (i + 1));
  // A slot-0 command waiting for the fast slots to drain goes first (gUmiSlot0Waiting); the 1 ms deadline re-checks without
  // needing a wakeup from slot 0's own path.
  while (((gUmiFastBusy & all) == all || gUmiSlot0Waiting) && !gHost.poisoned) {
    native_ufs_fast[3] = native_ufs_fast[3] + 1;
    uint64_t wake = 0;
    clock_interval_to_deadline (1000, kMicrosecondScale, &wake);
    IOLockSleepDeadline (gHost.lock, (event_t) &gUmiFastBusy, wake, THREAD_UNINT);
  }
  if (gHost.poisoned || !gHost.ready || (read32 (gHost.hc, 0x20) & kUfsFatalStatus)) {
    gHost.poisoned = true; gHost.ready = false;
    IOLockUnlock (gHost.lock);
    return kIOReturnNotReady;
  }
  unsigned slot = 2;
  while (gUmiFastBusy & (1U << slot)) slot += 2;
  const uint32_t bit = 1U << slot;
  gUmiFastBusy |= bit;
  gUmiFastInflight = gUmiFastInflight + 1;
  if (gUmiFastInflight > native_ufs_fast[1]) native_ufs_fast[1] = gUmiFastInflight;
  const uint32_t bytes = blocks * 4096U;
  const unsigned index = slot / 2 - 1;
  uint8_t *ucd = gUmiFastVirt + uint64_t (index) * kUmiFastStride;
  uint8_t *data = ucd + 4096;
  const uint64_t ucdPhys = gUmiFastPhys + uint64_t (index) * kUmiFastStride, dataPhys = ucdPhys + 4096;
  for (unsigned i = 0; i < (kPrdtOffset + 16) / 4; ++i) reinterpret_cast<uint32_t *> (ucd)[i] = 0;
  ucd[0] = 0x01;                      // COMMAND UPIU
  ucd[1] = 0x40;                      // R: data in
  ucd[2] = lun;
  ucd[3] = static_cast<uint8_t> (slot);   // task tag
  putBe32 (ucd + 12, bytes);
  const uint8_t cdb[10] = { 0x28, 0, uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                            0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  for (unsigned i = 0; i < 10; ++i) ucd[16 + i] = cdb[i];
  uint32_t *prd = reinterpret_cast<uint32_t *> (ucd + kPrdtOffset);
  prd[0] = static_cast<uint32_t> (dataPhys);
  prd[1] = static_cast<uint32_t> (dataPhys >> 32);
  prd[2] = 0;
  prd[3] = bytes - 1;
  uint32_t *utrd = reinterpret_cast<uint32_t *> (gHost.virt + 32U * slot);
  for (unsigned i = 0; i < 8; ++i) utrd[i] = 0;
  utrd[0] = 0x10000000U | (2U << 25);
  utrd[2] = 0x0fU;
  utrd[4] = static_cast<uint32_t> (ucdPhys);
  utrd[5] = static_cast<uint32_t> (ucdPhys >> 32);
  utrd[6] = (512U >> 2) | ((kRspOffset >> 2) << 16);
  utrd[7] = 1U | ((kPrdtOffset >> 2) << 16);
  cacheCivac (ucd, 4096 + bytes);
  cacheCivac (utrd, 32);
  hostWrite (0x58, bit);
  IOLockUnlock (gHost.lock);

  uint64_t deadline = 0;
  clock_interval_to_deadline (1000000, kMicrosecondScale, &deadline);
  const uint64_t started = umiFastNowUs ();
  bool done = false, fatal = false;
  for (unsigned spin = 0;; ++spin) {
    if (!(read32 (gHost.hc, 0x58) & bit)) { done = true; break; }
    if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) { fatal = true; break; }
    if (mach_absolute_time () >= deadline) break;
    if (spin < 64) IODelay (2); else IODelay (10);
  }
  const uint64_t took = umiFastNowUs () - started;

  IOLockLock (gHost.lock);
  native_ufs_fast[6] = native_ufs_fast[6] + took;
  if (took > native_ufs_fast[7]) native_ufs_fast[7] = took;
  IOReturn status = kIOReturnSuccess;
  if (!done || fatal) {
    // As slot 0: the controller may still own the buffers; keep the slot marked busy and quarantine the transport.
    gHost.poisoned = true; gHost.ready = false;
    status = done ? kIOReturnIOError : kIOReturnTimeout;
  } else {
    cacheInvalidate (utrd, 32);
    cacheInvalidate (ucd, 4096);
    const uint32_t ocs = utrd[2] & 0xff;
    const uint8_t *rsp = ucd + kRspOffset;
    const uint32_t scsi = (uint32_t (rsp[6]) << 8) | rsp[7];
    if (ocs || rsp[0] != 0x21 || rsp[3] != slot || rsp[2] != lun || scsi || (rsp[1] & 0x60) || loadBe32 (rsp + 12)) {
      status = kIOReturnIOError;
      if (!native_ufs_fast[4]) {
        native_ufs_fast_err[0] = ocs;
        native_ufs_fast_err[1] = uint32_t (rsp[0]) | uint32_t (rsp[1]) << 8 | uint32_t (rsp[2]) << 16 | uint32_t (rsp[3]) << 24;
        native_ufs_fast_err[2] = scsi; native_ufs_fast_err[3] = loadBe32 (rsp + 12);
      }
    } else {
      cacheInvalidate (data, bytes);
      memcpy (dst, data, bytes);
    }
    gUmiFastBusy &= ~bit;
  }
  if (status) native_ufs_fast[4] = native_ufs_fast[4] + 1;
  else native_ufs_fast[0] = native_ufs_fast[0] + 1;
  gUmiFastInflight = gUmiFastInflight - 1;
  thread_wakeup ((event_t) &gUmiFastBusy);
  if (!gUmiFastInflight) thread_wakeup ((event_t) &gUmiFastInflight);
  IOLockUnlock (gHost.lock);
  if (status == kIOReturnIOError && !gHost.poisoned) return hostRead4k (lun, lba, blocks, dst);   // a clean failure: retry on slot 0
  return status;
}

// Per-request bounce buffers for the root window reader (NativeUfsBlock's doAsyncReadWrite): with one shared bounce per window,
// ufsReadWindow held the window lock across the whole UFS read, so misses on the OS volume ran one at a time whatever the
// transport allowed. With a private bounce per request the lock has nothing left to protect (verify only counts here), so the
// build drops it while the pool exists. Pool readiness only ever goes false -> true, before any lock() of a request.
constexpr unsigned kUmiBounceCount = 16;
static uint8_t *gUmiBounce[kUmiBounceCount];
static uint32_t gUmiBounceFree = 0;           // bit n: gUmiBounce[n] free
static IOLock *gUmiBounceLock = nullptr;
static volatile int gUmiBounceReady = 0;      // 0 not tried, 1 ready, -1 failed
static bool umiBouncePoolReady (void) { return __atomic_load_n (&gUmiBounceReady, __ATOMIC_ACQUIRE) == 1; }
static uint8_t *umiBounceGet (void)
{
  if (__atomic_load_n (&gUmiBounceReady, __ATOMIC_ACQUIRE) < 0) return nullptr;
  if (!gUmiBounceLock) {
    IOLock *fresh = IOLockAlloc ();
    IOLock *expected = nullptr;
    if (!fresh) return nullptr;
    if (!__atomic_compare_exchange_n (&gUmiBounceLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) IOLockFree (fresh);
  }
  IOLockLock (gUmiBounceLock);
  if (!gUmiBounceReady) {
    unsigned qd = kUmiFastMaxSlots;
    PE_parse_boot_argn ("umi-ufs-qd", &qd, sizeof (qd));
    bool ok = qd != 0;
    for (unsigned i = 0; ok && i < kUmiBounceCount; ++i) ok = (gUmiBounce[i] = static_cast<uint8_t *> (IOMallocData (kDataMax))) != nullptr;
    if (ok) gUmiBounceFree = (1U << kUmiBounceCount) - 1U;
    __atomic_store_n (&gUmiBounceReady, ok ? 1 : -1, __ATOMIC_RELEASE);
  }
  if (gUmiBounceReady < 0) { IOLockUnlock (gUmiBounceLock); return nullptr; }
  while (!gUmiBounceFree) IOLockSleep (gUmiBounceLock, (event_t) &gUmiBounceFree, THREAD_UNINT);
  const unsigned index = static_cast<unsigned> (__builtin_ctz (gUmiBounceFree));
  gUmiBounceFree &= ~(1U << index);
  IOLockUnlock (gUmiBounceLock);
  return gUmiBounce[index];
}
static void umiBouncePut (uint8_t *buffer)
{
  IOLockLock (gUmiBounceLock);
  for (unsigned i = 0; i < kUmiBounceCount; ++i) if (gUmiBounce[i] == buffer) gUmiBounceFree |= 1U << i;
  IOLockUnlock (gUmiBounceLock);
  thread_wakeup ((event_t) &gUmiBounceFree);
}
