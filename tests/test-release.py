"""Verify publishing gates and artifact handoff without contacting LuaRocks."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


root = Path.cwd()
with tempfile.TemporaryDirectory(prefix="screenlock-release-") as directory:
    stage = Path(directory)
    shutil.copytree(root / "dist", stage / "dist")
    mock_bin = stage / "bin"
    mock_bin.mkdir()
    uploader = mock_bin / "luarocks"
    uploader.write_text(
        f"#!{sys.executable}\n"
        "import json, os, pathlib, sys\n"
        "pathlib.Path(os.environ['UPLOAD_RECORD']).write_text(json.dumps(sys.argv[1:]))\n"
    )
    uploader.chmod(0o755)
    record = stage / "upload.json"
    rockspec, = (stage / "dist").glob("*.rockspec")
    source_rock = rockspec.with_suffix(".src.rock")
    version = rockspec.stem.removeprefix("awesomewm-screenlock-plugin-").rsplit("-", 1)[0]
    environment = dict(
        os.environ,
        PATH=f"{mock_bin}:{os.environ['PATH']}",
        UPLOAD_RECORD=str(record),
        LUAROCKS_API_KEY="test-upload-key",
        RELEASE_TAG=f"v{version}",
    )

    def publish(overrides=None):
        return subprocess.run(
            ["sh", str(root / "build/publish.sh")],
            cwd=stage, env=environment | (overrides or {}),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10,
        )

    assert publish({"LUAROCKS_API_KEY": ""}).returncode != 0
    assert not record.exists(), "missing credentials reached the uploader"
    assert publish({"RELEASE_TAG": "v999.999.999"}).returncode != 0
    assert not record.exists(), "mismatched tag reached the uploader"
    result = publish()
    assert result.returncode == 0, result.stderr
    assert json.loads(record.read_text()) == [
        "upload", "--temp-key", "test-upload-key",
        f"dist/{rockspec.name}", f"dist/{source_rock.name}",
    ], "publishing must consume the explicit source rock without repacking or overwriting"
    record.unlink()
    source_rock.write_bytes(source_rock.read_bytes() + b"corrupt")
    assert publish().returncode != 0
    assert not record.exists(), "corrupt artifact reached the uploader"
    print("publish credentials, tag, checksum, and explicit artifact handoff checks passed")
