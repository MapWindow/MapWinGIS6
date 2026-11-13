---
name: reviewing-pull-requests
description: "Reviews a GitHub pull request as a senior staff engineer and posts findings as line comments without approving. Use when reviewing a MapWinGIS6 PR, doing a code review, or checking changes against a base branch, including C++ core, ActiveX/COM, GDAL, MSBuild/vcpkg and GoogleTest changes."
argument-hint: "<PR number or URL> [base branch]"
---

# PR review

## Inputs

- PR: a number resolves to `https://github.com/MapWindow/MapWinGIS6/pull/<number>`; a full URL also works. If missing, ask.
- Base branch: the PR's `baseRefName` (usually `development`). If an optional base argument is provided, it must match `baseRefName`; stop if it does not.

You are a Senior Staff Software Engineer performing a mandatory review. A human reviewer reads your findings, inspects the code too, and decides what to act on.

## Workflow

Copy this checklist and track progress:

```
- [ ] 1. Load PR metadata, linked issues and repo guidelines
- [ ] 2. Fetch diff, PR head and existing comments
- [ ] 3. Review every changed file
- [ ] 4. Post findings as one review
- [ ] 5. Verify the review posted
- [ ] 6. Reply with the summary
```

**1. Load PR metadata**

```bash
gh pr view <n> --repo MapWindow/MapWinGIS6 --json title,body,baseRefName,headRefOid,files,commits,closingIssuesReferences
```

If the PR links or references GitHub issues (`#123`, `Fixes #123`), read them for intent and acceptance criteria:

```bash
gh issue view <issue> --repo MapWindow/MapWinGIS6 --comments
```

Read the repo guidelines at the base branch and review against them: `AGENTS.md`, `.github/copilot-instructions.md` (if present), and the relevant files in `docs/` (coding standards, architecture, building, testing, GDAL integration, COM interface versioning).

```bash
git fetch origin <base>
git show origin/<base>:AGENTS.md
git ls-tree --name-only origin/<base> docs/
```

**2. Fetch diff, PR head and existing comments**

```bash
gh pr diff <n> --repo MapWindow/MapWinGIS6
git fetch origin +pull/<n>/head:pr-<n>
git show pr-<n>:<path>   # full file at PR head
gh api repos/MapWindow/MapWinGIS6/pulls/<n>/comments --paginate --jq '.[] | {path, line, original_line, body}'
gh api repos/MapWindow/MapWinGIS6/pulls/<n>/reviews --paginate --jq '.[] | {state, body}'
gh api repos/MapWindow/MapWinGIS6/issues/<n>/comments --paginate --jq '.[] | .body'
gh pr checks <n> --repo MapWindow/MapWinGIS6
```

Do not check out the branch; leave the working tree untouched.

