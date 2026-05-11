#include "arch/riscv/tlb_eviction_controller.hh"

#include <algorithm>
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

namespace
{

TlbEvictionController::Predictor
parsePredictor(const std::string &name)
{
    if (name == "deterministic")
        return TlbEvictionController::Predictor::Deterministic;
    if (name == "linear")
        return TlbEvictionController::Predictor::Linear;
    fatal("Unknown RISC-V TLB eviction predictor '%s'; expected "
          "'deterministic' or 'linear'\n", name.c_str());
    return TlbEvictionController::Predictor::Deterministic;
}

} // anonymous namespace

TlbEvictionController::TlbEvictionController(const Params &p)
    : SimObject(p), cachePort(name() + ".cache_port", *this),
      system(p.system),
      requestorId(system->getRequestorId(this)),
      numEntries(p.entries), cacheLineSize(p.cache_line_size),
      backingHitLatency(p.l2_hit_latency),
      highPriorityTouches(p.high_priority_touches),
      costThreshold(p.cost_threshold),
      dramWeight(p.dram_weight), walkWeight(p.walk_weight),
      smallPageExtraWeight(p.small_page_extra_weight),
      predictor(parsePredictor(p.predictor)),
      linearWeights(p.linear_weights),
      linearBias(p.linear_bias),
      linearThreshold(p.linear_threshold),
      stats(this)
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
    const double score = estimateScore(entry);
    const double threshold = retentionThreshold();
    const Addr block_addr = cacheBlockAddr(entry);

    stats.evictions++;
    stats.lastVaddr = entry.vaddr;
    stats.lastPaddr = entry.paddr;
    stats.lastLogBytes = entry.logBytes;
    stats.lastAsid = entry.asid;
    stats.lastPte = entry.pte;
    stats.lastCost = cost;
    stats.lastScore = score;
    stats.lastBlockAddr = block_addr;

    if (predictor == Predictor::Linear)
        stats.linearPredictions++;
    else
        stats.deterministicPredictions++;

    if (!numEntries || score < threshold) {
        stats.droppedEvictions++;
        DPRINTF(TLB, "TLB eviction controller dropped vaddr %#x asid %#x "
                "cost %u score %.3f threshold %.3f\n", entry.vaddr,
                entry.asid, cost, score, threshold);
        return;
    }

    Tick latency = 0;
    if (cachePort.isConnected()) {
        const unsigned touches = highPriorityTouches ? highPriorityTouches : 1;
        for (unsigned i = 0; i < touches; ++i)
            latency = touchCacheBlock(block_addr);
        stats.backingFills++;
    }
    stats.lastLatency = latency;

    VictimaEntry victim;
    victim.blockAddr = block_addr;
    victim.cost = cost;
    victim.score = score;
    victim.entry = entry;
    victim.entry.trieHandle = NULL;
    retain(makeKey(entry.vaddr, entry.asid), victim);
    stats.retainedEvictions++;

    DPRINTF(TLB, "TLB eviction controller retained vaddr %#x asid %#x "
            "paddr %#x pte %#x logBytes %u cost %u score %.3f victim block "
            "%#x latency %u\n", entry.vaddr, entry.asid, entry.paddr,
            entry.pte, entry.logBytes, cost, score, block_addr, latency);
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

        Tick latency = 0;
        if (cachePort.isConnected()) {
            latency = touchCacheBlock(victima_entry.blockAddr);
            stats.backingProbes++;
        }
        stats.lastLatency = latency;

        if (cachePort.isConnected() && !isBackingHit(latency)) {
            stats.victimMisses++;
            DPRINTF(TLB, "TLB eviction controller miss for vaddr %#x asid "
                    "%#x: backing block %#x latency %u exceeds hit latency %u\n",
                    vpn, asid, victima_entry.blockAddr, latency,
                    backingHitLatency);
            directory.erase(it);
            return false;
        }

        entry = victima_entry.entry;
        stats.victimHits++;
        insertionOrder.remove(makeKey(victima_entry.entry.vaddr,
                                      victima_entry.entry.asid));
        directory.erase(it);

        DPRINTF(TLB, "TLB eviction controller hit vaddr %#x asid %#x "
                "paddr %#x cost %u score %.3f block %#x latency %u\n",
                entry.vaddr, entry.asid, entry.paddr, victima_entry.cost,
                victima_entry.score,
                victima_entry.blockAddr, latency);
        return true;
    }

    stats.victimMisses++;
    return false;
}

