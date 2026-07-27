#!/usr/bin/env python3
"""Run the build-matched deterministic frame-state unit helper."""

import os
import subprocess

from deskpal_client import DESKPAL


def main():
    helper = os.path.join(os.path.dirname(DESKPAL), "frame-state-test")
    result = subprocess.run(
        [helper], check=True, capture_output=True, text=True
    )
    output = result.stdout.strip()
    assert output == "PASS: visual frame signatures, diffs, and settling", output
    print(output)


if __name__ == "__main__":
    main()
