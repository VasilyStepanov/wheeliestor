#!/bin/bash
#
# Runs fio against a wheelie server in two containers and prints what fio
# reports. The server is started with the variant to test, and fio runs in the
# client container with the wheelie engine, with the same variant on its side.
#
# See `bench/measure.sh --help`, and docs/bench.md.

set -eu

usage() {
    cat << 'USAGE'
Использование: bench/measure.sh [параметры]
Значение параметра можно писать и через равенство: --depth=16 вместо --depth 16.

Нагрузка:
  --bs SIZE         размер блока (по умолчанию: 4k)
  --mode MODE       rw у fio: randread, randwrite, read, write, randrw
                    (по умолчанию: randread)
  --depth N         запросов в полёте на одно задание fio (по умолчанию: 16)
  --numjobs N       заданий fio, у каждого своё соединение (по умолчанию: 1)
  --queues N        соединений у каждого задания fio, по ним раскладываются
                    его запросы (по умолчанию: 1)
  --sync 0|1        1: запись надёжна к моменту ответа (по умолчанию: 1)
  --runtime SEC     замеряемое время (по умолчанию: 10)
  --ramp SEC        разгон перед замером, его fio отбрасывает (по умолчанию: 3)
  --size SIZE       какую часть файла читать и писать (по умолчанию: 1g)
  --complete-max N  сколько готовых завершений fio забирает за один вызов
                    (по умолчанию: 1, как у fio)
  --complete-min N  сколько завершений fio ждёт за один вызов (по умолчанию: 1,
                    как у fio)
  --submit-batch N  сколько запросов fio подаёт за одну пачку (по умолчанию: 1,
                    как у fio)
  --fio-arg ARG     ещё один аргумент для fio, например --fio-arg --norandommap;
                    можно повторять
  --server-cpus LIST CPU, разрешённые серверу, например 8-11 (по умолчанию: все)
  --client-cpus LIST CPU для заданий fio, по одному на задание, например 12-19
                    (по умолчанию: все)

Вариант, который получают и сервер, и клиент, если клиенту не задан другой:
  --backend NAME    libc, alibc или uring (по умолчанию: uring)
  --multishot       принимать одним multishot recv вместо одного recv на кусок
  --client-backend NAME
                    бэкенд клиента, если он не тот, что у сервера
                    (по умолчанию: --backend)
  --client-multishot 0|1
                    multishot recv у клиента (по умолчанию: --multishot, если у
                    клиента бэкенд сервера, и 0, если другой)
  --direct 0|1      сервер открывает файл с O_DIRECT (по умолчанию: 1)
  --data disk|tmpfs|device
                    где лежит файл сервера: на томе диска, в памяти (tmpfs) или
                    на блочном устройстве WHEELIE_DEVICE без файловой системы
                    (по умолчанию: disk)
  --disk-threads N  дисковых потоков в пуле каждого реактора alibc (по умолчанию:
                    16; у других бэкендов пула нет)
  --threads N       потоков сервера, в каждом свой реактор (по умолчанию: по
                    числу его CPU)

Прогон:
  --name NAME       имя файлов результата в каталоге вывода
  --out DIR         куда класть результаты (по умолчанию: results)
  --no-build        не собирать образы
  --keep            оставить контейнеры работать после прогона
  --profile         записать сервер и fio через perf на часть замеряемого
                    времени и сохранить доли слоёв пути запроса в
                    NAME.profile.json (Linux, нужен perf и root или `sudo -n`;
                    см. bench/layers.py)
  --profile-window SEC
                    сколько писать профиль (по умолчанию: 15, и не больше
                    замеряемого времени минус 2)
  --submit URL      отправить результат на этот сервер результатов, а не на
                    сервер по умолчанию (http://wheelie.rawstor.io)
  --no-submit       не отправлять результат никуда; по умолчанию он отправляется
                    на сервер результатов после прогона (см. bench/submit.py)
  -h, --help        показать эту справку и выйти

Окружение:
  DOCKER            команда docker (по умолчанию её определяет скрипт: docker, а если
                    нет прав, то sudo docker; пароль спрашивается один раз)
USAGE
}

bs=4k mode=randread depth=16 numjobs=1 queues=1 sync=1 runtime=10 ramp=3 size=1g
backend=uring multishot=0 client_backend="" client_multishot="" direct=1 data=disk threads="" disk_threads=""
complete_max="" complete_min="" submit_batch="" server_cpus="" client_cpus=""
name="" out=results build=1 keep=0
profile=0 profile_window="" submit_url="" no_submit=0
extra_fio=()

while [ $# -gt 0 ]; do
    # --option=value is --option value: split once here, where a value that was taken
    # by an option never comes to be looked at.
    case $1 in --*=*) set -- "${1%%=*}" "${1#*=}" "${@:2}" ;; esac
    case $1 in
    --bs) bs=$2; shift 2 ;;
    --mode) mode=$2; shift 2 ;;
    --depth) depth=$2; shift 2 ;;
    --numjobs) numjobs=$2; shift 2 ;;
    --queues) queues=$2; shift 2 ;;
    --sync) sync=$2; shift 2 ;;
    --runtime) runtime=$2; shift 2 ;;
    --ramp) ramp=$2; shift 2 ;;
    --size) size=$2; shift 2 ;;
    --complete-max) complete_max=$2; shift 2 ;;
    --complete-min) complete_min=$2; shift 2 ;;
    --submit-batch) submit_batch=$2; shift 2 ;;
    --fio-arg) extra_fio+=("$2"); shift 2 ;;
    --profile) profile=1; shift ;;
    --profile-window) profile_window=$2; shift 2 ;;
    --submit) submit_url=$2; shift 2 ;;
    --no-submit) no_submit=1; shift ;;
    --server-cpus) server_cpus=$2; shift 2 ;;
    --client-cpus) client_cpus=$2; shift 2 ;;
    --backend) backend=$2; shift 2 ;;
    --multishot) multishot=1; shift ;;
    --client-backend) client_backend=$2; shift 2 ;;
    --client-multishot) client_multishot=$2; shift 2 ;;
    --direct) direct=$2; shift 2 ;;
    --data) data=$2; shift 2 ;;
    --threads) threads=$2; shift 2 ;;
    --disk-threads) disk_threads=$2; shift 2 ;;
    --name) name=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --no-build) build=0; shift ;;
    --keep) keep=1; shift ;;
    -h | --help) usage; exit 0 ;;
    *) echo "bench/measure.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

