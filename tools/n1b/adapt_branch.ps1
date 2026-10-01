# Adapt an open branch to a migration stage of planning/layout.md, or measure how it would fare.
#
# A stage rewrites most of the tree (moves, include spellings, names). Merging it by hand puts a developer's
# edits against a tree where every include, namespace and path has changed. Running the stage's own scripts on
# the branch first, and merging afterwards, leaves only the lines the branch itself changed that a hand-written
# commit also changed. Four steps, in order:
#   1. the branch contains the main that precedes the stage (the merge below reverts what it lacks);
#   2. run the stage's scripts on the branch and commit each;
#   3. `git merge -s ours <the stage's last scripted commit>` records the scripted commits as merged and keeps
#      the branch's tree;
#   4. `git merge <main>` at the stage's last commit: the merge base is now the scripts' own output, so the
#      hand-written commits that follow it are the only changes the branch has to reconcile.
#
# The scripts Run-Scripts runs are S2's and S3's (n1b_apply.py, n1b_cmake.py, n1b_paths.py, n1b_names.py). S4's
# (n1b_idents.py, n1b_sendspin.py), N1A's (n1b_programs.py) and S5's (n1b_docs.py, three phases) are not in it: a
# branch that predates them runs those by hand, in the order of the start-to-finish sections of README.md, and
# records each stage's scripted commits as merged the same way (steps 3 and 4).
#
#   -Apply    does steps 1 to 4 in the worktree -Root, which must be clean and on the branch.
#   -Measure  counts, without touching the branch, the files `git merge-tree` reports as conflicted when the
#             branch is merged by hand and when the scripts have run on it first (the table in planning/layout.md);
#             it needs a worktree of the migrated tree (-Root) and writes one result_<branch>.json to -Data.
#
# Usage:
#   adapt_branch.ps1 -Apply   -Root <worktree of the branch> -Base <main before the stage> -Scripted <ref> [-Main github/main]
#   adapt_branch.ps1 -Measure -Root <worktree of the migrated tree, clean> -Branch <name on github> -Migrated <ref>
#                    -Scripted <ref> -Cuts <ref> -Data <dir>
#     -Migrated  the migrated tree's tip (scripts plus the hand-written commits)
#     -Scripted  the commit that is the scripts' own output on main and nothing else
#     -Cuts      the main the scripts are run on (the last commit before the stage)
param(
    [switch]$Apply,
    [switch]$Measure,
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$Base = '',
    [string]$Scripted = '',
    [string]$Main = 'github/main',
    [string]$Branch = '',
    [string]$Migrated = '',
    [string]$Cuts = '',
    [string]$Data = ''
)
$ErrorActionPreference = 'Stop'
$env:PYTHONDONTWRITEBYTECODE = '1'
$tools = $PSScriptRoot
function G { git -C $Root @args }

# The stage's scripts, in the order a stage runs them. The moves are staged by `git mv` and the include edits
# are not, so the first commit holds the renames alone.
function Run-Scripts([string]$plan) {
    python "$tools\n1b_apply.py" --root $Root --phase all --quiet --json $plan
    if ($LASTEXITCODE) { throw 'n1b_apply.py failed' }
    G commit -q -m 'scripts: move the files' 2>$null
    G add -A
    G commit -q -m 'scripts: include spellings' 2>$null
    python "$tools\n1b_cmake.py" --root $Root --plan $plan
    if ($LASTEXITCODE) { throw 'n1b_cmake.py failed' }
    G add -A
    G commit -q -m 'scripts: build files' 2>$null
    python "$tools\n1b_paths.py" --root $Root --plan $plan
    if ($LASTEXITCODE) { throw 'n1b_paths.py failed' }
    G add -A
    G commit -q -m 'scripts: paths in text' 2>$null
    python "$tools\n1b_names.py" --root $Root
    if ($LASTEXITCODE) { throw 'n1b_names.py failed' }
    G add -A
    G commit -q -m 'scripts: names' 2>$null
}

