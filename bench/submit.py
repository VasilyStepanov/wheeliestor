#!/usr/bin/env python3
"""Sends the results of bench/run.sh to a server of results.

    bench/submit.py --url http://results.example:8080 --code THE-CODE results/matrix

What is sent for a result, from the files that bench/run.sh keeps next to it:

  * the JSON of fio, whole, and its plain output (`<name>.txt`), which is what a
    person reads;
  * the parameters of the run (the options fio recorded, the settings of the
    stand, the line the server printed on start);
  * the parameters of the machine: the kernel, the processor, the memory, the
    disks, the network cards, the virtualization, docker, and the profile of the
    frequency of the processor (the governor of cpufreq, the driver, the
    preference of energy, the boost, the range): is it set to save energy or to
    perform;
  * the profile, if there is a `<name>.profile.json` (bench/run.sh --profile),
    that is where the time goes.

The machine is told apart by a client id: a random number that this script
makes the first time and keeps in ~/.local/state/wheeliestor/client-id. The
name of the host does not leave the machine, only a hash of it. The server
answers with a name that is made of the id, and the script prints it with a
link, so that the results can be found in the server.

To know the name before anything is sent, ask for it:

    bench/submit.py --whoami --url http://results.example:8080

The name and the link of the results are printed at the start of every submit
too, and bench/run.sh --submit and bench/matrix.sh --submit print them before
the first run, so that you can look for yourself on the page while it goes.

The server takes a result with a token. The script gets it for a code and keeps
nothing: the token lives for a couple of hours. The code is in this file, in
PUBLIC_CODE, and it is no secret: it tells the server which version of the
script it talks to. When the owner of the server changes the code there, the
scripts that have the old one are refused, and have to be updated. The server
slows down the clients that submit too fast, and the script waits as long as it
is told to.

With --dry-run nothing is sent: the script says what it would send.

Environment:
  WHEELIE_RESULTS_URL    the address of the server (instead of --url)
  WHEELIE_RESULTS_CODE   another code than PUBLIC_CODE (instead of --code)
  WHEELIE_RESULTS_TOKEN  a token, if you have one (instead of --code)
  DOCKER                 the docker command, as for bench/run.sh
"""

import argparse
import datetime
import glob
import hashlib
import json
import os
import platform
import re
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

SCHEMA = 1

# The server takes the plain output of fio up to this many characters.
MAX_TEXT = 100_000

# The code that the server of results is asked for a token with. It is public
# and gives nothing but the right to submit; the server can stop accepting it,
# and then this version of the script is refused.
PUBLIC_CODE = "wr1-537aa06f3d3cb5d1cb99"
LAYER_NAMES = ("network", "copies", "storage", "io_uring", "scheduler", "code", "other")

# The options of fio that are the parameters of the run itself; the rest of
# the options fio recorded go to the server as they are, for the record.
KNOWN_OPTIONS = {
    "name", "ioengine", "filename", "wheelie_backend", "wheelie_multishot", "wheelie_queues", "rw", "bs",
    "iodepth", "numjobs", "sync", "size", "runtime", "ramp_time", "time_based", "thread", "group_reporting",
}


# ---------------------------------------------------------------- the machine

def run_text(command, env=None):
    """The output of a command, or None if it cannot be run or fails."""
    try:
        out = subprocess.run(command, capture_output=True, text=True, timeout=20, env=env)
    except (OSError, subprocess.SubprocessError):
        return None
    return out.stdout.strip() if out.returncode == 0 else None


def read_text(path):
    try:
        with open(path) as handle:
            return handle.read().strip()
    except OSError:
        return None


def state_dir():
    base = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(base, "wheeliestor")


def client_id(directory=None):
    """The random id of this client: made once, and kept."""
    directory = directory or state_dir()
    path = os.path.join(directory, "client-id")
    known = read_text(path)
    if known and re.fullmatch(r"[0-9a-f]{32}", known):
        return known
    value = uuid.uuid4().hex
    os.makedirs(directory, exist_ok=True)
    with open(path, "w") as handle:
        handle.write(value + "\n")
    return value


def host_hash():
    return hashlib.sha256(platform.node().encode()).hexdigest()[:16]


def parse_lscpu(text):
    found = {}
    for line in (text or "").splitlines():
        key, _, value = line.partition(":")
        found[key.strip()] = value.strip()
    return found


def cpu_info():
    info = parse_lscpu(run_text(["lscpu"], env=dict(os.environ, LC_ALL="C")))
    model = info.get("Model name")
    if not model:
        for line in (read_text("/proc/cpuinfo") or "").splitlines():
            if line.lower().startswith("model name"):
                model = line.partition(":")[2].strip()
                break
    if not model:
        model = run_text(["sysctl", "-n", "machdep.cpu.brand_string"]) or platform.processor() or "unknown"

    def number(key):
        value = info.get(key, "")
        return int(value) if value.isdigit() else None

    return {
        "cpu_model": model[:128],
        "cpus": os.cpu_count() or 1,
        "threads_per_core": number("Thread(s) per core"),
        "sockets": number("Socket(s)"),
        "numa_nodes": number("NUMA node(s)"),
    }


