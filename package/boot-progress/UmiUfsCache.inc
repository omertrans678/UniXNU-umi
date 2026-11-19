// Read-only root read-ahead cache for the umi NativeUfs block windows.
//
// Why: page faults during early userspace (dyld) arrive as ~6 KiB reads, each a separate synchronous
// single-slot UFS command costing ~0.26 ms; 12 s boots spent ~6 s there. The root windows are read-only
// (doAsyncReadWrite rejects writes), so a block cache can never go stale. A miss reads the 32-block
// (128 KiB) chunk aligned to its window that contains the request, in one command, and later faults on
// neighbouring pages are served from RAM.
//
// Bounds: chunks are aligned relative to the window's first LBA and clamped to the window's end, so
// read-ahead never leaves a published window. Requests outside any noted window, or full-size requests,
// go straight to hostRead4k. Boot-arg umi-ufs-ra=0 disables the cache.
//
// Locking: gUmiCacheLock covers the tags and every copy out of a slot. It is NOT held while a slot is
// being filled from UFS: the slot is marked `filling`, which keeps it out of lookups and eviction, so
// hits on other chunks proceed while the (single-slot, serialized) controller is busy.
constexpr unsigned kUmiCacheSpan = kDataMax / 4096;   // 32 blocks = one maximal transfer
constexpr unsigned kUmiCacheSlots = 1024;              // 128 MiB of RAM (umi-174: 32 MiB thrashed, ~100k fills/boot)
constexpr unsigned kUmiCacheWindows = 8;

struct UmiCacheSlot { uint64_t base; uint32_t count; uint32_t age; uint8_t *data; bool filling; };
struct UmiCacheWindow { uint64_t first, end; };       // absolute 4 KiB LBAs, end exclusive

static IOLock *volatile gUmiCacheLock = nullptr;
static UmiCacheSlot gUmiCacheSlot[kUmiCacheSlots];
static UmiCacheWindow gUmiCacheWindow[kUmiCacheWindows];
static volatile unsigned gUmiCacheWindowCount = 0;     // append-only, published with release
static unsigned gUmiCacheClock = 0;
static int gUmiCacheMode = -1;                           // -1 unknown, 0 off, 1 on

extern "C" {
volatile uint64_t native_ufs_cache_hits = 0, native_ufs_cache_fills = 0, native_ufs_cache_bypass = 0;
// Requests completed entirely from the cache without the window lock, and their 4 KiB blocks.
volatile uint64_t native_ufs_cache_fast = 0, native_ufs_cache_fast_blocks = 0;
}

