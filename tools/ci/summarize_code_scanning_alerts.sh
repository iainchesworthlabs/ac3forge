#!/usr/bin/env bash
# Summarize open GitHub Code Scanning alerts for triage after dependency bumps.
# Requires: gh auth login (read:security_events on the repository).
#
# Usage:
#   tools/ci/summarize_code_scanning_alerts.sh
#   tools/ci/summarize_code_scanning_alerts.sh --since 2026-09-15
set -euo pipefail

since=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --since) since="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [--since YYYY-MM-DD]"
      exit 0
      ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

repo="${GITHUB_REPOSITORY:-$(gh repo view --json nameWithOwner -q .nameWithOwner)}"

jq_filter='.[] | select(.state=="open")'
if [[ -n "$since" ]]; then
  jq_filter+=' | select(.created_at > "'"$since"'")'
fi

gh api "repos/${repo}/code-scanning/alerts" --paginate \
  -q "${jq_filter} | [.tool.name, .rule.id] | @tsv" \
  | sort | uniq -c | sort -rn

echo
echo "By tool:"
gh api "repos/${repo}/code-scanning/alerts" --paginate \
  -q "${jq_filter} | .tool.name" \
  | sort | uniq -c | sort -rn
