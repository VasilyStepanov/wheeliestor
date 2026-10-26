#!/bin/bash
#
# Starts wheelie-server on a file in /data, made and filled the first time:
# a file that is not written to has no blocks, and reading it would not read a
# disk. A block device is used as it is, and is not filled. What the server is
# started with comes from the environment:
#
#   WHEELIE_FILE           the file or block device to serve (default:
#                          /data/disk.img)
#   WHEELIE_BACKEND        libc, alibc or uring (default: uring)
#   WHEELIE_MULTISHOT      1 for multishot receive (default: 0)
#   WHEELIE_THREADS        threads, each with a reactor (default: the CPUs)
#   WHEELIE_QUEUE_DEPTH    operations a reactor may have in flight
#   WHEELIE_DIRECT         1 to open the file with O_DIRECT (default: 1)
#   WHEELIE_FILE_SIZE_MB   size of the file (default: 2048)

set -eu

file=${WHEELIE_FILE:-/data/disk.img}
size_mb=${WHEELIE_FILE_SIZE_MB:-2048}

if [ ! -b "$file" ] && [ "$(stat -c %s "$file" 2> /dev/null || echo 0)" -ne $((size_mb * 1024 * 1024)) ]; then
    echo "wheelie-server: filling $file with $size_mb MiB" >&2
    dd if=/dev/zero of="$file" bs=1M count="$size_mb" conv=fsync status=none
fi

args=(--bind 0.0.0.0:39900 --file "$file"
      --backend "${WHEELIE_BACKEND:-uring}"
      --direct "${WHEELIE_DIRECT:-1}")
[ "${WHEELIE_MULTISHOT:-0}" = 1 ] && args+=(--multishot)
[ -n "${WHEELIE_THREADS:-}" ] && args+=(--threads "$WHEELIE_THREADS")
[ -n "${WHEELIE_QUEUE_DEPTH:-}" ] && args+=(--queue-depth "$WHEELIE_QUEUE_DEPTH")

exec wheelie-server "${args[@]}"
