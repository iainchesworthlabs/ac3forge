# Known layering debts

[`check_layering.py`](../check_layering.py) fails an `#include` that crosses from one library of
`src/` into another that the includer's row of [`layering.json`](../layering.json) does not list. The
includes that a pending cut of the re-layout still removes (stage S1 of
[the layout study](../../../planning/layout.md)) are listed here instead, one `.txt` file per cut, so
that two cuts landing together never edit the same lines.

A line is `<path of the including file> <the spelling it includes>`; blank lines and lines that begin
with `#` are ignored. A listed include is reported as known and does not fail. A listed include that
is no longer in the tree fails, which makes the change that removes it delete its line, and the change
that removes a cut's last include delete the file. With no `.txt` file left the check has no debts;
this README keeps the directory in the tree.

`check_layering.py --debt-lines` prints every include the table forbids in the form a file takes here,
which is how a cut's file is written.