if [ "$no_submit" = 1 ] && [ -n "$submit_url" ]; then
    echo "bench/measure.sh: --submit and --no-submit together make no sense" >&2
    exit 2
fi
# Where a result goes: the server that is told, or, if none, the one that bench/submit.py has.
submit_args=()
[ -z "$submit_url" ] || submit_args=(--url "$submit_url")

case $backend in
libc | alibc | uring) ;;
*) echo "bench/measure.sh: --backend must be libc, alibc or uring" >&2; exit 2 ;;
esac
[ -n "$client_backend" ] || client_backend=$backend
case $client_backend in
libc | alibc | uring) ;;
*) echo "bench/measure.sh: --client-backend must be libc, alibc or uring" >&2; exit 2 ;;
esac
if [ -z "$client_multishot" ]; then
    client_multishot=0
    [ "$client_backend" = "$backend" ] && client_multishot=$multishot
fi
case $client_multishot in
0 | 1) ;;
*) echo "bench/measure.sh: --client-multishot must be 0 or 1" >&2; exit 2 ;;
esac
if [ "$client_multishot" = 1 ] && [ "$client_backend" != uring ]; then
    echo "bench/measure.sh: multishot receive is only for the uring backend of the client" >&2
    exit 2
fi
case $direct in
0 | 1) ;;
*) echo "bench/measure.sh: --direct must be 0 or 1" >&2; exit 2 ;;
esac
case $data in
disk | tmpfs) ;;
device)
    if [ -z "${WHEELIE_DEVICE:-}" ] || [ ! -b "$WHEELIE_DEVICE" ]; then
        echo "bench/measure.sh: --data device needs WHEELIE_DEVICE, a block device" >&2
        exit 2
    fi
    ;;
*) echo "bench/measure.sh: --data must be disk, tmpfs or device" >&2; exit 2 ;;
esac

fio_options=()
if [ -n "$complete_max" ]; then
    if ! [[ $complete_max =~ ^[1-9][0-9]*$ ]]; then
        echo "bench/measure.sh: --complete-max must be a positive number" >&2
        exit 2
    fi
    fio_options+=(--iodepth_batch_complete_max="$complete_max")
