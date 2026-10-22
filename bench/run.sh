#!/bin/bash
#
# Runs fio against a wheelie server in two containers and prints what fio
# reports. The server is started with the variant to test, and fio runs in the
# client container with the wheelie engine, with the same variant on its side.
#
# See `bench/run.sh --help`, and docs/bench.md.

set -eu

usage() {
    cat << 'USAGE'
Usage: bench/run.sh [options]

The load:
  --bs SIZE         block size (default: 4k)
  --mode MODE       fio's rw: randread, randwrite, read, write, randrw
                    (default: randread)
  --depth N         requests in flight on a connection (default: 16)
  --numjobs N       fio jobs, each with a connection of its own (default: 1)
  --sync 0|1        1 makes a write durable before it is answered (default: 0)
  --runtime SEC     measured time (default: 10)
  --ramp SEC        time before the measure, thrown away (default: 3)
  --size SIZE       the range of the file that is read and written (default: 1g)

The variant, which the server and the client both get:
  --backend NAME    libc, alibc or uring (default: uring)
  --multishot       receive with one multishot receive instead of one per chunk
  --direct 0|1      the server opens its file with O_DIRECT (default: 1)
  --threads N       server threads, each with a reactor (default: its CPUs)

The run:
  --name NAME       name of the result files in the output directory
  --out DIR         where the results go (default: results)
  --no-build        do not build the images
  --keep            leave the containers running afterwards
  -h, --help        show this help and exit

Environment:
  DOCKER            the docker command (default: docker; `DOCKER="sudo docker"`
                    if you are not in the docker group)
USAGE
}

bs=4k mode=randread depth=16 numjobs=1 sync=0 runtime=10 ramp=3 size=1g
backend=uring multishot=0 direct=1 threads=""
name="" out=results build=1 keep=0

while [ $# -gt 0 ]; do
    case $1 in
    --bs) bs=$2; shift 2 ;;
    --mode) mode=$2; shift 2 ;;
    --depth) depth=$2; shift 2 ;;
    --numjobs) numjobs=$2; shift 2 ;;
    --sync) sync=$2; shift 2 ;;
    --runtime) runtime=$2; shift 2 ;;
    --ramp) ramp=$2; shift 2 ;;
    --size) size=$2; shift 2 ;;
    --backend) backend=$2; shift 2 ;;
    --multishot) multishot=1; shift ;;
    --direct) direct=$2; shift 2 ;;
    --threads) threads=$2; shift 2 ;;
    --name) name=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --no-build) build=0; shift ;;
    --keep) keep=1; shift ;;
    -h | --help) usage; exit 0 ;;
    *) echo "bench/run.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

case $backend in
libc | alibc | uring) ;;
*) echo "bench/run.sh: --backend must be libc, alibc or uring" >&2; exit 2 ;;
esac
case $direct in
0 | 1) ;;
*) echo "bench/run.sh: --direct must be 0 or 1" >&2; exit 2 ;;
esac

DOCKER=${DOCKER:-docker}
cd "$(dirname "$0")/.."

# fio's size, in MiB, rounded up.
to_mb() {
    local text=${1,,} number unit
    number=${text//[^0-9]/}
    unit=${text//[0-9]/}
    case $unit in
    "" | b) echo $(((number + 1048575) / 1048576)) ;;
    k | kb | kib) echo $(((number + 1023) / 1024)) ;;
    m | mb | mib) echo "$number" ;;
    g | gb | gib) echo $((number * 1024)) ;;
    *) echo "bench/run.sh: cannot read the size '$1'" >&2; exit 2 ;;
    esac
}

size_mb=$(to_mb "$size")
file_mb=$((size_mb > 2048 ? size_mb : 2048))

# A connection keeps one receive, and each request that is not answered yet a
# disk operation or a send: the server has to have room for all of them.
queue_depth=$((numjobs * (depth + 2) * 2))
[ "$queue_depth" -lt 1024 ] && queue_depth=1024

# How the variant is told in a name and in a line: the value of a command
# substitution that fails would stop the script, so it is not used for this.
ms_name="" ms_text=""
if [ "$multishot" = 1 ]; then
    ms_name=_ms
    ms_text=" multishot"
