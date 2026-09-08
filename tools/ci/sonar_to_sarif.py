"""Convert unresolved SonarQube Cloud issues into a GitHub Code Scanning SARIF
file, for the nightly SonarCloud scan (sonarcloud.yml) to upload alongside
CodeQL, PREfast and clang-tidy. SonarQube Cloud findings do not reach
Security > Code scanning on their own - see sonarcloud.yml's own header and
the (still open, as of writing) "Export to SARIF" request in Sonar's
community forum. There is no server-side toggle for this; it has to be
bridged by querying the Web API after each scan and converting the response.

Scoped to BUG and VULNERABILITY by default, not CODE_SMELL: of the ~2700
issues unresolved on this project as this was written, ~2600 were CODE_SMELL
(style/maintainability) and 138 were BUG/VULNERABILITY. Code Scanning is
where CodeQL and PREfast report defects, not a maintainability audit, so the
maintainability view stays on the SonarCloud dashboard where it already
lives - pass --types to widen it if that scope ever needs to change.

No third-party SARIF-converter action: this repo pins every action to a
commit SHA and runs Scorecard/zizmor/dependency-review specifically to catch
what an unaudited action holding SONAR_TOKEN would be. stdlib-only (urllib),
matching every other script in this directory - no new pip dependency to add
to requirements-coverage.txt for one script.

Rule names come from a single batched /api/rules/search call, not the
per-issue rule metadata some SonarQube deployments inline into
/api/issues/search's response - this API build returns that response's
`rules` array empty (confirmed against the live project), so relying on it
silently drops every rule name. htmlDesc/mdDesc came back empty even when
explicitly requested (f=htmlDesc) on an authenticated request too, so rather
than depend on that, each rule's SARIF helpUri links straight to its
SonarCloud coding_rules page instead of trying to embed a full description.

SonarQube's Elasticsearch backend hard-caps a single query at 10,000 results
(paging past that silently returns nothing further). This project is nowhere
near that (order of thousands unresolved, total), so this script paginates
plainly and warns loudly if the reported total ever approaches the cap,
rather than the date-bisection partitioning a much larger project would need.

Security Hotspots are a separate SonarQube concept with their own endpoint
(/api/hotspots/search) - issues/search never returns them regardless of
`types`. Out of scope here; add a second fetch if hotspots are wanted later.
"""

import argparse
import base64
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

PAGE_SIZE = 500
ES_RESULT_CAP = 10_000
RULES_CHUNK = 200

SEVERITY_TO_LEVEL = {
    "BLOCKER": "error",
    "CRITICAL": "error",
    "MAJOR": "warning",
    "MINOR": "note",
    "INFO": "note",
}

# GitHub's security-severity badge wants a CVSS-shaped score; Sonar doesn't
# hand one back, so this is the same fixed BLOCKER..INFO ladder other
# Sonar-to-SARIF bridges use rather than anything derived from the API.
SECURITY_SEVERITY_SCORE = {
    "BLOCKER": "9.0",
    "CRITICAL": "8.0",
    "MAJOR": "6.0",
    "MINOR": "3.0",
    "INFO": "0.0",
}


def sonar_get(sonar_url, token, path, params):
    query = urllib.parse.urlencode(params)
    request = urllib.request.Request(f"{sonar_url}{path}?{query}")
    # HTTP Basic with the token as username and an empty password - the
    # form documented across SonarQube Server and Cloud alike, rather than a
    # bearer header this API's support for isn't confirmed here.
    credentials = base64.b64encode(f"{token}:".encode("ascii")).decode("ascii")
    request.add_header("Authorization", f"Basic {credentials}")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", "replace")
        raise SystemExit(
            f"Sonar API request to {path} failed: HTTP {error.code}\n{detail}"
        )


def fetch_issues(sonar_url, token, organization, project_key, branch, types):
    issues = []
    components = {}
    page = 1
    total = None
    while total is None or len(issues) < total:
        params = {
            "organization": organization,
            "componentKeys": project_key,
            "resolved": "false",
            "types": ",".join(types),
            "ps": PAGE_SIZE,
            "p": page,
        }
        if branch:
            params["branch"] = branch
        data = sonar_get(sonar_url, token, "/api/issues/search", params)
        total = data["paging"]["total"]
        if page == 1 and total > ES_RESULT_CAP:
            print(
                f"::warning::Sonar reports {total} unresolved issues for the "
                f"requested types, above the {ES_RESULT_CAP} Elasticsearch "
                f"pagination cap - only the first {ES_RESULT_CAP} will be "
                "exported.",
                file=sys.stderr,
            )
        issues.extend(data["issues"])
        for component in data.get("components", []):
            components[component["key"]] = component.get("path", component["key"])
        if len(data["issues"]) < PAGE_SIZE:
            break
        page += 1
    return issues, components


def fetch_rule_names(sonar_url, token, organization, rule_keys):
    names = {}
    rule_keys = sorted(rule_keys)
    for start in range(0, len(rule_keys), RULES_CHUNK):
        chunk = rule_keys[start : start + RULES_CHUNK]
        data = sonar_get(
            sonar_url,
            token,
            "/api/rules/search",
            {
                "organization": organization,
                "rule_keys": ",".join(chunk),
                "f": "name",
                "ps": RULES_CHUNK,
            },
        )
        for rule in data.get("rules", []):
            names[rule["key"]] = rule["name"]
    return names


