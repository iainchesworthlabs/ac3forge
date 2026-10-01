"""Unit tests for ir_compare.py: the IR of two trees read alike, so that only a change of meaning
is left.

stdlib `unittest`. The demangler is replaced by a table, so the cases need no LLVM; what is written
out is the IR as clang prints it for the forms the S6 trees gave: a function named by its mangled
name, a string constant that carries a qualified name, a typeinfo name, a name the demangler cannot
read.
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ir_compare as ir

OLD_ROOT = "/w/old/src"
NEW_ROOT = "/w/new/src"


def table(pairs: dict[str, str]) -> ir.Demangler:
    def demangler(names: list[str]) -> dict[str, str]:
        return {n: pairs.get(n, n) for n in names}

    return demangler


MANGLED_OLD = "_ZN8iclforge12FrameDecoder6decodeEv"
MANGLED_NEW = "_ZN8iclforge3ac312FrameDecoder6decodeEv"
DEMANGLE = table(
    {
        MANGLED_OLD: "iclforge::FrameDecoder::decode()",
        MANGLED_NEW: "iclforge::ac3::FrameDecoder::decode()",
        "_ZTSN8iclforge5FrameE": "typeinfo name for iclforge::Frame",
        "_ZTSN8iclforge3ac35FrameE": "typeinfo name for iclforge::ac3::Frame",
    }
)


class Convert(unittest.TestCase):
    CMD = (
        '/usr/bin/clang++-22 -DX=1 -DY="a b" -I/s/inc -O3 -DNDEBUG -g -std=c++23 -Werror -Wall '
        "-fcolor-diagnostics -MD -MT t.o -MF t.o.d -o src/x/CMakeFiles/x.dir/a.cpp.o "
        "-c /s/src/a.cpp"
    )

    def test_the_command_writes_unoptimised_ir_and_keeps_the_rest(self) -> None:
        cmd = ir.convert(self.CMD, "/o/a.ll")
        self.assertEqual(cmd[0], "/usr/bin/clang++-22")
        for kept in ("-DX=1", "-DY=a b", "-I/s/inc", "-DNDEBUG", "-std=c++23", "-Wall"):
            self.assertIn(kept, cmd)
        for dropped in (
            "-O3",
            "-g",
            "-Werror",
            "-fcolor-diagnostics",
            "-MD",
            "-MT",
            "t.o",
            "-MF",
            "-c",
        ):
            self.assertNotIn(dropped, cmd)
        self.assertEqual(
            cmd[-8:],
            ["-O0", "-g0", "-Wno-error", "-S", "-emit-llvm", "-o", "/o/a.ll", "/s/src/a.cpp"],
        )

    def test_asserts_are_switched_on_by_undefining_ndebug_after_the_definitions(self) -> None:
        cmd = ir.convert(self.CMD, "/o/a.ll", asserts=True)
        self.assertIn("-DNDEBUG", cmd)
        self.assertGreater(cmd.index("-UNDEBUG"), cmd.index("-DNDEBUG"))
        self.assertNotIn("-UNDEBUG", ir.convert(self.CMD, "/o/a.ll"))

    def test_the_source_is_the_last_argument_and_the_output_the_one_before_it(self) -> None:
        cmd = ir.convert(self.CMD, "/o/a.ll")
        self.assertEqual(cmd[-1], "/s/src/a.cpp")
        self.assertEqual(cmd[-3:-1], ["-o", "/o/a.ll"])
        self.assertNotIn("src/x/CMakeFiles/x.dir/a.cpp.o", cmd)


class UnitName(unittest.TestCase):
    def test_a_unit_of_the_tree_is_named_by_its_path_and_its_object(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            root = Path(t).resolve()
            (root / "src" / "src" / "ac3").mkdir(parents=True)
            (root / "build").mkdir()
            source = root / "src" / "src" / "ac3" / "a.cpp"
            source.write_text("", encoding="utf-8")
            a = ir.unit_name(
                str(source), "src/ac3/x.o", str(root / "build"), root / "build", root / "src"
            )
            self.assertTrue(
                a is not None and a.startswith("src__ac3__a.cpp.") and a.endswith(".ll")
            )

    def test_the_same_unit_in_two_trees_has_the_same_name(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            names = []
            for tree in ("old", "new"):
                root = Path(t).resolve() / tree
                (root / "src" / "src").mkdir(parents=True)
                (root / "build").mkdir()
                (root / "src" / "src" / "a.cpp").write_text("", encoding="utf-8")
                names.append(
                    ir.unit_name(
                        str(root / "src" / "src" / "a.cpp"),
                        str(root / "build" / "src" / "x.o"),
                        str(root / "build"),
                        root / "build",
                        root / "src",
                    )
                )
            self.assertIsNotNone(names[0])
            self.assertEqual(names[0], names[1])

    def test_a_source_outside_the_tree_is_not_a_unit(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            root = Path(t).resolve()
            (root / "src").mkdir()
            self.assertIsNone(
                ir.unit_name(str(root / "build" / "moc.cpp"), "x.o", str(root), root, root / "src")
            )


class Normalise(unittest.TestCase):
    def lines(self, text: str, new: bool) -> list[str]:
        return ir.normalise(text, NEW_ROOT if new else OLD_ROOT, new, DEMANGLE)

    def test_a_mangled_name_is_read_and_the_new_namespace_taken_out(self) -> None:
        old = f"define void @{MANGLED_OLD}(ptr %0) {{\n  ret void\n}}\n"
        new = f"define void @{MANGLED_NEW}(ptr %0) {{\n  ret void\n}}\n"
        self.assertEqual(self.lines(old, False), self.lines(new, True))
        self.assertIn("@iclforge::FrameDecoder::decode()", "\n".join(self.lines(new, True)))

    def test_the_trees_path_is_not_part_of_the_text(self) -> None:
        old = f'source_filename = "{OLD_ROOT}/src/ac3/a.cpp"\n'
        new = f'source_filename = "{NEW_ROOT}/src/ac3/a.cpp"\n'
        self.assertEqual(self.lines(old, False), self.lines(new, True))

    def test_a_constant_that_differs_is_a_difference(self) -> None:
        old = "  %1 = icmp eq i32 %0, 22584\n"
        new = "  %1 = icmp eq i32 %0, 2935\n"
        self.assertNotEqual(self.lines(old, False), self.lines(new, True))

    def test_a_string_that_prints_a_qualified_name_has_its_length_followed(self) -> None:
        body = "iclforge::io::scan failed\\00"
        new_body = "iclforge::ac3::io::scan failed\\00"
        old = (
            f"@.str.1 = private unnamed_addr constant [{ir.string_length(body)} x i8] "
            f'c"{body}", align 1\n'
        )
        new = (
            f"@.str.1 = private unnamed_addr constant [{ir.string_length(new_body)} x i8] "
            f'c"{new_body}", align 1\n'
        )
        self.assertNotEqual(old, new)
        self.assertEqual(self.lines(old, False), self.lines(new, True))

    def test_a_hex_escape_in_a_string_counts_one_byte(self) -> None:
        self.assertEqual(ir.string_length("ab\\0A\\00"), 4)

    def test_a_string_that_names_a_pretty_function_is_read_in_its_name_too(self) -> None:
        old = (
            "@__PRETTY_FUNCTION__.std::vector<iclforge::A>::back() = private unnamed_addr "
            'constant [25 x i8] c"std::vector<iclforge::A>\\00", align 1\n'
        )
        new = (
            "@__PRETTY_FUNCTION__.std::vector<iclforge::ac3::A>::back() = private unnamed_addr "
            'constant [30 x i8] c"std::vector<iclforge::ac3::A>\\00", align 1\n'
        )
        self.assertEqual(self.lines(old, False), self.lines(new, True))

    def test_the_typeinfo_name_of_a_type_is_not_compared(self) -> None:
        old = (
            '@"typeinfo name for iclforge::Frame" = linkonce_odr constant [16 x i8] '
            'c"N8iclforge5FrameE\\00"\n'
        )
        new = (
            '@"typeinfo name for iclforge::ac3::Frame" = linkonce_odr constant [21 x i8] '
            'c"N8iclforge3ac35FrameE\\00"\n'
        )
        self.assertEqual(self.lines(old, False), self.lines(new, True))

    def test_a_name_the_demangler_cannot_read_is_compared_without_the_component_and_the_indices(
        self,
    ) -> None:
        old = "call void @_ZZN8iclforge12_GLOBAL__N_110fold_blockERKNS0_8FoldPlanEE3$_0clEv()\n"
        new = "call void @_ZZN8iclforge3ac312_GLOBAL__N_110fold_blockERKNS1_8FoldPlanEE3$_0clEv()\n"
        self.assertEqual(self.lines(old, False), self.lines(new, True))

    def test_a_callee_that_differs_is_a_difference(self) -> None:
        old = f"  call void @{MANGLED_OLD}()\n"
        new = "  call void @_ZN8iclforge3ac35OtherEv()\n"
        self.assertNotEqual(self.lines(old, False), self.lines(new, True))

    def test_the_old_ir_is_not_rewritten(self) -> None:
        text = f"  call void @{MANGLED_NEW}()\n"
        self.assertIn("iclforge::ac3::FrameDecoder::decode()", "\n".join(self.lines(text, False)))


class Catch2(unittest.TestCase):
    def lines(self, text: str, new: bool) -> list[str]:
        return ir.normalise(text, NEW_ROOT if new else OLD_ROOT, new, DEMANGLE)

    def test_the_line_a_macro_passes_is_not_compared(self) -> None:
        old = (
            "  call void @Catch::SourceLineInfo::SourceLineInfo(char const*, unsigned long)"
            "(ptr noundef nonnull align 8 dereferenceable(16) %10, ptr noundef @.str, "
            "i64 noundef 53) #17\n"
        )
        new = old.replace("i64 noundef 53", "i64 noundef 54")
        self.assertEqual(self.lines(old, False), self.lines(new, True))

    def test_the_length_of_the_text_a_macro_stringifies_is_not_compared(self) -> None:
        old = (
            '  %40 = call { ptr, i64 } @operator"" _catch_sr(char const*, unsigned long)'
            "(ptr noundef @.str.24, i64 noundef 81) #17\n"
        )
        new = old.replace("i64 noundef 81", "i64 noundef 86")
        self.assertEqual(self.lines(old, False), self.lines(new, True))

    def test_a_template_made_over_a_string_literal_is_the_same_whatever_its_length(self) -> None:
        old = (
            "$Catch::MessageStream& Catch::MessageStream::operator<<<char [35]>"
            "(char const (&) [35]) = comdat any\n"
        )
        new = old.replace("35", "40")
        self.assertEqual(self.lines(old, False), self.lines(new, True))

    def test_a_pointer_attribute_over_such_a_literal_is_the_same_too(self) -> None:
        old = (
            "  %27 = invoke ptr @Catch::MessageStream::operator<<<char [35]>(char const (&) [35])"
            "(ptr dereferenceable(16) %8, ptr noundef nonnull align 1 dereferenceable(35) %27)\n"
        )
        new = old.replace("[35]", "[40]").replace("dereferenceable(35)", "dereferenceable(40)")
        self.assertEqual(self.lines(old, False), self.lines(new, True))

    def test_an_array_bound_outside_catch_is_still_compared(self) -> None:
        old = "  %1 = getelementptr inbounds [4 x i32], ptr %0, i64 0, i64 1\n"
        new = old.replace("[4 x i32]", "[5 x i32]")
        self.assertNotEqual(self.lines(old, False), self.lines(new, True))

    def test_the_bytes_a_string_is_indexed_over_are_not_compared(self) -> None:
        old = "  %9 = getelementptr inbounds [35 x i8], ptr %8, i64 0, i64 0\n"
        new = old.replace("[35 x i8]", "[40 x i8]")
        self.assertEqual(self.lines(old, False), self.lines(new, True))

    def test_the_build_directory_is_not_part_of_a_string(self) -> None:
        old = f'@.str = private constant [20 x i8] c"{OLD_ROOT[:-4]}/build/scratch\\00"\n'
        new = f'@.str = private constant [20 x i8] c"{NEW_ROOT[:-4]}/build/scratch\\00"\n'
        self.assertEqual(self.lines(old, False), self.lines(new, True))


TWO_FUNCTIONS = [
    "define void @f() {",
    "  ret void",
    "}",
    "",
    "define void @g() {",
    "  ret void",
    "}",
    "",
]


class Verdict(unittest.TestCase):
    def test_the_same_lines_are_identical(self) -> None:
        self.assertEqual(ir.verdict(TWO_FUNCTIONS, list(TWO_FUNCTIONS)), "identical")

    def test_the_same_blocks_in_another_order_are_the_same(self) -> None:
        b = [
            "define void @g() {",
            "  ret void",
            "}",
            "",
            "define void @f() {",
            "  ret void",
            "}",
            "",
        ]
        self.assertEqual(ir.verdict(TWO_FUNCTIONS, b), "order")

    def test_a_block_that_changed_is_a_difference(self) -> None:
        b = [
            "define void @f() {",
            "  unreachable",
            "}",
            "",
            "define void @g() {",
            "  ret void",
            "}",
            "",
        ]
        self.assertEqual(ir.verdict(TWO_FUNCTIONS, b), "differs")

    def test_white_space_in_the_text_of_an_assertion_is_not(self) -> None:
        old = '@.str.1 = private unnamed_addr constant [12 x i8] c"f(x).has_value()\\00", align 1'
        new = '@.str.1 = private unnamed_addr constant [13 x i8] c"f(x) .has_value()\\00", align 1'
        self.assertEqual(ir.verdict([old], [new]), "text")

    def test_white_space_elsewhere_is_a_difference(self) -> None:
        self.assertEqual(ir.verdict(["  %1 = add i32 %0, 1"], ["  %1 = add i32  %0, 1"]), "differs")

    def test_the_chunks_are_the_blocks_between_blank_lines(self) -> None:
        self.assertEqual(ir.by_chunks(["b", "b2", "", "a", "", "", "c"]), ["a", "b\nb2", "c"])


class Directories(unittest.TestCase):
    def test_the_comparison_is_clean_when_every_unit_is_the_same_modulo_names(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            old, new = Path(t, "old"), Path(t, "new")
            old.mkdir()
            new.mkdir()
            (old / "a.ll").write_text(
                f'source_filename = "{OLD_ROOT}/a.cpp"\n  %x = i32 1\n', encoding="utf-8"
            )
            (new / "a.ll").write_text(
                f'source_filename = "{NEW_ROOT}/a.cpp"\n  %x = i32 1\n', encoding="utf-8"
            )
            code = ir.compare_dirs(old, new, OLD_ROOT, NEW_ROOT, str(Path(t, "diffs")), 1)
            self.assertEqual(code, 0)

    def test_a_difference_is_reported_and_its_diff_written(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            old, new = Path(t, "old"), Path(t, "new")
            old.mkdir()
            new.mkdir()
            (old / "a.ll").write_text("  %x = i32 1\n", encoding="utf-8")
            (new / "a.ll").write_text("  %x = i32 2\n", encoding="utf-8")
            code = ir.compare_dirs(old, new, OLD_ROOT, NEW_ROOT, str(Path(t, "diffs")), 1)
            self.assertEqual(code, 1)
            self.assertIn("i32 2", Path(t, "diffs", "a.ll.diff").read_text(encoding="utf-8"))

    def test_a_unit_in_one_tree_only_fails(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            old, new = Path(t, "old"), Path(t, "new")
            old.mkdir()
            new.mkdir()
            (old / "a.ll").write_text("x\n", encoding="utf-8")
            self.assertEqual(ir.compare_dirs(old, new, OLD_ROOT, NEW_ROOT, None, 1), 1)


if __name__ == "__main__":
    unittest.main()