fi
if [ -n "$complete_min" ]; then
    if ! [[ $complete_min =~ ^[0-9]+$ ]]; then
        echo "bench/measure.sh: --complete-min must be a non-negative number" >&2
        exit 2
    fi
    fio_options+=(--iodepth_batch_complete_min="$complete_min")
fi
if [ -n "$submit_batch" ]; then
    if ! [[ $submit_batch =~ ^[1-9][0-9]*$ ]]; then
        echo "bench/measure.sh: --submit-batch must be a positive number" >&2
        exit 2
    fi
    fio_options+=(--iodepth_batch_submit="$submit_batch")
fi
if [ -n "$client_cpus" ]; then
    fio_options+=(--cpus_allowed="$client_cpus" --cpus_allowed_policy=split)
fi

cd "$(dirname "$0")/.."
. bench/docker.sh
choose_docker

# fio's size, in MiB, rounded up.
to_mb() {
    local text number unit
    text=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')
    number=${text//[^0-9]/}
    unit=${text//[0-9]/}
    case $unit in
    "" | b) echo $(((number + 1048575) / 1048576)) ;;
    k | kb | kib) echo $(((number + 1023) / 1024)) ;;
    m | mb | mib) echo "$number" ;;
    g | gb | gib) echo $((number * 1024)) ;;
    *) echo "bench/measure.sh: cannot read the size '$1'" >&2; exit 2 ;;
    esac
}

size_mb=$(to_mb "$size")
file_mb=$((size_mb > 2048 ? size_mb : 2048))

if [ "$profile" = 1 ]; then
    if [ -z "$profile_window" ]; then
        profile_window=15
        [ $((runtime - 2)) -lt "$profile_window" ] && profile_window=$((runtime - 2))
    fi
    case $profile_window in
    '' | *[!0-9]* | 0)
        echo "bench/measure.sh: --profile-window must be a positive number" >&2
        exit 2
        ;;
    esac
    if [ "$profile_window" -gt $((runtime - 2)) ] || [ "$profile_window" -lt 1 ]; then
        echo "bench/measure.sh: --profile needs a measured time (--runtime) of at least window + 2 seconds" >&2
        exit 2
    fi
    if ! command -v perf > /dev/null 2>&1; then
        echo "bench/measure.sh: --profile needs perf" >&2
        exit 2
    fi
fi

case $queues in
'' | *[!0-9]* | 0)
    echo "bench/measure.sh: --queues must be a positive number" >&2
    exit 2
    ;;
esac

# A connection keeps one receive, and each request that is not answered yet a
# disk operation or a send: the server has to have room for all of them.
queue_depth=$((numjobs * (depth + 2 * queues) * 2))
[ "$queue_depth" -lt 1024 ] && queue_depth=1024

# How the variant is told in a name and in a line: the value of a command
# substitution that fails would stop the script, so it is not used for this.
queues_name=""
if [ "$queues" -gt 1 ]; then
    queues_name=_q$queues
fi
ms_name="" ms_text=""
if [ "$multishot" = 1 ]; then
    ms_name=_ms
    ms_text=" multishot"
fi
# The name of a run says who the client is only when it is not the server's variant.
client_name=""
if [ "$client_backend" != "$backend" ] || [ "$client_multishot" != "$multishot" ]; then
    client_name=_client-$client_backend
    [ "$client_multishot" = 1 ] && client_name=${client_name}-ms
fi
direct_text=buffered
if [ "$direct" = 1 ]; then
    direct_text=O_DIRECT
fi

[ -n "$name" ] || name="${mode}_bs${bs}_qd${depth}_nj${numjobs}${queues_name}_${backend}${ms_name}${client_name}_sync${sync}"
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
WHEELIE_DATA=$data
WHEELIE_DEVICE=${WHEELIE_DEVICE:-}
WHEELIE_THREADS=$threads
WHEELIE_DISK_THREADS=$disk_threads
WHEELIE_QUEUE_DEPTH=$queue_depth
WHEELIE_FILE_SIZE_MB=$file_mb
WHEELIE_RESULTS=$out_abs
WHEELIE_SERVER_CPUS=$server_cpus
WHEELIE_CLIENT_CPUS=$client_cpus
ENV

