#ifndef __ARCH_RISCV_TLB_EVICTION_CONTROLLER_HH__
#define __ARCH_RISCV_TLB_EVICTION_CONTROLLER_HH__

#include <string>
#include <unordered_map>

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
        TlbEntry entry;
    };

    uint64_t estimateCost(const TlbEntry &entry) const;
    uint64_t makeKey(Addr vaddr, uint16_t asid) const;
    Addr cacheBlockAddr(const TlbEntry &entry) const;
    Tick touchCacheBlock(Addr block_addr);
    bool isL2Hit(Tick latency) const;

    CachePort cachePort;
    System *system;
    const RequestorID requestorId;
    const uint64_t numEntries;
    const unsigned cacheLineSize;
    const Tick l2HitLatency;
    const unsigned highPriorityTouches;
    const uint64_t costThreshold;
    const uint64_t dramWeight;
    const uint64_t walkWeight;
    const uint64_t smallPageExtraWeight;
    std::unordered_map<uint64_t, VictimaEntry> directory;

    struct ControllerStats : public statistics::Group
    {
        ControllerStats(statistics::Group *parent);

        statistics::Scalar evictions;
        statistics::Scalar retainedEvictions;
        statistics::Scalar droppedEvictions;
        statistics::Scalar l2Hits;
        statistics::Scalar l2Misses;
        statistics::Scalar l2Fills;
        statistics::Scalar l2Probes;
        statistics::Scalar disconnectedDrops;
        statistics::Scalar lastVaddr;
        statistics::Scalar lastPaddr;
        statistics::Scalar lastLogBytes;
        statistics::Scalar lastAsid;
        statistics::Scalar lastPte;
        statistics::Scalar lastCost;
        statistics::Scalar lastBlockAddr;
        statistics::Scalar lastLatency;
    } stats;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_TLB_EVICTION_CONTROLLER_HH__
