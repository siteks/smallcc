#!/bin/bash
# Test every possible optimization mask (0x000–0x7FF, 2048 combinations)
# against coremark_single_file.  One line per run:
#   mask  pass/FAIL  cycles  CoreMark/MHz  [enabled passes]
#
# Usage:  ./test_masks.sh [-j N]       (N = parallel jobs, default 1)
#         ./test_masks.sh -j8          (8 parallel workers)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SMALLCC="$SCRIPT_DIR/smallcc"
SIM="$SCRIPT_DIR/sim_c"
COREMARK_DIR="$SCRIPT_DIR/../coremark_single_file"
COREMARK_SRC="$COREMARK_DIR/coremark_single.c"
MAXSTEPS=4000000
MAX_MASK=0x7FF  # 11 bits

# Pass names indexed by bit position
PASS_NAMES=(
    "fold_br"        # bit 0
    "dead_blocks"    # bit 1
    "copy_prop"      # bit 2
    "cse"            # bit 3
    "redundant_bool" # bit 4
    "narrow_loads"   # bit 5
    "licm"           # bit 6
    "jump_thread"    # bit 7
    "unroll"         # bit 8
    "leg_e"          # bit 9
    "leg_f"          # bit 10
)

decode_mask() {
    local mask=$1
    local names=""
    for i in "${!PASS_NAMES[@]}"; do
        if (( mask & (1 << i) )); then
            names="${names:+$names,}${PASS_NAMES[$i]}"
        fi
    done
    echo "${names:--none-}"
}

run_one() {
    local mask=$1
    local hex
    hex=$(printf "0x%03x" "$mask")
    local tmpdir
    tmpdir=$(mktemp -d)
    local asm="$tmpdir/cm.s"

    # Compile
    if ! "$SMALLCC" -arch cpu4 "-Omask=$mask" -o "$asm" "$COREMARK_SRC" 2>/dev/null; then
        printf "%-7s  %-4s  %10s  %10s  [%s]\n" "$hex" "FAIL" "compile" "-" "$(decode_mask "$mask")"
        rm -rf "$tmpdir"
        return
    fi

    # Run
    local output
    output=$("$SIM" -arch cpu4 -maxsteps "$MAXSTEPS" "$asm" 2>&1) || true

    # Parse
    local correct="FAIL"
    if echo "$output" | grep -q "Correct values"; then
        correct="pass"
    fi

    local cycles="-"
    local cm_mhz="-"
    local cyc_match
    cyc_match=$(echo "$output" | grep -o "cycles:[0-9]*" | head -1)
    if [ -n "$cyc_match" ]; then
        cycles="${cyc_match#cycles:}"
    fi

    # CoreMark/MHz = Iterations/Sec as reported by CoreMark itself
    # (uses the MMIO cycle counter at 0xFF00 as a 1 MHz clock)
    local iter_sec
    iter_sec=$(echo "$output" | grep "Iterations/Sec" | awk '{print $NF}')
    if [ -n "$iter_sec" ]; then
        cm_mhz="$iter_sec"
    fi

    printf "%-7s  %-4s  %10s  %10s  [%s]\n" "$hex" "$correct" "$cycles" "$cm_mhz" "$(decode_mask "$mask")"
    rm -rf "$tmpdir"
}

# Parse -j flag
JOBS=1
while getopts "j:" opt; do
    case $opt in
        j) JOBS="$OPTARG" ;;
        *) echo "Usage: $0 [-j N]" >&2; exit 1 ;;
    esac
done

# Header
printf "%-7s  %-4s  %10s  %10s  %s\n" "mask" "ok?" "cycles" "CM/MHz" "passes"
printf "%s\n" "-------  ----  ----------  ----------  ------"

if [ "$JOBS" -le 1 ]; then
    # Sequential
    for (( mask=0; mask<=MAX_MASK; mask++ )); do
        run_one "$mask"
    done
else
    # Parallel: export function and vars, use xargs
    # Bash cannot export arrays; serialize PASS_NAMES as a string.
    export -f run_one decode_mask
    export SMALLCC SIM COREMARK_SRC MAXSTEPS
    export PASS_NAMES_STR="${PASS_NAMES[*]}"

    seq 0 $((MAX_MASK)) | xargs -P "$JOBS" -I{} bash -c 'PASS_NAMES=($PASS_NAMES_STR); run_one "$@"' _ {}
fi
