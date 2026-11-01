#!/usr/bin/env python3
"""Developer-only: prepares Freedom Unite for a local build (docs/02-architecture.md §4).

1. Extracts the code overlays from the developer's own ISO (mhfu_extract_overlays).
2. Recompiles the decrypted executable together with its overlays (mhf_recomp).

Everything is written under user_data/ (git-ignored). Then configure CMake with -DMHF_MHFU_GENERATED=<printed folder>.

usage: games/mhfu/tools/prepare.py --iso <image.iso> --exe <decrypted executable> --tools <build dir> [--region ULES01213]
"""
import argparse
import hashlib
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



def expected_executable_identity(region_dir):
    """Read the plain executable's size and SHA-1 from the region identification file."""
    path = os.path.join(region_dir, "region.toml")
    section = None
    values = {}
    with open(path, encoding="utf-8") as source:
        for raw in source:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip()
                continue
            if section == "dev_executable" and "=" in line:
                key, value = (part.strip() for part in line.split("=", 1))
                values[key] = value.strip().strip('"')
    try:
        return int(values["size"], 0), values["sha1"].lower()
    except (KeyError, ValueError):
        sys.exit(f"error: region.toml has no valid [dev_executable] size/SHA-1: {path}")


def verify_executable(exe_path, region_dir):
    expected_size, expected_sha1 = expected_executable_identity(region_dir)
    actual_size = os.path.getsize(exe_path)
    if actual_size != expected_size:
        sys.exit(f"error: executable size is {actual_size}, expected {expected_size} for this region; "
                 "use the prepared executable from your own matching game copy")
    digest = hashlib.sha1()
    with open(exe_path, "rb") as executable:
        for chunk in iter(lambda: executable.read(1024 * 1024), b""):
            digest.update(chunk)
    actual_sha1 = digest.hexdigest()
    if actual_sha1 != expected_sha1:
        sys.exit(f"error: executable SHA-1 is {actual_sha1}, expected {expected_sha1} for this region; "
                 "the ISO/executable may be from another release or modified")


def manifest_disc_id(manifest_path):
    with open(manifest_path, encoding="utf-8") as manifest:
        for raw in manifest:
            line = raw.split("#", 1)[0].strip()
            if line.startswith("disc_id") and "=" in line:
                return line.split("=", 1)[1].strip().strip('"')
    return ""


def region_symbol(region_dir, section_name, symbol_name):
    """Read one numeric symbol from a named TOML table in symbols.toml."""
    section = None
    with open(os.path.join(region_dir, "symbols.toml"), encoding="utf-8") as source:
        for raw in source:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip()
                continue
            if section == section_name and "=" in line:
                key, value = (part.strip() for part in line.split("=", 1))
                if key == symbol_name:
                    return int(value, 0)
    sys.exit(f"error: missing [{section_name}].{symbol_name} in {region_dir}/symbols.toml")

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

    if not os.path.isfile(os.path.join(region_dir, "region.toml")):
        sys.exit(f"error: unknown region {a.region}; no region.toml found")
    verify_executable(a.exe, region_dir)

    subprocess.run([tool(a.tools, "mhfu_extract_overlays"), "--iso", a.iso, "--exe", a.exe, "--out", overlays], check=True)
    manifest = os.path.join(overlays, "overlays.toml")
    actual_region = manifest_disc_id(manifest)
    if actual_region != a.region:
        sys.exit(f"error: ISO DISC_ID is {actual_region or 'unknown'}, but --region is {a.region}; "
                 "use the matching ISO and region")

    camera_supported = a.region in ("ULES01213", "ULUS10391")
    game_sys_singleton = region_symbol(region_dir, "data", "mh_game_sys_singleton") if camera_supported else 0
    recomp_args = [tool(a.tools, "mhf_recomp"), "--elf", a.exe, "--out", generated, "--name", "mhfu", "--parts", a.parts,
                   "--overlays", os.path.join(overlays, "overlays.toml"),
                   "--symbols", os.path.join(region_dir, "symbols.toml"), "--hook", "mh_overlay_prepare"]
    if camera_supported:
        recomp_args.extend(["--replace", "mh_camera_orbit_update",
                             "--replace", "mh_camera_stage_update"])
    subprocess.run(recomp_args, check=True)

    # The host keeps the legacy digital fallback for regions that do not yet have a verified camera hook.
    os.makedirs(generated, exist_ok=True)
    with open(os.path.join(generated, "mhfu_camera_support.hpp"), "w", encoding="utf-8") as header:
        header.write("#pragma once\n")
        header.write(f"#define MHF_MHFU_HAS_ANALOG_CAMERA {1 if camera_supported else 0}\n")
        header.write(f"#define MHF_MHFU_GAME_SYS_SINGLETON 0x{game_sys_singleton:08X}u\n")
    print(f"\nGenerated code: {generated}\nConfigure with: cmake --preset <preset> -DMHF_MHFU_GENERATED={generated}")


if __name__ == "__main__":
    main()
