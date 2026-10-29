#!/bin/bash
#
# Measures what the disk of the stand can do without wheelie: fio on the volume
# that holds the file of the server, from the image of the client. The numbers
# are the ceiling that the results of bench/run.sh are compared with.
#
# See `bench/ceiling.sh --help`, and docs/methodology.md.

set -eu

DOCKER=${DOCKER:-docker}

usage() {
    cat << 'USAGE'
Usage: bench/ceiling.sh [options]

  --out DIR         where the results go (default: results/ceiling)
  --runtime SEC     measured time of each run (default: 30)
  --ramp SEC        time before the measure, thrown away (default: 5)
  --size SIZE       size of the test file on the volume (default: 1g)
  --volume NAME     the docker volume of the stand (default: the one that
                    `docker volume ls` shows ending in _data)
  --sync 0|1        1 makes the writes durable before they finish, as
                    bench/run.sh --sync 1 does: fio with sync=dsync, which is
                    what the server asks of the kernel (default: 1)
  --stand-file      measure the file of the server (disk.img) instead of a new
                    one: the speed of writes of a virtual disk can differ
                    between files and between hours, and this is the number to
                    compare the results of bench/run.sh with
  --device DEV      also read this block device, read only (e.g. /dev/sda)
  --image NAME      the image with fio (default: wheeliestor-client)
  -h, --help        show this help and exit

The runs, each with O_DIRECT:

  io_uring   randread 4k   depth 1, 16, 64 with one job; 16 and 64 with 8 jobs
  io_uring   randwrite 4k  the same
  io_uring   read and write 1m, depth 4, 4 jobs
  psync      randread and randwrite 4k with 1, 8 and 16 jobs; read and write
             1m with 4 jobs (a blocking call per request: what libc can do)
  device     (with --device) randread 4k and read 1m, no file system

Environment:
  DOCKER            the docker command (default: docker; `DOCKER="sudo docker"`)

The images have to be built already (bench/run.sh builds them).
USAGE
}

out=results/ceiling runtime=30 ramp=5 size=1g volume="" device="" image=wheeliestor-client
stand_file=0 sync=1

while [ $# -gt 0 ]; do
    case $1 in
    --out) out=$2; shift 2 ;;
    --runtime) runtime=$2; shift 2 ;;
    --ramp) ramp=$2; shift 2 ;;
    --size) size=$2; shift 2 ;;
    --volume) volume=$2; shift 2 ;;
    --sync) sync=$2; shift 2 ;;
    --stand-file) stand_file=1; shift ;;
    --device) device=$2; shift 2 ;;
    --image) image=$2; shift 2 ;;
    -h | --help) usage; exit 0 ;;
    *) echo "bench/ceiling.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$volume" ]; then
    volumes=$($DOCKER volume ls --format '{{.Name}}' | grep '_data$' || true)
    case $(printf '%s\n' "$volumes" | grep -c .) in
    0)
        echo "bench/ceiling.sh: no docker volume of the stand; run bench/run.sh once or give --volume" >&2
        exit 2
        ;;
    1) volume=$volumes ;;
    *)
        # Every checkout directory is a compose project of its own, with its
        # own volume: the one to measure is the one that bench/run.sh uses.
        echo "bench/ceiling.sh: several volumes, give the one of the stand with --volume:" >&2
        printf '  %s\n' $volumes >&2
        exit 2
        ;;
    esac
fi
if ! $DOCKER image inspect "$image" > /dev/null 2>&1; then
    echo "bench/ceiling.sh: no image '$image'; build it with bench/run.sh" >&2
    exit 2
fi

test_file=/data/ceiling-test
[ "$stand_file" = 1 ] && test_file=/data/disk.img

mkdir -p "$out"
out_abs=$(cd "$out" && pwd)

# One fio run; $1 is its name, $2 file or device, the rest are fio options.
run() {
    local name=$1 target=$2
    shift 2
    local target_options
    if [ "$target" = file ]; then
        target_options=(--filename="$test_file" --size="$size")
    else
        target_options=(--filename="$device" --readonly --size=20g)
    fi
    # The writes of the stand are durable by default, so the ceiling of a write is
    # that of a durable write; a read has nothing to make durable.
    case $name in
    *write*) [ "$sync" = 1 ] && set -- "$@" --sync=dsync ;;
    esac
    local mount=(-v "$volume":/data)
    [ "$target" = device ] && mount=(--device "$device")

    $DOCKER run --rm --security-opt seccomp=unconfined "${mount[@]}" \
        --entrypoint fio "$image" \
        --name="$name" "${target_options[@]}" --direct=1 --thread \
        --group_reporting --time_based --runtime="$runtime" \
        --ramp_time="$ramp" --output-format=json "$@" \
        > "$out_abs/$name.json" 2> "$out_abs/$name.err" || {
        echo "bench/ceiling.sh: $name failed:" >&2
        tail -3 "$out_abs/$name.err" >&2
        exit 1
    }
    python3 -I - "$out_abs/$name.json" "$name" << 'PY'
import json, sys

text = open(sys.argv[1]).read()
job = json.loads(text[text.index("{"):])["jobs"][0]
stats = job["read"] if job["read"]["io_bytes"] else job["write"]
deviation = 100 * stats["iops_stddev"] / stats["iops_mean"] if stats["iops_mean"] else 0
print("%-34s iops=%7d  bw=%7.0f MiB/s  clat=%7.0f us  p99=%7.0f us  stddev=%4.1f%%" % (
    sys.argv[2], stats["iops"], stats["bw"] / 1024, stats["clat_ns"]["mean"] / 1000,
    stats["clat_ns"]["percentile"]["99.000000"] / 1000, deviation))
PY
}

summary="$out_abs/summary.txt"
{
    echo "== ceiling of $volume on $test_file, ${runtime}s measured after ${ramp}s ramp, size $size"
    for rw in randread randwrite; do
        for shape in 1:1 16:1 64:1 16:8 64:8; do
            depth=${shape%:*} jobs=${shape#*:}
            run "uring_${rw}_qd${depth}_nj${jobs}" file --ioengine=io_uring \
                --rw=$rw --bs=4k --iodepth="$depth" --numjobs="$jobs"
        done
    done
    run uring_seqread_1m file --ioengine=io_uring --rw=read --bs=1m --iodepth=4 --numjobs=4
    run uring_seqwrite_1m file --ioengine=io_uring --rw=write --bs=1m --iodepth=4 --numjobs=4
    for rw in randread randwrite; do
        for jobs in 1 8 16; do
            run "psync_${rw}_nj${jobs}" file --ioengine=psync \
                --rw=$rw --bs=4k --numjobs="$jobs"
        done
    done
    run psync_seqread_1m file --ioengine=psync --rw=read --bs=1m --numjobs=4
    run psync_seqwrite_1m file --ioengine=psync --rw=write --bs=1m --numjobs=4
    if [ -n "$device" ]; then
        for shape in 1:1 16:1 64:1 64:8; do
            depth=${shape%:*} jobs=${shape#*:}
            run "device_randread_qd${depth}_nj${jobs}" device --ioengine=io_uring \
                --rw=randread --bs=4k --iodepth="$depth" --numjobs="$jobs"
        done
        run device_seqread_1m device --ioengine=io_uring --rw=read --bs=1m --iodepth=4 --numjobs=4
    fi
} | tee "$summary"

if [ "$stand_file" = 0 ]; then
    $DOCKER run --rm -v "$volume":/data --entrypoint rm "$image" -f /data/ceiling-test
fi
echo "results: $out/ (summary in $out/summary.txt)"
