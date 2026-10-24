#!/usr/bin/env python3
"""
boot_unixnu.py
Stages and launches unixnu_run_payload.bin on Xiaomi Mi 10 via fastboot / xnuloader.
Fetches live kernel log and screenshot.
"""
import os
import subprocess
import sys
import time
from pathlib import Path

BASE_DIR = Path("C:/MyData/Umi/UniXNU-umi")
PAYLOAD = BASE_DIR / "unixnu_run_payload.bin"
LOG_OUT = Path("C:/MyData/Umi/live_running_log.txt")
SCREEN_RAW = Path("C:/MyData/Umi/live_running_screen.raw")
SCREEN_PNG = Path("C:/MyData/Umi/live_running_screen.png")

def run(cmd):
    p = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    out = (p.stdout + p.stderr).strip()
    return out

def main():
    print("=== Checking device state ===")
    devs = run("fastboot devices")
    if not devs:
        print("Waiting for device in fastboot mode...")
        print("Please ensure the phone is connected in fastboot mode (xnuloader).")
        print("Run: fastboot devices")
        return 1

    print(f"Device found:\n{devs}")
    
    # If a kernel is already running, stop it first
    busy = run("fastboot getvar busy")
    if "busy: 1" in busy or "busy: 0x1" in busy:
        print("A kernel was running, stopping it cleanly...")
        run("fastboot oem stop")
        time.sleep(1)

    print(f"=== 1. Staging {PAYLOAD.name} ({PAYLOAD.stat().st_size} bytes) ===")
    res = run(f'fastboot stage "{PAYLOAD}"')
    print(res)
    if "FAIL" in res:
        print("Failed to stage payload.")
        return 1

    print("=== 2. Starting UniXNU kernel (fastboot oem run) ===")
    res = run("fastboot oem run")
    print(res)

    print("=== 3. Waiting 12 seconds for kernel & SpringBoard initialization ===")
    for s in range(12, 0, -2):
        print(f"  Waiting {s}s...")
        time.sleep(2)

    print("=== 4. Fetching kernel log ===")
    run("fastboot oem log")
    if LOG_OUT.exists():
        LOG_OUT.unlink()
    run(f'fastboot get_staged "{LOG_OUT}"')

    if LOG_OUT.exists():
        size = LOG_OUT.stat().st_size
        print(f"Downloaded log: {size} bytes")
        with open(LOG_OUT, "r", encoding="utf-8", errors="ignore") as f:
            lines = f.readlines()
        
        print("\n--- GPU & SpringBoard Highlights ---")
        for line in lines:
            l = line.lower()
            if any(k in l for k in ["adreno", "redwood", "gpu", "vibrant", "clock", "renderer", "step 2", "touch"]):
                print(" ", line.rstrip())
    else:
        print("Could not download log.")

    print("=== 5. Fetching screen capture ===")
    run("fastboot oem screen")
    if SCREEN_RAW.exists():
        SCREEN_RAW.unlink()
    run(f'fastboot get_staged "{SCREEN_RAW}"')

    if SCREEN_RAW.exists():
        print(f"Screen raw fetched: {SCREEN_RAW.stat().st_size} bytes")
        # Convert raw RGBA 1080x2340 to PNG
        try:
            from PIL import Image
            raw_data = SCREEN_RAW.read_bytes()
            img = Image.frombytes("RGBA", (1080, 2340), raw_data)
            img.save(SCREEN_PNG)
            print(f"Saved PNG screenshot: {SCREEN_PNG}")
        except Exception as e:
            print(f"PNG conversion notice: {e}")

    print("=== BOOT WORKFLOW COMPLETE ===")
    return 0

if __name__ == "__main__":
    sys.exit(main())