compose_files=(-f docker-compose.yml)
if [ "$data" = tmpfs ]; then
    compose_files+=(-f docker-compose.tmpfs.yml)
elif [ "$data" = device ]; then
    compose_files+=(-f docker-compose.device.yml)
fi

# Nothing is read from the terminal: with `sudo docker` the plugin of compose is a
# background process of a terminal of its own and is stopped when it reads from it.
compose() { $DOCKER compose --env-file "$env_file" "${compose_files[@]}" "$@" < /dev/null; }

cleanup() {
    if [ "$keep" = 0 ]; then
        compose down > /dev/null 2>&1 || true
    fi
    rm -f "$env_file"
}
trap cleanup EXIT

cp "$env_file" "$out_abs/$name.settings"

# Who is sending, said before the run: the page of results can be opened and
# searched for the name while the run goes.
if [ "$no_submit" = 0 ]; then
    python3 -I "$(dirname "$0")/submit.py" --whoami ${submit_args[@]+"${submit_args[@]}"} || true
fi

if [ "$build" = 1 ]; then
    echo "== building the images"
    compose build
fi

if ! compose up -d --force-recreate --wait server; then
    echo "bench/measure.sh: the server does not start:" >&2
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
    echo "bench/measure.sh: the server was not started as asked ($backend$ms_text, direct=$direct)" >&2
    exit 1
    ;;
esac
echo "${started#wheelie-server: }" > "$out_abs/$name.server"

# The same for where the file is: a volume that was made before may be on a disk.
if [ "$data" = device ]; then
    echo "== server: the file is the block device $WHEELIE_DEVICE"
else
    data_type=$(compose exec -T server stat -f -c %T /data | tr -d '\r')
    echo "== server: the file is on $data_type"
    if [ "$data" = tmpfs ] && [ "$data_type" != tmpfs ]; then
        echo "bench/measure.sh: the server's file is not in memory ($data_type)" >&2
        exit 1
    fi
fi

if [ -n "$client_name" ]; then
    echo "== client: ${client_name#_client-}, not the variant of the server"
fi
echo "== fio: $mode bs=$bs depth=$depth numjobs=$numjobs queues=$queues sync=$sync, ${ramp}s ramp and ${runtime}s measured"
# fio also writes the IOPS of every second of the measured part, all jobs together,
# to <name>_iops.log next to the result: the course of a run, which its summary hides.
# fio prints its progress, a line a second (the ETA), and then its normal output
# and its JSON, all to its standard output: that is what is kept, and told apart
# below. Without a terminal fio shows no progress unless it is made to.
run_fio() {
    compose run --rm -T --user "$(id -u):$(id -g)" client \
        fio --name="$name" \
        --ioengine=external:/usr/local/lib/wheelie.so \
        --filename='wheelie\://server\:39900' \
        --wheelie_backend="$client_backend" --wheelie_multishot="$client_multishot" \
        --rw="$mode" --bs="$bs" --iodepth="$depth" --numjobs="$numjobs" \
        --wheelie_queues="$queues" \
        --sync="$sync" --size="$size" \
        --runtime="$runtime" --ramp_time="$ramp" --time_based \
        --thread --group_reporting \
        ${fio_options[@]+"${fio_options[@]}"} \
        ${extra_fio[@]+"${extra_fio[@]}"} \
        --write_iops_log="/results/$name" --log_avg_msec=1000 --per_job_logs=0 \
        --eta=always --eta-newline=1 \
        --output-format=normal,json > "$out_abs/$name.out"
}

# A command that may need root: perf of the processes of the containers does.
privileged() {
    if [ "$(id -u)" = 0 ]; then "$@"; else sudo -n "$@"; fi
}

# The server and fio run in the containers, and the host sees them as
# processes: perf records them from outside, for a window after the ramp.
record_profile() {
    local server_pid="" fio_pid="" i
    for i in $(seq 1 120); do
        server_pid=$(pgrep -xo wheelie-server || true)
        fio_pid=$(pgrep -xn fio || true)
        [ -n "$server_pid" ] && [ -n "$fio_pid" ] && break
        sleep 0.5
    done
    if [ -z "$server_pid" ] || [ -z "$fio_pid" ]; then
        echo "bench/measure.sh: the processes of the server and of fio are not found, no profile" >&2
        return 1
    fi
    sleep $((ramp + 1))
    echo "== perf: recording the server ($server_pid) and fio ($fio_pid) for ${profile_window}s"
    privileged perf record -q -F 1999 -p "$server_pid" -o "$out_abs/$name.server.perf" -- sleep "$profile_window" > /dev/null 2>&1 &
    local server_perf=$!
    privileged perf record -q -F 1999 -p "$fio_pid" -o "$out_abs/$name.client.perf" -- sleep "$profile_window" > /dev/null 2>&1 &
    wait "$server_perf" "$!" 2> /dev/null || true
}

