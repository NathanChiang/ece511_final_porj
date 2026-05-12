#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GEM5_BIN="${GEM5_BIN:-$ROOT_DIR/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT_DIR/configs/example/gem5_library/riscv-fs.py}"
KERNEL="${KERNEL:-$HOME/.cache/gem5/riscv-bootloader-vmlinux-5.10}"
DISK_IMAGE="${DISK_IMAGE:-$ROOT_DIR/riscv-disk.img}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT_DIR/results/oracle_fft}"

DTB_SIZES="${DTB_SIZES:-8 16}"
RATIOS="${RATIOS:-1 2 4 8}"
PREDICTOR="${PREDICTOR:-linear}"

# For training traces, keep the real Victima path disabled so the policy under
# test does not change the future DTLB miss stream. The oracle shadow buffer
# still labels what would have been reused for each target capacity.
REAL_VICTIM_ENTRIES="${REAL_VICTIM_ENTRIES:-0}"

COMMON_ARGS=(
    --kernel="$KERNEL"
    --disk-image="$DISK_IMAGE"
    --cpu-type=TimingSimpleCPU
    --caches
    --l1i_size=32kB
    --l1d_size=32kB
    --l2cache
    --l2_size=256kB
    --l2_assoc=8
    --mem-type=DDR4_2400_8x8
    --mem-size=1GB
)

mkdir -p "$RESULT_ROOT"

for dtb_size in $DTB_SIZES; do
    for ratio in $RATIOS; do
        oracle_entries=$((dtb_size * ratio))
        outdir="$RESULT_ROOT/dtb${dtb_size}_oracle${oracle_entries}_ratio${ratio}x"

        echo "==> dtb_size=$dtb_size oracle_entries=$oracle_entries ratio=${ratio}x"
        echo "    outdir=$outdir"

        "$GEM5_BIN" \
            --outdir="$outdir" \
            "$CONFIG" \
            "${COMMON_ARGS[@]}" \
            --dtb-size="$dtb_size" \
            --itb-size=64 \
            --victim-entries="$REAL_VICTIM_ENTRIES" \
            --oracle-entries="$oracle_entries" \
            --oracle-trace \
            --oracle-trace-file="$outdir/dtb_oracle_trace.csv" \
            --predictor="$PREDICTOR"

        if [[ ! -s "$outdir/dtb_oracle_trace.csv" ]]; then
            echo "ERROR: missing oracle trace: $outdir/dtb_oracle_trace.csv" >&2
            exit 1
        fi
    done
done

echo "Sweep complete: $RESULT_ROOT"
