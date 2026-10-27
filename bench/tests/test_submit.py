import http.server
import json
import os
import sys
import tempfile
import threading
import unittest
import unittest.mock

BENCH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, BENCH)

import submit  # noqa: E402


def fio_json(rw="randwrite", bs="4k", depth=16, numjobs=8, backend="uring", multishot=False,
             iops=30000, sync=1, runtime=30):
    stats = {"io_bytes": 1 << 30, "iops": iops, "bw": iops * 4, "runtime": runtime * 1000,
             "iops_mean": iops, "iops_stddev": 10,
             "clat_ns": {"mean": 2.0e6, "percentile": {"50.000000": 1900000, "99.000000": 3400000}}}
    idle = {"io_bytes": 0, "iops": 0, "bw": 0, "runtime": 0}
    reads = rw.endswith("read")
    return {
        "fio version": "fio-3.41", "timestamp": 1793031689,
        "jobs": [{
            "error": 0,
            "job options": {
                "name": "case", "ioengine": "external:/usr/local/lib/wheelie.so", "filename": "x",
                "wheelie_backend": backend, "wheelie_multishot": "1" if multishot else "0",
                "rw": rw, "bs": bs, "iodepth": str(depth), "numjobs": str(numjobs), "wheelie_queues": "1",
                "sync": str(sync), "size": "1g", "runtime": str(runtime), "ramp_time": "5",
                "time_based": "", "thread": "", "group_reporting": "", "norandommap": "",
            },
            "read": stats if reads else idle,
            "write": idle if reads else stats,
        }],
    }


MACHINE = {"client_id": "a" * 32, "kernel": "7.0", "cpu_model": "cpu", "cpus": 8, "memory_mb": 1024}


def write_result(directory, name="case", settings=True, banner=True, profile=None, plain=None, **kw):
    path = os.path.join(directory, name + ".json")
    with open(path, "w") as handle:
        handle.write("fio: a warning in front of the JSON\n")
        json.dump(fio_json(**kw), handle)
    if settings:
        with open(os.path.join(directory, name + ".settings"), "w") as handle:
            handle.write("WHEELIE_BACKEND=uring\nWHEELIE_DIRECT=1\nWHEELIE_DATA=tmpfs\nWHEELIE_THREADS=\n"
                         "WHEELIE_SERVER_CPUS=0-3\nWHEELIE_CLIENT_CPUS=\n")
    if banner:
        with open(os.path.join(directory, name + ".server"), "w") as handle:
            handle.write("listening on 0.0.0.0:39900 (uring multishot, 4 threads, queue depth 1024, O_DIRECT)\n")
    if plain is not None:
        with open(os.path.join(directory, name + ".txt"), "w") as handle:
            handle.write(plain)
    if profile:
        with open(os.path.join(directory, name + ".profile.json"), "w") as handle:
            json.dump(profile, handle)
    return path


