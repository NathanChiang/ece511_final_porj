#include "arch/riscv/tlb_eviction_controller.hh"

#include <cassert>

#include "base/trace.hh"
#include "debug/TLB.hh"

namespace gem5
{

namespace RiscvISA
{

TlbEvictionController::TlbEvictionController(const Params &p)
    : SimObject(p), numEntries(p.entries), costThreshold(p.cost_threshold),
      dramWeight(p.dram_weight), walkWeight(p.walk_weight),
      smallPageExtraWeight(p.small_page_extra_weight), lruSeq(0),
      l2Entries(numEntries), stats(this)
{
}

void
TlbEvictionController::notifyEviction(const TlbEntry &entry)
{
    const uint64_t cost = estimateCost(entry);

    stats.evictions++;
    stats.lastVaddr = entry.vaddr;
    stats.lastPaddr = entry.paddr;
    stats.lastLogBytes = entry.logBytes;
    stats.lastAsid = entry.asid;
    stats.lastPte = entry.pte;
    stats.lastCost = cost;

    if (!numEntries || cost < costThreshold) {
        stats.droppedEvictions++;
        DPRINTF(TLB, "TLB eviction controller dropped vaddr %#x asid %#x "
                "cost %u threshold %u\n", entry.vaddr, entry.asid, cost,
                costThreshold);
        return;
    }

    const size_t victim = chooseVictim();
    if (l2Entries[victim].valid)
        stats.l2Replacements++;

    l2Entries[victim].valid = true;
    l2Entries[victim].highPriority = true;
    l2Entries[victim].lruSeq = nextSeq();
    l2Entries[victim].cost = cost;
    l2Entries[victim].entry = entry;
    l2Entries[victim].entry.trieHandle = NULL;
    stats.retainedEvictions++;

    DPRINTF(TLB, "TLB eviction controller retained vaddr %#x asid %#x "
            "paddr %#x pte %#x logBytes %u cost %u\n", entry.vaddr,
            entry.asid, entry.paddr, entry.pte, entry.logBytes, cost);
}

bool
TlbEvictionController::lookup(Addr vpn, uint16_t asid, TlbEntry &entry)
{
    for (auto &l2_entry : l2Entries) {
        if (!l2_entry.valid || l2_entry.entry.asid != asid)
            continue;

        const Addr mask = ~(l2_entry.entry.size() - 1);
        if ((vpn & mask) != l2_entry.entry.vaddr)
            continue;

        entry = l2_entry.entry;
        l2_entry.lruSeq = nextSeq();
        stats.l2Hits++;

        DPRINTF(TLB, "TLB eviction controller hit vaddr %#x asid %#x "
                "paddr %#x cost %u high_priority %u\n", entry.vaddr,
                entry.asid, entry.paddr, l2_entry.cost,
                l2_entry.highPriority);
        return true;
    }

    stats.l2Misses++;
    return false;
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

size_t
TlbEvictionController::chooseVictim() const
{
    assert(numEntries);

    for (size_t i = 0; i < l2Entries.size(); ++i) {
        if (!l2Entries[i].valid)
            return i;
    }

    size_t victim = 0;
    for (size_t i = 1; i < l2Entries.size(); ++i) {
        if (l2Entries[victim].highPriority && !l2Entries[i].highPriority) {
            victim = i;
        } else if (l2Entries[victim].highPriority == l2Entries[i].highPriority &&
                   l2Entries[i].lruSeq < l2Entries[victim].lruSeq) {
            victim = i;
        }
    }

    return victim;
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
             "Number of TLB lookups missed in the controller"),
    ADD_STAT(l2Replacements, statistics::units::Count::get(),
             "Number of controller entries replaced"),
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
             "Estimated cost of the last evicted TLB entry")
{
}

} // namespace RiscvISA
} // namespace gem5
