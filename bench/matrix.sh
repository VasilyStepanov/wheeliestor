#!/bin/bash
#
# Runs bench/measure.sh for every variant on every load of a fixed set, keeps the
# results in one directory and makes a table and a chart of them with
# bench/report.py.
#
# See `bench/matrix.sh --help`, and docs/bench.md.

set -eu

usage() {
    cat << 'USAGE'
Использование: bench/matrix.sh [параметры] [параметры bench/measure.sh]

  --out DIR         куда класть результаты (по умолчанию: results/matrix-ДАТА)
  --variants LIST   варианты, список в кавычках (по умолчанию: "libc alibc uring")
  --seed N          зерно порядка вариантов (по умолчанию: время; оно
                    печатается, чтобы порядок можно было повторить)
  --dry-run         только сказать, что будет запущено
  --no-build        не собирать образы
  --submit URL      отправлять результаты на этот сервер результатов, а не на
                    сервер по умолчанию (http://wheelie.rawstor.io)
  --no-submit       не отправлять результаты никуда; по умолчанию каждый
                    результат отправляется на сервер сразу после замера
  -h, --help        показать эту справку и выйти

Вариант: libc, alibc, uring или uring-ms (uring с multishot recv).
Всё, что не параметр этого скрипта, например --runtime, --ramp, --direct,
--sync, --threads или --size, передаётся в bench/measure.sh для каждого прогона.
Прогон длится 15 секунд после разгона 5 (прогонов в матрице немного, чтобы
каждый мог быть долгим); --runtime и --ramp меняют это.

Нагрузки, каждая на каждом варианте:

  randread  4k, глубина 1, одно задание
  randwrite 4k, глубина 1, одно задание
  read      4k, глубина 1, одно задание (то же, что запись журнала, но чтение)
  write     4k, глубина 1, одно задание (коммит журнала: одна небольшая запись
            за раз)
  randread  4k, глубина 16, одно задание и 8 заданий
  randwrite 4k, глубина 16, одно задание и 8 заданий
  read      1m, глубина 4, 4 задания
  write     1m, глубина 4, 4 задания

Нагрузки идут в порядке таблицы в docs/bench.md: по блоку, глубине, числу
заданий и режиму. Варианты одной нагрузки идут подряд, в случайном порядке,
который для каждой нагрузки свой: первый прогон после тяжёлой нагрузки немного
медленнее, и что-то может прогреваться, так что первым не должен быть всегда
один и тот же вариант. Ни у какого варианта нет приоритета.

Прогон, у которого в каталоге уже есть результат, не запускается снова, так что
прерванная матрица продолжается с того места, где остановилась, если запустить
её снова с тем же --out.

Окружение:
  DOCKER            команда docker, как у bench/measure.sh
USAGE
}

# name:mode:bs:depth:numjobs
LOADS=(
    randread_qd1_nj1:randread:4k:1:1
    randwrite_qd1_nj1:randwrite:4k:1:1
    seqread_4k_qd1_nj1:read:4k:1:1
    seqwrite_4k_qd1_nj1:write:4k:1:1
    randread_qd16_nj1:randread:4k:16:1
    randwrite_qd16_nj1:randwrite:4k:16:1
    randread_qd16_nj8:randread:4k:16:8
    randwrite_qd16_nj8:randwrite:4k:16:8
    seqread_1m_nj4:read:1m:4:4
    seqwrite_1m_nj4:write:1m:4:4
)

out="" variants="libc alibc uring" dry_run=0 build=1 seed="" submit_url="" no_submit=0
passed=()
# A run is long unless the caller says otherwise.
runtime_default=(--runtime 15) ramp_default=(--ramp 5)

while [ $# -gt 0 ]; do
    case $1 in
    --out) out=$2; shift 2 ;;
    --variants) variants=$2; shift 2 ;;
    --seed) seed=$2; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    --submit) submit_url=$2; shift 2 ;;
    --no-submit) no_submit=1; shift ;;
    --no-build) build=0; shift ;;
    -h | --help) usage; exit 0 ;;
    --runtime) runtime_default=(); passed+=("$1"); shift ;;
    --ramp) ramp_default=(); passed+=("$1"); shift ;;
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

[ -n "$seed" ] || seed=$(($(date +%s) % 32768))
RANDOM=$seed
echo "== order of the variants: --seed $seed"

# The output of every run goes to a log, so who is sending is said here, once,
# after the output of the build and right before the first run: it is the last
# thing on the screen before the runs, and the page can be searched for the name.
if [ "$no_submit" = 1 ] && [ -n "$submit_url" ]; then
    echo "bench/matrix.sh: --submit and --no-submit together make no sense" >&2
    exit 2
fi
submit_options=() url_options=()
[ -z "$submit_url" ] || url_options=(--url "$submit_url")
if [ "$no_submit" = 1 ]; then
    submit_options=(--no-submit)
elif [ -n "$submit_url" ]; then
    submit_options=(--submit "$submit_url")
fi
if [ "$dry_run" = 0 ] && [ "$no_submit" = 0 ]; then
    python3 -I "$(dirname "$0")/submit.py" --whoami ${url_options[@]+"${url_options[@]}"} || true
fi

[ "$dry_run" = 1 ] || mkdir -p "$out"
total=$(($(echo $variants | wc -w) * ${#LOADS[@]}))
number=0
variant_list=($variants)

# A random order of the arguments, in the array `shuffled`. The seed is told, and
# can be given, so that an order of a matrix can be repeated.
shuffle() {
    shuffled=("$@")
    local i j swap
    for ((i = ${#shuffled[@]} - 1; i > 0; i--)); do
        j=$((RANDOM % (i + 1)))
        swap=${shuffled[i]}
        shuffled[i]=${shuffled[j]}
        shuffled[j]=$swap
    done
}
for load in "${LOADS[@]}"; do
    IFS=: read -r name mode bs depth numjobs <<< "$load"
    # Every load has an order of the variants of its own.
    shuffle "${variant_list[@]}"
    order=("${shuffled[@]}")
    for variant in "${order[@]}"; do
        number=$((number + 1))
        run_name="${name}_${variant}"
        if [ -s "$out/$run_name.json" ]; then
            echo "== [$number/$total] $run_name: done already"
            continue
        fi
        command=(bench/measure.sh --no-build --name "$run_name" --out "$out"
            --mode "$mode" --bs "$bs" --depth "$depth" --numjobs "$numjobs"
            $(variant_options "$variant") ${runtime_default[@]+"${runtime_default[@]}"} ${ramp_default[@]+"${ramp_default[@]}"}
            ${submit_options[@]+"${submit_options[@]}"} ${passed[@]+"${passed[@]}"})
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
    bench/report.py "$out" --ratio | tee "$out/table.md"
    # The chart is alibc against io_uring: the threads of alibc are what io_uring
    # replaces, so it is the pair that tells what the ring gives.
    bench/report.py "$out" --baseline alibc --against uring --plot "$out/alibc-vs-uring.svg" > /dev/null
fi
