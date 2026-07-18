#!/usr/bin/env python3
"""
fetch_deps.py — Downloads Dear ImGui into editor/third_party/imgui

This does NOT require git. It downloads the release zip over HTTPS
using only the Python standard library, then extracts it.

Run this once before configuring CMake:
    python scripts/fetch_deps.py   (Windows)
    python3 scripts/fetch_deps.py  (Linux/Mac)
"""
import os
import sys
import shutil
import zipfile
import urllib.request
import tempfile

IMGUI_BRANCH = "docking"
IMGUI_ZIP_URL = f"https://github.com/ocornut/imgui/archive/refs/heads/{IMGUI_BRANCH}.zip"

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
DEST = os.path.join(ROOT, "editor", "third_party", "imgui")


def already_present():
    imconfig_h = os.path.join(DEST, "imgui.h")
    core_files_exist = (
        os.path.isfile(imconfig_h) and
        os.path.isfile(os.path.join(DEST, "imgui.cpp")) and
        os.path.isfile(os.path.join(DEST, "backends", "imgui_impl_sdl2.cpp"))
    )
    if not core_files_exist:
        return False
    # Confirm it's actually the docking branch (has ImGuiConfigFlags_DockingEnable).
    # A plain release build of imgui.h won't have this — if it's missing,
    # the existing copy is the wrong (non-docking) branch and must be replaced.
    try:
        with open(imconfig_h, "r", encoding="utf-8", errors="ignore") as f:
            contents = f.read()
        if "DockingEnable" not in contents:
            print("[fetch_deps] Existing ImGui copy lacks docking support — re-fetching docking branch.")
            return False
    except Exception:
        return False
    return True


def download(url, dest_path):
    print(f"[fetch_deps] Downloading: {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=60) as resp, open(dest_path, "wb") as f:
        shutil.copyfileobj(resp, f)


def main():
    if already_present():
        print(f"[OK] Dear ImGui already present at: {DEST}")
        return 0

    os.makedirs(os.path.dirname(DEST), exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        zip_path = os.path.join(tmp, "imgui.zip")
        try:
            download(IMGUI_ZIP_URL, zip_path)
        except Exception as ex:
            print(f"[ERROR] Download failed: {ex}")
            print("[ERROR] Check your internet connection, or manually download:")
            print(f"        {IMGUI_ZIP_URL}")
            print(f"        and extract so that {DEST}/imgui.h exists.")
            return 1

        print("[fetch_deps] Extracting...")
        with zipfile.ZipFile(zip_path) as zf:
            zf.extractall(tmp)

        # Extracted folder is named imgui-<branch>/ e.g. imgui-docking
        extracted_name = f"imgui-{IMGUI_BRANCH}"
        extracted_path = os.path.join(tmp, extracted_name)
        if not os.path.isdir(extracted_path):
            # Fallback: find the single top-level dir
            entries = [e for e in os.listdir(tmp) if os.path.isdir(os.path.join(tmp, e))]
            if len(entries) == 1:
                extracted_path = os.path.join(tmp, entries[0])
            else:
                print(f"[ERROR] Could not locate extracted ImGui folder in {tmp}")
                return 1

        if os.path.isdir(DEST):
            shutil.rmtree(DEST)
        shutil.copytree(extracted_path, DEST)

    if already_present():
        print(f"[OK] Dear ImGui ({IMGUI_BRANCH} branch) installed at: {DEST}")
        return 0
    else:
        print("[ERROR] Extraction completed but expected files are missing.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
