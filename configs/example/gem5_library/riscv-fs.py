# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
This example runs a simple linux boot. It uses the 'riscv-disk-img' resource.
It is built with the sources in `src/riscv-fs` in [gem5 resources](
https://gem5.googlesource.com/public/gem5-resources).

Characteristics
---------------

* Runs exclusively on the RISC-V ISA with the classic caches
* Assumes that the kernel is compiled into the bootloader
* Automatically generates the DTB file
* Will boot but requires a user to login using `m5term` (username: `root`,
  password: `root`)
"""

from gem5.components.boards.riscv_board import RiscvBoard
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.cachehierarchies.classic.\
    private_l1_private_l2_cache_hierarchy import (
        PrivateL1PrivateL2CacheHierarchy,
    )
from gem5.components.processors.cpu_types import CPUTypes
from gem5.isas import ISA
from gem5.utils.requires import requires
from gem5.resources.resource import CustomDiskImageResource, Resource
from gem5.simulate.simulator import Simulator
import m5

# Run a check to ensure the right version of gem5 is being used.
requires(isa_required=ISA.RISCV)

# Setup the cache hierarchy.
# For classic, PrivateL1PrivateL2 and NoCache have been tested.
# For Ruby, MESI_Two_Level and MI_example have been tested.
cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size="32KiB", l1i_size="32KiB", l2_size="512KiB"
)

# Setup the system memory.
memory = SingleChannelDDR3_1600()

# # Setup a single core Processor.
# processor = SimpleProcessor(
#     cpu_type=CPUTypes.TIMING, isa=ISA.RISCV, num_cores=1
# )

processor = SimpleProcessor(
    cpu_type=CPUTypes.TIMING,
    isa=ISA.RISCV,
    num_cores=1,
)

# Setup the board.
board = RiscvBoard(
    clk_freq="1GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)


for core in processor.cores:
    mmu = core.get_mmu()

    # Stress the DTLB so the deterministic victim path can be compared against
    # the linear predictor with the same TLB and victim-buffer capacity.
    mmu.dtb.size = 8
    mmu.dtb.eviction_controller.entries = 64
    mmu.dtb.eviction_controller.oracle_trace = True
    mmu.dtb.eviction_controller.oracle_entries = 64
    mmu.dtb.eviction_controller.oracle_trace_file = (
        f"{m5.options.outdir}/dtb_oracle_trace.csv"
    )

    for tlb in (mmu.itb, mmu.dtb):
        ctrl = tlb.eviction_controller
        ctrl.predictor = "linear"
        ctrl.cost_threshold = 3
        ctrl.linear_bias = -1.25
        ctrl.linear_threshold = 0.0
        ctrl.linear_weights = [
            0.8, 0.5, 1.2, 0.7, 1.0, 0.1, 0.1, 0.0, -0.05,
            -0.05, -0.25, 0.4, 0.1,
        ]


# Set the Full System workload.
board.set_kernel_disk_workload(
                   kernel=Resource("riscv-bootloader-vmlinux-5.10"),
                   disk_image=CustomDiskImageResource("riscv-disk.img"),
)

# simulator = Simulator(board=board)

simulator = Simulator(board=board)

print("Beginning simulation!")
# Note: This simulation will never stop. You can access the terminal upon boot
# using m5term (`./util/term`): `./m5term localhost <port>`. Note the `<port>`
# value is obtained from the gem5 terminal stdout. Look out for
# "system.platform.terminal: Listening for connections on port <port>".

# Dump stats every 5 trillion ticks so you can monitor progress
m5.stats.periodicStatDump(5000000000000)

simulator.run()
