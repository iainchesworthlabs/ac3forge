# Negative control for adapt_branch.ps1 -Measure: a branch that edits a line the hand-written commit also
# rewrote must show up as a conflict, or a result of zero conflicts with the scripts run first proves nothing.
#
# Usage: control_experiment.ps1 -Root <worktree of the migrated tree, clean> -Scripted <ref> -Migrated <ref>
#                               -File <path in the scripted tree> -Anchor <text on a line the hand-written commit rewrote>
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Scripted,
    [Parameter(Mandatory = $true)][string]$Migrated,
    [Parameter(Mandatory = $true)][string]$File,
    [Parameter(Mandatory = $true)][string]$Anchor
)
$ErrorActionPreference = 'Stop'
function G { git -C $Root @args }
if (G status --porcelain) { throw "$Root is not clean" }
G checkout -q -B proto/control $Scripted
$path = Join-Path $Root $File
$text = [IO.File]::ReadAllText($path)
if (-not $text.Contains($Anchor)) { throw 'anchor text not found' }
[IO.File]::WriteAllText($path, $text.Replace($Anchor, "$Anchor (control edit)"), (New-Object Text.UTF8Encoding($false)))
G commit -q -am 'control: edit a line the hand-written commit rewrote'
"--- control (edits $File), scripts first:"
G merge-tree --write-tree --name-only --no-messages "--merge-base=$Scripted" $Migrated proto/control
"--- control, merged by hand (no shared base):"
G merge-tree --write-tree --name-only --no-messages $Migrated proto/control
G checkout -q $Migrated
