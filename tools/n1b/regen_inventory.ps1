# Re-run every census script against one worktree and write the results into a data directory, from which
# appendix.py writes planning/layout-inventory.md.
#
# Usage: regen_inventory.ps1 -Root <worktree> -Out <dir> [-Commit <sha> -Date <yyyy-mm-dd> -Appendix <file>]
#
# The scripts read the tracked files of -Root and write nothing into it. With -Appendix the last step
# assembles the appendix from what the others wrote.
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Commit = '?',
    [string]$Date = (Get-Date -Format 'yyyy-MM-dd'),
    [string]$Appendix = ''
)
$ErrorActionPreference = 'Stop'
$env:PYTHONDONTWRITEBYTECODE = '1'
New-Item -ItemType Directory -Force $Out | Out-Null
Set-Location $PSScriptRoot

function Step([string]$name, [scriptblock]$body) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $body
    if ($LASTEXITCODE -ne 0) { throw "$name failed ($LASTEXITCODE)" }
    '{0,-16} {1,6:N1} s' -f $name, $sw.Elapsed.TotalSeconds
}

Step 'include_graph' { python include_graph.py --root $Root --json "$Out\include_graph.json" --md "$Out\include_graph.md" | Out-Null }
Step 'inventory'     { python inventory.py --root $Root --graph "$Out\include_graph.json" --json "$Out\inventory.json" --md "$Out\inventory.md" | Out-Null }
Step 'namespaces'    { python namespaces.py --root $Root --json "$Out\namespaces.json" --md "$Out\namespaces.md" | Out-Null }
Step 'ident_census'  { python ident_census.py --root $Root --json "$Out\ident_census.json" --md "$Out\ident_census.md" | Out-Null }
Step 'naming_counts' { python naming_counts.py --root $Root --json "$Out\naming_counts.json" --md "$Out\naming_counts.md" | Out-Null }
Step 'path_keyed'    { python path_keyed.py --root $Root --json "$Out\path_keyed.json" --md "$Out\path_keyed.md" | Out-Null }
Step 'overlap'       { python overlap.py --root $Root --json "$Out\overlap.json" --md "$Out\overlap.md" | Out-Null }
Step 'reflow'        { python reflow_cost.py --root $Root --out "$Out\reflow.md" }
Step 'dryrun'        { python layout_dryrun.py --root $Root --graph "$Out\include_graph.json" --pathkeys "$Out\path_keyed.json" --json "$Out\dryrun.json" --md "$Out\dryrun.md" | Out-Null }
Step 'violations'    { python violations.py --root $Root --graph "$Out\include_graph.json" --out "$Out\violations_v2.txt" }
Step 'privcross'     { python privcross.py --root $Root --graph "$Out\include_graph.json" --out "$Out\privcross.txt" }
Step 'symtab'        { python symtab.py --root $Root --json "$Out\symtab.json" | Out-Null }
Step 'cmake_repeat'  { python cmake_repeat.py --root $Root --out "$Out\cmake_repeat.md" }
# The TrueHD branch is not always fetched; the appendix leaves its section out without the file.
python truehd_includes.py --repo $Root 2> $null | Out-File "$Out\truehd_includes.txt" -Encoding utf8
if ($Appendix) {
    Step 'appendix'  { python appendix.py --data $Out --commit $Commit --date $Date --out $Appendix }
}
