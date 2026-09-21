import os
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = ROOT / "scripts" / "fstests-mount-helper.sh"


class FstestsMountHelperTest(unittest.TestCase):
    def test_preserves_fstests_device_as_fuse_fsname(self):
        with tempfile.TemporaryDirectory() as temporary:
            temp = pathlib.Path(temporary)
            capture = temp / "args.txt"
            test_dev = temp / "test"
            scratch_dev = temp / "scratch"
            test_dev.mkdir()
            scratch_dev.mkdir()
            fake_swordfs = temp / "swordfs"
            fake_swordfs.write_text(
                "#!/usr/bin/env bash\n"
                "printf '%s\\n' \"$@\" > \"${FSTESTS_CAPTURE}\"\n",
                encoding="utf-8",
            )
            fake_swordfs.chmod(0o755)

            config = temp / "mount.env"
            config.write_text(
                "\n".join(
                    [
                        f"FSTESTS_SWORDFS_BIN={fake_swordfs}",
                        "FSTESTS_METADATA_URL=redis://127.0.0.1:6379/13",
                        f"FSTESTS_TEST_DEV={test_dev}",
                        "FSTESTS_TEST_VOLUME=test-volume",
                        f"FSTESTS_SCRATCH_DEV={scratch_dev}",
                        "FSTESTS_SCRATCH_VOLUME=scratch-volume",
                        "FSTESTS_MINIO_ROOT_USER=minioadmin",
                        "FSTESTS_MINIO_ROOT_PASSWORD=minioadmin",
                        f"FSTESTS_LOG_DIR={temp}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            env = os.environ.copy()
            env["SWORDFS_FSTESTS_MOUNT_CONFIG"] = str(config)
            env["FSTESTS_CAPTURE"] = str(capture)
            subprocess.run(
                ["bash", str(HELPER), str(test_dev), str(test_dev), "-o", "allow_other,ro"],
                check=True,
                env=env,
            )

            args = capture.read_text(encoding="utf-8").splitlines()
            option_index = args.index("-o")
            options = set(args[option_index + 1].split(","))
            self.assertIn("allow_other", options)
            self.assertIn("ro", options)
            self.assertIn(f"fsname={test_dev}", options)


if __name__ == "__main__":
    unittest.main()
