#!/bin/bash
#
# Runs fio with the wheelie engine against a wheelie server, for the clients of
# every backend with and without multishot receive, writing with a checksum
# and reading it back: what comes back from the server has to be what was
# written.
#
# Environment:
#   WHEELIE_FIO_ENGINE  the engine, wheelie.so
#   WHEELIE_SERVER      the wheelie-server binary
#   FIO                 fio, which has to be of the version the engine was
#                       built for (default: fio)
#   FIO_VERSION         that version, e.g. fio-3.41
#   WHEELIE_FIO_PRELOAD   a library to preload into fio, the runtime of the
#                         sanitizer if the engine is built with one
#   WHEELIE_FIO_REQUIRED  if set, a test that cannot run is a failure, not a
#                         skip

set -u

FIO=${FIO:-fio}

skip() {
    echo "SKIP: $*"
    if [ -n "${WHEELIE_FIO_REQUIRED:-}" ]; then
        exit 1
    fi
    exit 0
}

command -v "$FIO" > /dev/null 2>&1 || skip "no fio ($FIO)"
[ -f "${WHEELIE_FIO_ENGINE:-}" ] || skip "no engine (WHEELIE_FIO_ENGINE=${WHEELIE_FIO_ENGINE:-})"
[ -x "${WHEELIE_SERVER:-}" ] || skip "no server (WHEELIE_SERVER=${WHEELIE_SERVER:-})"

have=$("$FIO" --version 2> /dev/null)
if [ -n "${FIO_VERSION:-}" ] && [ "$have" != "$FIO_VERSION" ]; then
    skip "the engine is built for $FIO_VERSION, and $FIO is $have"
fi

# fio leaves files in the directory it runs in: it runs in one of its own, and
# the paths that are given relative are made absolute first.
FIO=$(command -v "$FIO")
WHEELIE_FIO_ENGINE=$(realpath "$WHEELIE_FIO_ENGINE")
WHEELIE_SERVER=$(realpath "$WHEELIE_SERVER")
workdir=$(mktemp -d)
cd "$workdir" || exit 1
server_pid=""
trap 'stop_server; rm -rf "$workdir"' EXIT

stop_server() {
    if [ -n "$server_pid" ]; then
        kill -TERM "$server_pid" 2> /dev/null
        wait "$server_pid" 2> /dev/null
        server_pid=""
    fi
}

free_port() {
    python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

# Starts a server; true if it is listening.
start_server() {
    local backend=$1 multishot=$2
    port=$(free_port)
    location="wheelie://127.0.0.1:$port"
    : > "$workdir/disk"
    truncate -s 64M "$workdir/disk"
    "$WHEELIE_SERVER" --bind "127.0.0.1:$port" --file "$workdir/disk" \
        --backend "$backend" ${multishot:+--multishot} --threads 2 --direct 0 \
        2> "$workdir/server.log" &
    server_pid=$!
    for _ in $(seq 1 50); do
        grep -q listening "$workdir/server.log" 2> /dev/null && return 0
        kill -0 "$server_pid" 2> /dev/null || break
        sleep 0.1
    done
    server_pid=""
    return 1
}

failed=0
ran=0

# One fio job; $1 is a description, the rest are options of the job.
job() {
    local what=$1
    shift
    if ! env ${WHEELIE_FIO_PRELOAD:+LD_PRELOAD="$WHEELIE_FIO_PRELOAD" ASAN_OPTIONS=detect_leaks=0} \
        "$FIO" --name=wheelie --ioengine="external:$WHEELIE_FIO_ENGINE" \
        --filename="${location//:/\\:}" \
        --wheelie_backend="$client_backend" \
        --wheelie_multishot="$client_multishot" \
        --thread --group_reporting --verify_fatal=1 --verify_state_save=0 "$@" \
        > "$workdir/fio.log" 2>&1; then
        echo "FAIL: $combo: $what"
        # The start of the log says what went wrong, the end where.
        if [ "$(wc -l < "$workdir/fio.log")" -gt 40 ]; then
            { head -15 "$workdir/fio.log"; echo "    ..."; tail -15 "$workdir/fio.log"; } | sed 's/^/    /'
        else
            sed 's/^/    /' "$workdir/fio.log"
        fi
        failed=$((failed + 1))
        return
    fi
    ran=$((ran + 1))
}

run_combo() {
    local server_backend=$1 server_multishot=$2
    client_backend=$3
    client_multishot=$4
    combo="client $client_backend/multishot=$client_multishot, server $server_backend${server_multishot:+/multishot}"

    if ! start_server "$server_backend" "$server_multishot"; then
        echo "skip $combo: the server does not start ($(tail -1 "$workdir/server.log"))"
        return
    fi

    job "write and verify" --size=8m --bs=4k --iodepth=16 --rw=write \
        --verify=crc32c --do_verify=1
    job "random read" --size=8m --bs=4k --iodepth=16 --rw=randread \
        --verify=crc32c --do_verify=0
    job "sync write and verify" --size=1m --bs=4k --iodepth=8 --rw=write \
        --sync=1 --verify=crc32c --do_verify=1
    job "large blocks" --size=16m --bs=1m --iodepth=4 --rw=write \
        --verify=crc32c --do_verify=1
    job "two clients" --size=4m --bs=4k --iodepth=16 --rw=write \
        --numjobs=2 --offset_increment=4m --verify=crc32c --do_verify=1
    job "queue depth 1" --size=1m --bs=4k --iodepth=1 --rw=write \
        --verify=crc32c --do_verify=1
    job "four connections" --size=8m --bs=4k --iodepth=16 --rw=write \
        --wheelie_queues=4 --verify=crc32c --do_verify=1
    stop_server
}

for backend in libc alibc uring; do
    for multishot in 0 1; do
        run_combo libc "" "$backend" "$multishot"
    done
done
run_combo uring multishot uring 1
run_combo alibc "" alibc 0

echo "fio jobs run: $ran, failed: $failed"
[ "$failed" -eq 0 ] && [ "$ran" -gt 0 ]
