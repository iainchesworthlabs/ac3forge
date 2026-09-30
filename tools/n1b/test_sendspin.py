"""Unit tests for n1b_sendspin.py: the rename of `sendspin::ac3forge` to `sendspin::player`.

stdlib `unittest`. The cases are lines written out here, in the forms the tree has them: a
declaration and its closing comment, a qualified use through the family root, the alias `ss` and
the fully spelled name, a namespace alias whose right side is the old namespace, and the
unqualified name inside `iclforge::sendspin` itself. What must not change is as much of the test as
what must: the ESP-IDF component's own `ac3forge::` namespace and the names that only start with
the old word.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import n1b_sendspin as s

SENDSPIN_HEADER = "src/sendspin/include/iclforge/sendspin/ac3forge_player.hpp"


class Qualified(unittest.TestCase):
    def check(self, before: str, after: str, path: str = "apps/hearth/engine/x.cpp") -> None:
        self.assertEqual(s.transform(path, before), after)

    def test_a_declaration_and_its_closing_comment(self) -> None:
        self.check(
            "namespace iclforge::sendspin::ac3forge {\n"
            "}  // namespace iclforge::sendspin::ac3forge\n",
            "namespace iclforge::sendspin::player {\n}  // namespace iclforge::sendspin::player\n",
            SENDSPIN_HEADER,
        )

    def test_every_way_of_qualifying_it(self) -> None:
        self.check(
            "auto a = sendspin::ac3forge::DataType::kAc3;\n"
            "auto b = ss::ac3forge::Settings{};\n"
            "auto c = ::iclforge::sendspin::ac3forge::State{};\n"
            "std::optional<iclforge::sendspin::ac3forge::Support> d;\n",
            "auto a = sendspin::player::DataType::kAc3;\n"
            "auto b = ss::player::Settings{};\n"
            "auto c = ::iclforge::sendspin::player::State{};\n"
            "std::optional<iclforge::sendspin::player::Support> d;\n",
        )

    def test_the_right_side_of_a_namespace_alias(self) -> None:
        self.check(
            "namespace ac = ss::ac3forge;\n"
            "namespace forge = iclforge::sendspin::ac3forge;\n"
            "namespace ac = sendspin::ac3forge;\n",
            "namespace ac = ss::player;\n"
            "namespace forge = iclforge::sendspin::player;\n"
            "namespace ac = sendspin::player;\n",
        )

    def test_a_comment_that_quotes_it(self) -> None:
        self.check(
            "// (iclforge::sendspin::ac3forge::DataType names, in the order the sink listed them\n",
            "// (iclforge::sendspin::player::DataType names, in the order the sink listed them\n",
        )


class Unqualified(unittest.TestCase):
    def test_inside_sendspin_the_bare_name_is_this_namespace(self) -> None:
        text = "    if (key == ac3forge::kObjectKey) {\n"
        self.assertEqual(
            s.transform("src/sendspin/src/messages.cpp", text),
            "    if (key == player::kObjectKey) {\n",
        )
        self.assertEqual(
            s.transform(SENDSPIN_HEADER, "std::optional<ac3forge::State> x;\n"),
            "std::optional<player::State> x;\n",
        )

    def test_the_controller_comment_that_quotes_it(self) -> None:
        text = "    // sink's own support object lists (ac3forge::kDecoderSettingNames) -\n"
        self.assertEqual(
            s.transform("apps/hearth/ui/network_controller.hpp", text),
            "    // sink's own support object lists (player::kDecoderSettingNames) -\n",
        )

    def test_outside_it_the_bare_name_is_the_esp_component(self) -> None:
        for path in (
            "apps/hearth/engine/sink_firmware.cpp",
            "tests/ac3/io/test_interleave.cpp",
            "esp-idf/iclforge/src/tcp_arrivals.cpp",
        ):
            with self.subTest(path):
                text = (
                    "    ac3forge::FirmwareSlot slot;\n    namespace ta = ac3forge::tcp_arrivals;\n"
                )
                self.assertEqual(s.transform(path, text), text)


class LeftAlone(unittest.TestCase):
    def test_names_that_only_start_with_the_old_word(self) -> None:
        text = (
            '#include "iclforge/sendspin/ac3forge_player.hpp"\n'
            "    client.ac3forge_support = ss::ac3forge_state{};\n"
            "    .ac3forge = std::nullopt,\n"
            '    "_ac3forge_player@v1"\n'
        )
        self.assertEqual(s.transform("src/sendspin/src/messages.cpp", text), text)

    def test_a_second_run_changes_nothing(self) -> None:
        once = s.transform(
            "src/sendspin/src/messages.cpp",
            "namespace ac = ss::ac3forge;\nauto x = ac3forge::State{};\n",
        )
        self.assertEqual(s.transform("src/sendspin/src/messages.cpp", once), once)


class Problems(unittest.TestCase):
    def test_ss_without_its_alias_is_listed(self) -> None:
        before = "auto b = ss::ac3forge::Settings{};\n"
        found = s.problems("a.cpp", before, s.transform("a.cpp", before))
        self.assertEqual(len(found), 1)
        self.assertIn("ss::ac3forge", found[0])

    def test_ss_with_its_alias_is_not(self) -> None:
        before = "namespace ss = iclforge::sendspin;\nauto b = ss::ac3forge::Settings{};\n"
        self.assertEqual(s.problems("a.cpp", before, s.transform("a.cpp", before)), [])


if __name__ == "__main__":
    unittest.main()
