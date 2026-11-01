import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

BENCH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALL_VARIANTS = "libc alibc uring uring-ms"
sys.path.insert(0, BENCH)

import report  # noqa: E402

SVG = "{http://www.w3.org/2000/svg}"


def fio_json(rw, bs, depth, numjobs, backend, multishot, iops, bw_kib,
             spread=0.0, sync=0, timestamp=1):
    """The part of what fio writes that the report reads."""
    direction = {"io_bytes": 1 << 20, "iops": iops, "bw": bw_kib,
                 "iops_mean": iops, "iops_stddev": iops * spread / 100}
    idle = {"io_bytes": 0, "iops": 0, "bw": 0, "iops_mean": 0, "iops_stddev": 0}
    reads = rw.endswith("read")
    return {
        "timestamp_ms": timestamp,
        "jobs": [{
            "job options": {
                "wheelie_backend": backend,
                "wheelie_multishot": "1" if multishot else "0",
                "rw": rw, "bs": bs, "iodepth": str(depth),
                "numjobs": str(numjobs), "sync": str(sync),
            },
            "read": direction if reads else idle,
            "write": idle if reads else direction,
        }],
    }


class Results:
    """A directory of results."""

    def __init__(self, test):
        self.directory = tempfile.TemporaryDirectory()
        test.addCleanup(self.directory.cleanup)
        self.path = self.directory.name

    def add(self, name, settings=None, **kwargs):
        with open(os.path.join(self.path, name + ".json"), "w") as handle:
            json.dump(fio_json(**kwargs), handle)
        if settings:
            with open(os.path.join(self.path, name + ".settings"), "w") as handle:
                handle.write(settings)


