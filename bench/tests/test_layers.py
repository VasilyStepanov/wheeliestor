import json
import os
import sys
import tempfile
import unittest
from unittest import mock

BENCH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, BENCH)

import layers  # noqa: E402

REPORT = """# To display the perf.data header info, please use --header/--header-only options.
#
# Total Lost Samples: 0
#
# Samples: 83K of event 'cpu/cycles/P'
# Event count (approx.): 67534786951
#
# Overhead  Shared Object      Symbol
# ........  .................  ..........................................
#
     7.43%  [kernel.kallsyms]  [k] rep_movs_alternative
     5.33%  [kernel.kallsyms]  [k] native_queued_spin_lock_slowpath
     4.00%  [kernel.kallsyms]  [k] tcp_sendmsg
     3.00%  [kernel.kallsyms]  [k] virtqueue_add_split
     2.00%  [kernel.kallsyms]  [k] io_submit_sqes
     2.00%  libc.so.6          [.] __libc_start_main
     1.00%  wheelie-server     [.] wheelie::Server::run
     0.50%  [kernel.kallsyms]  [k] native_read_msr
"""


class ClassifyTest(unittest.TestCase):
    def test_a_symbol_goes_to_its_layer(self):
        cases = {
            ("[kernel.kallsyms]", "rep_movs_alternative"): "copies",
            ("[kernel.kallsyms]", "tcp_sendmsg"): "network",
            ("[kernel.kallsyms]", "br_handle_frame"): "network",
            ("[kernel.kallsyms]", "unix_stream_sendmsg"): "network",
            ("[kernel.kallsyms]", "virtqueue_add_split"): "storage",
            ("[kernel.kallsyms]", "blk_mq_submit_bio"): "storage",
            ("[kernel.kallsyms]", "io_submit_sqes"): "io_uring",
            ("[kernel.kallsyms]", "native_queued_spin_lock_slowpath"): "scheduler",
            ("[kernel.kallsyms]", "native_read_msr"): "other",
            ("libc.so.6", "memcpy"): "code",
            ("wheelie-server", "main"): "code",
            ("fio", "td_io_queue"): "code",
        }
        for (dso, symbol), layer in cases.items():
            self.assertEqual(layers.classify(dso, symbol), layer, symbol)

    def test_the_report_is_read(self):
        rows = layers.parse_report(REPORT)
        self.assertEqual(len(rows), 8)
        self.assertEqual(rows[0], (7.43, "[kernel.kallsyms]", "rep_movs_alternative"))
        self.assertEqual(layers.parse_header(REPORT), (67534786951, 83000))

    def test_the_layers_add_up_to_a_hundred(self):
        result = layers.profiled_process(REPORT, "wheelie-server")
        self.assertAlmostEqual(sum(result["layers"].values()), 100, delta=0.1)
        self.assertEqual(set(result["layers"]), set(layers.LAYERS))
        self.assertGreater(result["layers"]["copies"], result["layers"]["io_uring"])
        self.assertEqual(result["top"][0]["symbol"], "rep_movs_alternative")
        self.assertEqual(result["cycles"], 67534786951)

    def test_an_empty_report_is_an_error(self):
        with self.assertRaises(ValueError):
            layers.profiled_process("# nothing here\n", "fio")


class BuildTest(unittest.TestCase):
    def test_the_cycles_per_request_come_from_the_cycles_and_the_iops(self):
        fio = {"jobs": [{"read": {"io_bytes": 1, "iops": 100000}, "write": {"io_bytes": 0, "iops": 0}}]}
        with tempfile.TemporaryDirectory() as d:
            fio_path = os.path.join(d, "r.json")
            data = os.path.join(d, "server.perf")
            for path, text in ((fio_path, json.dumps(fio)), (data, "x")):
                with open(path, "w") as handle:
                    handle.write(text)
            with mock.patch.object(layers, "perf_report", return_value=REPORT):
                profile = layers.build(10, fio_path, server=data)
        self.assertEqual(profile["window_s"], 10)
        self.assertNotIn("client", profile)
        # 67534786951 cycles over 10 s at 100000 requests a second.
        self.assertAlmostEqual(profile["server_cycles_per_request"], 67534.8, places=1)

    def test_nothing_recorded_is_an_error(self):
        with self.assertRaises(ValueError):
            layers.build(10, "none.json")


if __name__ == "__main__":
    unittest.main()
