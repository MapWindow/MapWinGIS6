---
name: resolving-pr-comments
disable-model-invocation: true
description:
  Brings the active pull request to a mergeable state by handling all review comments (Copilot and team members), merging the PR's base branch into the PR branch, and fixing failing PR checks.
---

# Resolving PR comments

## Base branch

Before starting, read the base branch (target branch) from the active pull request, for example with `gh pr view --json baseRefName`. Use `origin/<baseRefName>` as the base branch in all steps below.

If the PR info can't be read, ask the user for the base branch and wait for the answer.

## Goal

Bring the active pull request to a mergeable state: all review feedback handled, branch synced with the base branch, all PR checks green.

Run the whole workflow to completion without asking for confirmation. Check in only when the base branch can't be determined, or when different readings of a review comment would lead to materially different changes.

## Scope

- Change only what a review comment, merge conflict, or failing check requires. No unrelated refactors, cleanups, or extra features.
- If a comment asks for something mistaken, say so in the reply instead of quietly doing something else.

## Communication

- Before the first tool call, state in one sentence what you are about to do.
- While working, report only important findings or changes of direction.
- Keep PR replies short and factual.

## Comment handling

Reviewers are GitHub Copilot (user `Copilot` / `copilot-pull-request-reviewer`) or team members. Apply to every review thread:

1. **Analyze.** A comment is valid when it points to a real bug, risk, convention violation, or clear improvement within the PR's scope. Judge on merits, regardless of author.
2. **Valid:** fix it. Use Copilot's suggested fix when it is correct and fits the surrounding code; otherwise write your own.
3. **Not valid:** leave the code unchanged.
4. **Reply** after the fix commit is pushed:
   - Fixed: `Fixed in <short SHA>: <one-line description of the change>.`
   - Not fixed: `Not changed: <one or two sentence reason>.`
5. **Resolve or leave open:**
   - Copilot thread: reply, then resolve.
   - Team member thread: reply, do NOT resolve. The reviewer resolves it.
6. **Suppressed Copilot comments** (low-confidence items in the Copilot review body, no thread): analyze and fix with the same rules. No reply or resolve possible; list them in the final summary.

A comment is **handled** when:

- Copilot thread: replied and resolved.
- Team member thread: replied by you, with no newer reviewer reply after yours. Stays unresolved.
- Suppressed Copilot comment: fixed, or deliberately skipped with a reason.

A new reviewer reply on a handled thread makes it unhandled again.

### Posting replies from PowerShell

PowerShell does not expand an indexed or member expression inside a bare command argument: in `gh api ... -f b=$replies[$k]` only `$replies` is expanded, so the reply body becomes `System.Collections.Specialized.OrderedDictionary[<key>]`.

- Assign each value to a plain variable first, then pass it quoted: `$body = $replies[$k]; gh api graphql ... -f "b=$body"`. Inside a quoted string use `$($replies[$k])` for any indexed or member access.
- After posting, read the returned comment body (for example `--jq '...comment.body'`) and check it matches the intended text. Fix a wrong reply in place with the `updatePullRequestReviewComment` mutation instead of posting a second reply.

## Workflow

Copy this checklist and track progress:

```
PR Progress:
- [ ] Phase 1: Review loop (all comments and quality gates handled)
- [ ] Phase 2: Branch sync (base branch merged)
- [ ] Phase 3: Checks loop (all checks green)
- [ ] Final summary
```

### Phase 1: Review loop

1. Collect all unhandled review comments, and suppressed Copilot comments.
2. Handle each per **Comment handling**.
3. If code changed: commit with a clear message, push, then post replies and resolve Copilot threads.
4. Wait for new review activity (comments, threads, Copilot reviews, reviewer replies). Prefer active monitoring; otherwise wait up to 3 minutes.
5. New unhandled comments: repeat from step 1. Otherwise go to Phase 2.

### Phase 2: Branch sync

1. Fetch and merge the base branch into the PR branch.
2. Resolve merge conflicts, keeping the intent of both sides.
3. If anything changed: commit and push.
4. New unhandled comments after the push: return to Phase 1. Otherwise go to Phase 3.

### Phase 3: Checks loop

1. Monitor all PR checks until they complete.
2. On a failing check:
   - Investigate the cause.
   - Merge the base branch and resolve conflicts; it may already contain the fix.
   - If the failure persists, fix it.
   - Commit and push, then return to Phase 1.
3. All checks green: if any unhandled comments exist, return to Phase 1.

### Done when

- All PR checks are green.
- Every review comment is handled.
- No new review activity since the last push.
- The PR branch contains the latest base branch.

## Final summary

Lead with the final PR status in one sentence. Then list briefly (omit empty sections):

- Commits created (short SHA + message).
- Comments fixed, per reviewer.
- Comments not fixed, with reason.
- Team member threads left open for the reviewer.
- Suppressed Copilot comments handled.
- Failed checks and their fixes.
- Merge conflicts resolved.
