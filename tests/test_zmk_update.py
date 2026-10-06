"""Verify upstream updates preserve custom fork changes and expose conflicts."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class FirmwareUpdateTests(unittest.TestCase):
    def test_upstream_merge_keeps_fork_features_and_stops_on_conflicts(self):
        script = Path(__file__).resolve().parents[1] / "tools/merge_zmk_update.sh"
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def git(*args):
                return subprocess.run(
                    ["git", "-C", str(repo), *args], check=True,
                    capture_output=True, text=True, timeout=10,
                ).stdout.strip()

            git("init", "-b", "upstream")
            git("config", "user.name", "Firmware test")
            git("config", "user.email", "firmware-test@example.invalid")
            (repo / "shared.c").write_text("base\n")
            git("add", ".")
            git("commit", "-m", "Base firmware")
            git("checkout", "-b", "custom")
            (repo / "diagnostics.c").write_text("persistent diagnostics\n")
            (repo / "studio.c").write_text("runtime configuration and battery\n")
            git("add", ".")
            git("commit", "-m", "Custom features")
            custom = git("rev-parse", "HEAD")
            git("checkout", "upstream")
            (repo / "shared.c").write_text("upstream fix\n")
            git("commit", "-am", "Upstream fix")
            upstream = git("rev-parse", "HEAD")
            git("checkout", "custom")
            result = subprocess.run(
                ["bash", str(script), str(repo), upstream],
                capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(git("rev-parse", "HEAD"), custom)
            self.assertEqual((repo / "shared.c").read_text(), "upstream fix\n")
            self.assertEqual((repo / "diagnostics.c").read_text(), "persistent diagnostics\n")
            self.assertEqual((repo / "studio.c").read_text(), "runtime configuration and battery\n")
            git("commit", "-m", "Merge upstream")
            git("merge-base", "--is-ancestor", upstream, "HEAD")

            # Both sides editing the same binding must require a manual merge.
            git("checkout", "upstream")
            (repo / "shared.c").write_text("upstream binding\n")
            git("commit", "-am", "Upstream binding")
            conflicting = git("rev-parse", "HEAD")
            git("checkout", "custom")
            (repo / "shared.c").write_text("custom binding\n")
            git("commit", "-am", "Custom binding")
            before = git("rev-parse", "HEAD")
            result = subprocess.run(
                ["bash", str(script), str(repo), conflicting],
                capture_output=True, text=True, timeout=10,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(git("rev-parse", "HEAD"), before)
            self.assertEqual(git("diff", "--name-only", "--diff-filter=U"), "shared.c")


if __name__ == "__main__":
    unittest.main()
