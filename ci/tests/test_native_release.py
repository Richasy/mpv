import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import subprocess
import sys


spec = importlib.util.spec_from_file_location(
    "native_release", Path(__file__).parents[1] / "publish_native_release.py")
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class NativeReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.feed = self.root / "feed"
        self.feed.mkdir()
        self.commit = "a" * 40

    def source(self, architecture, commit=None):
        source = self.root / architecture
        source.mkdir(exist_ok=True)
        for name in publisher.required_files(architecture):
            (source / name).write_bytes(f"{architecture}:{name}".encode())
        (source / "build-info.txt").write_text(
            f"mpv_commit={commit or self.commit}\n"
            f"target_arch={publisher.ARCHITECTURES[architecture]}\nbuild_type=release\n",
            encoding="utf-8")
        return source

    def stage(self, architecture, run_id="123", attempt=1):
        return publisher.stage(
            self.feed, self.source(architecture), architecture, run_id, attempt, self.commit)

    def test_only_complete_pair_publishes_a_frozen_pointer(self):
        self.stage("x64")
        with self.assertRaises(FileNotFoundError):
            publisher.complete(self.feed, "123", 1, self.commit)
        self.assertFalse((self.feed / "native/latest.json").exists())
        self.stage("arm64")
        first = publisher.complete(self.feed, "123", 1, self.commit)
        pointer = json.loads((self.feed / "native/latest.json").read_text())
        self.assertEqual(pointer["sha256"], publisher.sha256(first))
        frozen = first.read_bytes()
        self.stage("x64", "124")
        self.stage("arm64", "124")
        publisher.complete(self.feed, "124", 1, self.commit)
        self.assertEqual(frozen, first.read_bytes())
        self.assertEqual(pointer["sha256"], publisher.sha256(self.feed / pointer["path"]))
        with self.assertRaisesRegex(ValueError, "newer native workflow"):
            publisher.complete(self.feed, "123", 1, self.commit)

    def test_completion_resumes_exact_bytes_after_pointer_write_failure(self):
        self.stage("x64")
        self.stage("arm64")
        replace = publisher.os.replace

        def interrupt(source, destination):
            if destination.name == "latest.json":
                raise OSError("interrupted pointer publication")
            return replace(source, destination)

        with mock.patch.object(publisher.os, "replace", side_effect=interrupt):
            with self.assertRaisesRegex(OSError, "interrupted"):
                publisher.complete(self.feed, "123", 1, self.commit)
        manifest = self.feed / "native/builds/123-1/manifest.json"
        expected = manifest.read_bytes()
        self.assertFalse((self.feed / "native/latest.json").exists())
        publisher.complete(self.feed, "123", 1, self.commit)
        self.assertEqual(expected, manifest.read_bytes())
        self.assertEqual(publisher.sha256(manifest),
                         json.loads((self.feed / "native/latest.json").read_text())["sha256"])

    def test_process_exit_releases_publication_ownership(self):
        child = (
            "import importlib.util,os,sys; from pathlib import Path; "
            "s=importlib.util.spec_from_file_location('publisher',sys.argv[1]); "
            "m=importlib.util.module_from_spec(s); s.loader.exec_module(m); "
            "lock=m.publication_lock(Path(sys.argv[2])); lock.__enter__(); os._exit(0)"
        )
        subprocess.run([sys.executable, "-B", "-c", child, str(Path(publisher.__file__)), str(self.feed)],
                       check=True, timeout=10)
        with publisher.publication_lock(self.feed):
            pass

    def test_local_workflow_requires_both_architectures(self):
        for x64, arm64 in ((True, False), (False, True), (False, False)):
            with self.assertRaisesRegex(ValueError, "both x64 and ARM64"):
                publisher.select_matrix(x64, arm64, "local")
        self.assertEqual(2, len(publisher.select_matrix(True, True, "local")["include"]))

    def test_missing_or_tampered_runtime_never_completes(self):
        x64 = self.stage("x64")
        self.stage("arm64")
        (x64 / "ggml-cpu.dll").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "differs"):
            publisher.complete(self.feed, "123", 1, self.commit)
        self.assertFalse((self.feed / "native/latest.json").exists())
        self.assertFalse((x64.parent / "manifest.json").exists())

    def test_mixed_attempt_or_commit_is_rejected(self):
        self.stage("x64")
        arm64 = self.stage("arm64")
        path = arm64 / "manifest.json"
        document = json.loads(path.read_text())
        document["runAttempt"] = 2
        path.write_text(json.dumps(document))
        with self.assertRaisesRegex(ValueError, "one source"):
            publisher.complete(self.feed, "123", 1, self.commit)
        source = self.source("x64", commit="b" * 40)
        with self.assertRaisesRegex(ValueError, "selected release source"):
            publisher.stage(self.feed, source, "x64", "124", 1, self.commit)
        self.assertFalse((self.feed / "native/latest.json").exists())

    def test_staging_is_immutable_and_requires_every_dependency(self):
        self.stage("x64")
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.stage("x64")
        source = self.source("arm64")
        (source / "libwhisper.dll").unlink()
        with self.assertRaisesRegex(ValueError, "incomplete"):
            publisher.stage(self.feed, source, "arm64", "123", 1, self.commit)
        self.assertFalse((self.feed / "native/builds/123-1/arm64").exists())


if __name__ == "__main__":
    unittest.main()
