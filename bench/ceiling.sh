#!/bin/bash
#
# Measures what the disk of the stand can do without wheelie: fio on the volume
# that holds the file of the server, from the image of the client. The numbers
# are the ceiling that the results of bench/measure.sh are compared with.
#
# See `bench/ceiling.sh --help`, and docs/methodology.md.

set -eu

DOCKER=${DOCKER:-docker}

usage() {
    cat << 'USAGE'
Использование: bench/ceiling.sh [параметры]

  --out DIR         куда класть результаты (по умолчанию: results/ceiling)
  --runtime SEC     замеряемое время каждого прогона (по умолчанию: 15)
  --ramp SEC        разгон перед замером, его отбрасывают (по умолчанию: 5)
  --size SIZE       размер тестового файла на томе (по умолчанию: 1g)
  --volume NAME     docker-том стенда (по умолчанию: тот, что `docker volume ls`
                    показывает с именем на _data)
  --sync 0|1        1: запись надёжна к моменту завершения, как у
                    bench/measure.sh --sync 1: fio с sync=dsync, это то, что
                    сервер просит у ядра (по умолчанию: 1)
  --stand-file      мерить файл сервера (disk.img) вместо нового: скорость
                    записи виртуального диска может различаться между файлами и
                    между часами, и это число надо сравнивать с результатами
                    bench/measure.sh
  --device DEV      ещё и читать этот блочный диск, только чтение (например,
                    /dev/sda)
  --image NAME      образ с fio (по умолчанию: wheeliestor-client)
  -h, --help        показать эту справку и выйти

Прогоны, все с O_DIRECT:

  io_uring   randread 4k   глубина 1, 16, 64 с одним заданием; 16 и 64 с 8 заданиями
  io_uring   randwrite 4k  то же
  io_uring   read и write 1m, глубина 4, 4 задания
  psync      randread и randwrite 4k с 1, 8 и 16 заданиями; read и write 1m с
             4 заданиями (блокирующий вызов на запрос: что умеет libc)
  device     (с --device) randread 4k и read 1m, без файловой системы

Окружение:
  DOCKER            команда docker (по умолчанию: docker; `DOCKER="sudo docker"`)

Образы должны быть уже собраны (их собирает bench/measure.sh).
USAGE
}

out=results/ceiling runtime=15 ramp=5 size=1g volume="" device="" image=wheeliestor-client
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
        echo "bench/ceiling.sh: no docker volume of the stand; run bench/measure.sh once or give --volume" >&2
        exit 2
        ;;
    1) volume=$volumes ;;
    *)
        # Every checkout directory is a compose project of its own, with its
        # own volume: the one to measure is the one that bench/measure.sh uses.
        echo "bench/ceiling.sh: several volumes, give the one of the stand with --volume:" >&2
        printf '  %s\n' $volumes >&2
        exit 2
        ;;
    esac
fi
if ! $DOCKER image inspect "$image" > /dev/null 2>&1; then
    echo "bench/ceiling.sh: no image '$image'; build it with bench/measure.sh" >&2
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
print("%-34s iops=%7d  bw=%7.0f MiB/s  clat=%7.0f us  p95=%7.0f us  stddev=%4.1f%%" % (
    sys.argv[2], stats["iops"], stats["bw"] / 1024, stats["clat_ns"]["mean"] / 1000,
    stats["clat_ns"]["percentile"]["95.000000"] / 1000, deviation))
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