class BuildTest(unittest.TestCase):
    def test_the_run_comes_from_fio_and_the_files_next_to_it(self):
        with tempfile.TemporaryDirectory() as d:
            body = submit.build_submission(write_result(d, multishot=True), MACHINE)
        run = body["run"]
        self.assertEqual(body["schema"], 1)
        self.assertEqual((run["mode"], run["bs"], run["depth"], run["numjobs"], run["sync"]),
                         ("randwrite", "4k", 16, 8, 1))
        self.assertEqual((run["backend"], run["multishot"], run["direct"], run["data"]), ("uring", True, True, "tmpfs"))
        self.assertEqual(run["threads"], 4)  # from the line of the server
        self.assertEqual(run["server_cpus"], "0-3")
        self.assertNotIn("client_cpus", run)
        # What is not a parameter of the run goes to the options, if it has a value.
        self.assertEqual(run["options"], {})
        self.assertEqual(body["fio"]["fio version"], "fio-3.41")
        self.assertIn("listening on", body["server"])
        self.assertNotIn("profile", body)
        # The run started its run time before fio wrote the JSON.
        self.assertTrue(body["started_at"].startswith("2026-10-26T"))

    def test_without_the_files_next_to_it_the_defaults_are_taken(self):
        with tempfile.TemporaryDirectory() as d:
            body = submit.build_submission(write_result(d, settings=False, banner=False), MACHINE)
        self.assertEqual((body["run"]["direct"], body["run"]["data"], body["run"]["transport"]), (True, "disk", "tcp"))
        self.assertNotIn("server", body)

    def test_a_profile_next_to_the_result_goes_with_it(self):
        profile = {"tool": "perf", "window_s": 15}
        with tempfile.TemporaryDirectory() as d:
            body = submit.build_submission(write_result(d, profile=profile), MACHINE)
        self.assertEqual(body["profile"], profile)

    def test_the_plain_output_of_fio_goes_with_the_result(self):
        with tempfile.TemporaryDirectory() as d:
            body = submit.build_submission(write_result(d, plain="case: (g=0): rw=randwrite\n  write: IOPS=30.0k\n"), MACHINE)
            self.assertIn("IOPS=30.0k", body["fio_text"])
            self.assertNotIn("fio_text", submit.build_submission(write_result(d, "b"), MACHINE))
            too_long = write_result(d, "c", plain="x" * (submit.MAX_TEXT + 1))
            self.assertNotIn("fio_text", submit.build_submission(too_long, MACHINE))

    def test_what_is_not_a_result_of_the_stand_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "plain.json")
            data = fio_json()
            del data["jobs"][0]["job options"]["wheelie_backend"]
            with open(path, "w") as handle:
                json.dump(data, handle)
            with self.assertRaises(ValueError):
                submit.build_submission(path, MACHINE)

    def test_find_results_skips_profiles(self):
        with tempfile.TemporaryDirectory() as d:
            write_result(d, "a", profile={"x": 1})
            write_result(d, "b")
            found = [os.path.basename(p) for p in submit.find_results([d])]
        self.assertEqual(found, ["a.json", "b.json"])


class ClientIdTest(unittest.TestCase):
    def test_it_is_made_once_and_kept(self):
        with tempfile.TemporaryDirectory() as d:
            first = submit.client_id(d)
            self.assertRegex(first, r"^[0-9a-f]{32}$")
            self.assertEqual(submit.client_id(d), first)
            with open(os.path.join(d, "client-id"), "w") as handle:
                handle.write("not an id\n")
            self.assertNotEqual(submit.client_id(d), "not an id")

    def test_the_machine_does_not_tell_the_name_of_the_host(self):
        with tempfile.TemporaryDirectory() as d:
            machine = submit.collect_machine(d)
        import platform
        self.assertNotIn(platform.node(), json.dumps(machine) if platform.node() not in ("", "localhost") else "")
        self.assertRegex(machine["host_hash"], r"^[0-9a-f]{16}$")
        for key in ("client_id", "kernel", "cpu_model", "cpus", "memory_mb"):
            self.assertIn(key, machine)


