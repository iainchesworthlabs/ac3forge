#!/usr/bin/env python3
"""Assert that the Android app's JNI names agree at every end that reads them.

The app is built in CI only (Gradle and the NDK), and a JNI name that one end spells differently
from the other does not fail a build: the native function is simply not found, and the first call
from Kotlin throws UnsatisfiedLinkError at run time. The package has been renamed once (N1A took
com.ac3forge.shield to com.iclforge.shield), and the Java_... names of about forty native functions
are that package written into the C++. This reads the files and checks, without a device:

  (a) apps/android/app/build.gradle.kts gives one name to `namespace` and to `applicationId`, every
      Kotlin source of the app's main and androidTest source sets declares that package, and sits
      in the directory the package names;
  (b) every `external fun` of a Kotlin class has a native definition whose name is the mangled
      package, class and function (Java_com_iclforge_shield_NativeBridge_nativeGetScene), and every
      native definition of that class has an `external fun`: a function on one side only is the
      UnsatisfiedLinkError this exists to prevent;
  (c) System.loadLibrary("x") names a library the app's CMakeLists.txt builds
      (add_library(x SHARED); the file is libx.so);
  (d) every class path the C++ hands to FindClass (com/iclforge/shield/Name) and every class the
      ProGuard rules keep names a class the Kotlin sources declare.

    python3 tools/checks/check_android_jni.py [--root <repo>]

Exit 1 with one ::error:: line per disagreement, naming file:line.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

APP = "apps/android/app"
NATIVE_DIRS = ("apps/android/app/src/main/cpp", "src/audio/src/backend/android")
# the app's Gradle source sets, under <app>/src: their Kotlin and the native code of `main`
SOURCE_SETS = ("main", "androidTest")
KOTLIN_DIRS = tuple("/".join(("src", name, "java")) for name in SOURCE_SETS)


@dataclass(frozen=True)
class Problem:
    where: str
    message: str

    def __str__(self) -> str:
        return f"::error file={self.where}::{self.message}"


def mangle(name: str) -> str:
    """One identifier the way the JNI spec writes it in a function name: `_` is `_1`."""
    out = []
    for ch in name:
        if ch == "_":
            out.append("_1")
        elif ch.isascii() and (ch.isalnum()):
            out.append(ch)
        else:
            out.append(f"_0{ord(ch):04x}")
    return "".join(out)


def jni_prefix(package: str, class_name: str) -> str:
    return (
        "Java_" + "_".join(mangle(p) for p in package.split(".")) + "_" + mangle(class_name) + "_"
    )


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def gradle_names(text: str) -> dict[str, tuple[str, int]]:
    found: dict[str, tuple[str, int]] = {}
    for number, line in enumerate(text.splitlines(), 1):
        m = re.match(r'\s*(namespace|applicationId)\s*=\s*"([^"]+)"', line)
        if m:
            found[m.group(1)] = (m.group(2), number)
    return found


PACKAGE_RX = re.compile(r"^package\s+([\w.]+)", re.M)
TOP_LEVEL_RX = re.compile(
    r"^(?:(?:public|internal|private|open|abstract|final|data)\s+)*(?:object|class)\s+(\w+)", re.M
)
EXTERNAL_RX = re.compile(r"\bexternal\s+fun\s+(\w+)\s*\(")
NATIVE_RX = re.compile(r"\b(Java_[A-Za-z0-9_]+)\s*\(")
LOAD_RX = re.compile(r'System\.loadLibrary\(\s*"([^"]+)"\s*\)')
ADD_LIBRARY_RX = re.compile(r"add_library\(\s*([\w.+-]+)\s+SHARED", re.I)
FIND_CLASS_RX = re.compile(r'FindClass\(\s*"([^"]+)"')
KEEP_RX = re.compile(r"^\s*-keep\w*\s+(?:,\w+\s+)*class\s+([\w.$]+)", re.M)


def kotlin_sources(root: Path) -> list[Path]:
    files: list[Path] = []
    for sub in KOTLIN_DIRS:
        base = root / APP / sub
        if base.is_dir():
            files += sorted(base.rglob("*.kt"))
    return files


def check(root: Path) -> list[Problem]:
    problems: list[Problem] = []
    gradle = root / APP / "build.gradle.kts"
    names = gradle_names(read(gradle)) if gradle.is_file() else {}
    rel_gradle = f"{APP}/build.gradle.kts"
    if set(names) != {"namespace", "applicationId"}:
        return [Problem(rel_gradle, "namespace and applicationId must both be set")]
    namespace, application = names["namespace"][0], names["applicationId"][0]
    if namespace != application:
        problems.append(
            Problem(
                f"{rel_gradle}:{names['applicationId'][1]}",
                f'applicationId "{application}" is not the namespace "{namespace}" the code is in',
            )
        )
    package = namespace

    # (a) the Kotlin sources
    classes: dict[str, str] = {}  # class name -> the file it is declared in
    external: dict[str, tuple[str, int]] = {}  # (class, function) key as "Class.fn" -> where
    for path in kotlin_sources(root):
        rel = path.relative_to(root).as_posix()
        text = read(path)
        m = PACKAGE_RX.search(text)
        declared = m.group(1) if m else ""
        if declared != package:
            problems.append(Problem(rel, f'declares package "{declared}", not "{package}"'))
        directory = path.parent.relative_to(root / APP).as_posix()
        if not directory.endswith("/" + package.replace(".", "/")):
            problems.append(
                Problem(rel, f"sits in {directory}, which is not the directory of {package}")
            )
        tops = TOP_LEVEL_RX.findall(text)
        for top in tops:
            classes.setdefault(top, rel)
        externals = list(EXTERNAL_RX.finditer(text))
        if externals:
            if len(tops) != 1:
                problems.append(
                    Problem(rel, "external functions in a file with no single class to own them")
                )
                continue
            for m in externals:
                line = text.count("\n", 0, m.start()) + 1
                external[f"{tops[0]}.{m.group(1)}"] = (f"{rel}:{line}", line)

    # (b) the native definitions
    natives: dict[str, str] = {}  # function name -> file:line
    for sub in NATIVE_DIRS:
        base = root / sub
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".cpp", ".cc", ".c", ".hpp", ".h") or not path.is_file():
                continue
            text = read(path)
            for m in NATIVE_RX.finditer(text):
                # a definition or a declaration; a mention in a comment is not one
                start = text.rfind("\n", 0, m.start()) + 1
                prefix = text[start : m.start()].strip()
                if prefix.startswith(("//", "*", "/*", "#")):
                    continue
                line = text.count("\n", 0, m.start()) + 1
                natives.setdefault(m.group(1), f"{path.relative_to(root).as_posix()}:{line}")
    owned_prefixes = {jni_prefix(package, cls) for cls in {k.split(".")[0] for k in external}}
    for key, (where, _) in sorted(external.items()):
        cls, fn = key.split(".")
        expected = jni_prefix(package, cls) + mangle(fn)
        if expected not in natives:
            problems.append(
                Problem(where, f"`external fun {fn}` has no native definition {expected}")
            )
    declared_functions = {
        jni_prefix(package, k.split(".")[0]) + mangle(k.split(".")[1]) for k in external
    }
    for name, where in sorted(natives.items()):
        if (
            any(name.startswith(prefix) for prefix in owned_prefixes)
            and name not in declared_functions
        ):
            problems.append(Problem(where, f"native {name} has no `external fun` in its class"))
        elif not any(name.startswith(prefix) for prefix in owned_prefixes):
            problems.append(
                Problem(
                    where,
                    f"native {name} is not in package {package} of a class with external functions",
                )
            )

    # (c) the library
    cmake = root / APP / "src" / "main" / "cpp" / "CMakeLists.txt"
    built = set(ADD_LIBRARY_RX.findall(read(cmake))) if cmake.is_file() else set()
    for path in kotlin_sources(root):
        for m in LOAD_RX.finditer(read(path)):
            if m.group(1) not in built:
                line = read(path).count("\n", 0, m.start()) + 1
                problems.append(
                    Problem(
                        f"{path.relative_to(root).as_posix()}:{line}",
                        f'loads library "{m.group(1)}", which {cmake.relative_to(root).as_posix()} '
                        "does not build",
                    )
                )

    # (d) class paths the C++ and ProGuard name
    slash = package.replace(".", "/")
    for sub in NATIVE_DIRS:
        base = root / sub
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".cpp", ".cc", ".c", ".hpp", ".h") or not path.is_file():
                continue
            text = read(path)
            for m in FIND_CLASS_RX.finditer(text):
                cls = m.group(1)
                line = text.count("\n", 0, m.start()) + 1
                where = f"{path.relative_to(root).as_posix()}:{line}"
                if cls.startswith(slash + "/"):
                    if cls[len(slash) + 1 :].split("$")[0] not in classes:
                        problems.append(
                            Problem(
                                where, f'FindClass("{cls}") names no class of the Kotlin sources'
                            )
                        )
                elif "iclforge" in cls or "ac3forge" in cls:
                    problems.append(
                        Problem(where, f'FindClass("{cls}") is not in package {package}')
                    )
    proguard = root / APP / "proguard-rules.pro"
    if proguard.is_file():
        text = read(proguard)
        for m in KEEP_RX.finditer(text):
            cls = m.group(1)
            line = text.count("\n", 0, m.start()) + 1
            if cls.startswith(package + "."):
                if cls[len(package) + 1 :].split("$")[0] not in classes:
                    problems.append(
                        Problem(
                            f"{APP}/proguard-rules.pro:{line}",
                            f"keeps {cls}, which no Kotlin source declares",
                        )
                    )
            elif cls.startswith(("com.iclforge", "com.ac3forge")):
                problems.append(
                    Problem(
                        f"{APP}/proguard-rules.pro:{line}",
                        f"keeps {cls}, which is not in package {package}",
                    )
                )
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = ap.parse_args(argv)
    problems = check(args.root)
    for problem in problems:
        print(problem)
    if problems:
        return 1
    print(f"android jni: {APP} agrees at every end (package, native names, library, class paths)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
