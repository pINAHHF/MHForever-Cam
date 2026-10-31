#!/usr/bin/env python3
"""Developer-only: prepares Freedom Unite for a local build (docs/02-architecture.md §4).

1. Extracts the code overlays from the developer's own ISO (mhfu_extract_overlays).
2. Recompiles the decrypted executable together with its overlays (mhf_recomp).

Everything is written under user_data/ (git-ignored). Then configure CMake with -DMHF_MHFU_GENERATED=<printed folder>.

usage: games/mhfu/tools/prepare.py --iso <image.iso> --exe <decrypted executable> --tools <build dir> [--region ULES01213]
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def tool(build, name):
    for sub in ("games/mhfu", "platforms/psp", ""):
        for suffix in ("", ".exe"):
            path = os.path.join(build, sub, name + suffix)
            if os.path.exists(path):
                return path
    sys.exit(f"error: {name} not found under {build}; build the tools first")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--iso", required=True)
    p.add_argument("--exe", required=True)
    p.add_argument("--tools", required=True, help="CMake build folder that contains mhf_recomp and mhfu_extract_overlays")
    p.add_argument("--region", default="ULES01213")
    p.add_argument("--parts", default="32")
    a = p.parse_args()

    user_data = os.path.join(ROOT, "user_data")
    overlays = os.path.join(user_data, "overlays", a.region)
    generated = os.path.join(user_data, "generated", a.region)
    region_dir = os.path.join(ROOT, "games", "mhfu", "regions", a.region)

    subprocess.run([tool(a.tools, "mhfu_extract_overlays"), "--iso", a.iso, "--exe", a.exe, "--out", overlays], check=True)
    camera_supported = a.region in ("ULES01213", "ULUS10391")
    recomp_args = [tool(a.tools, "mhf_recomp"), "--elf", a.exe, "--out", generated, "--name", "mhfu", "--parts", a.parts,
                   "--overlays", os.path.join(overlays, "overlays.toml"),
                   "--symbols", os.path.join(region_dir, "symbols.toml"), "--hook", "mh_overlay_prepare"]
    if camera_supported:
        recomp_args.extend(["--entry", "mh_camera_orbit_update", "--hook-pc", "mh_camera_orbit_inputs"])
    subprocess.run(recomp_args, check=True)

    # The host keeps the legacy digital fallback for regions that do not yet have a verified camera hook.
    os.makedirs(generated, exist_ok=True)
    with open(os.path.join(generated, "mhfu_camera_support.hpp"), "w", encoding="utf-8") as header:
        header.write("#pragma once\n")
        header.write(f"#define MHF_MHFU_HAS_ANALOG_CAMERA {1 if camera_supported else 0}\n")
    print(f"\nGenerated code: {generated}\nConfigure with: cmake --preset <preset> -DMHF_MHFU_GENERATED={generated}")


if __name__ == "__main__":
    main()