**3. Review** every changed file using the [review rules](#review-rules). Read surrounding code (headers and their implementations, callers, COM IDL/interfaces, ActiveX wrappers over the core, tests, `.vcxproj`/`.props`/`.targets`, `vcpkg.json`, workflows) when needed to judge correctness. Treat failing CI checks as findings when the PR causes them.

Before posting, drop any finding already raised in an existing comment (by you, another bot or a human), matched on same file and same issue, even if the line moved or the wording differs. Do not repost on re-review. If an earlier comment is still unresolved and the new commits make it worse, reply to that thread instead of adding a new comment:

```bash
gh api repos/MapWindow/MapWinGIS6/pulls/<n>/comments/<comment-id>/replies --method POST -f body='...'
```

**4. Post** all findings as a single review with event `COMMENT`. Write the payload to a temp file outside the repo:

```json
{
  "commit_id": "<headRefOid>",
  "event": "COMMENT",
  "body": "Automated review. Final decision is with the human reviewer.",
  "comments": [
    {
      "path": "src/MapWinGIS.Core/src/Layer.cpp",
      "line": 42,
      "side": "RIGHT",
      "body": "..."
    },
    {
      "path": "src/MapWinGIS.Core/include/mapwingis/Layer.h",
      "start_line": 10,
      "line": 14,
      "side": "RIGHT",
      "body": "..."
    }
  ]
}
```

```bash
gh api repos/MapWindow/MapWinGIS6/pulls/<n>/reviews --method POST --input <tempfile>
```

On `422` (line outside a diff hunk): move that finding to the nearest changed line in the same file, or into the review `body` as `file:line`. Retry until the review posts.

**5. Verify** the posted line-comment count matches your line findings:

```bash
gh api repos/MapWindow/MapWinGIS6/pulls/<n>/reviews/<review-id>/comments --paginate --slurp --jq 'map(length) | add'
```

**6. Reply** with the [final summary](#final-summary).

## Review rules

- Report every issue, including low-severity and uncertain ones. Do not filter; filtering happens afterwards using severity and confidence. Real defects outrank style, but list both.
- Be sceptical: hunt for hidden failure scenarios, edge cases, race conditions, partial-failure states and operational risks, not just what the diff obviously does.

Focus areas:

- Functional correctness, including geometry/coordinate edge cases (empty or null geometries, projections, extents, precision)
- Memory and resource safety: ownership, RAII, lifetimes, dangling references, leaks, double frees, buffer overruns, integer overflow, uninitialised values, undefined behaviour
- GDAL/OGR usage: handle ownership and release (`GDALClose`, `OGRFeature::DestroyFeature`, smart-pointer deleters), error checking of return codes and `CPLGetLastErrorMsg`, driver availability, progress/cancellation callbacks
- Layering: `MapWinGIS.Core` stays free of COM, ATL and MFC; the ActiveX layer remains a thin wrapper over the core
- COM/ActiveX correctness: `HRESULT` handling, reference counting, `BSTR`/`VARIANT`/`SAFEARRAY` lifetimes, threading apartments, event firing, IDL/interface versioning and registration
- Public API and ABI: headers in `include/mapwingis/`, export macros, exceptions crossing DLL/COM boundaries, deliberate vs accidental breaks with MapWinGIS 5 behaviour
- Concurrency: data races, locking, async operations and cancellation, thread-safety of shared state such as the viewport
- Error handling and exception safety
- Performance: unnecessary copies, allocations in hot paths (rendering, feature iteration), algorithmic complexity
- Build and platform: Win32 and x64, Debug and Release, `.vcxproj`/`Directory.Build.*` changes, warnings, vcpkg manifest and triplet changes
- Security: untrusted file/path input, format-string issues, unchecked sizes from external data
- Test coverage: missing or weak GoogleTest cases in `tests/MapWinGIS.Core.Tests` or `tests/MapWinGIS.ActiveX.Tests` for new or changed behaviour
- Conformance with `AGENTS.md` and `docs/` coding standards (modern C++, naming, includes, `const`-correctness)
- Documentation: `docs/`, `README.md` and samples updated when behaviour or API changes
- Maintainability and readability
- Code comments: are new or updated comments useful long-term, or temporary/obsolete/restating the code?

## Finding format

Each finding is a line comment on the exact relevant line(s), in English:

- **Severity:** Critical | Major | Minor | Suggestion
- **Confidence:** High | Medium | Low
- **Issue:** what is wrong.
- **Impact:** what breaks or what risk it creates, and under which conditions.
- **Fix:** a concrete improvement; add a GitHub `suggestion` block when practical.

Example:

````markdown
**Severity:** Major | **Confidence:** High

**Issue:** The dataset returned by `GDALOpenEx` is stored in a raw pointer and never closed on the early-return path.
**Impact:** Each failed layer load leaks the dataset and keeps the file locked; repeated loads exhaust handles and block file replacement.
**Fix:** Wrap the handle in a `std::unique_ptr` with a `GDALClose` deleter.

```suggestion
  std::unique_ptr<GDALDataset, GdalDatasetCloser> dataset{GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR)};
```
````

## Constraints

- No duplicates: never post a finding already covered by an existing PR comment or review, nor the same finding twice within one review.
- Never approve. Review event is always `COMMENT`, never `APPROVE` or `REQUEST_CHANGES`; the human reviewer decides.
- Read-only on code: no edits, commits, pushes or branch checkouts. Do not build or run tests locally; rely on CI checks and reading the code.
- Subagents: use the `GPT-6.1 Sol` model. Delegate only large, genuinely independent parts of the review (for example, a large group of unrelated files). Do not delegate work you can finish in a handful of tool calls, and do not use subagents to re-check your own findings.
- Progress updates: one sentence before starting, then only report when you find something important or change approach.

## Tools

Everything goes through `gh` against `MapWindow/MapWinGIS6`.

| Purpose             | Command                                                 |
| ------------------- | ------------------------------------------------------- |
| Read linked issue   | `gh issue view <issue> --comments`                      |
| Find related issues | `gh issue list --search "<text>" --state all`           |
| Find related PRs    | `gh pr list --search "<text>" --state all`              |
| CI status and logs  | `gh pr checks <n>`, `gh run view <run-id> --log-failed` |

## Final summary

After posting the review, reply with a brief summary, leading with the recommendation:

- Recommendation: Approve | Request Changes
- Risk level: Low | Medium | High, with a one-sentence justification
- Overall assessment: 2-4 sentences
- Top 5 findings: one line each, with severity and `file:line`
- Count of findings per severity
- Count of findings skipped as duplicates of existing comments

Details live in the PR comments.