class ReportTest(unittest.TestCase):
    def test_the_variant_is_that_of_the_server(self):
        results = Results(self)
        results.add("a", settings="WHEELIE_BACKEND=uring\nWHEELIE_MULTISHOT=1\n", rw="randread", bs="4k",
                    depth=1, numjobs=1, backend="libc", multishot=False, iops=5000, bw_kib=20000)
        self.assertEqual(list(report.read_results([results.path]).values())[0].keys(), {"uring-ms"})

    def results(self):
        results = Results(self)
        for backend, multishot, iops in (("libc", False, 5334), ("uring", False, 4796),
                                         ("uring", True, 5950)):
            variant = backend + ("_ms" if multishot else "")
            results.add("a_" + variant, rw="randread", bs="4k", depth=1, numjobs=1,
                        backend=backend, multishot=multishot, iops=iops,
                        bw_kib=iops * 4, spread=8.7)
            results.add("b_" + variant, rw="randread", bs="4k", depth=16, numjobs=8,
                        backend=backend, multishot=multishot, iops=iops * 5,
                        bw_kib=iops * 20)
        results.add("c_libc", rw="read", bs="1m", depth=4, numjobs=4, backend="libc",
                    multishot=False, iops=363, bw_kib=363 * 1024)
        results.add("c_uring", rw="read", bs="1m", depth=4, numjobs=4,
                    backend="uring", multishot=False, iops=700, bw_kib=700 * 1024)
        return results

    def test_the_variant_and_the_load_come_from_the_options(self):
        stats = fio_json("randwrite", "4k", 16, 8, "uring", True, 1000, 4000, sync=1)
        result = report.Result(stats["jobs"][0], 1)
        self.assertEqual(result.variant, "uring-ms")
        self.assertEqual(result.load, ("randwrite", "4k", 16, 8, 1))
        self.assertEqual(report.load_label(result.load), "randwrite 4k qd16 nj8 sync")

    def test_a_result_that_is_not_the_engine_s_is_refused(self):
        stats = fio_json("read", "4k", 1, 1, "libc", False, 1, 4)
        del stats["jobs"][0]["job options"]["wheelie_backend"]
        with self.assertRaises(ValueError):
            report.Result(stats["jobs"][0], 1)

    def test_failed_fio_jobs_are_refused(self):
        stats = fio_json("read", "4k", 16, 1, "uring", True, 100, 400)
        stats["jobs"][0]["error"] = 5
        with self.assertRaisesRegex(ValueError, "fio job failed"):
            report.Result(stats["jobs"][0], 1)

    def test_different_tuning_and_server_settings_keep_separate_rows(self):
        results = Results(self)
        cases = [({}, {}), ({"wheelie_queues": "4"}, {}),
                 ({"iodepth_batch": "16"}, {}),
                 ({"iodepth_batch_complete_min": "16"}, {}),
                 ({"iodepth_batch_complete_max": "16"}, {}),
                 ({"cpus_allowed": "12-19", "cpus_allowed_policy": "split"}, {}),
                 ({}, {"WHEELIE_SERVER_CPUS": "8-11", "WHEELIE_THREADS": "4"}),
                 ({}, {"WHEELIE_SERVER_CPUS": "16-19", "WHEELIE_THREADS": "4"}),
                 ({}, {"WHEELIE_DIRECT": "0"})]
        for i, (options, settings) in enumerate(cases):
            stats = fio_json("randread", "4k", 16, 8, "uring", True,
                             100 + i, 400 + i * 4, timestamp=i)
            stats["jobs"][0]["job options"].update(options)
            path = os.path.join(results.path, str(i))
            with open(path + ".json", "w") as handle:
                json.dump(stats, handle)
            if settings:
                with open(path + ".settings", "w") as handle:
                    for key, value in settings.items():
                        handle.write(key + "=" + value + "\n")
        read = report.read_results([results.path])
        self.assertEqual(len(read), len(cases))
        table = report.results_table(read, report.variants_of(read))
        self.assertIn("queues=4", table)
        self.assertIn("complete-max=16", table)
        self.assertIn("server-cpus=8-11", table)

    def test_a_cell_is_iops_bandwidth_and_spread(self):
        results = report.read_results([self.results().path])
        cell = results[("randread", "4k", 1, 1, 0)]["libc"].cell()
        self.assertEqual(cell, "5334 / 20.8 / 8.7%")

    def test_the_table_has_a_row_per_load_and_a_column_per_variant(self):
        results = report.read_results([self.results().path])
        variants = report.variants_of(results)
        self.assertEqual(variants, ["libc", "uring", "uring-ms"])

        lines = report.results_table(results, variants).splitlines()
        self.assertEqual(len(lines), 2 + 3)
        header = [cell.strip() for cell in lines[0].strip("|").split("|")]
        self.assertEqual(header, ["load", "libc", "uring", "uring-ms"])
        # The loads are in order: random reads before sequential ones, and a
        # lower depth before a higher one.
        order = [line.split("|")[1].strip() for line in lines[2:]]
        self.assertEqual(order, ["randread 4k qd1 nj1", "randread 4k qd16 nj8",
                                 "read 1m qd4 nj4"])
        # What was not measured for a variant is a dash: the sequential read
        # has no result for uring-ms.
        sequential = [cell.strip() for cell in lines[4].strip("|").split("|")]
        self.assertEqual(sequential[1:], ["363 / 363.0 / 0.0%", "700 / 700.0 / 0.0%", "-"])

    def test_rows_go_by_block_size_then_depth_then_jobs(self):
        results = Results(self)
        for name, bs, depth, numjobs in (("a", "4k", 16, 1), ("b", "4k", 1, 8),
                                         ("c", "1m", 1, 1), ("d", "4k", 1, 1)):
            results.add(name, rw="randread", bs=bs, depth=depth, numjobs=numjobs,
                        backend="libc", multishot=False, iops=100, bw_kib=400)
        read = report.read_results([results.path])
        order = [report.load_label(load) for load in sorted(read, key=report.load_sort_key)]
        self.assertEqual(order, ["randread 4k qd1 nj1", "randread 4k qd1 nj8",
                                 "randread 4k qd16 nj1", "randread 1m qd1 nj1"])

    def test_ratios_are_against_the_baseline(self):
        results = report.read_results([self.results().path])
        text = report.ratio_table(results, report.variants_of(results), "libc")
        first = text.splitlines()[2]
        self.assertIn("x0.90", first)
        self.assertIn("x1.12", first)

    def test_the_latest_run_of_a_load_is_the_one_that_counts(self):
        results = Results(self)
        results.add("old", rw="read", bs="4k", depth=1, numjobs=1, backend="libc",
                    multishot=False, iops=100, bw_kib=400, timestamp=1)
        results.add("new", rw="read", bs="4k", depth=1, numjobs=1, backend="libc",
                    multishot=False, iops=200, bw_kib=800, timestamp=2)
        read = report.read_results([results.path])
        self.assertEqual(read[("read", "4k", 1, 1, 0)]["libc"].iops, 200)

    def test_what_is_not_a_result_is_skipped_with_a_word(self):
        results = Results(self)
        results.add("good", rw="read", bs="4k", depth=1, numjobs=1, backend="libc",
                    multishot=False, iops=100, bw_kib=400)
        with open(os.path.join(results.path, "junk.json"), "w") as handle:
            handle.write("{}")
        result = subprocess.run([sys.executable, os.path.join(BENCH, "report.py"),
                                 results.path], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("junk.json", result.stderr)
        self.assertIn("100 / 0.4 / 0.0%", result.stdout)

    def test_the_chart_is_an_svg_with_a_point_for_each_result(self):
        results = report.read_results([self.results().path])
        svg = report.chart(results, "libc", report.variants_of(results))
        root = ET.fromstring(svg)
        self.assertEqual(root.tag, SVG + "svg")
        self.assertEqual(root.get("role"), "img")

        groups = [g for g in root.iter(SVG + "g") if g.get("class") == "pt"]
        # Two small-block loads and one large-block load, for uring and
        # uring-ms where they were measured: 2 + 2 small, 1 large.
        self.assertEqual(len(groups), 5)
        titles = [g.find(SVG + "title").text for g in groups]
        self.assertTrue(any(t.startswith("uring, randread 4k qd1 nj1") and "x0.90" in t
                            for t in titles), titles)

        panels = [t.text for t in root.iter(SVG + "text") if t.get("class") == "panel-title"]
        self.assertEqual(panels, ["Small blocks", "Large blocks"])
        diagonals = [e for e in root.iter(SVG + "line") if e.get("class") == "diagonal"]
        self.assertEqual(len(diagonals), 2 + 1)  # one in each panel, one in the legend

    def test_a_panel_with_nothing_in_it_is_left_out(self):
        results = Results(self)
        for backend in ("libc", "uring"):
            results.add(backend, rw="randread", bs="4k", depth=1, numjobs=1,
                        backend=backend, multishot=False, iops=100, bw_kib=400)
        read = report.read_results([results.path])
        root = ET.fromstring(report.chart(read, "libc", report.variants_of(read)))
        panels = [t.text for t in root.iter(SVG + "text") if t.get("class") == "panel-title"]
        self.assertEqual(panels, ["Small blocks"])

    def test_a_chart_of_one_variant_against_another(self):
        results = Results(self)
        for backend, multishot, iops in (("alibc", False, 1000), ("uring", False, 4000), ("uring", True, 4200)):
            results.add("%s%s" % (backend, "-ms" if multishot else ""), rw="randread", bs="4k", depth=16,
                        numjobs=1, backend=backend, multishot=multishot, iops=iops, bw_kib=iops * 4)
        read = report.read_results([results.path])
        root = ET.fromstring(report.chart(read, "alibc", report.variants_of(read), ["uring"]))
        text = [t.text for t in root.iter(SVG + "text")]
        self.assertIn("alibc against uring", text)
        # The baseline is not a series, and what is not asked for is not drawn.
        titles = [t.text for t in root.iter(SVG + "title")]
        self.assertEqual(len([t for t in titles if t.startswith("uring, ")]), 1)
        self.assertFalse([t for t in titles if t.startswith("alibc, ")])
        self.assertFalse([t for t in titles if t.startswith("uring-ms, ")])
        # Without --against all the others are drawn, but not the baseline.
        root = ET.fromstring(report.chart(read, "alibc", report.variants_of(read)))
        titles = [t.text for t in root.iter(SVG + "title")]
        self.assertTrue([t for t in titles if t.startswith("uring-ms, ")])
        self.assertFalse([t for t in titles if t.startswith("alibc, ")])

    def test_without_the_baseline_there_is_no_chart(self):
        results = Results(self)
        results.add("only", rw="read", bs="4k", depth=1, numjobs=1, backend="uring",
                    multishot=False, iops=100, bw_kib=400)
        read = report.read_results([results.path])
        with self.assertRaises(ValueError):
            report.chart(read, "libc", report.variants_of(read))


class MatrixTest(unittest.TestCase):
    def dry_run(self, *arguments):
        return subprocess.run(
            [os.path.join(BENCH, "matrix.sh"), "--dry-run", "--out", "/nonexistent",
             *arguments], capture_output=True, text=True)

    def test_every_variant_runs_every_load(self):
        result = self.dry_run("--variants", ALL_VARIANTS)
        self.assertEqual(result.returncode, 0, result.stderr)
        runs = [line for line in result.stdout.splitlines() if "bench/measure.sh" in line]
        self.assertEqual(len(runs), 4 * 10)
        multishot = next(r for r in runs if "randread_qd16_nj8_uring-ms" in r)
        self.assertIn("--mode randread --bs 4k --depth 16 --numjobs 8 "
                      "--backend uring --multishot", multishot)
        journal = next(r for r in runs if "seqwrite_4k_qd1_nj1_libc" in r)
        self.assertIn("--mode write --bs 4k --depth 1 --numjobs 1 --backend libc", journal)
        sequential = next(r for r in runs if "seqwrite_1m_nj4_libc" in r)
        self.assertIn("--mode write --bs 1m --depth 4 --numjobs 4 --backend libc",
                      sequential)

    def runs_of(self, *arguments):
        result = self.dry_run(*arguments)
        self.assertEqual(result.returncode, 0, result.stderr)
        names = []
        for line in result.stdout.splitlines():
            if "bench/measure.sh" in line:
                names.append(line.split("--name ")[1].split(" ")[0])
        return names

    def test_the_loads_are_in_the_order_of_the_table(self):
        names = self.runs_of()
        # The loads are in the order of the table of the docs: by block, depth, jobs and mode.
        loads = []
        for name in names:
            load = name.rsplit("_", 1)[0] if not name.endswith("uring-ms") else name[: -len("_uring-ms")]
            if load not in loads:
                loads.append(load)
        self.assertEqual(loads, ["randread_qd1_nj1", "randwrite_qd1_nj1", "seqread_4k_qd1_nj1",
                                 "seqwrite_4k_qd1_nj1", "randread_qd16_nj1", "randwrite_qd16_nj1",
                                 "randread_qd16_nj8", "randwrite_qd16_nj8", "seqread_1m_nj4",
                                 "seqwrite_1m_nj4"])

    @staticmethod
    def variant_of(name):
        return "uring-ms" if name.endswith("uring-ms") else name.rsplit("_", 1)[1]

    def test_the_order_of_the_variants_is_random_for_every_load(self):
        names = self.runs_of("--seed", "1", "--variants", ALL_VARIANTS)
        # The variants of one load are run next to each other, each once.
        for i in range(0, len(names), 4):
            self.assertEqual(len({n.rsplit("_", 1)[0] if not n.endswith("uring-ms") else n[: -len("_uring-ms")]
                                  for n in names[i:i + 4]}), 1)
            self.assertEqual({self.variant_of(n) for n in names[i:i + 4]},
                             {"libc", "alibc", "uring", "uring-ms"})
        # No variant is always the first, and the orders of the loads are not all one.
        starts = set()
        orders = set()
        for seed in range(1, 21):
            names = self.runs_of("--seed", str(seed), "--variants", ALL_VARIANTS)
            variants = [self.variant_of(n) for n in names]
            starts |= {variants[i] for i in range(0, len(variants), 4)}
            orders |= {tuple(variants[i:i + 4]) for i in range(4, len(variants), 4)}
        self.assertEqual(starts, {"libc", "alibc", "uring", "uring-ms"})
        self.assertGreater(len(orders), 4)

    def test_a_seed_repeats_the_order(self):
        self.assertEqual(self.runs_of("--seed", "5"), self.runs_of("--seed", "5"))
        self.assertNotEqual(self.runs_of("--seed", "5"), self.runs_of("--seed", "6"))
        self.assertIn("--seed 5", self.dry_run("--seed", "5").stdout)

    def test_the_name_of_the_client_is_told_after_the_build_and_before_the_first_run(self):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            docker = os.path.join(d, "docker")
            with open(docker, "w") as handle:
                handle.write("#!/bin/bash\nexit 0\n")
            os.chmod(docker, 0o755)
            result = subprocess.run(
                [os.path.join(BENCH, "matrix.sh"), "--out", os.path.join(d, "out"), "--submit", "http://127.0.0.1:9"],
                capture_output=True, text=True, env=dict(os.environ, DOCKER=docker,
                                                         XDG_STATE_HOME=os.path.join(d, "state")))
        lines = result.stdout.splitlines()
        building = next(i for i, l in enumerate(lines) if "building the images" in l)
        who = next(i for i, l in enumerate(lines) if l.startswith("== your client id") or "== you are" in l)
        first = next(i for i, l in enumerate(lines) if "[1/30]" in l)
        self.assertLess(building, who)
        self.assertEqual(who + 1, first)

    def test_what_is_not_its_own_goes_to_run_sh(self):
        result = self.dry_run("--variants", "libc", "--runtime", "5", "--direct", "0")
        runs = [line for line in result.stdout.splitlines() if "bench/measure.sh" in line]
        self.assertEqual(len(runs), 10)
        self.assertTrue(all(line.endswith("--runtime 5 --direct 0") for line in runs))
        # The runtime that was asked for is the only one, and the ramp keeps its default.
        self.assertTrue(all(line.count("--runtime") == 1 and "--ramp 5" in line for line in runs))

    def test_a_run_is_long_unless_it_is_told_otherwise(self):
        result = self.dry_run("--variants", "libc")
        runs = [line for line in result.stdout.splitlines() if "bench/measure.sh" in line]
        self.assertTrue(all("--runtime 15 --ramp 5" in line for line in runs))
        result = self.dry_run("--variants", "libc", "--ramp", "2")
        runs = [line for line in result.stdout.splitlines() if "bench/measure.sh" in line]
        self.assertTrue(all("--runtime 15" in line and line.count("--ramp") == 1 for line in runs))

    def test_the_results_go_to_the_default_server_unless_told_otherwise(self):
        def run_lines(*arguments):
            result = self.dry_run("--variants", "libc", *arguments)
            self.assertEqual(result.returncode, 0, result.stderr)
            return [line for line in result.stdout.splitlines() if "bench/measure.sh" in line]

        # Nothing is said: measure.sh sends to its default, so the command has no option of it.
        self.assertTrue(all("submit" not in line.replace("--no-build", "") for line in run_lines()))
        self.assertTrue(all(line.endswith("--no-submit") for line in run_lines("--no-submit")))
        self.assertTrue(all(line.endswith("--submit http://results.example") for line in run_lines("--submit", "http://results.example")))
        both = self.dry_run("--submit", "http://results.example", "--no-submit")
        self.assertEqual(both.returncode, 2)
        self.assertIn("together", both.stderr)

    def test_an_unknown_variant_is_refused(self):
        result = self.dry_run("--variants", "libc epoll")
        self.assertEqual(result.returncode, 2)
        self.assertIn("epoll", result.stderr)


if __name__ == "__main__":
    unittest.main()


class DockerCommandTest(unittest.TestCase):
    """bench/docker.sh: docker as it is, or with sudo if the user has no right to it."""

    def choose(self, docker=None, sudo=None, **env):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            for name, body in (("docker", docker), ("sudo", sudo)):
                if body is not None:
                    path = os.path.join(d, name)
                    with open(path, "w") as handle:
                        handle.write("#!/bin/bash\n" + body)
                    os.chmod(path, 0o755)
            # Only the commands of the test are on the PATH, besides the few that the script uses.
            for tool in ("bash", "grep", "true", "env"):
                os.symlink(shutil.which(tool), os.path.join(d, tool))
            environment = {"PATH": d}
            environment.update(env)
            return subprocess.run(
                ["bash", "-c", f'. {BENCH}/docker.sh; choose_docker; echo "[$DOCKER]"'],
                capture_output=True, text=True, env=environment)

    # A docker that answers, and one that refuses a user who is not in its group.
    OPEN = "exit 0\n"
    DENIED = ('if [ "$FAKE_SUDO" = 1 ]; then exit 0; fi\n'
              'echo "permission denied while trying to connect to the docker API at unix:///var/run/docker.sock" >&2\n'
              "exit 1\n")
    SUDO = 'case "$1" in -n|-v) shift;; esac\n[ "$1" = true ] && exit 0\n[ -z "$1" ] && exit 0\nexec env FAKE_SUDO=1 "$@"\n'

    def test_docker_is_used_as_it_is_when_it_answers(self):
        result = self.choose(docker=self.OPEN)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "[docker]"), result.stderr)

    def test_sudo_is_used_when_there_is_no_permission(self):
        result = self.choose(docker=self.DENIED, sudo=self.SUDO)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "[sudo docker]"), result.stderr)

    def test_a_command_that_is_given_is_kept(self):
        result = self.choose(DOCKER="podman")
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "[podman]"), result.stderr)

    def test_no_docker_and_a_daemon_that_does_not_answer_are_told(self):
        missing = self.choose()
        self.assertEqual(missing.returncode, 2)
        self.assertIn("docker is not installed", missing.stderr)
        down = self.choose(docker='echo "Cannot connect to the Docker daemon" >&2\nexit 1\n')
        self.assertEqual(down.returncode, 2)
        self.assertIn("does not answer", down.stderr)
        no_sudo = self.choose(docker=self.DENIED)
        self.assertEqual(no_sudo.returncode, 2)
        self.assertIn("group docker", no_sudo.stderr)

    def test_matrix_builds_the_images_with_sudo_when_there_is_no_permission(self):
        import tempfile

        docker = ('if [ "$FAKE_SUDO" = 1 ]; then\n'
                  '    [ "$1" = compose ] && { echo "built with sudo"; exit 1; }\n'
                  '    exit 0\n'
                  'fi\n'
                  'echo "permission denied while trying to connect to the docker API" >&2\n'
                  'exit 1\n')
        with tempfile.TemporaryDirectory() as d:
            for name, body in (("docker", docker), ("sudo", self.SUDO)):
                path = os.path.join(d, name)
                with open(path, "w") as handle:
                    handle.write("#!/bin/bash\n" + body)
                os.chmod(path, 0o755)
            result = subprocess.run(
                [os.path.join(BENCH, "matrix.sh"), "--no-submit"], capture_output=True, text=True,
                env=dict(os.environ, PATH=d + ":" + os.environ["PATH"], DOCKER=""))
        self.assertIn("built with sudo", result.stdout + result.stderr)


