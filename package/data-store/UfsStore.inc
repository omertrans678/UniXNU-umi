// ---- Persistent Data store window (sheng-data-persist) ----------------------------------------------------------
// The only part of this driver that ever writes. The consumer (NativeRamBlock's overlay) asks for a window of the given
// size at the END of the userdata partition. Mode 1 (boot argument sheng-data-persist=1) only reads: it proves the window and
// lets the consumer inspect its header. Mode 2 additionally opens the write window. A window is refused unless it lies inside
// the partition, clear of the flashed root image plus a 4 MiB guard, and is at most a quarter of the partition.
namespace {
struct StoreGeometry {
  bool valid;
  uint64_t first, last, imageBytes, entriesLba;
  uint32_t headerCrc, entriesCrc, entryCount;
};
StoreGeometry gStoreGeometry;
uint64_t gStoreRegionFirst, gStoreRegionBlocks;
// The diagnostic log (sheng-log=1): always the last 32 MiB of userdata, so the Data store's place does not depend on logging.
constexpr uint64_t kLogReserveBlocks = 8192;
// The log's export region: the 256 MiB just below the log window (the render server's pipeline caches, kits/sheng BootLog.c). It is
// part of the log's write window, so the store ends below both.
constexpr uint64_t kExportBlocks = 65536;
uint64_t gLogRegionFirst, gLogRegionBlocks;
// The reserve at the end of userdata that the Data store stays clear of: the log and its export region sit inside it (sheng-log=1), the rest
// is unused. sheng-log-reserve=<MiB>, default 1024; 0 gives all the room to Data and is only accepted with the log off. The store's place
// depends on it, so an image keeps one value for its whole life (a different value moves the store: the old one is read as blank).
constexpr uint32_t kDefaultReserveMiB = 1024;
static uint64_t reserveBlocks (void)
{
  static int64_t cached = -1;
  if (cached < 0) {
    uint32_t mib = kDefaultReserveMiB;
    if (!PE_parse_boot_argn ("sheng-log-reserve", &mib, sizeof (mib)) || mib > 16384) mib = kDefaultReserveMiB;
    cached = static_cast<int64_t> (mib) * 256;   // 4096-byte blocks
  }
  return static_cast<uint64_t> (cached);
}

static unsigned storeMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("sheng-data-persist", &value, sizeof (value)) || value > 2) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}
}

extern "C" unsigned native_ufs_store_mode (void) { return storeMode (); }

