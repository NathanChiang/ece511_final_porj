#include "arch/riscv/tlb_eviction_controller.hh"

#include <cassert>
#include <memory>

#include "arch/riscv/page_size.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/TLB.hh"
#include "mem/request.hh"
#include "sim/system.hh"

namespace gem5
{

namespace RiscvISA
{

TlbEvictionController::TlbEvictionController(const Params &p)
    : SimObject(p), cachePort(name() + ".cache_port", *this),
      system(p.system),
      requestorId(system->getRequestorId(this)),
      numEntries(p.entries), cacheLineSize(p.cache_line_size),
      l2HitLatency(p.l2_hit_latency),
      highPriorityTouches(p.high_priority_touches),
      costThreshold(p.cost_threshold),
      dramWeight(p.dram_weight), walkWeight(p.walk_weight),
      smallPageExtraWeight(p.small_page_extra_weight), stats(this)
{
}

Port &
TlbEvictionController::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cache_port")
        return cachePort;
    return SimObject::getPort(if_name, idx);
}

void
TlbEvictionController::notifyEviction(const TlbEntry &entry)
{
    const uint64_t cost = estimateCost(entry);
    const Addr block_addr = cacheBlockAddr(entry);

    stats.evictions++;
    stats.lastVaddr = entry.vaddr;
    stats.lastPaddr = entry.paddr;
    stats.lastLogBytes = entry.logBytes;
    stats.lastAsid = entry.asid;
    stats.lastPte = entry.pte;
    stats.lastCost = cost;
    stats.lastBlockAddr = block_addr;

    if (!numEntries || cost < costThreshold) {
        stats.droppedEvictions++;
        DPRINTF(TLB, "TLB eviction controller dropped vaddr %#x asid %#x "
                "cost %u threshold %u\n", entry.vaddr, entry.asid, cost,
                costThreshold);
        return;
    }

    if (!cachePort.isConnected()) {
        stats.disconnectedDrops++;
        DPRINTF(TLB, "TLB eviction controller has no L2 cache port; "
                "dropping vaddr %#x asid %#x\n", entry.vaddr, entry.asid);
        return;
    }

    Tick latency = 0;
    const unsigned touches = highPriorityTouches ? highPriorityTouches : 1;
    for (unsigned i = 0; i < touches; ++i)
        latency = touchCacheBlock(block_addr);
    stats.lastLatency = latency;
    stats.l2Fills++;

    VictimaEntry victim;
    victim.blockAddr = block_addr;
    victim.cost = cost;
    victim.entry = entry;
    victim.entry.trieHandle = NULL;
    directory[makeKey(entry.vaddr, entry.asid)] = victim;
    stats.retainedEvictions++;

    DPRINTF(TLB, "TLB eviction controller retained vaddr %#x asid %#x "
            "paddr %#x pte %#x logBytes %u cost %u in L2 block %#x "
            "latency %u\n", entry.vaddr, entry.asid, entry.paddr,
            entry.pte, entry.logBytes, cost, block_addr, latency);
}

bool
TlbEvictionController::lookup(Addr vpn, uint16_t asid, TlbEntry &entry)
{
    for (auto it = directory.begin(); it != directory.end(); ++it) {
        VictimaEntry &victima_entry = it->second;
        if (victima_entry.entry.asid != asid)
            continue;

        const Addr mask = ~(victima_entry.entry.size() - 1);
        if ((vpn & mask) != victima_entry.entry.vaddr)
            continue;

        if (!cachePort.isConnected()) {
            stats.disconnectedDrops++;
            stats.l2Misses++;
            return false;
        }

        Tick latency = touchCacheBlock(victima_entry.blockAddr);
        stats.lastLatency = latency;
        stats.l2Probes++;

        if (!isL2Hit(latency)) {
            stats.l2Misses++;
            DPRINTF(TLB, "TLB eviction controller miss for vaddr %#x asid "
                    "%#x: L2 block %#x latency %u exceeds hit latency %u\n",
                    vpn, asid, victima_entry.blockAddr, latency,
                    l2HitLatency);
            directory.erase(it);
            return false;
        }

        entry = victima_entry.entry;
        stats.l2Hits++;

        DPRINTF(TLB, "TLB eviction controller hit vaddr %#x asid %#x "
                "paddr %#x cost %u block %#x latency %u\n", entry.vaddr,
                entry.asid, entry.paddr, victima_entry.cost,
                victima_entry.blockAddr, latency);
        return true;
    }

    stats.l2Misses++;
    return false;
}

uint64_t
TlbEvictionController::makeKey(Addr vaddr, uint16_t asid) const
{
    return (static_cast<uint64_t>(asid) << 48) | (vaddr & mask(48));
}

Addr
TlbEvictionController::cacheBlockAddr(const TlbEntry &entry) const
{
    const Addr page_base = entry.paddr << PageShift;
    const Addr page_bytes = entry.size();
    const Addr line_mask = cacheLineSize - 1;

    Addr offset = 0;
    if (page_bytes > cacheLineSize) {
        const Addr lines = page_bytes / cacheLineSize;
        const uint64_t key = makeKey(entry.vaddr, entry.asid);
        offset = (key % lines) * cacheLineSize;
    }

    return (page_base + offset) & ~line_mask;
}

Tick
TlbEvictionController::touchCacheBlock(Addr block_addr)
{
    Request::Flags flags = Request::PHYSICAL;
    RequestPtr req = std::make_shared<Request>(
            block_addr, cacheLineSize, flags, requestorId);
    Packet pkt(req, MemCmd::ReadReq);
    pkt.allocate();
    return cachePort.sendAtomic(&pkt);
}

bool
TlbEvictionController::isL2Hit(Tick latency) const
{
    return latency <= l2HitLatency;
}

uint64_t
TlbEvictionController::estimateCost(const TlbEntry &entry) const
{
    uint64_t cost = walkWeight * (entry.logBytes <= 12 ? 3 : 1);

    if (!entry.pte.a || !entry.pte.d)
        cost += dramWeight;

    if (entry.logBytes <= 12)
        cost += smallPageExtraWeight;

    return cost;
}

TlbEvictionController::CachePort::CachePort(
        const std::string &name, TlbEvictionController &owner)
    : RequestPort(name, &owner), owner(owner)
{
}

bool
TlbEvictionController::CachePort::recvTimingResp(PacketPtr pkt)
{
    panic("%s only issues atomic Victima cache probes\n", owner.name());
}

void
TlbEvictionController::CachePort::recvReqRetry()
{
    panic("%s only issues atomic Victima cache probes\n", owner.name());
}

TlbEvictionController::ControllerStats::ControllerStats(
        statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(evictions, statistics::units::Count::get(),
             "Number of TLB evictions observed by this controller"),
    ADD_STAT(retainedEvictions, statistics::units::Count::get(),
             "Number of high-cost TLB evictions retained in the controller"),
    ADD_STAT(droppedEvictions, statistics::units::Count::get(),
             "Number of low-cost TLB evictions dropped by the controller"),
    ADD_STAT(l2Hits, statistics::units::Count::get(),
             "Number of TLB lookups served by retained high-cost entries"),
    ADD_STAT(l2Misses, statistics::units::Count::get(),
             "Number of Victima lookups that missed in L2"),
    ADD_STAT(l2Fills, statistics::units::Count::get(),
             "Number of Victima blocks injected into L2"),
    ADD_STAT(l2Probes, statistics::units::Count::get(),
             "Number of Victima lookup probes sent to L2"),
    ADD_STAT(disconnectedDrops, statistics::units::Count::get(),
             "Number of Victima operations dropped without a connected port"),
    ADD_STAT(lastVaddr, statistics::units::Count::get(),
             "Virtual page address of the last evicted TLB entry"),
    ADD_STAT(lastPaddr, statistics::units::Count::get(),
             "Physical page address of the last evicted TLB entry"),
    ADD_STAT(lastLogBytes, statistics::units::Count::get(),
             "Log2 page size of the last evicted TLB entry"),
    ADD_STAT(lastAsid, statistics::units::Count::get(),
             "ASID of the last evicted TLB entry"),
    ADD_STAT(lastPte, statistics::units::Count::get(),
             "PTE of the last evicted TLB entry"),
    ADD_STAT(lastCost, statistics::units::Count::get(),
             "Estimated cost of the last evicted TLB entry"),
    ADD_STAT(lastBlockAddr, statistics::units::Count::get(),
             "Physical L2 block address used for the last Victima entry"),
    ADD_STAT(lastLatency, statistics::units::Count::get(),
             "Atomic latency of the last Victima L2 access")
{
}

} // namespace RiscvISA
} // namespace gem5
