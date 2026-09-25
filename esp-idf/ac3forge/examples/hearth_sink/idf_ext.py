"""idf.py ota: push this example's build to boards over the network.

ESP-IDF v6.1's idf.py loads idf_ext.py from the project directory and adds the
actions its action_extensions() returns. This adds `ota`, which runs
tools/hearth/ota.py push on the build directory with the Python that runs
idf.py, so a build and an update are one command, as `build flash` is over USB:

    idf.py -C <this directory> -B <build dir> ... build ota --host hearth-eb2c64.local

--host names a board by its IP address or its mDNS name; repeat it for
several, which are updated one at a time. --yes stops a board that is playing
without asking, and --force pushes to a board that already runs the image.
`ota` runs after `build` or `app` when either is on the same command line, and
builds nothing itself. ota.py makes every check and prints every outcome
(planning/esp32-ota.md); a non-zero exit status from it fails the idf.py run.
"""

from __future__ import annotations

import os
import subprocess
import sys
from typing import Any

# The example is esp-idf/ac3forge/examples/hearth_sink, four levels below the
# repository root.
OTA_PY = os.path.join("..", "..", "..", "..", "tools", "hearth", "ota.py")

# ota.py's exit statuses, for the line idf.py prints when one is not 0.
OUTCOMES = {
    1: "a board refused the image, or the upload failed",
    2: "a board rolled back to the image before",
    3: "a board did not come back",
}


def failure(message: str) -> Exception:
    """idf.py's own error, which it prints without a traceback; SystemExit outside idf.py."""
    try:
        from idf_py_actions.errors import FatalError  # noqa: PLC0415 - present only in idf.py
    except ImportError:
        return SystemExit(message)
    return FatalError(message)


def action_extensions(base_actions: dict, project_path: str) -> dict:
    ota_py = os.path.normpath(os.path.join(project_path, OTA_PY))

    def ota(
        action: str, ctx: Any, args: Any, host: tuple[str, ...], yes: bool, force: bool
    ) -> None:
        if not host:
            raise failure("idf.py ota needs a board: --host hearth-eb2c64.local, or its IP address")
        if not os.path.isfile(ota_py):
            raise failure(f"idf.py ota runs {ota_py}, which is not there")
        command = [sys.executable, ota_py, "push", "--build-dir", args.build_dir]
        for name in host:
            command += ["--host", name]
        if yes:
            command.append("--yes")
        if force:
            command.append("--force")
        code = subprocess.run(command, check=False).returncode
        if code != 0:
            raise failure(f"ota.py push exited with {code}: {OUTCOMES.get(code, 'see above')}")

    return {
        "actions": {
            "ota": {
                "callback": ota,
                "short_help": "Push the app to boards over the network (tools/hearth/ota.py).",
                "help": (
                    "Push the built app to boards over the network with tools/hearth/ota.py, and "
                    "wait for each to accept it. Runs after build or app on the same command "
                    "line; builds nothing itself."
                ),
                "order_dependencies": ["all", "app"],
                "options": [
                    {
                        "names": ["--host"],
                        "multiple": True,
                        "help": "A board: its IP address or mDNS name (hearth-eb2c64.local). "
                        "Repeat for several; they are updated one at a time.",
                    },
                    {
                        "names": ["--yes"],
                        "is_flag": True,
                        "help": "Stop a board that is playing without asking.",
                    },
                    {
                        "names": ["--force"],
                        "is_flag": True,
                        "help": "Push to a board that already runs this image.",
                    },
                ],
            }
        }
    }