def memory_mb():
    for line in (read_text("/proc/meminfo") or "").splitlines():
        if line.startswith("MemTotal:"):
            return int(line.split()[1]) // 1024
    value = run_text(["sysctl", "-n", "hw.memsize"])
    return int(value) // (1024 * 1024) if value and value.isdigit() else 64


def os_name():
    for line in (read_text("/etc/os-release") or "").splitlines():
        if line.startswith("PRETTY_NAME="):
            return line.partition("=")[2].strip().strip('"')[:128]
    return platform.platform()[:128]


def disks():
    text = run_text(["lsblk", "-J", "-b", "-d", "-o", "NAME,SIZE,TYPE,MODEL,ROTA"])
    found = []
    try:
        devices = json.loads(text)["blockdevices"] if text else []
    except (ValueError, KeyError):
        devices = []
    for device in devices:
        if device.get("type") != "disk" or device["name"].startswith(("loop", "ram", "zram")):
            continue
        name = device["name"]
        size = device.get("size")
        rotational = device.get("rota")
        found.append({
            "name": name[:64],
            "size_bytes": int(size) if size is not None else None,
            "model": (device.get("model") or "").strip()[:128] or None,
            "rotational": bool(int(rotational)) if rotational is not None else None,
            "scheduler": (read_text("/sys/block/%s/queue/scheduler" % name) or "").split("[")[-1].split("]")[0][:64] or None,
        })
    return found[:64]


def nics():
    found = []
    for path in sorted(glob.glob("/sys/class/net/*")):
        name = os.path.basename(path)
        if name == "lo" or name.startswith(("veth", "br-", "docker")):
            continue
        speed = read_text(path + "/speed")
        driver = os.path.basename(os.path.realpath(path + "/device/driver")) if os.path.exists(path + "/device/driver") else None
        found.append({
            "name": name[:64],
            "speed_mbps": int(speed) if speed and speed.lstrip("-").isdigit() and int(speed) > 0 else None,
            "driver": driver[:64] if driver else None,
        })
    return found[:64]


def cpu_power():
    """The profile of the frequency of the processor, from cpufreq. A virtual machine has none."""
    base = "/sys/devices/system/cpu"
    cores = sorted(glob.glob(base + "/cpu[0-9]*"))

    def all_of(name):
        return {read_text("%s/cpufreq/%s" % (core, name)) for core in cores} - {None}

    found = {}
    governors = all_of("scaling_governor")
    if governors:
        found["governor"] = governors.pop() if len(governors) == 1 else "mixed"
    first = cores[0] if cores else None
    if first:
        driver = read_text(first + "/cpufreq/scaling_driver")
        if driver:
            found["cpu_driver"] = driver
        preferences = all_of("energy_performance_preference")
        if preferences:
            found["energy_preference"] = preferences.pop() if len(preferences) == 1 else "mixed"
        for key, name in (("freq_min_mhz", "cpuinfo_min_freq"), ("freq_max_mhz", "cpuinfo_max_freq")):
            khz = read_text("%s/cpufreq/%s" % (first, name))
            if khz and khz.isdigit() and int(khz) > 0:
                found[key] = int(khz) // 1000
    # The boost: a switch of the driver, or the opposite of the switch of intel_pstate.
    boost = read_text(base + "/cpufreq/boost")
    if boost in ("0", "1"):
        found["boost"] = boost == "1"
    else:
        no_turbo = read_text(base + "/intel_pstate/no_turbo")
        if no_turbo in ("0", "1"):
            found["boost"] = no_turbo == "0"
    return found


def docker_version():
    command = shlex.split(os.environ.get("DOCKER", "docker")) + ["version", "--format", "{{.Server.Version}}"]
    return (run_text(command) or "")[:64] or None


def collect_machine(directory=None):
    machine = {
        "client_id": client_id(directory),
        "host_hash": host_hash(),
        "os": os_name(),
        "kernel": platform.release()[:128],
        "arch": platform.machine()[:32],
        "memory_mb": memory_mb(),
        "virt": (run_text(["systemd-detect-virt"]) or None),
        "docker": docker_version(),
        "disks": disks(),
        "nics": nics(),
    }
    machine.update(cpu_info())
    machine.update(cpu_power())
    return {key: value for key, value in machine.items() if value not in (None, "")}


# ------------------------------------------------------------------ a result

def read_json(path):
    """The JSON of fio, which may have a warning of fio in front of it."""
    with open(path) as handle:
        text = handle.read()
    return json.loads(text[text.index("{"):])


