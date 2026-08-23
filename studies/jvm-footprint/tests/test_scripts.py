#!/usr/bin/env python3
"""Unit tests for JVM-study lifecycle and publication helpers."""

# SPDX-License-Identifier: Apache-2.0

import importlib.util
import json
import pathlib
import tempfile
import unittest
from unittest import mock


STUDY_DIR = pathlib.Path(__file__).resolve().parent.parent


def load_module(name, relative_path):
    spec = importlib.util.spec_from_file_location(name, STUDY_DIR / relative_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load_module("jvm_study_runner", "scripts/run_study.py")
summarizer = load_module("jvm_study_summarizer", "scripts/summarize.py")


class RunnerLifecycleTests(unittest.TestCase):
    def test_target_identity_requires_class_and_exact_pid_file(self):
        pid_file = pathlib.Path("/tmp/study-a/target.pid")
        command = b"java\0-Dmemftprt.study.pidFile=/tmp/study-a/target.pid\0RuntimeExercise\0"
        with mock.patch.object(pathlib.Path, "read_bytes", return_value=command):
            self.assertTrue(runner.target_is_expected(123, "RuntimeExercise", pid_file))
            self.assertFalse(runner.target_is_expected(
                123, "RuntimeExercise", pathlib.Path("/tmp/study-b/target.pid")
            ))
            self.assertFalse(runner.target_is_expected(123, "HelloIdle", pid_file))

    def test_resume_retires_running_attempt_before_retry(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary) / "measure-01"
            base.mkdir()
            runner.write_json(base / "run.json", {
                "status": "running",
                "workload_class": "RuntimeExercise",
            })

            retry, complete = runner.next_attempt_directory(base)

            self.assertFalse(complete)
            self.assertEqual(retry.name, "measure-01-retry-02")
            retired = json.loads((base / "run.json").read_text(encoding="utf-8"))
            self.assertEqual(retired["status"], "abandoned")
            self.assertEqual(retired["cleanup"]["note"], "pid_file_missing")

            retry.mkdir()
            runner.write_json(retry / "run.json", {
                "status": "success",
                "nmt": "on",
                "cleanup": {"exited": True},
            })
            with mock.patch.object(runner, "validate_capture", return_value={}):
                selected, complete = runner.next_attempt_directory(base)
            self.assertTrue(complete)
            self.assertEqual(selected, retry)

    def test_resume_invalidates_success_with_missing_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary) / "measure-01"
            base.mkdir()
            runner.write_json(base / "run.json", {
                "status": "success",
                "nmt": "on",
                "cleanup": {"exited": True},
            })

            retry, complete = runner.next_attempt_directory(base)

            self.assertFalse(complete)
            self.assertEqual(retry.name, "measure-01-retry-02")
            invalidated = json.loads((base / "run.json").read_text(encoding="utf-8"))
            self.assertEqual(invalidated["status"], "invalidated")
            self.assertIn("resume validation failed", invalidated["error"])


class PublicationTests(unittest.TestCase):
    def test_cross_run_analysis_uses_exercise_rows_only(self):
        rows = [
            {"workload": "exercise", "pss_kb": 200, "nmt_java_heap_committed_kb": 100,
             "workload_duration_ms": 150000},
            {"workload": "exercise", "pss_kb": 300, "nmt_java_heap_committed_kb": 200,
             "workload_duration_ms": 150010},
            {"workload": "hello", "pss_kb": 50, "nmt_java_heap_committed_kb": 40,
             "workload_duration_ms": None},
        ]

        analysis = summarizer.publication_analysis(rows)

        self.assertEqual(analysis["exercise_captures"], 2)
        self.assertEqual(analysis["pss_vs_committed_java_heap_pearson"], 1.0)
        self.assertEqual(analysis["exercise_duration_ms"], {
            "minimum": 150000,
            "maximum": 150010,
        })


if __name__ == "__main__":
    unittest.main()
