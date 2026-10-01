"""Unit tests for check_android_jni.py.

stdlib `unittest`. Each case builds the smallest Android app tree that has the thing being checked
(a package, a Kotlin class with external functions, the native definitions, the library, a class
path, a ProGuard rule), proves the check is quiet on it, and then breaks one end.
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_android_jni as C

GRADLE = (
    "android {\n"
    '    namespace = "com.example.shield"\n'
    "    defaultConfig {\n"
    '        applicationId = "com.example.shield"\n'
    "    }\n"
    "}\n"
)
BRIDGE = """package com.example.shield

object NativeBridge {
    init {
        System.loadLibrary("example_jni")
    }
    external fun nativeVersion(): String
    external fun nativeSetScene(scene: Int)
    external fun nativeGet_value(): Int
}
"""
NATIVE = """#include <jni.h>
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_shield_NativeBridge_nativeVersion(JNIEnv* env, jclass) { return nullptr; }
extern "C" JNIEXPORT void JNICALL
Java_com_example_shield_NativeBridge_nativeSetScene(JNIEnv*, jclass, jint) {}
extern "C" JNIEXPORT jint JNICALL
Java_com_example_shield_NativeBridge_nativeGet_1value(JNIEnv*, jclass) { return 0; }
"""
CMAKE = "project(example_jni_project LANGUAGES CXX)\nadd_library(example_jni SHARED jni.cpp)\n"
PROGUARD = (
    "-keepclasseswithmembernames class com.example.shield.NativeBridge {\n"
    "    native <methods>;\n"
    "}\n"
)


class Fixture(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self.put("apps/android/app/build.gradle.kts", GRADLE)
        self.put("apps/android/app/src/main/java/com/example/shield/NativeBridge.kt", BRIDGE)
        self.put(
            "apps/android/app/src/main/java/com/example/shield/Main.kt",
            "package com.example.shield\n\nclass Main\n",
        )
        self.put("apps/android/app/src/main/cpp/jni.cpp", NATIVE)
        self.put("apps/android/app/src/main/cpp/CMakeLists.txt", CMAKE)
        self.put("apps/android/app/proguard-rules.pro", PROGUARD)

    def put(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")

    def messages(self) -> list[str]:
        return [p.message for p in C.check(self.root)]


class Agreement(Fixture):
    def test_a_tree_whose_ends_agree_is_quiet(self) -> None:
        self.assertEqual(self.messages(), [])

    def test_mangling_follows_the_jni_spec(self) -> None:
        self.assertEqual(C.mangle("nativeGet_value"), "nativeGet_1value")
        self.assertEqual(
            C.jni_prefix("com.iclforge.shield", "NativeBridge"),
            "Java_com_iclforge_shield_NativeBridge_",
        )


class Package(Fixture):
    def test_an_application_id_that_is_not_the_namespace(self) -> None:
        self.put(
            "apps/android/app/build.gradle.kts",
            GRADLE.replace(
                'applicationId = "com.example.shield"', 'applicationId = "com.other.shield"'
            ),
        )
        self.assertTrue(any("is not the namespace" in m for m in self.messages()))

    def test_a_source_in_the_old_package(self) -> None:
        self.put(
            "apps/android/app/src/main/java/com/example/shield/Main.kt",
            "package com.old.shield\n\nclass Main\n",
        )
        self.assertTrue(any('declares package "com.old.shield"' in m for m in self.messages()))

    def test_a_source_in_the_wrong_directory(self) -> None:
        self.put(
            "apps/android/app/src/main/java/com/old/shield/Late.kt",
            "package com.example.shield\n\nclass Late\n",
        )
        self.assertTrue(
            any("is not the directory of com.example.shield" in m for m in self.messages())
        )

    def test_the_instrumented_tests_are_held_to_the_same_package(self) -> None:
        self.put(
            "apps/android/app/src/androidTest/java/com/old/shield/T.kt",
            "package com.old.shield\n\nclass T\n",
        )
        self.assertTrue(any("declares package" in m for m in self.messages()))


class Native(Fixture):
    def test_an_external_function_with_no_definition(self) -> None:
        self.put(
            "apps/android/app/src/main/cpp/jni.cpp",
            NATIVE.replace("nativeSetScene", "nativeSetSceneX"),
        )
        found = self.messages()
        self.assertTrue(
            any("`external fun nativeSetScene` has no native definition" in m for m in found)
        )
        self.assertTrue(any("nativeSetSceneX has no `external fun`" in m for m in found))

    def test_a_definition_in_the_old_package(self) -> None:
        self.put(
            "apps/android/app/src/main/cpp/jni.cpp",
            NATIVE.replace("Java_com_example_shield", "Java_com_old_shield", 1),
        )
        found = self.messages()
        self.assertTrue(any("is not in package com.example.shield" in m for m in found))
        self.assertTrue(
            any("`external fun nativeVersion` has no native definition" in m for m in found)
        )

    def test_a_comment_that_names_a_function_is_not_a_definition(self) -> None:
        self.put(
            "apps/android/app/src/main/cpp/jni.cpp",
            NATIVE + "// Java_com_old_shield_NativeBridge_gone(JNIEnv*) was here\n",
        )
        self.assertEqual(self.messages(), [])

    def test_a_definition_outside_the_app_directory_counts(self) -> None:
        self.put(
            "apps/android/app/src/main/cpp/jni.cpp",
            NATIVE.replace("nativeVersion", "nativeVersionGone"),
        )
        self.put(
            "src/audio/src/backend/android/passthrough.cpp",
            'extern "C" void Java_com_example_shield_NativeBridge_nativeVersion(JNIEnv*, jclass)'
            " {}\n",
        )
        found = self.messages()
        self.assertFalse(
            any("`external fun nativeVersion` has no native definition" in m for m in found)
        )


class Library(Fixture):
    def test_a_library_the_build_does_not_make(self) -> None:
        self.put(
            "apps/android/app/src/main/cpp/CMakeLists.txt",
            CMAKE.replace("example_jni SHARED", "other_jni SHARED"),
        )
        self.assertTrue(any('loads library "example_jni"' in m for m in self.messages()))


class ClassPaths(Fixture):
    def test_find_class_of_a_class_that_exists(self) -> None:
        self.put(
            "apps/android/app/src/main/cpp/jni.cpp",
            NATIVE + 'auto c = env->FindClass("com/example/shield/Main");\n',
        )
        self.assertEqual(self.messages(), [])

    def test_find_class_of_a_class_that_does_not(self) -> None:
        self.put(
            "apps/android/app/src/main/cpp/jni.cpp",
            NATIVE + 'auto c = env->FindClass("com/example/shield/Missing");\n',
        )
        self.assertTrue(any("names no class of the Kotlin sources" in m for m in self.messages()))

    def test_find_class_in_the_old_package(self) -> None:
        self.put(
            "apps/android/app/src/main/cpp/jni.cpp",
            NATIVE + 'auto c = env->FindClass("com/ac3forge/shield/Main");\n',
        )
        self.assertTrue(any("is not in package com.example.shield" in m for m in self.messages()))

    def test_a_proguard_rule_for_a_class_that_does_not_exist(self) -> None:
        self.put(
            "apps/android/app/proguard-rules.pro",
            PROGUARD + "-keep class com.example.shield.Missing { *; }\n",
        )
        self.assertTrue(
            any(
                "keeps com.example.shield.Missing, which no Kotlin source declares" in m
                for m in self.messages()
            )
        )

    def test_a_proguard_rule_for_the_old_package(self) -> None:
        self.put(
            "apps/android/app/proguard-rules.pro",
            PROGUARD + "-keep class com.ac3forge.shield.Main { *; }\n",
        )
        self.assertTrue(any("is not in package com.example.shield" in m for m in self.messages()))


class TheTree(unittest.TestCase):
    def test_the_repository_agrees(self) -> None:
        root = Path(__file__).resolve().parents[2]
        if not (root / C.APP).is_dir():
            self.skipTest("no apps/android in this tree")
        self.assertEqual([str(p) for p in C.check(root)], [])


if __name__ == "__main__":
    unittest.main()