void
TlbEvictionController::demapPage(Addr vpn, uint16_t asid)
{
    if (vpn == 0 && asid == 0) {
        flushAll();
        return;
    }

    for (auto it = directory.begin(); it != directory.end();) {
        const TlbEntry &entry = it->second.entry;
        const Addr mask = ~(entry.size() - 1);
        const bool vpn_match = vpn == 0 || (vpn & mask) == entry.vaddr;
        const bool asid_match = asid == 0 || entry.asid == asid;

        if (vpn_match && asid_match) {
            insertionOrder.remove(it->first);
            it = directory.erase(it);
        } else {
            ++it;
        }
    }
}

void
TlbEvictionController::flushAll()
{
    directory.clear();
    insertionOrder.clear();
}

uint64_t
TlbEvictionController::makeKey(Addr vaddr, uint16_t asid) const
{
    return (static_cast<uint64_t>(asid) << 48) | (vaddr & mask(48));
}

void
TlbEvictionController::retain(uint64_t key, const VictimaEntry &victim)
{
    auto existing = directory.find(key);
    if (existing != directory.end()) {
        existing->second = victim;
        insertionOrder.remove(key);
        insertionOrder.push_back(key);
        return;
    }

    while (directory.size() >= numEntries && !insertionOrder.empty()) {
        directory.erase(insertionOrder.front());
        insertionOrder.pop_front();
    }

    directory[key] = victim;
    insertionOrder.push_back(key);
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
TlbEvictionController::isBackingHit(Tick latency) const
{
    return latency <= backingHitLatency;
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

double
TlbEvictionController::estimateScore(const TlbEntry &entry) const
{
    switch (predictor) {
      case Predictor::Deterministic:
        return static_cast<double>(estimateCost(entry));
      case Predictor::Linear:
        return estimateLinearScore(entry);
    }

    panic("Unhandled RISC-V TLB eviction predictor\n");
    return 0.0;
}

double
TlbEvictionController::estimateLinearScore(const TlbEntry &entry) const
{
    const double deterministic_cost = estimateCost(entry);
    const double walk_levels = entry.logBytes <= 12 ? 3.0 : 1.0;
    const double is_small_page = entry.logBytes <= 12 ? 1.0 : 0.0;

    const double features[] = {
        deterministic_cost,
        walk_levels,
        is_small_page,
        entry.pte.a ? 0.0 : 1.0,
        entry.pte.d ? 0.0 : 1.0,
        entry.pte.w ? 1.0 : 0.0,
        entry.pte.x ? 1.0 : 0.0,
        entry.pte.u ? 1.0 : 0.0,
        static_cast<double>(entry.logBytes),
    };

    double score = linearBias;
    const size_t feature_count = sizeof(features) / sizeof(features[0]);
    const size_t count = std::min(linearWeights.size(), feature_count);
    for (size_t i = 0; i < count; ++i)
        score += linearWeights[i] * features[i];
    return score;
}

double
TlbEvictionController::retentionThreshold() const
{
    if (predictor == Predictor::Linear)
        return linearThreshold;
    return static_cast<double>(costThreshold);
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
    ADD_STAT(victimHits, statistics::units::Count::get(),
             "Number of TLB misses served by retained victim-cache entries"),
    ADD_STAT(victimMisses, statistics::units::Count::get(),
             "Number of victim-cache lookups that missed"),
    ADD_STAT(backingFills, statistics::units::Count::get(),
             "Number of retained victim entries touched in a backing cache"),
    ADD_STAT(backingProbes, statistics::units::Count::get(),
             "Number of victim-cache hits probed in a backing cache"),
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
    ADD_STAT(lastScore, statistics::units::Count::get(),
             "Predictor score of the last evicted TLB entry"),
    ADD_STAT(lastBlockAddr, statistics::units::Count::get(),
             "Physical backing block address used for the last Victima entry"),
    ADD_STAT(lastLatency, statistics::units::Count::get(),
             "Atomic latency of the last Victima backing-cache access"),
    ADD_STAT(deterministicPredictions, statistics::units::Count::get(),
             "Number of evictions scored by the deterministic predictor"),
    ADD_STAT(linearPredictions, statistics::units::Count::get(),
             "Number of evictions scored by the offline linear predictor")
{
}

} // namespace RiscvISA
} // namespace gem5