static bool umiCacheEnabled (void)
{
  if (gUmiCacheMode < 0) {
    unsigned ra = 1;
    PE_parse_boot_argn ("umi-ufs-ra", &ra, sizeof (ra));
    gUmiCacheMode = ra ? 1 : 0;
  }
  if (!gUmiCacheMode) return false;
  if (!gUmiCacheLock) {
    IOLock *lock = IOLockAlloc ();
    if (!lock) return false;
    IOLock *expected = nullptr;
    if (!__atomic_compare_exchange_n (&gUmiCacheLock, &expected, lock, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      IOLockFree (lock);
  }
  return gUmiCacheLock != nullptr;
}

static bool umiCacheWindowKnown (uint64_t first, uint64_t end)
{
  const unsigned n = __atomic_load_n (&gUmiCacheWindowCount, __ATOMIC_ACQUIRE);
  for (unsigned i = 0; i < n; ++i)
    if (gUmiCacheWindow[i].first == first && gUmiCacheWindow[i].end == end) return true;
  return false;
}

// Called for every window request before it is read; idempotent and lock-free once noted.
static void umiCacheNoteWindow (uint64_t absolute, uint64_t bytes)
{
  if (!umiCacheEnabled () || !bytes || (absolute & 4095)) return;
  const uint64_t first = absolute / 4096, end = first + (bytes + 4095) / 4096;
  if (umiCacheWindowKnown (first, end)) return;
  IOLockLock (gUmiCacheLock);
  const unsigned n = gUmiCacheWindowCount;
  if (!umiCacheWindowKnown (first, end) && n < kUmiCacheWindows) {
    gUmiCacheWindow[n] = { first, end };
    __atomic_store_n (&gUmiCacheWindowCount, n + 1, __ATOMIC_RELEASE);
  }
  IOLockUnlock (gUmiCacheLock);
}

// Lock held. A ready slot holding exactly chunk `base`, or null.
static UmiCacheSlot *umiCacheLookup (uint64_t base, uint32_t count)
{
  for (unsigned i = 0; i < kUmiCacheSlots; ++i) {
    UmiCacheSlot &s = gUmiCacheSlot[i];
    if (!s.filling && s.data && s.count && s.base == base && s.count == count) return &s;
  }
  return nullptr;
}

// Lock held. Least recently used slot that is not being filled (empty slots first), or null.
static UmiCacheSlot *umiCacheVictim (void)
{
  UmiCacheSlot *victim = nullptr;
  for (unsigned i = 0; i < kUmiCacheSlots; ++i) {
    UmiCacheSlot &s = gUmiCacheSlot[i];
    if (s.filling) continue;
    if (!victim || !s.count || (victim->count && s.age < victim->age)) victim = &s;
    if (!victim->count) break;
  }
  return victim;
}

// Fast path, called by doAsyncReadWrite BEFORE ufsReadWindow takes the per-window lock (which also
// guards the shared bounce buffer and serializes every request of a window, hits included). If every
// chunk covering the request is resident, copy straight into the caller's descriptor under the cache
// lock only and report the byte count; otherwise return false and leave the request to the normal path
// (which fills the cache). Same argument validation as ufsReadWindow; verification is a no-op there.
static bool umiCacheServe (uint64_t absolute, uint64_t bytes, uint64_t block, uint64_t count,
                           IOMemoryDescriptor *buffer, uint64_t *done)
{
  *done = 0;
  if (!umiCacheEnabled () || !buffer || !count || !bytes || (absolute & 4095) ||
      block >= bytes / 512 || count > bytes / 512 - block) return false;
  const uint64_t first = absolute / 4096, end = first + (bytes + 4095) / 4096;
  if (!umiCacheWindowKnown (first, end)) return false;
  const uint64_t start = absolute + block * 512, length = count * 512;
  const uint64_t firstLba = start / 4096, lastLba = (start + length - 1) / 4096;
  IOLockLock (gUmiCacheLock);
  // All-or-nothing: check residency before copying anything.
  for (uint64_t lba = firstLba; lba <= lastLba;) {
    const uint64_t base = first + ((lba - first) / kUmiCacheSpan) * kUmiCacheSpan;
    const uint64_t limit = end - base;
    const uint32_t cnt = static_cast<uint32_t> (limit < kUmiCacheSpan ? limit : kUmiCacheSpan);
    if (!umiCacheLookup (base, cnt)) { IOLockUnlock (gUmiCacheLock); return false; }
    lba = base + cnt;
  }
  uint64_t copied = 0;
  while (copied < length) {
    const uint64_t at = start + copied, lba = at / 4096;
    const uint64_t base = first + ((lba - first) / kUmiCacheSpan) * kUmiCacheSpan;
    const uint64_t limit = end - base;
    const uint32_t cnt = static_cast<uint32_t> (limit < kUmiCacheSpan ? limit : kUmiCacheSpan);
    UmiCacheSlot *slot = umiCacheLookup (base, cnt);
    const uint64_t offset = at - base * 4096;           // byte offset inside the slot
    uint64_t take = uint64_t (cnt) * 4096 - offset;
    if (take > length - copied) take = length - copied;
    if (!slot || buffer->writeBytes (copied, slot->data + offset, take) != take) {
      IOLockUnlock (gUmiCacheLock);
      return false;                                       // nothing completed: slow path redoes all
    }
    slot->age = ++gUmiCacheClock;
    copied += take;
  }
  IOLockUnlock (gUmiCacheLock);
  __atomic_fetch_add (&native_ufs_cache_fast, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add (&native_ufs_cache_fast_blocks, lastLba - firstLba + 1, __ATOMIC_RELAXED);
  *done = length;
  return true;
}

static IOReturn umiCachedRead4k (uint64_t lba, uint32_t blocks, void *out)
{
  if (!blocks || blocks >= kUmiCacheSpan || !umiCacheEnabled ()) {
    __atomic_fetch_add (&native_ufs_cache_bypass, 1, __ATOMIC_RELAXED);
    return umiFastRead4k (0, lba, blocks, out);
  }
  UmiCacheWindow window = { 0, 0 };
  {
    const unsigned n = __atomic_load_n (&gUmiCacheWindowCount, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < n; ++i)
      if (lba >= gUmiCacheWindow[i].first && lba + blocks <= gUmiCacheWindow[i].end) { window = gUmiCacheWindow[i]; break; }
  }
  if (!window.end) {
    __atomic_fetch_add (&native_ufs_cache_bypass, 1, __ATOMIC_RELAXED);
    return umiFastRead4k (0, lba, blocks, out);
  }
  uint8_t *dst = static_cast<uint8_t *> (out);
  uint64_t at = lba, left = blocks;
  while (left) {
    const uint64_t base = window.first + ((at - window.first) / kUmiCacheSpan) * kUmiCacheSpan;
    const uint64_t limit = window.end - base;
    const uint32_t count = static_cast<uint32_t> (limit < kUmiCacheSpan ? limit : kUmiCacheSpan);
    const uint64_t skip = at - base;
    uint64_t take = count - skip;
    if (take > left) take = left;

    IOLockLock (gUmiCacheLock);
    UmiCacheSlot *slot = umiCacheLookup (base, count);
    if (slot) {
      slot->age = ++gUmiCacheClock;
      memcpy (dst, slot->data + skip * 4096, take * 4096);
      IOLockUnlock (gUmiCacheLock);
      __atomic_fetch_add (&native_ufs_cache_hits, 1, __ATOMIC_RELAXED);
    } else {
      UmiCacheSlot *victim = umiCacheVictim ();
      if (victim && !victim->data) {
        // Allocate outside the lock; the slot is reserved meanwhile.
        victim->filling = true;
        IOLockUnlock (gUmiCacheLock);
        uint8_t *mem = static_cast<uint8_t *> (IOMalloc (kDataMax));
        IOLockLock (gUmiCacheLock);
        victim->data = mem;
        victim->filling = false;
        if (!mem) victim = nullptr;
      }
      if (!victim) {
        // Every slot busy filling or no memory: read just this piece directly.
        IOLockUnlock (gUmiCacheLock);
        __atomic_fetch_add (&native_ufs_cache_bypass, 1, __ATOMIC_RELAXED);
        IOReturn status = umiFastRead4k (0, at, static_cast<uint32_t> (take), dst);
        if (status != kIOReturnSuccess) return status;
      } else {
        victim->filling = true;
        victim->count = 0;
        IOLockUnlock (gUmiCacheLock);
        IOReturn status = umiFastRead4k (0, base, count, victim->data);
        IOLockLock (gUmiCacheLock);
        victim->filling = false;
        if (status != kIOReturnSuccess) {
          IOLockUnlock (gUmiCacheLock);
          return status;
        }
        victim->base = base;
        victim->count = count;
        victim->age = ++gUmiCacheClock;
        memcpy (dst, victim->data + skip * 4096, take * 4096);
        IOLockUnlock (gUmiCacheLock);
        __atomic_fetch_add (&native_ufs_cache_fills, 1, __ATOMIC_RELAXED);
      }
    }
    dst += take * 4096;
    at += take;
    left -= take;
  }
  return kIOReturnSuccess;
}