// Returns 1 and the first absolute 4096-byte LBA of the window, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_store_open (uint64_t blocks, uint64_t *firstLba, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!storeMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gStoreRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    const uint64_t reserve = reserveBlocks ();
    if (!blocks || reserve >= partition || blocks > partition - reserve) reason = "size";
    else if (partition - blocks - reserve < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      // The reserve at the end of the partition belongs to the diagnostic log (its last 32 MiB, with the export region below them);
      // the store ends just below it.
      uint64_t first = gStoreGeometry.last + 1 - reserve - blocks;
      if (storeMode () == 2 && !hostStoreOpen (first, blocks)) reason = "window refused";
      else {
        gStoreRegionFirst = first;
        gStoreRegionBlocks = blocks;
        if (firstLba) *firstLba = first;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

// umi-170 read-ahead for the store (umi-store-ra, default 1): the overlay reads present Data pages one APFS request at a time
// (4-16 KiB), each a synchronous single-slot UFS command; one boot read ~160k such pages (UFSSTAT read ~118 s), which is what
// Settings and lsd waited on. A small read fills the 32-block (128 KiB) chunk aligned to the region that holds it; later reads of
// its neighbours come from RAM. Writes update the cached copies, so the cache never goes stale. The overlay serializes all store
// I/O (DirectDisk.h), so no lock. Absent pages in a chunk are cached too but never asked for (the overlay reads only present ones).
namespace {
constexpr uint32_t kStoreSpan = kDataMax / 4096;   // 32 blocks
constexpr unsigned kStoreSlots = 128;              // 16 MiB
struct StoreSlot { uint64_t base; uint32_t count, age; uint8_t *data; };
StoreSlot gStoreSlot[kStoreSlots];
uint32_t gStoreClock;
int gStoreRa = -1;
}
extern "C" { volatile uint64_t native_ufs_store_cache[4]; }   // [0] hits, [1] fills, [2] bypass (large or off), [3] write updates

static int storeRawRead (uint64_t rel, uint32_t blocks, void *dst)
{
  while (blocks) {
    uint32_t chunk = blocks > kStoreSpan ? kStoreSpan : blocks;
    if (umiFastRead4k (0, gStoreRegionFirst + rel, chunk, dst) != kIOReturnSuccess) return 0;
    dst = static_cast<uint8_t *> (dst) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_read (uint64_t rel, uint32_t blocks, void *dst)
{
  if (!gStoreRegionBlocks || !dst || !blocks || rel >= gStoreRegionBlocks || blocks > gStoreRegionBlocks - rel) return 0;
  if (gStoreRa < 0) { unsigned ra = 1; PE_parse_boot_argn ("umi-store-ra", &ra, sizeof (ra)); gStoreRa = ra ? 1 : 0; }
  if (!gStoreRa || blocks >= kStoreSpan) { native_ufs_store_cache[2] = native_ufs_store_cache[2] + 1; return storeRawRead (rel, blocks, dst); }
  uint8_t *out = static_cast<uint8_t *> (dst);
  while (blocks) {
    const uint64_t base = rel - rel % kStoreSpan;
    const uint32_t count = static_cast<uint32_t> (gStoreRegionBlocks - base < kStoreSpan ? gStoreRegionBlocks - base : kStoreSpan);
    StoreSlot *slot = nullptr, *victim = nullptr;
    for (unsigned i = 0; i < kStoreSlots; ++i) {
      StoreSlot &s = gStoreSlot[i];
      if (s.data && s.count && s.base == base) { slot = &s; break; }
      if (!victim || !s.count || (victim->count && s.age < victim->age)) victim = &s;
    }
    if (slot) native_ufs_store_cache[0] = native_ufs_store_cache[0] + 1;
    else {
      if (!victim->data) victim->data = static_cast<uint8_t *> (IOMallocData (kStoreSpan * 4096ULL));
      if (!victim->data) { native_ufs_store_cache[2] = native_ufs_store_cache[2] + 1; return storeRawRead (rel, blocks, dst); }
      victim->count = 0;
      if (umiFastRead4k (0, gStoreRegionFirst + base, count, victim->data) != kIOReturnSuccess) return 0;
      victim->base = base; victim->count = count;
      native_ufs_store_cache[1] = native_ufs_store_cache[1] + 1;
      slot = victim;
    }
    slot->age = ++gStoreClock;
    const uint32_t skip = static_cast<uint32_t> (rel - base);
    const uint32_t take = blocks < count - skip ? blocks : count - skip;
    memcpy (out, slot->data + skip * 4096ULL, take * 4096ULL);
    out += take * 4096ULL; rel += take; blocks -= take;
  }
  return 1;
}

extern "C" int native_ufs_store_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (storeMode () != 2 || !gStoreRegionBlocks || !src || !blocks || rel >= gStoreRegionBlocks ||
      blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    // Forced unit access: the block is on the medium when the command completes, so the overlay needs no cache flush.
    if (hostWrite4k (0, gStoreRegionFirst + rel, chunk, src, true) != kIOReturnSuccess) {
      for (auto &s : gStoreSlot) if (s.count && rel < s.base + s.count && s.base < rel + chunk) s.count = 0;   // unknown medium state
      return 0;
    }
    for (auto &s : gStoreSlot) {                       // keep cached chunks equal to the medium
      if (!s.count || rel >= s.base + s.count || s.base >= rel + chunk) continue;
      const uint64_t from = rel > s.base ? rel : s.base, to = rel + chunk < s.base + s.count ? rel + chunk : s.base + s.count;
      memcpy (s.data + (from - s.base) * 4096ULL, static_cast<const uint8_t *> (src) + (from - rel) * 4096ULL, (to - from) * 4096ULL);
      native_ufs_store_cache[3] = native_ufs_store_cache[3] + 1;
    }
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_sync (void)
{
  return storeMode () == 2 && gStoreRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

extern "C" unsigned native_ufs_store_gpt_check (void)
{
  if (!gStoreGeometry.valid || !gHost.ready) return 1;
  static uint8_t block[4096];
  if (hostRead4k (0, 1, 1, block) != kIOReturnSuccess) return 2;
  uint32_t headerBytes = le32 (block + 12);
  if (headerBytes < 92 || headerBytes > 4096 || rootCrc32 (block, headerBytes, true) != le32 (block + 16) ||
      le32 (block + 16) != gStoreGeometry.headerCrc) return 3;
  uint32_t blocks = (gStoreGeometry.entryCount * 128 + 4095) / 4096;
  static uint8_t entries[4096 * 4];
  if (blocks > 4 || hostRead4k (0, gStoreGeometry.entriesLba, blocks, entries) != kIOReturnSuccess) return 4;
  return rootCrc32 (entries, gStoreGeometry.entryCount * 128) == gStoreGeometry.entriesCrc ? 0 : 5;
}