# What the server spends, from /proc of the container, where the server is the process 1:
# a line a second of the time (in ticks) of all its threads in user and in kernel code, and
# of their context switches, voluntary and not. It needs nothing of the host, so it is there
# on any Docker. bench/submit.py takes the seconds of the measured window out of the lines.
sample_server() {
    compose exec -T server sh -c '
        i=0
        while [ "$i" -lt "$1" ]; do
            echo "$(date +%s%3N) $(sed "s/^.*) //" /proc/1/stat | cut -d" " -f12,13)" \
                "$(cat /proc/1/task/*/status | awk "/^voluntary_ctxt_switches/ {v += \$2} /^nonvoluntary_ctxt_switches/ {n += \$2} END {print v, n}")"
            i=$((i + 1))
            sleep 1
        done' sh $((ramp + runtime + 30)) > "$out_abs/$name.server-stat" 2> /dev/null &
    sampler_pid=$!
}

sample_server
if [ "$profile" = 1 ]; then
    run_fio &
    fio_run_pid=$!
    record_profile || true
    wait "$fio_run_pid"
else
    run_fio
fi
kill "$sampler_pid" 2> /dev/null || true
wait "$sampler_pid" 2> /dev/null || true

# fio wrote its progress, its normal output and its JSON into one file: the
# JSON is told apart by its braces, and the rest is the text of the result. The
# progress comes with carriage returns, which are made line ends, and the lines
# of progress that repeat are kept once.
tr '\r' '\n' < "$out_abs/$name.out" | awk -v txt="$out_abs/$name.txt" -v json="$out_abs/$name.json" '
    { sub(/[ \t]+$/, "") }
    /^{$/ { in_json = 1 }
    in_json { print > json }
    !in_json && !(/^Jobs:/ && $0 == previous) && !(/^$/ && previous ~ /^Jobs:/) { print > txt }
    !in_json { previous = $0 }
    /^}$/ { in_json = 0 }
'
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
    print("%-5s iops=%d (stddev %.1f%%)  bw=%.1f MiB/s  clat mean=%.1f us  p95=%.1f us" % (
        direction, stats["iops"], iops_stddev, stats["bw"] / 1024,
        clat["mean"] / 1000, clat["percentile"]["95.000000"] / 1000))
PY
fi
here=$(cd "$(dirname "$0")" && pwd)

if [ "$profile" = 1 ]; then
    if [ -f "$out_abs/$name.server.perf" ] || [ -f "$out_abs/$name.client.perf" ]; then
        # perf report needs the same rights as perf record had.
        if privileged python3 -I "$here/layers.py" build --window "$profile_window" --fio "$out_abs/$name.json" \
            --server "$out_abs/$name.server.perf" --client "$out_abs/$name.client.perf" \
            --out "$out_abs/$name.profile.json"; then
            privileged chown "$(id -u):$(id -g)" "$out_abs/$name.profile.json" 2> /dev/null || true
            privileged rm -f "$out_abs/$name.server.perf" "$out_abs/$name.client.perf"
            echo "profile: $out/$name.profile.json"
        else
            echo "bench/measure.sh: the profile was not made; the recordings are $out/$name.*.perf" >&2
        fi
    else
        echo "bench/measure.sh: no recordings of perf, no profile" >&2
    fi
fi

echo "results: $out/$name.txt, $out/$name.json, $out/$name.server"

if [ "$no_submit" = 0 ]; then
    # A result that is not sent is not a failed run: it stays in the directory.
    python3 -I "$here/submit.py" --no-whoami ${submit_args[@]+"${submit_args[@]}"} "$out_abs/$name.json" \
        || echo "bench/measure.sh: the result was not submitted; bench/submit.py can send it later" >&2
fi
