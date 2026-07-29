#!/usr/bin/env python3
"""Run the build-matched GNOME Shell bridge parser tests."""

import os
import subprocess

from deskpal_client import DESKPAL


def main():
    helper = os.path.join(os.path.dirname(DESKPAL), "shell-bridge-test")
    result = subprocess.run([helper], check=True, capture_output=True, text=True)
    output = result.stdout.strip()
    assert output == (
        "PASS: Shell bridge responses are versioned, bounded, and fail closed"
    ), output
    print(output)


if __name__ == "__main__":
    main()