class OptionSyntaxTest(unittest.TestCase):
    """--option=value is --option value, in the scripts that parse their options themselves."""

    def run_script(self, script, *arguments):
        return subprocess.run([os.path.join(BENCH, script), *arguments], capture_output=True, text=True)

    def test_a_value_after_an_equals_sign_is_taken_as_a_separate_one(self):
        # The check of the value comes before anything is started: it names the option and
        # fails for the value that the equals sign carried.
        self.assertIn("--backend must be libc, alibc or uring",
                      self.run_script("measure.sh", "--backend=epoll").stderr)
        self.assertIn("--direct must be 0 or 1", self.run_script("measure.sh", "--direct=7").stderr)
        # A value that has an equals sign of its own is split at the first one only.
        self.assertIn("unknown option '--nosuch'", self.run_script("measure.sh", "--nosuch=a=b").stderr)

    def test_the_matrix_takes_equals_signs_and_commas(self):
        spelled = self.run_script("matrix.sh", "--dry-run", "--seed=5", "--variants=libc,uring", "--sync=0")
        self.assertEqual(spelled.returncode, 0, spelled.stderr)
        runs = [line for line in spelled.stdout.splitlines() if "measure.sh" in line]
        self.assertEqual(len(runs), 2 * 10)
        self.assertEqual({line.split("--backend ")[1].split(" ")[0] for line in runs}, {"libc", "uring"})
        self.assertTrue(all(line.endswith("--sync 0") for line in runs))
        separate = self.run_script("matrix.sh", "--dry-run", "--seed", "5", "--variants", "libc,uring", "--sync", "0")
        self.assertEqual([l for l in spelled.stdout.splitlines() if "measure.sh" in l],
                         [l for l in separate.stdout.splitlines() if "measure.sh" in l])
        self.assertIn("unknown variant", self.run_script("matrix.sh", "--dry-run", "--variants=libc,epoll").stderr)

    def test_the_matrix_refuses_what_it_sets_itself(self):
        for argument in ("--depth", "--bs", "--mode", "--numjobs", "--queues", "--name"):
            for spelled in ([argument, "x"], [argument + "=x"]):
                result = self.run_script("matrix.sh", "--dry-run", *spelled)
                self.assertEqual(result.returncode, 2, spelled)
                self.assertIn("is set by the matrix for every load", result.stderr)
        for argument in ("--backend", "--multishot"):
            result = self.run_script("matrix.sh", "--dry-run", argument, "uring")
            self.assertEqual(result.returncode, 2, argument)
            self.assertIn("--variants", result.stderr)
        # What does not change the load still goes through to the runs.
        for argument in (["--sync=0"], ["--runtime", "20"], ["--client-backend=libc"], ["--threads=4"]):
            self.assertEqual(self.run_script("matrix.sh", "--dry-run", *argument).returncode, 0, argument)

    def test_a_fio_argument_with_an_equals_sign_is_not_split(self):
        # The value of --fio-arg is taken by the option, not looked at as an option.
        text = self.run_script("measure.sh", "--fio-arg=--rate_iops=100", "--backend=epoll").stderr
        self.assertIn("--backend must be", text)
        self.assertNotIn("unknown option", text)