def read_settings(path):
    found = {}
    for line in (read_text(path) or "").splitlines():
        key, sep, value = line.partition("=")
        if sep:
            found[key.strip()] = value.strip()
    return found


def iso(seconds):
    return datetime.datetime.fromtimestamp(seconds, datetime.timezone.utc).isoformat()


def build_submission(json_path, machine):
    """A submission from `<name>.json` and the files next to it. Raises ValueError."""
    base = json_path[: -len(".json")]
    fio = read_json(json_path)
    jobs = fio.get("jobs") or []
    if not jobs:
        raise ValueError("no jobs in the JSON of fio")
    job = jobs[0]
    if job.get("error", 0):
        raise ValueError("the job of fio failed: error %s" % job["error"])
    options = job.get("job options", {})
    if "wheelie_backend" not in options:
        raise ValueError("not a result of the wheelie engine")
    settings = read_settings(base + ".settings")
    banner = read_text(base + ".server")
    plain = read_text(base + ".txt")

    threads = None
    found = re.search(r"(\d+) threads", banner or "")
    if found:
        threads = int(found.group(1))
    elif settings.get("WHEELIE_THREADS", "").isdigit():
        threads = int(settings["WHEELIE_THREADS"])

    run = {
        "name": options.get("name", os.path.basename(base))[:200],
        "mode": options["rw"],
        "bs": options["bs"],
        "depth": int(options["iodepth"]),
        "numjobs": int(options.get("numjobs", 1)),
        "queues": int(options.get("wheelie_queues", 1)),
        "sync": int(options.get("sync", 0) or 0),
        "runtime": int(options["runtime"]),
        "ramp": int(options.get("ramp_time", 0) or 0),
        "size": options["size"],
        "backend": options["wheelie_backend"],
        "multishot": options.get("wheelie_multishot", "0") not in ("0", ""),
        "direct": settings.get("WHEELIE_DIRECT", "1") != "0",
        "data": settings.get("WHEELIE_DATA") or "disk",
        "transport": settings.get("WHEELIE_TRANSPORT") or "tcp",
        "options": {k: str(v)[:256] for k, v in options.items() if k not in KNOWN_OPTIONS and v != ""},
    }
    for key, name in (("server_cpus", "WHEELIE_SERVER_CPUS"), ("client_cpus", "WHEELIE_CLIENT_CPUS")):
        if settings.get(name):
            run[key] = settings[name]
    if threads:
        run["threads"] = threads

    stats = job["read"] if job["read"].get("io_bytes") else job["write"]
    ended = fio.get("timestamp") or time.time()
    started = ended - (stats.get("runtime", 0) / 1000.0)

    body = {
        "schema": SCHEMA,
        "started_at": iso(started),
        "run": run,
        "fio": fio,
        "machine": machine,
        "tool": {"name": "wheeliestor", "version": git_version()},
    }
    if banner:
        body["server"] = banner[:500]
    if plain and len(plain) <= MAX_TEXT:
        body["fio_text"] = plain
    profile = base + ".profile.json"
    if os.path.exists(profile):
        body["profile"] = read_json(profile)
    return body


def git_version():
    here = os.path.dirname(os.path.abspath(__file__))
    return (run_text(["git", "-C", here, "rev-parse", "--short=12", "HEAD"]) or None)


def find_results(paths):
    """The `.json` files of results under the paths, in the order of their time."""
    found = []
    for path in paths:
        if os.path.isdir(path):
            found += glob.glob(os.path.join(path, "**", "*.json"), recursive=True)
        else:
            found.append(path)
    found = [p for p in found if not p.endswith((".profile.json", "summary.json"))]
    return sorted(found)


# -------------------------------------------------------------------- server

class Refused(Exception):
    def __init__(self, status, message, problems=None):
        super().__init__(message)
        self.status = status
        self.problems = problems or []


class Server:
    def __init__(self, url, code=None, token=None, sleep=time.sleep):
        self.url = url.rstrip("/")
        self.code = code
        self.token = token
        self.sleep = sleep

    def call(self, method, path, body=None, auth=False):
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.url + path, data=data, method=method)
        request.add_header("Content-Type", "application/json")
        if auth and self.token:
            request.add_header("Authorization", "Bearer " + self.token)
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.loads(response.read() or b"{}")
        except urllib.error.HTTPError as error:
            raw = error.read()
            try:
                detail = json.loads(raw)
            except ValueError:
                detail = {"detail": raw.decode(errors="replace")[:200]}
            refused = Refused(error.code, str(detail.get("detail", error.reason)), detail.get("problems"))
            refused.retry_after = error.headers.get("Retry-After")
            raise refused from None

    def who(self, client):
        """The name that the server gives a client id, whether or not it has results."""
        return self.call("GET", "/api/clients/" + client)

    def login(self):
        if not self.code:
            raise Refused(401, "no token and no code")
        try:
            self.token = self.call("POST", "/api/token", {"code": self.code})["token"]
        except Refused as refused:
            if refused.status == 403:
                raise Refused(403, "the server does not accept this version of wheeliestor any more: update it") from None
            raise

    def submit(self, body, waits=20):
        """Sends one result. Waits when the server says to, logs in again when the token has expired."""
        relogged = False
        while True:
            if not self.token:
                self.login()
            try:
                return self.call("POST", "/api/results", body, auth=True)
            except Refused as refused:
                if refused.status == 401 and self.code and not relogged:
                    self.token, relogged = None, True
                    continue
                if refused.status == 429 and waits > 0:
                    waits -= 1
                    self.sleep(min(float(getattr(refused, "retry_after", None) or 2), 120))
                    continue
                raise