class CpuPowerTest(unittest.TestCase):
    def fake_sys(self, root, files):
        for path, text in files.items():
            full = os.path.join(root, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w") as handle:
                handle.write(text + "\n")

    def read_with(self, files):
        with tempfile.TemporaryDirectory() as root:
            self.fake_sys(root, files)
            real_glob, real_read = submit.glob.glob, submit.read_text
            base = "/sys/devices/system/cpu"
            submit.glob.glob = lambda pattern: real_glob(pattern.replace(base, root + base))
            submit.read_text = lambda path: real_read(root + path if path.startswith(base) else path)
            try:
                return submit.cpu_power()
            finally:
                submit.glob.glob, submit.read_text = real_glob, real_read

    def test_a_machine_that_performs(self):
        cpu = "sys/devices/system/cpu"
        files = {}
        for n in (0, 1):
            files["/%s/cpu%d/cpufreq/scaling_governor" % (cpu, n)] = "performance"
            files["/%s/cpu%d/cpufreq/scaling_driver" % (cpu, n)] = "amd-pstate"
            files["/%s/cpu%d/cpufreq/cpuinfo_min_freq" % (cpu, n)] = "400000"
            files["/%s/cpu%d/cpufreq/cpuinfo_max_freq" % (cpu, n)] = "3400000"
        files["/%s/cpufreq/boost" % cpu] = "1"
        found = self.read_with({k[1:]: v for k, v in files.items()})
        self.assertEqual(found, {"governor": "performance", "cpu_driver": "amd-pstate", "freq_min_mhz": 400,
                                 "freq_max_mhz": 3400, "boost": True})

    def test_cores_that_differ_are_mixed_and_intel_boost_is_inverted(self):
        cpu = "sys/devices/system/cpu"
        found = self.read_with({
            cpu + "/cpu0/cpufreq/scaling_governor": "powersave",
            cpu + "/cpu1/cpufreq/scaling_governor": "performance",
            cpu + "/cpu0/cpufreq/energy_performance_preference": "balance_power",
            cpu + "/intel_pstate/no_turbo": "1",
        })
        self.assertEqual(found["governor"], "mixed")
        self.assertEqual(found["energy_preference"], "balance_power")
        self.assertIs(found["boost"], False)

    def test_a_virtual_machine_has_nothing(self):
        self.assertEqual(self.read_with({"sys/devices/system/cpu/cpu0/online": "1"}), {})


class FakeServer:
    """A server of results that answers as scripted: [(status, body, headers)] for each submit."""

    def __init__(self, script):
        self.script = list(script)
        self.requests = []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                outer.requests.append((self.path, self.headers.get("Authorization"), body))
                if self.path == "/api/token":
                    status, answer, headers = 200, {"token": "tok%d" % len(outer.requests)}, {}
                else:
                    status, answer, headers = outer.script.pop(0)
                data = json.dumps(answer).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                for key, value in headers.items():
                    self.send_header(key, value)
                self.end_headers()
                self.wfile.write(data)

        self.httpd = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.url = "http://127.0.0.1:%d" % self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.slept = []

    def server(self, script, **kw):
        fake = FakeServer(script)
        self.addCleanup(fake.close)
        return fake, submit.Server(fake.url, sleep=self.slept.append, **kw)

    def test_it_logs_in_with_the_code_and_submits_with_the_token(self):
        fake, server = self.server([(201, {"id": "r1"}, {})], code="the-code")
        self.assertEqual(server.submit({"a": 1})["id"], "r1")
        self.assertEqual(fake.requests[0][0:3:2], ("/api/token", {"code": "the-code"}))
        self.assertEqual(fake.requests[1][1], "Bearer tok1")

    def test_it_waits_as_long_as_it_is_told_to(self):
        fake, server = self.server([(429, {"detail": "slow"}, {"Retry-After": "7"}), (201, {"id": "r1"}, {})], code="c")
        self.assertEqual(server.submit({})["id"], "r1")
        self.assertEqual(self.slept, [7.0])

    def test_it_gives_up_when_it_is_always_told_to_wait(self):
        fake, server = self.server([(429, {"detail": "slow"}, {"Retry-After": "1"})] * 4, code="c")
        with self.assertRaises(submit.Refused) as raised:
            server.submit({}, waits=3)
        self.assertEqual(raised.exception.status, 429)
        self.assertEqual(len(self.slept), 3)

    def test_it_logs_in_again_when_the_token_has_expired(self):
        fake, server = self.server([(401, {"detail": "expired"}, {}), (201, {"id": "r2"}, {})], code="c")
        self.assertEqual(server.submit({})["id"], "r2")
        self.assertEqual([r[0] for r in fake.requests], ["/api/token", "/api/results", "/api/token", "/api/results"])

    def test_a_refusal_carries_the_problems(self):
        fake, server = self.server([(422, {"detail": "not consistent", "problems": ["bad bandwidth"]}, {})], code="c")
        with self.assertRaises(submit.Refused) as raised:
            server.submit({})
        self.assertEqual((raised.exception.status, raised.exception.problems), (422, ["bad bandwidth"]))

    def test_without_a_code_and_a_token_it_says_so(self):
        fake, server = self.server([])
        with self.assertRaises(submit.Refused):
            server.submit({})

    def test_a_code_that_the_server_does_not_know_means_an_old_version(self):
        fake = FakeServer([])
        self.addCleanup(fake.close)
        server = submit.Server(fake.url, code="old", sleep=self.slept.append)
        server.call = lambda *a, **k: (_ for _ in ()).throw(submit.Refused(403, "the code is not known"))
        with self.assertRaises(submit.Refused) as raised:
            server.submit({})
        self.assertEqual(raised.exception.status, 403)
        self.assertIn("update", str(raised.exception))

    def test_the_public_code_is_the_default(self):
        fake = FakeServer([(201, {"id": "r1", "client": {"id": "a" * 32, "name": "n"}}, {})])
        self.addCleanup(fake.close)
        env = dict(os.environ)
        env.pop("WHEELIE_RESULTS_CODE", None)
        with tempfile.TemporaryDirectory() as d, unittest.mock.patch.dict(os.environ, env, clear=True):
            write_result(d)
            self.assertEqual(submit.main(["--url", fake.url, d]), 0)
        self.assertEqual(fake.requests[0][2], {"code": submit.PUBLIC_CODE})


class MainTest(unittest.TestCase):
    def test_a_dry_run_sends_nothing_and_needs_no_server(self):
        with tempfile.TemporaryDirectory() as d:
            write_result(d)
            code = submit.main(["--dry-run", d])
        self.assertEqual(code, 0)

    def test_it_sends_the_results_of_a_directory(self):
        fake = FakeServer([(201, {"id": "r1", "client": {"id": "a" * 32, "name": "swift-otter-0001"}}, {}),
                           (409, {"detail": "stored already"}, {})])
        self.addCleanup(fake.close)
        with tempfile.TemporaryDirectory() as d:
            write_result(d, "a")
            write_result(d, "b")
            code = submit.main(["--url", fake.url, "--code", "c", d])
        self.assertEqual(code, 0)
        self.assertEqual(len([r for r in fake.requests if r[0] == "/api/results"]), 2)

    def test_the_name_is_told_before_anything_is_sent(self):
        import io
        from contextlib import redirect_stdout

        answer = {"id": "r1", "client": {"id": "a" * 32, "name": "swift-otter-3f2a"}}
        fake = FakeServer([(201, answer, {}), (201, answer, {})])
        self.addCleanup(fake.close)
        original = submit.Server.who
        submit.Server.who = lambda self, client: {"name": "swift-otter-3f2a"}
        self.addCleanup(setattr, submit.Server, "who", original)

        def said(arguments):
            out = io.StringIO()
            with tempfile.TemporaryDirectory() as d, redirect_stdout(out):
                write_result(d)
                self.assertEqual(submit.main([a.replace("DIR", d) for a in arguments]), 0)
            return [l for l in out.getvalue().splitlines() if "swift-otter-3f2a" in l]

        lines = said(["--whoami", "--url", fake.url])
        self.assertEqual(len(lines), 1)
        self.assertIn("/?client=", lines[0])
        # A submit says it at the start and again with the link at the end; with
        # --no-whoami, which bench/run.sh uses after it has said it, only the end.
        self.assertEqual(len(said(["--url", fake.url, "--code", "c", "DIR"])), 2)
        self.assertEqual(len(said(["--no-whoami", "--url", fake.url, "--code", "c", "DIR"])), 1)

    def test_without_an_answer_the_client_id_is_told(self):
        import io
        from contextlib import redirect_stdout

        out = io.StringIO()
        with redirect_stdout(out):
            code = submit.say_who("http://127.0.0.1:9", "b" * 32)
        self.assertEqual(code, 1)
        self.assertIn("b" * 32, out.getvalue())

    def test_a_refused_result_fails_the_run_of_the_script(self):
        fake = FakeServer([(422, {"detail": "no", "problems": ["x"]}, {})])
        self.addCleanup(fake.close)
        with tempfile.TemporaryDirectory() as d:
            write_result(d)
            self.assertEqual(submit.main(["--url", fake.url, "--code", "c", d]), 1)


if __name__ == "__main__":
    unittest.main()
