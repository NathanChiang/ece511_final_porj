#ifndef __ARCH_RISCV_TLB_EVICTION_CONTROLLER_HH__
#define __ARCH_RISCV_TLB_EVICTION_CONTROLLER_HH__

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "arch/riscv/pagetable.hh"
#include "base/bitfield.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/RiscvTlbEvictionController.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class System;

namespace RiscvISA
{

class TlbEvictionController : public SimObject
{
  public:
    using Params = RiscvTlbEvictionControllerParams;

    TlbEvictionController(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

    void notifyEviction(const TlbEntry &entry);
    bool lookup(Addr vpn, uint16_t asid, TlbEntry &entry);
    void demapPage(Addr vpn, uint16_t asid);
    void flushAll();

    enum class Predictor
    {
        Deterministic,
        Linear,
    };

  private:
    class CachePort : public RequestPort
    {
      public:
        CachePort(const std::string &name, TlbEvictionController &owner);

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;

      private:
        TlbEvictionController &owner;
    };

    struct VictimaEntry
    {
        Addr blockAddr = 0;
        uint64_t cost = 0;
        double score = 0.0;
        TlbEntry entry;
    };

    uint64_t estimateCost(const TlbEntry &entry) const;
    double estimateScore(const TlbEntry &entry) const;
    double estimateLinearScore(const TlbEntry &entry) const;
    double retentionThreshold() const;
    uint64_t makeKey(Addr vaddr, uint16_t asid) const;
    void retain(uint64_t key, const VictimaEntry &victim);
    Addr cacheBlockAddr(const TlbEntry &entry) const;
    Tick touchCacheBlock(Addr block_addr);
    bool isBackingHit(Tick latency) const;

    CachePort cachePort;
    System *system;
    const RequestorID requestorId;
    const uint64_t numEntries;
    const unsigned cacheLineSize;
    const Tick backingHitLatency;
    const unsigned highPriorityTouches;
    const uint64_t costThreshold;
    const uint64_t dramWeight;
    const uint64_t walkWeight;
    const uint64_t smallPageExtraWeight;
    const Predictor predictor;
    const std::vector<double> linearWeights;
    const double linearBias;
    const double linearThreshold;
    std::unordered_map<uint64_t, VictimaEntry> directory;
    std::list<uint64_t> insertionOrder;

    struct ControllerStats : public statistics::Group
    {
        ControllerStats(statistics::Group *parent);

        statistics::Scalar evictions;
        statistics::Scalar retainedEvictions;
        statistics::Scalar droppedEvictions;
        statistics::Scalar victimHits;
        statistics::Scalar victimMisses;
        statistics::Scalar backingFills;
        statistics::Scalar backingProbes;
        statistics::Scalar disconnectedDrops;
        statistics::Scalar lastVaddr;
        statistics::Scalar lastPaddr;
        statistics::Scalar lastLogBytes;
        statistics::Scalar lastAsid;
        statistics::Scalar lastPte;
        statistics::Scalar lastCost;
        statistics::Scalar lastScore;
        statistics::Scalar lastBlockAddr;
        statistics::Scalar lastLatency;
        statistics::Scalar deterministicPredictions;
        statistics::Scalar linearPredictions;
    } stats;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_TLB_EVICTION_CONTROLLER_HH__
