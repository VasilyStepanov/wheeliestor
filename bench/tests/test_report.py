import json
import os
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

BENCH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
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

    def add(self, name, **kwargs):
        with open(os.path.join(self.path, name + ".json"), "w") as handle:
            json.dump(fio_json(**kwargs), handle)


class ReportTest(unittest.TestCase):
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
        result = self.dry_run()
        self.assertEqual(result.returncode, 0, result.stderr)
        runs = [line for line in result.stdout.splitlines() if "bench/run.sh" in line]
        self.assertEqual(len(runs), 4 * 8)
        multishot = next(r for r in runs if "randread_qd16_nj8_uring-ms" in r)
        self.assertIn("--mode randread --bs 4k --depth 16 --numjobs 8 "
                      "--backend uring --multishot", multishot)
        sequential = next(r for r in runs if "seqwrite_1m_nj4_libc" in r)
        self.assertIn("--mode write --bs 1m --depth 4 --numjobs 4 --backend libc",
                      sequential)

    def runs_of(self, *arguments):
        result = self.dry_run(*arguments)
        self.assertEqual(result.returncode, 0, result.stderr)
        names = []
        for line in result.stdout.splitlines():
            if "bench/run.sh" in line:
                names.append(line.split("--name ")[1].split(" ")[0])
        return names

    def test_the_first_point_of_the_default_chart_comes_after_two_runs(self):
        names = self.runs_of()
        self.assertEqual(names[:2], ["randread_qd16_nj1_alibc", "randread_qd16_nj1_uring"])
        # The loads that tell the most come first, the long blocks last.
        loads = []
        for name in names:
            load = name.rsplit("_", 1)[0] if not name.endswith("uring-ms") else name[: -len("_uring-ms")]
            if load not in loads:
                loads.append(load)
        self.assertEqual(loads[0], "randread_qd16_nj1")
        self.assertEqual(loads[-2:], ["seqread_1m_nj4", "seqwrite_1m_nj4"])

    def test_the_variant_that_starts_a_load_changes(self):
        names = self.runs_of()
        starts = [names[i].rsplit("_", 1)[1] if not names[i].endswith("uring-ms") else "uring-ms"
                  for i in range(0, len(names), 4)]
        self.assertEqual(starts[:4], ["alibc", "uring", "libc", "uring-ms"])
        # The variants of one load are run next to each other.
        for i in range(0, len(names), 4):
            self.assertEqual(len({n.rsplit("_", 1)[0] if not n.endswith("uring-ms") else n[: -len("_uring-ms")]
                                  for n in names[i:i + 4]}), 1)

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
        first = next(i for i, l in enumerate(lines) if "[1/32]" in l)
        self.assertLess(building, who)
        self.assertEqual(who + 1, first)

    def test_what_is_not_its_own_goes_to_run_sh(self):
        result = self.dry_run("--variants", "libc", "--runtime", "5", "--direct", "0")
        runs = [line for line in result.stdout.splitlines() if "bench/run.sh" in line]
        self.assertEqual(len(runs), 8)
        self.assertTrue(all(line.endswith("--runtime 5 --direct 0") for line in runs))

    def test_an_unknown_variant_is_refused(self):
        result = self.dry_run("--variants", "libc epoll")
        self.assertEqual(result.returncode, 2)
        self.assertIn("epoll", result.stderr)


if __name__ == "__main__":
    unittest.main()
