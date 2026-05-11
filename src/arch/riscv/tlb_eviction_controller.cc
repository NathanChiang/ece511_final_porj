#include "arch/riscv/tlb_eviction_controller.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>

#include "arch/riscv/page_size.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/TLB.hh"
#include "mem/request.hh"
#include "sim/core.hh"
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
      oracleTrace(p.oracle_trace),
      oracleTraceFile(p.oracle_trace_file),
      oracleEntries(p.oracle_entries),
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
TlbEvictionController::resetStats()
{
    SimObject::resetStats();

    if (!oracleTrace)
        return;

    oracleDirectory.clear();
    oracleInsertionOrder.clear();
    nextOracleId = 0;

    if (oracleStream.is_open())
        oracleStream.close();
    oracleHeaderWritten = false;

    // gem5 invokes resetStats() once before simulation starts. The wrapper's
    // m5_reset_stats() call happens later and marks the benchmark ROI.
    oracleActive = curTick() > 0;
    if (oracleActive)
        openOracleTrace();
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

    observeEviction(entry, block_addr, cost, score);

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
    observeLookup(vpn, asid);

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

    finalizeMatchingOracleEntries(vpn, asid, "demap");
}

void
TlbEvictionController::flushAll()
{
    for (const auto &oracle_entry : oracleDirectory)
        finalizeOracleEntry(oracle_entry.second, 0, "flush");
    oracleDirectory.clear();
    oracleInsertionOrder.clear();

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

void
TlbEvictionController::observeEviction(const TlbEntry &entry, Addr block_addr,
                                       uint64_t cost, double score)
{
    if (!oracleTrace || !oracleEntries)
        return;
    if (!oracleActive)
        return;

    OracleEntry oracle_entry;
    oracle_entry.id = ++nextOracleId;
    oracle_entry.blockAddr = block_addr;
    oracle_entry.cost = cost;
    oracle_entry.score = score;
    oracle_entry.evictionTick = curTick();
    oracle_entry.entry = entry;
    oracle_entry.entry.trieHandle = NULL;

    oracleRetain(makeKey(entry.vaddr, entry.asid), oracle_entry);
}

void
TlbEvictionController::observeLookup(Addr vpn, uint16_t asid)
{
    if (!oracleTrace || !oracleEntries)
        return;
    if (!oracleActive)
        return;

    for (auto it = oracleDirectory.begin(); it != oracleDirectory.end(); ++it) {
        const TlbEntry &entry = it->second.entry;
        if (entry.asid != asid)
            continue;

        const Addr mask = ~(entry.size() - 1);
        if ((vpn & mask) != entry.vaddr)
            continue;

        finalizeOracleEntry(it->second, 1, "reuse");
        oracleInsertionOrder.remove(it->first);
        oracleDirectory.erase(it);
        return;
    }
}

void
TlbEvictionController::oracleRetain(uint64_t key, const OracleEntry &entry)
{
    auto existing = oracleDirectory.find(key);
    if (existing != oracleDirectory.end()) {
        finalizeOracleEntry(existing->second, 0, "replace");
        existing->second = entry;
        oracleInsertionOrder.remove(key);
        oracleInsertionOrder.push_back(key);
        return;
    }

    while (oracleDirectory.size() >= oracleEntries &&
           !oracleInsertionOrder.empty()) {
        const uint64_t old_key = oracleInsertionOrder.front();
        auto old_entry = oracleDirectory.find(old_key);
        if (old_entry != oracleDirectory.end()) {
            finalizeOracleEntry(old_entry->second, 0, "capacity");
            oracleDirectory.erase(old_entry);
        }
        oracleInsertionOrder.pop_front();
    }

    oracleDirectory[key] = entry;
    oracleInsertionOrder.push_back(key);
}

void
TlbEvictionController::finalizeMatchingOracleEntries(Addr vpn, uint16_t asid,
                                                     const char *reason)
{
    if (!oracleTrace)
        return;
    if (!oracleActive)
        return;

    for (auto it = oracleDirectory.begin(); it != oracleDirectory.end();) {
        const TlbEntry &entry = it->second.entry;
        const Addr mask = ~(entry.size() - 1);
        const bool vpn_match = vpn == 0 || (vpn & mask) == entry.vaddr;
        const bool asid_match = asid == 0 || entry.asid == asid;

        if (vpn_match && asid_match) {
            finalizeOracleEntry(it->second, 0, reason);
            oracleInsertionOrder.remove(it->first);
            it = oracleDirectory.erase(it);
        } else {
            ++it;
        }
    }
}

void
TlbEvictionController::finalizeOracleEntry(const OracleEntry &oracle_entry,
                                           unsigned label,
                                           const char *reason)
{
    if (!oracleTrace)
        return;
    if (!oracleActive)
        return;

    openOracleTrace();
    if (!oracleStream.is_open())
        return;

    const TlbEntry &entry = oracle_entry.entry;
    const double walk_levels = entry.logBytes <= 12 ? 3.0 : 1.0;
    const double is_small_page = entry.logBytes <= 12 ? 1.0 : 0.0;
    const Tick now = curTick();
    const Tick victim_residency = now >= oracle_entry.evictionTick ?
        now - oracle_entry.evictionTick : 0;

    oracleStream
        << label << ','
        << reason << ','
        << oracle_entry.id << ','
        << oracle_entry.evictionTick << ','
        << now << ','
        << victim_residency << ','
        << entry.vaddr << ','
        << entry.asid << ','
        << entry.paddr << ','
        << entry.logBytes << ','
        << entry.pte.a << ','
        << entry.pte.d << ','
        << entry.pte.w << ','
        << entry.pte.x << ','
        << entry.pte.u << ','
        << oracle_entry.cost << ','
        << oracle_entry.score << ','
        << walk_levels << ','
        << is_small_page << ','
        << residencyFeature(entry) << ','
        << recencyFeature(entry) << ','
        << accessFeature(entry) << ','
        << reuseDensityFeature(entry)
        << '\n';
}

void
TlbEvictionController::openOracleTrace()
{
    if (oracleStream.is_open())
        return;

    oracleStream.open(oracleTraceFile, std::ios::out | std::ios::trunc);
    if (!oracleStream.is_open()) {
        warn("Could not open RISC-V TLB oracle trace file '%s'\n",
             oracleTraceFile);
        return;
    }

    if (!oracleHeaderWritten) {
        oracleStream
            << "label,reason,eviction_id,eviction_tick,resolve_tick,"
            << "victim_residency,vaddr,asid,paddr,log_bytes,pte_a,pte_d,"
            << "pte_w,pte_x,pte_u,deterministic_cost,predictor_score,"
            << "walk_levels,is_small_page,log_residency,log_recency,"
            << "log_access_count,reuse_density\n";
        oracleHeaderWritten = true;
    }
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
TlbEvictionController::log2Feature(uint64_t value) const
{
    return std::log2(static_cast<double>(value) + 1.0);
}

double
TlbEvictionController::residencyFeature(const TlbEntry &entry) const
{
    const Tick now = curTick();
    const Tick residency = now >= entry.insertTick ? now - entry.insertTick : 0;
    return log2Feature(residency);
}

double
TlbEvictionController::recencyFeature(const TlbEntry &entry) const
{
    const Tick now = curTick();
    const Tick recency = now >= entry.lastAccessTick ?
        now - entry.lastAccessTick : 0;
    return log2Feature(recency);
}

double
TlbEvictionController::accessFeature(const TlbEntry &entry) const
{
    return log2Feature(entry.accessCount);
}

double
TlbEvictionController::reuseDensityFeature(const TlbEntry &entry) const
{
    const Tick now = curTick();
    const Tick residency = now >= entry.insertTick ? now - entry.insertTick : 0;
    return static_cast<double>(entry.accessCount) /
        static_cast<double>(residency + 1);
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
        residencyFeature(entry),
        recencyFeature(entry),
        accessFeature(entry),
        reuseDensityFeature(entry),
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
