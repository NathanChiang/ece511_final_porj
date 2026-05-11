# Reproducing the FFT TLB Victima Experiment

This flow assumes you have access to the git branch containing the modified
gem5 source/config files. The disk image and build outputs are not committed;
each person should create those locally.

The expected experiment is:

- RISC-V full-system gem5
- TimingSimpleCPU from boot
- FFT benchmark auto-launched from `/etc/rcS`
- Wrapper prints benchmark start/end markers
- DTLB stress setting: `dtb.size = 8`
- Victima controller setting: `entries = 4096`
- Linear eviction predictor enabled

## 1. Pull the Project

```bash
cd /home/$USER
```

```bash
git clone <YOUR_REPO_URL> ece511_final_porj
```

If the repo already exists:

```bash
cd /home/$USER/ece511_final_porj
```

```bash
git pull
```

## 2. Install Required Host Tools

You need a RISC-V cross compiler and basic filesystem tools.

```bash
sudo apt update
```

```bash
sudo apt install gcc-riscv64-linux-gnu scons e2fsprogs
```

## 3. Build gem5

The TLB/Victima implementation lives in gem5 source files, so gem5 must be
rebuilt after pulling the branch.

```bash
cd /home/$USER/ece511_final_porj
```

```bash
scons build/RISCV/gem5.opt -j$(nproc)
```

## 4. Obtain a Clean RISC-V Disk Image

Run gem5 once, or otherwise allow gem5 to populate its resource cache. The
standard RISC-V disk image should appear at:

```text
~/.cache/gem5/riscv-disk-img
```

Then copy it into the project. Do not edit the cached original directly.

```bash
cd /home/$USER/ece511_final_porj
```

```bash
cp ~/.cache/gem5/riscv-disk-img ./riscv-disk.img
```

## 5. Build the m5 Library

The wrapper uses gem5 pseudo-instructions to reset/dump stats and exit the
simulation cleanly.

```bash
cd /home/$USER/ece511_final_porj/util/m5
```

```bash
scons build/riscv/out/libm5.a riscv.CROSS_COMPILE=riscv64-linux-gnu-
```

## 6. Build the Wrapper

The wrapper is committed as `wrapper.c`. It prints benchmark markers, resets
stats, runs FFT, dumps stats, and exits gem5.

```bash
cd /home/$USER/ece511_final_porj
```

```bash
riscv64-linux-gnu-gcc -static -O2 -I include wrapper.c util/m5/build/riscv/out/libm5.a -o benchmarks/riscv/wrapper
```

## 7. Verify the FFT Binary Exists

This flow expects the FFT benchmark binary at:

```text
benchmarks/riscv/FFT
```

Check it:

```bash
cd /home/$USER/ece511_final_porj
```

```bash
file benchmarks/riscv/FFT
```

It should be a RISC-V executable. Static linking is strongly preferred for this
busybox full-system image.

## 8. Inject FFT, Wrapper, and rcS into the Disk Image

The committed `rcS.benchmark` replaces the guest `/etc/rcS`. It launches
`/root/wrapper` automatically after the kernel boots.

The committed `debugfs_install_benchmark.cmds` installs:

- `rcS.benchmark` as `/etc/rcS`
- `benchmarks/riscv/FFT` as `/root/benchmark`
- `benchmarks/riscv/wrapper` as `/root/wrapper`

Run:

```bash
cd /home/$USER/ece511_final_porj
```

```bash
debugfs -w -f debugfs_install_benchmark.cmds riscv-disk.img
```

Make sure `/etc/rcS` is executable:

```bash
debugfs -w -R 'set_inode_field /etc/rcS mode 0100755' riscv-disk.img
```

Optionally check the filesystem:

```bash
e2fsck -fn riscv-disk.img
```

## 9. Confirm the Experiment Knobs

The experiment knobs are set in:

```text
configs/example/gem5_library/riscv-fs.py
```

The important settings are:

```python
mmu.dtb.size = 8
mmu.dtb.eviction_controller.entries = 4096

ctrl.predictor = "linear"
ctrl.linear_bias = -1.25
ctrl.linear_threshold = 0.0
ctrl.linear_weights = [0.8, 0.5, 1.2, 0.7, 1.0, 0.1, 0.1, 0.0, -0.05]
```

