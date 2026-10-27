#!/bin/bash
#
# Runs bench/run.sh for every variant on every load of a fixed set, keeps the
# results in one directory and makes a table and a chart of them with
# bench/report.py.
#
# See `bench/matrix.sh --help`, and docs/bench.md.

set -eu

usage() {
    cat << 'USAGE'
Usage: bench/matrix.sh [options] [options of bench/run.sh]

  --out DIR         where the results go (default: results/matrix-DATE)
  --variants LIST   the variants, a quoted list (default: "alibc uring libc uring-ms")
  --dry-run         only say what would be run
  --no-build        do not build the images
  -h, --help        show this help and exit

A variant is libc, alibc, uring or uring-ms (uring with multishot receive).
What is not an option of this script, such as --runtime, --ramp, --direct,
--sync, --threads or --size, goes to bench/run.sh for every run.

The loads, each of which is run for every variant:

  randread  4k, depth 16, one job and 8 jobs; depth 1, one job
  randwrite 4k, depth 16, one job and 8 jobs; depth 1, one job
  read      1m, depth 4, 4 jobs
  write     1m, depth 4, 4 jobs

The order is made for a chart of results that is filled as early as it can be
(bench/run.sh --submit sends every result when it is measured): the loads that
tell the most come first, and the first two variants of the first load are the
two that the chart compares by default, so the first point is there after two
runs. The variants of one load are run next to each other, and the variant
that starts a load changes from load to load, so that the first run after a
heavy load, which is a little slower, is not always the same variant. The order
of --variants is the order of the first load.

A run that has a result in the directory already is not run again, so a
matrix that was interrupted goes on where it stopped when it is run again with
the same --out.

Environment:
  DOCKER            the docker command, as for bench/run.sh
USAGE
}

# name:mode:bs:depth:numjobs
LOADS=(
    randread_qd16_nj1:randread:4k:16:1
    randwrite_qd16_nj1:randwrite:4k:16:1
    randread_qd16_nj8:randread:4k:16:8
    randwrite_qd16_nj8:randwrite:4k:16:8
    randread_qd1_nj1:randread:4k:1:1
    randwrite_qd1_nj1:randwrite:4k:1:1
    seqread_1m_nj4:read:1m:4:4
    seqwrite_1m_nj4:write:1m:4:4
)

out="" variants="alibc uring libc uring-ms" dry_run=0 build=1
passed=()

while [ $# -gt 0 ]; do
    case $1 in
    --out) out=$2; shift 2 ;;
    --variants) variants=$2; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    --no-build) build=0; shift ;;
    -h | --help) usage; exit 0 ;;
    *) passed+=("$1"); shift ;;
    esac
done

cd "$(dirname "$0")/.."
[ -n "$out" ] || out="results/matrix-$(date +%Y%m%d-%H%M%S)"

variant_options() {
    case $1 in
    libc | alibc | uring) echo "--backend $1" ;;
    uring-ms) echo "--backend uring --multishot" ;;
    *) echo "bench/matrix.sh: unknown variant '$1'" >&2; exit 2 ;;
    esac
}

for variant in $variants; do
    variant_options "$variant" > /dev/null
done

DOCKER=${DOCKER:-docker}

if [ "$dry_run" = 0 ] && [ "$build" = 1 ]; then
    echo "== building the images"
    $DOCKER compose build
fi

[ "$dry_run" = 1 ] || mkdir -p "$out"
total=$(($(echo $variants | wc -w) * ${#LOADS[@]}))
number=0
load_number=0
variant_list=($variants)
for load in "${LOADS[@]}"; do
    IFS=: read -r name mode bs depth numjobs <<< "$load"
    # The variants of a load start from another one than those of the load before.
    shift_by=$((load_number % ${#variant_list[@]}))
    load_number=$((load_number + 1))
    for ((i = 0; i < ${#variant_list[@]}; i++)); do
        variant=${variant_list[$(((i + shift_by) % ${#variant_list[@]}))]}
        number=$((number + 1))
        run_name="${name}_${variant}"
        if [ -s "$out/$run_name.json" ]; then
            echo "== [$number/$total] $run_name: done already"
            continue
        fi
        command=(bench/run.sh --no-build --name "$run_name" --out "$out"
            --mode "$mode" --bs "$bs" --depth "$depth" --numjobs "$numjobs"
            $(variant_options "$variant") ${passed[@]+"${passed[@]}"})
        echo "== [$number/$total] ${command[*]}"
        if [ "$dry_run" = 0 ]; then
            "${command[@]}" > "$out/run.log" 2>&1 || {
                echo "bench/matrix.sh: $run_name failed, see $out/run.log" >&2
                tail -5 "$out/run.log" >&2
                exit 1
            }
            grep -E "^(read|write) +iops" "$out/run.log" || true
        fi
    done
done

if [ "$dry_run" = 0 ]; then
    echo "== report"
    bench/report.py "$out" --ratio --plot "$out/libc-vs-others.svg" | tee "$out/table.md"
fi
