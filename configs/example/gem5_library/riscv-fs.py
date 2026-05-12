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

import argparse

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
from gem5.resources.resource import CustomDiskImageResource, CustomResource, Resource
from gem5.simulate.simulator import Simulator
import m5


parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--kernel", type=str, default=None)
parser.add_argument("--disk-image", type=str, default="riscv-disk.img")
parser.add_argument("--dtb-size", type=int, default=8)
parser.add_argument("--itb-size", type=int, default=64)
parser.add_argument("--victim-entries", type=int, default=64)
parser.add_argument("--oracle-trace", action="store_true", default=True)
parser.add_argument("--no-oracle-trace", dest="oracle_trace",
                    action="store_false")
parser.add_argument("--oracle-entries", type=int, default=64)
parser.add_argument("--oracle-trace-file", type=str, default=None)
parser.add_argument("--predictor", choices=("linear", "deterministic"),
                    default="linear")
parser.add_argument("--cost-threshold", type=int, default=3)
parser.add_argument("--linear-bias", type=float, default=-1.25)
parser.add_argument("--linear-threshold", type=float, default=0.0)
parser.add_argument(
    "--linear-weights",
    type=str,
    default="0.8,0.5,1.2,0.7,1.0,0.1,0.1,0.0,-0.05,-0.05,-0.25,0.4,0.1",
)
args, _unknown = parser.parse_known_args()
linear_weights = [float(weight) for weight in args.linear_weights.split(",")]

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

    mmu.dtb.size = args.dtb_size
    mmu.itb.size = args.itb_size
    mmu.dtb.eviction_controller.entries = args.victim_entries
    mmu.dtb.eviction_controller.oracle_trace = args.oracle_trace
    mmu.dtb.eviction_controller.oracle_entries = args.oracle_entries
    mmu.dtb.eviction_controller.oracle_trace_file = (
        args.oracle_trace_file or f"{m5.options.outdir}/dtb_oracle_trace.csv"
    )

    for tlb in (mmu.itb, mmu.dtb):
        ctrl = tlb.eviction_controller
        ctrl.predictor = args.predictor
        ctrl.cost_threshold = args.cost_threshold
        ctrl.linear_bias = args.linear_bias
        ctrl.linear_threshold = args.linear_threshold
        ctrl.linear_weights = linear_weights


# Set the Full System workload.
kernel = CustomResource(args.kernel) if args.kernel else Resource(
    "riscv-bootloader-vmlinux-5.10"
)
board.set_kernel_disk_workload(
                   kernel=kernel,
                   disk_image=CustomDiskImageResource(args.disk_image),
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