def file_path(component_key, components, project_key):
    if component_key in components:
        return components[component_key]
    # Fallback for a component the search response didn't describe (should
    # not happen in practice - every issue's component is in that page's
    # `components` array): component keys are "<projectKey>:<path>".
    prefix = f"{project_key}:"
    if component_key.startswith(prefix):
        return component_key[len(prefix) :]
    return component_key


def issue_to_result(issue, components, project_key, rule_index):
    result = {
        "ruleId": issue["rule"],
        "ruleIndex": rule_index,
        "level": SEVERITY_TO_LEVEL.get(issue["severity"], "warning"),
        "message": {"text": issue["message"]},
    }

    text_range = issue.get("textRange")
    line = issue.get("line")
    if text_range:
        region = {
            "startLine": text_range["startLine"],
            "startColumn": text_range["startOffset"] + 1,
            "endLine": text_range["endLine"],
            "endColumn": text_range["endOffset"] + 1,
        }
    elif line:
        region = {"startLine": line}
    else:
        region = None

    if region is not None:
        result["locations"] = [
            {
                "physicalLocation": {
                    "artifactLocation": {
                        "uri": file_path(issue["component"], components, project_key)
                    },
                    "region": region,
                }
            }
        ]

    if issue.get("hash"):
        # Lets GitHub track the same underlying finding as surrounding lines
        # shift, the same role SonarQube's own issue.hash plays server-side.
        result["partialFingerprints"] = {"issueHash": issue["hash"]}

    return result


def rule_to_descriptor(rule_key, name, issue_type, sonar_url, organization):
    encoded_key = urllib.parse.quote(rule_key, safe="")
    encoded_org = urllib.parse.quote(organization, safe="")
    descriptor = {
        "id": rule_key,
        "name": name,
        "shortDescription": {"text": name},
        "helpUri": (
            f"{sonar_url}/coding_rules?open={encoded_key}"
            f"&rule_key={encoded_key}&organization={encoded_org}"
        ),
        "properties": {"tags": [issue_type.lower()]},
    }
    if issue_type == "VULNERABILITY":
        # Only field GitHub's security tab actually reads to badge/rank an
        # alert; unset for BUG so it doesn't show as a fabricated CVSS score
        # for something that was never a security finding.
        descriptor["properties"]["security-severity"] = SECURITY_SEVERITY_SCORE[
            "MAJOR"
        ]
    return descriptor


def build_sarif(issues, components, rule_names, project_key, sonar_url, organization):
    rule_types = {issue["rule"]: issue["type"] for issue in issues}
    rule_keys = sorted(rule_types)
    rule_index = {key: index for index, key in enumerate(rule_keys)}
    rules = [
        rule_to_descriptor(
            key, rule_names.get(key, key), rule_types[key], sonar_url, organization
        )
        for key in rule_keys
    ]
    results = [
        issue_to_result(issue, components, project_key, rule_index[issue["rule"]])
        for issue in issues
    ]
    encoded_project = urllib.parse.quote(project_key, safe="")
    return {
        "version": "2.1.0",
        "$schema": (
            "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/"
            "Schemata/sarif-schema-2.1.0.json"
        ),
        "runs": [
            {
                "tool": {
                    "driver": {
                        "name": "SonarQube Cloud",
                        "informationUri": (
                            f"{sonar_url}/project/overview?id={encoded_project}"
                        ),
                        "rules": rules,
                    }
                },
                "results": results,
            }
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--organization", required=True)
    parser.add_argument("--project-key", required=True)
    parser.add_argument("--sonar-url", default="https://sonarcloud.io")
    parser.add_argument("--branch", default=None)
    parser.add_argument(
        "--types",
        default="BUG,VULNERABILITY",
        help=(
            "Comma-separated Sonar issue types to export (default: "
            "BUG,VULNERABILITY - excludes CODE_SMELL, the maintainability/"
            "style bucket, which stays on the SonarCloud dashboard rather "
            "than Code Scanning)."
        ),
    )
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    token = os.environ.get("SONAR_TOKEN")
    if not token:
        raise SystemExit("SONAR_TOKEN must be set in the environment.")

    types = [t.strip() for t in args.types.split(",") if t.strip()]
    if not types:
        raise SystemExit("--types resolved to an empty list.")

    issues, components = fetch_issues(
        args.sonar_url, token, args.organization, args.project_key, args.branch, types
    )
    rule_names = fetch_rule_names(
        args.sonar_url,
        token,
        args.organization,
        {issue["rule"] for issue in issues},
    )
    sarif = build_sarif(
        issues,
        components,
        rule_names,
        args.project_key,
        args.sonar_url,
        args.organization,
    )

    with open(args.output, "w", encoding="utf-8") as handle:
        json.dump(sarif, handle, indent=2)

    print(
        f"Wrote {len(issues)} issues across {len(rule_names)} distinct rules "
        f"to {args.output}"
    )


if __name__ == "__main__":
    main()
