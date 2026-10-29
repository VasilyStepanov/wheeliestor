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
Использование: bench/matrix.sh [параметры] [параметры bench/run.sh]

  --out DIR         куда класть результаты (по умолчанию: results/matrix-ДАТА)
  --variants LIST   варианты, список в кавычках (по умолчанию: "alibc uring libc uring-ms")
  --seed N          зерно порядка вариантов (по умолчанию: время; оно
                    печатается, чтобы порядок можно было повторить)
  --dry-run         только сказать, что будет запущено
  --no-build        не собирать образы
  -h, --help        показать эту справку и выйти

Вариант: libc, alibc, uring или uring-ms (uring с multishot recv).
Всё, что не параметр этого скрипта, например --runtime, --ramp, --direct,
--sync, --threads или --size, передаётся в bench/run.sh для каждого прогона.
Прогон длится 15 секунд после разгона 5 (прогонов в матрице немного, чтобы
каждый мог быть долгим); --runtime и --ramp меняют это.

Нагрузки, каждая на каждом варианте:

  randread  4k, глубина 16, одно задание и 8 заданий; глубина 1, одно задание
  randwrite 4k, глубина 16, одно задание и 8 заданий; глубина 1, одно задание
  write     4k, глубина 1, одно задание (коммит журнала: одна небольшая запись
            за раз)
  read      4k, глубина 1, одно задание (то же, чтение)
  read      1m, глубина 4, 4 задания
  write     1m, глубина 4, 4 задания

Порядок сделан для графика результатов, который наполняется как можно раньше
(bench/run.sh --submit отправляет каждый результат сразу после замера):
нагрузки, которые говорят больше всего, идут первыми, а первые два варианта
первой нагрузки это те, что график сравнивает по умолчанию, так что первая
точка появляется после двух прогонов. Варианты одной нагрузки идут подряд, в
случайном порядке, который для каждой нагрузки свой: первый прогон после тяжёлой
нагрузки немного медленнее, и что-то может прогреваться, так что первым не
должен быть всегда один и тот же вариант. Только в первой нагрузке первыми идут
alibc и uring, в случайном порядке между собой.

Прогон, у которого в каталоге уже есть результат, не запускается снова, так что
прерванная матрица продолжается с того места, где остановилась, если запустить
её снова с тем же --out.

Окружение:
  DOCKER            команда docker, как у bench/run.sh
USAGE
}

# name:mode:bs:depth:numjobs
LOADS=(
    randread_qd16_nj1:randread:4k:16:1
    randwrite_qd16_nj1:randwrite:4k:16:1
    randread_qd16_nj8:randread:4k:16:8
    randwrite_qd16_nj8:randwrite:4k:16:8
    seqwrite_4k_qd1_nj1:write:4k:1:1
    seqread_4k_qd1_nj1:read:4k:1:1
    randread_qd1_nj1:randread:4k:1:1
    randwrite_qd1_nj1:randwrite:4k:1:1
    seqread_1m_nj4:read:1m:4:4
    seqwrite_1m_nj4:write:1m:4:4
)

out="" variants="alibc uring libc uring-ms" dry_run=0 build=1 seed=""
passed=()
# A run is long unless the caller says otherwise.
runtime_default=(--runtime 15) ramp_default=(--ramp 5)

while [ $# -gt 0 ]; do
    case $1 in
    --out) out=$2; shift 2 ;;
    --variants) variants=$2; shift 2 ;;
    --seed) seed=$2; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
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
if [ "$dry_run" = 0 ]; then
    previous=""
    for argument in ${passed[@]+"${passed[@]}"}; do
        if [ "$previous" = --submit ]; then
            python3 -I "$(dirname "$0")/submit.py" --whoami --url "$argument" || true
        fi
        previous=$argument
    done
fi

[ "$dry_run" = 1 ] || mkdir -p "$out"
total=$(($(echo $variants | wc -w) * ${#LOADS[@]}))
number=0
load_number=0
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
    # Every load has an order of the variants of its own. In the first one the two
    # of the default chart come first, so that a chart has its first point early.
    if [ "$load_number" = 0 ]; then
        first=() others=()
        for variant in "${variant_list[@]}"; do
            case $variant in
            alibc | uring) first+=("$variant") ;;
            *) others+=("$variant") ;;
            esac
        done
        shuffle ${first[@]+"${first[@]}"}
        order=(${shuffled[@]+"${shuffled[@]}"})
        shuffle ${others[@]+"${others[@]}"}
        order+=(${shuffled[@]+"${shuffled[@]}"})
    else
        shuffle "${variant_list[@]}"
        order=("${shuffled[@]}")
    fi
    load_number=$((load_number + 1))
    for variant in "${order[@]}"; do
        number=$((number + 1))
        run_name="${name}_${variant}"
        if [ -s "$out/$run_name.json" ]; then
            echo "== [$number/$total] $run_name: done already"
            continue
        fi
        command=(bench/run.sh --no-build --name "$run_name" --out "$out"
            --mode "$mode" --bs "$bs" --depth "$depth" --numjobs "$numjobs"
            $(variant_options "$variant") ${runtime_default[@]+"${runtime_default[@]}"} ${ramp_default[@]+"${ramp_default[@]}"}
            ${passed[@]+"${passed[@]}"})
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
