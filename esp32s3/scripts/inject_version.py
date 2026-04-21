"""
Pre-build script: capture `git describe` → -DP4MSLO_FIRMWARE_VERSION="..."
Mirrors what factory_demo/CMakeLists.txt does on the P4 side so both halves
of the system report the same version string.
"""
import subprocess
import os

Import("env")  # noqa: F821

def get_version():
    try:
        # Run from the repo root (parent of esp32s3/)
        repo_root = os.path.join(os.path.dirname(env["PROJECT_DIR"]), ".")
        if not os.path.isdir(os.path.join(env["PROJECT_DIR"], "..", ".git")):
            # Repo root is one level above the platformio project
            repo_root = os.path.abspath(os.path.join(env["PROJECT_DIR"], ".."))
        result = subprocess.run(
            ["git", "describe", "--always", "--dirty=+", "--tags"],
            cwd=repo_root,
            capture_output=True,
            text=True,
            timeout=5,
        )
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    except Exception as e:
        print(f"inject_version: git describe failed: {e}")
    return "unknown"

version = get_version()
env.Append(CPPDEFINES=[("P4MSLO_FIRMWARE_VERSION", env.StringifyMacro(version))])
print(f"inject_version: firmware version = {version}")
