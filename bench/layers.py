#!/usr/bin/env python3
"""Says where a process spends its cycles, from a profile of perf.

    bench/layers.py perf.data --process wheelie-server

bench/measure.sh --profile records the server and the client of a run with
`perf record`, and this script turns each recording into the share of the
cycles that every layer of the path of a request has:

  network     the TCP/IP stack, netfilter, the bridge and the layer of sockets
  copies      the copies of data between the memory of the process and the kernel
  storage     the file system, the block layer, the driver of the disk
  io_uring    the ring itself
  scheduler   the scheduler, waits and locks
  code        the code of the program and of libc
  other       the rest of the kernel: system calls, memory, interrupts, timers

A layer is found by the name of the symbol and by the object it is in, so the
shares are an estimate, good for comparing two runs on one machine.
"""

import argparse
import json
import os
import re
import subprocess
import sys

LAYERS = ("network", "copies", "storage", "io_uring", "scheduler", "code", "other")

_SYMBOLS = [
    ("copies", r"^(rep_movs_alternative|_copy_(to|from)_iter|copy_user|__copy_msghdr|copy_page|csum_partial|"
               r"copy_from_user|copy_to_user|copy_mc|memcpy_orig|__memcpy)"),
    ("network", r"^(tcp_|__tcp|ip_|__ip|ipv4|inet_|__inet|nf_|__nf|nft_|br_|veth|__netif|netif_|__dev_queue|"
                r"dev_queue|dev_hard|__dev_|process_backlog|net_rx|net_tx|__napi|napi_|neigh_|eth_|fib_|"
                r"__netdev|raw_|skb_|__skb|kfree_skb|consume_skb|__alloc_skb|build_skb|sk_|__sk_|sock_|__sock|"
                r"unix_|__unix|__sys_send|__sys_recv|sys_send|sys_recv|__scm|scm_)"),
    ("io_uring", r"^(io_|__io_|uring_|wio_|io_uring)"),
    ("storage", r"^(virtqueue_|virtio|scsi_|__dd_|dd_|iowrite|ioread|blk_|__blk|bio_|__bio|nvme|iomap_|"
                r"__iomap|submit_bio|dma_|__dma|iommu_|__iommu|__map_single|__unmap|pin_user|gup_|__gup|"
                r"unpin_user|ext4_|__ext4|direct_|shmem_|__shmem|xas_|filemap_|__filemap|kernel_init_pages|"
                r"clear_page|find_get|folio_|__folio|generic_file|vfs_|iov_iter_extract|pt_iommu)"),
    ("scheduler", r"^(schedule|__schedule|finish_task_switch|try_to_wake|ttwu|wake_|__wake|"
                  r"native_queued_spin_lock|_raw_spin|_raw_read|_raw_write|queued_|mutex_|__mutex|osq_|rcu_|"
                  r"__rcu|enqueue_|dequeue_|pick_next|update_|select_task|idle|cpuidle|default_idle|do_idle|"
                  r"intel_idle|mwait|native_safe_halt|psi_|sched_|switch_to|__switch|migrate_|resched|__lock|"
                  r"irq_|asm_sysvec|sysvec|handle_irq|smp_|llist|task_work|__x2apic)"),
]
_SYMBOL_RES = [(layer, re.compile(pattern)) for layer, pattern in _SYMBOLS]
_USER_DSO = re.compile(r"^(wheelie|libwheel|fio|libc|libstdc|ld-|libgcc|libm)")

_LINE = re.compile(r"^\s+([\d.]+)%\s+(\S+)\s+\[(.)\]\s+(.*?)\s*$")


def classify(dso, symbol):
    """The layer of a symbol that is in an object."""
    base = os.path.basename(dso)
    if _USER_DSO.match(base):
        return "code"
    for layer, pattern in _SYMBOL_RES:
        if pattern.match(symbol):
            return layer
    return "other"


def parse_report(text):
    """The shares of the symbols out of `perf report --stdio --sort dso,sym`: [(percent, dso, symbol)]."""
    found = []
    for line in text.splitlines():
        match = _LINE.match(line)
        if match:
            found.append((float(match.group(1)), match.group(2), match.group(4)))
    return found


