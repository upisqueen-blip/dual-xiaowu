"""在干净环境中跑 ninja 构建 ESP32-S3 固件。

踩坑记录：
- MSYSTEM / MINGW_PREFIX / MSYS* 会让 IDF 的 cmake 校验拒绝配置，必须剔除；
  用 `env -u` 在 Git Bash 里会被吞掉，所以这里用 Python 构造 env。
- IDF_MAINTAINER=1 跳过工具链版本校验（本机 IDF 5.5.2 + 工具链版本不完全对齐）。
"""
import os
import subprocess
import sys

IDF = r"C:\Users\upisq\esp\v5.5.2\esp-idf"
TC = r"C:\Users\upisq\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin"
NINJA_DIR = r"C:\Espressif\tools\ninja\1.12.1"
CMAKE_DIR = r"C:\Espressif\tools\cmake\3.30.2\bin"
PY_DIR = r"C:\Users\upisq\.espressif\python_env\idf5.5_py3.11_env\Scripts"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_ROOT = os.path.join(ROOT, "firmware", "s3", "build")

DROP_PREFIXES = ("MSYSTEM", "MINGW", "MSYS", "CYGWIN")
env = {k: v for k, v in os.environ.items() if not k.upper().startswith(DROP_PREFIXES)}
env["IDF_PATH"] = IDF
env["IDF_MAINTAINER"] = "1"
env["PATH"] = os.pathsep.join(
    [TC, NINJA_DIR, CMAKE_DIR, PY_DIR, env.get("PATH", "")]
)

target = sys.argv[1] if len(sys.argv) > 1 else ""
ninja = os.path.join(NINJA_DIR, "ninja.exe")
cmd = [ninja] + ([target] if target else [])

print("[build] cwd =", BUILD_ROOT)
print("[build] cmd =", " ".join(cmd))
sys.stdout.flush()
rc = subprocess.run(cmd, cwd=BUILD_ROOT, env=env).returncode
print("[build] exit =", rc)
sys.exit(rc)