This intentionally stresses the DTLB so victim hits become visible.

## 10. Run the Simulation

Use a fresh output directory so results are easy to compare.

```bash
cd /home/$USER/ece511_final_porj
```

```bash
./build/RISCV/gem5.opt --outdir=m5out_dtb8_victim4096_fixed configs/example/gem5_library/riscv-fs.py --kernel=/home/$USER/.cache/gem5/riscv-bootloader-vmlinux-5.10 --disk-image=/home/$USER/ece511_final_porj/riscv-disk.img --cpu-type=TimingSimpleCPU --caches --l1i_size=32kB --l1d_size=32kB --l2cache --l2_size=256kB --l2_assoc=8 --mem-type=DDR4_2400_8x8 --mem-size=1GB
```

## 11. Verify the Benchmark Ran

Check the guest terminal log:

```bash
cd /home/$USER/ece511_final_porj
```

```bash
rg -n "RCS|BENCHMARK|FFT|Complex Doubles|exit_code|panic|segfault" m5out_dtb8_victim4096_fixed/board.platform.terminal
```

A successful run should contain lines like:

```text
===== RCS_LAUNCHING_BENCHMARK_WRAPPER =====
===== BENCHMARK_START: /root/benchmark -p 1 -m18 =====
FFT with Blocking Transpose
   262144 Complex Doubles
===== BENCHMARK_DONE: exit_code=0 =====
```

There should be no kernel panic and no init segfault.

## 12. Verify the TLB/Victima Result

Check the key stats:

```bash
cd /home/$USER/ece511_final_porj
```

```bash
rg -n "simSeconds|simTicks|finalTick|numCycles|dtb.readHits|dtb.readMisses|dtb.writeHits|dtb.writeMisses|dtb.victimHits|dtb.victimMisses|dtb.hits|dtb.misses|dtb.accesses|eviction_controller.evictions|retainedEvictions|droppedEvictions|eviction_controller.victimHits|eviction_controller.victimMisses|deterministicPredictions|linearPredictions" m5out_dtb8_victim4096_fixed/stats.txt
```

The important signs are:

- `linearPredictions == eviction_controller.evictions`
- `deterministicPredictions == 0`
- `retainedEvictions + droppedEvictions == eviction_controller.evictions`
- `dtb.victimHits > 0`
- `dtb.eviction_controller.victimHits > 0`

Those confirm that:

1. The linear predictor is being used.
2. Evictions are being classified by the linear threshold.
3. Some DTLB misses are rescued from retained victim entries.

## 13. Optional Baseline Comparison

To show usefulness, compare against a baseline where the victim controller keeps
no entries.

In `configs/example/gem5_library/riscv-fs.py`, temporarily set:

```python
mmu.dtb.size = 8
mmu.dtb.eviction_controller.entries = 0
```

Rebuild is not required for this config-only change. Rerun with a different
output directory:

```bash
cd /home/$USER/ece511_final_porj
```

```bash
./build/RISCV/gem5.opt --outdir=m5out_dtb8_victim0 configs/example/gem5_library/riscv-fs.py --kernel=/home/$USER/.cache/gem5/riscv-bootloader-vmlinux-5.10 --disk-image=/home/$USER/ece511_final_porj/riscv-disk.img --cpu-type=TimingSimpleCPU --caches --l1i_size=32kB --l1d_size=32kB --l2cache --l2_size=256kB --l2_assoc=8 --mem-type=DDR4_2400_8x8 --mem-size=1GB
```

Compare:

```bash
rg -n "simTicks|numCycles|dtb.victimHits|dtb.misses|dtb.accesses|dptw_caches.demandAccesses|dptw_caches.demandMisses" m5out_dtb8_victim4096_fixed/stats.txt m5out_dtb8_victim0/stats.txt
```

For the optimized run to be useful, expect some combination of:

- more `dtb.victimHits`
- fewer `dtb.misses`
- fewer page-table-walker cache accesses
- lower `numCycles` or `simTicks`

## 14. Notes on Generated Files

Do not commit these:

```text
build/
m5out*/
riscv-disk.img
*.core
```

Each user should generate their own disk image and output directories locally.