function Count-Conflicts([string]$a, [string]$b, [string]$mergeBase = '') {
    if ($mergeBase) {
        $out = G merge-tree --write-tree --name-only --no-messages "--merge-base=$mergeBase" $a $b 2>&1
    } else {
        $out = G merge-tree --write-tree --name-only --no-messages $a $b 2>&1
    }
    $rc = $LASTEXITCODE
    $lines = @($out)
    $paths = @()
    if ($lines.Count -gt 1) { $paths = @($lines[1..($lines.Count - 1)] | Where-Object { $_ -and $_ -notmatch '^\s*$' }) }
    return [pscustomobject]@{ Exit = $rc; Paths = $paths }
}

if ($Apply -eq $Measure) { throw 'give exactly one of -Apply and -Measure' }
if (G status --porcelain) { throw "$Root is not clean" }

if ($Apply) {
    if (-not $Base -or -not $Scripted) { throw '-Apply needs -Base and -Scripted' }
    G merge-base --is-ancestor $Base HEAD
    if ($LASTEXITCODE) { throw "the branch does not contain $Base: merge it first (never rebase a pushed branch)" }
    $plan = Join-Path ([IO.Path]::GetTempPath()) ("n1b-plan-" + [guid]::NewGuid().ToString('N') + '.json')
    Run-Scripts $plan
    G merge -s ours -q -m "merge the scripted commits of $Scripted" $Scripted
    if ($LASTEXITCODE) { throw "git merge -s ours $Scripted failed" }
    G merge --no-edit $Main
    if ($LASTEXITCODE) {
        'conflicts, each a line the branch changed and the hand-written commits changed too:'
        G diff --name-only --diff-filter=U
        exit 1
    }
    'merged cleanly'
    return
}

foreach ($v in 'Branch', 'Migrated', 'Scripted', 'Cuts', 'Data') {
    if (-not (Get-Variable $v -ValueOnly)) { throw "-Measure needs -$v" }
}
New-Item -ItemType Directory -Force $Data | Out-Null
$slug = (($Branch -replace '[^A-Za-z0-9]+', '-').Trim('-'))
$work = "proto/adapt-$slug"
$tip = (G rev-parse --short "github/$Branch").Trim()
$base = (G merge-base "github/$Branch" $Main).Trim().Substring(0, 9)
$touched = @(G diff --name-only $base "github/$Branch")
$naive = Count-Conflicts $Migrated "github/$Branch"

G checkout -q -B $work "github/$Branch"
$null = G merge --no-edit -q $Cuts 2>&1
if ($LASTEXITCODE -ne 0) {
    G merge --abort 2>$null
    G checkout -q $Migrated
    [pscustomobject]@{ Branch = $Branch; Tip = $tip; Files = $touched.Count; Naive = $naive.Paths.Count; Note = 'the stage before conflicts' } | ConvertTo-Json -Compress
    return
}
$pre = (G rev-parse HEAD).Trim()
Run-Scripts (Join-Path $Data "plan_$slug.json")
$changed = @(G diff --name-status -M $pre HEAD)
$oldNames = @{}
foreach ($l in $changed) { $oldNames[($l -split "`t")[1]] = 1 }
$scriptOnBranch = @($touched | Where-Object { $oldNames.ContainsKey($_) }).Count
$renamed = @($changed | Where-Object { $_ -match '^R' }).Count
$first = Count-Conflicts $Migrated $work $Scripted
$scripted = Count-Conflicts $Scripted $work $Scripted
[pscustomobject]@{
    Branch = $Branch; Tip = $tip; Base = $base; Files = $touched.Count
    ScriptTouchesOfThose = $scriptOnBranch; ScriptChangedFiles = $changed.Count; ScriptRenames = $renamed
    Naive = $naive.Paths.Count; NaivePaths = ($naive.Paths -join ';')
    ScriptFirst = $first.Paths.Count; ScriptFirstPaths = ($first.Paths -join ';')
    MergedAtScripted = $scripted.Paths.Count
} | ConvertTo-Json -Compress | Out-File "$Data\result_$slug.json" -Encoding utf8
Get-Content "$Data\result_$slug.json"
G checkout -q $Migrated