fi
direct_text=buffered
if [ "$direct" = 1 ]; then
    direct_text=O_DIRECT
fi

[ -n "$name" ] || name="${mode}_bs${bs}_qd${depth}_nj${numjobs}_${backend}${ms_name}_sync${sync}"
mkdir -p "$out"
out_abs=$(cd "$out" && pwd)

# What compose is told goes to it in a file, not in the environment: docker run
# with sudo does not get the environment of the script, and a variant that never
# arrived would be a run of another one.
env_file=$(mktemp)
cat > "$env_file" << ENV
WHEELIE_BACKEND=$backend
WHEELIE_MULTISHOT=$multishot
WHEELIE_DIRECT=$direct
WHEELIE_THREADS=$threads
WHEELIE_QUEUE_DEPTH=$queue_depth
WHEELIE_FILE_SIZE_MB=$file_mb
WHEELIE_RESULTS=$out_abs
ENV

compose() { $DOCKER compose --env-file "$env_file" "$@"; }

cleanup() {
    if [ "$keep" = 0 ]; then
        compose down > /dev/null 2>&1 || true
    fi
    rm -f "$env_file"
}
trap cleanup EXIT

if [ "$build" = 1 ]; then
    echo "== building the images"
    compose build
fi

if ! compose up -d --force-recreate --wait server; then
    echo "bench/run.sh: the server does not start:" >&2
    compose logs server >&2 || true
    exit 1
fi

# What the server says it was started with, which is what it was: a variant
# that is only asked for is not a variant that is run.
started=$(compose logs --no-log-prefix server 2> /dev/null | grep "listening on" | tail -1)
echo "== server: ${started#wheelie-server: }"
case $started in
*"($backend$ms_text, "*"$direct_text)") ;;
*)
    echo "bench/run.sh: the server was not started as asked ($backend$ms_text, direct=$direct)" >&2
    exit 1
    ;;
esac
echo "${started#wheelie-server: }" > "$out_abs/$name.server"

echo "== fio: $mode bs=$bs depth=$depth numjobs=$numjobs sync=$sync, ${ramp}s ramp and ${runtime}s measured"
raw="/results/$name.out"
compose run --rm -T --user "$(id -u):$(id -g)" client \
    fio --name="$name" \
    --ioengine=external:/usr/local/lib/wheelie.so \
    --filename='wheelie\://server\:39900' \
    --wheelie_backend="$backend" --wheelie_multishot="$multishot" \
    --rw="$mode" --bs="$bs" --iodepth="$depth" --numjobs="$numjobs" \
    --sync="$sync" --size="$size" \
    --runtime="$runtime" --ramp_time="$ramp" --time_based \
    --thread --group_reporting \
    --output-format=normal,json --output="$raw"

# fio wrote its normal output and its JSON into one file: they are told apart
# by the braces of the JSON.
awk -v txt="$out_abs/$name.txt" -v json="$out_abs/$name.json" '
    /^{$/ { in_json = 1 }
    { print > (in_json ? json : txt) }
    /^}$/ { in_json = 0 }
' "$out_abs/$name.out"
rm -f "$out_abs/$name.out"

cat "$out_abs/$name.txt"

if command -v python3 > /dev/null 2>&1; then
    echo "== $name"
    python3 -I - "$out_abs/$name.json" << 'PY'
import json, sys

job = json.load(open(sys.argv[1]))["jobs"][0]
for direction in ("read", "write"):
    stats = job[direction]
    if stats["io_bytes"] == 0:
        continue
    clat = stats["clat_ns"]
    iops_stddev = 100 * stats["iops_stddev"] / stats["iops_mean"] if stats["iops_mean"] else 0
    print("%-5s iops=%d (stddev %.1f%%)  bw=%.1f MiB/s  clat mean=%.1f us  p99=%.1f us" % (
        direction, stats["iops"], iops_stddev, stats["bw"] / 1024,
        clat["mean"] / 1000, clat["percentile"]["99.000000"] / 1000))
PY
fi
echo "results: $out/$name.txt, $out/$name.json, $out/$name.server"