# ---------------------------------------------------------------------- main

def say_who(url, client):
    """Tells the name of a client and where its results are. Returns 0, or 1 if the server cannot be asked."""
    if not url:
        print("bench/submit.py: give --url or set WHEELIE_RESULTS_URL", file=sys.stderr)
        return 2
    try:
        who = Server(url).who(client)
    except (Refused, OSError) as error:
        print("== your client id is %s (the server does not answer: %s)" % (client, error))
        return 1
    print("== you are %s; your results: %s/?client=%s" % (who["name"], url.rstrip("/"), client))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Sends the results of bench/run.sh to a server of results.",
                                     epilog="See the head of this file for what is sent.")
    parser.add_argument("paths", nargs="*", help="directories of results or .json files of fio")
    parser.add_argument("--url", default=os.environ.get("WHEELIE_RESULTS_URL"), help="the address of the server")
    parser.add_argument("--code", default=os.environ.get("WHEELIE_RESULTS_CODE") or PUBLIC_CODE,
                        help="the code for a token (default: the public code of this script)")
    parser.add_argument("--token", default=os.environ.get("WHEELIE_RESULTS_TOKEN"), help="a token, instead of the code")
    parser.add_argument("--dry-run", action="store_true", help="say what would be sent and send nothing")
    parser.add_argument("--whoami", action="store_true", help="say the name of this client and the link of its results, and stop")
    parser.add_argument("--no-whoami", action="store_true", help="do not say the name at the start (the caller has said it)")
    args = parser.parse_args(argv)

    if args.whoami:
        return say_who(args.url, collect_machine()["client_id"])
    if not args.paths:
        parser.error("give the results to send, or --whoami")
    files = find_results(args.paths)
    if not files:
        print("bench/submit.py: no results in %s" % " ".join(args.paths), file=sys.stderr)
        return 2
    if not args.dry_run and not args.url:
        print("bench/submit.py: give --url or set WHEELIE_RESULTS_URL", file=sys.stderr)
        return 2

    machine = collect_machine()
    server = None if args.dry_run else Server(args.url, code=args.code, token=args.token)
    if not args.dry_run and not args.no_whoami:
        say_who(args.url, machine["client_id"])
    sent = stored = failed = 0
    identity = None
    for path in files:
        try:
            body = build_submission(path, machine)
        except (ValueError, KeyError, OSError) as error:
            print("skip  %s: %s" % (path, error))
            continue
        run = body["run"]
        label = "%s %s %s d%d j%d%s" % (run["name"], run["backend"] + ("-ms" if run["multishot"] else ""), run["mode"],
                                       run["depth"], run["numjobs"], " sync" if run["sync"] else "")
        if args.dry_run:
            print("would send  %s (%d bytes%s)" % (label, len(json.dumps(body)), ", with a profile" if "profile" in body else ""))
            sent += 1
            continue
        try:
            answer = server.submit(body)
        except Refused as refused:
            if refused.status == 409:
                stored += 1
                print("known %s: %s" % (label, refused))
                continue
            failed += 1
            print("FAIL  %s: %s (%s)" % (label, refused, refused.status))
            if refused.status == 403:
                # No result will be taken: the code is not accepted any more.
                break
            for problem in refused.problems:
                print("        - %s" % problem)
            continue
        except OSError as error:
            print("bench/submit.py: cannot reach %s: %s" % (args.url, error), file=sys.stderr)
            return 1
        sent += 1
        identity = answer.get("client", identity)
        print("sent  %s  %s/r/%s" % (label, args.url.rstrip("/"), answer["id"]))

    print("== %s: %d sent, %d stored already, %d refused" % ("dry run" if args.dry_run else "submit", sent, stored, failed))
    if identity:
        print("== you are %s; your results: %s/?client=%s" % (identity["name"], args.url.rstrip("/"), identity["id"]))
    elif args.dry_run:
        print("== your client id: %s" % machine["client_id"])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