def parse_header(text):
    """The number of cycles and of samples that perf wrote in the head of the report."""
    cycles = samples = 0
    for line in text.splitlines():
        found = re.match(r"# Event count \(approx\.\): (\d+)", line)
        if found:
            cycles = int(found.group(1))
        found = re.match(r"# Samples: ([\d.]+)([KMG]?)", line)
        if found:
            samples = int(float(found.group(1)) * {"": 1, "K": 10**3, "M": 10**6, "G": 10**9}[found.group(2)])
    return cycles, samples


def layers_of(rows):
    """The shares of the layers, in percent, adding up to 100, and the heaviest symbols."""
    total = sum(percent for percent, _, _ in rows) or 1.0
    shares = {name: 0.0 for name in LAYERS}
    for percent, dso, symbol in rows:
        shares[classify(dso, symbol)] += 100.0 * percent / total
    heaviest = [
        {"symbol": symbol[:200], "dso": os.path.basename(dso)[:100], "percent": round(percent, 3)}
        for percent, dso, symbol in sorted(rows, reverse=True)[:30]
    ]
    return {name: round(share, 2) for name, share in shares.items()}, heaviest


def profiled_process(text, process):
    rows = parse_report(text)
    if not rows:
        raise ValueError("no symbols in the report of %s" % process)
    cycles, samples = parse_header(text)
    shares, heaviest = layers_of(rows)
    return {"process": process, "cycles": cycles, "samples": samples, "layers": shares, "top": heaviest}


def perf_report(path):
    out = subprocess.run(
        ["perf", "report", "-i", path, "--no-children", "--stdio", "-g", "none", "--sort", "dso,sym"],
        capture_output=True, text=True, check=False)
    if out.returncode != 0:
        raise ValueError("perf report failed: %s" % out.stderr.strip()[:200])
    return out.stdout


def requests_per_second(fio):
    job = fio["jobs"][0]
    stats = job["read"] if job["read"]["io_bytes"] else job["write"]
    return stats["iops"]


def build(window_s, fio_path, server=None, client=None):
    """The profile of a run, as the server of results takes it."""
    profile = {"tool": "perf", "window_s": window_s}
    for key, (path, process) in (("server", (server, "wheelie-server")), ("client", (client, "fio"))):
        if path and os.path.exists(path):
            profile[key] = profiled_process(perf_report(path), process)
    if "server" not in profile and "client" not in profile:
        raise ValueError("nothing was recorded")
    if "server" in profile and profile["server"]["cycles"]:
        with open(fio_path) as handle:
            text = handle.read()
        iops = requests_per_second(json.loads(text[text.index("{"):]))
        if iops > 0:
            profile["server_cycles_per_request"] = round(profile["server"]["cycles"] / (iops * window_s), 1)
    return profile


class RussianFormatter(argparse.HelpFormatter):
    def add_usage(self, usage, actions, groups, prefix=None):
        super().add_usage(usage, actions, groups, "Использование: " if prefix is None else prefix)


def russian(parser):
    """The help of a parser in Russian: argparse has no Russian of its own here."""
    parser.add_argument("-h", "--help", action="help", help="показать эту справку и выйти")
    parser._positionals.title = "аргументы"
    parser._optionals.title = "параметры"
    return parser


def main(argv=None):
    parser = argparse.ArgumentParser(description="Куда уходят циклы процесса.",
                                     formatter_class=RussianFormatter, add_help=False)
    sub = parser.add_subparsers(dest="command", required=True, title="команды")

    one = sub.add_parser("layers", help="доли слоёв одной записи perf",
                         formatter_class=RussianFormatter, add_help=False)
    one.add_argument("data")
    one.add_argument("--process", default="process")
    russian(one)

    both = sub.add_parser("build", help="профиль прогона, для bench/submit.py",
                          formatter_class=RussianFormatter, add_help=False)
    both.add_argument("--server")
    both.add_argument("--client")
    both.add_argument("--window", type=float, required=True, help="сколько секунд записано")
    both.add_argument("--fio", required=True, help="JSON fio этого прогона")
    both.add_argument("--out", required=True)
    russian(both)
    russian(parser)

    args = parser.parse_args(argv)
    try:
        if args.command == "layers":
            result = profiled_process(perf_report(args.data), args.process)
            for name in LAYERS:
                print("%-10s %5.1f%%" % (name, result["layers"][name]))
            return 0
        profile = build(args.window, args.fio, args.server, args.client)
    except (ValueError, OSError, KeyError) as error:
        print("bench/layers.py: %s" % error, file=sys.stderr)
        return 1
    with open(args.out, "w") as handle:
        json.dump(profile, handle, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
