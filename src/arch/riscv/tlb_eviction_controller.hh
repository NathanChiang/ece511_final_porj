#ifndef __ARCH_RISCV_TLB_EVICTION_CONTROLLER_HH__
#define __ARCH_RISCV_TLB_EVICTION_CONTROLLER_HH__

#include <vector>

#include "arch/riscv/pagetable.hh"
#include "base/statistics.hh"
#include "params/RiscvTlbEvictionController.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace RiscvISA
{

class TlbEvictionController : public SimObject
{
  public:
    using Params = RiscvTlbEvictionControllerParams;

    TlbEvictionController(const Params &p);

    void notifyEviction(const TlbEntry &entry);
    bool lookup(Addr vpn, uint16_t asid, TlbEntry &entry);

  private:
    struct L2Entry
    {
        bool valid = false;
        bool highPriority = false;
        uint64_t lruSeq = 0;
        uint64_t cost = 0;
        TlbEntry entry;
    };

    uint64_t estimateCost(const TlbEntry &entry) const;
    size_t chooseVictim() const;
    uint64_t nextSeq() { return ++lruSeq; }

    const size_t numEntries;
    const uint64_t costThreshold;
    const uint64_t dramWeight;
    const uint64_t walkWeight;
    const uint64_t smallPageExtraWeight;
    uint64_t lruSeq;
    std::vector<L2Entry> l2Entries;

    struct ControllerStats : public statistics::Group
    {
        ControllerStats(statistics::Group *parent);

        statistics::Scalar evictions;
        statistics::Scalar retainedEvictions;
        statistics::Scalar droppedEvictions;
        statistics::Scalar l2Hits;
        statistics::Scalar l2Misses;
        statistics::Scalar l2Replacements;
        statistics::Scalar lastVaddr;
        statistics::Scalar lastPaddr;
        statistics::Scalar lastLogBytes;
        statistics::Scalar lastAsid;
        statistics::Scalar lastPte;
        statistics::Scalar lastCost;
    } stats;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_TLB_EVICTION_CONTROLLER_HH__
