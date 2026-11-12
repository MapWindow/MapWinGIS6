# AI Memory

This directory contains concise, verified project knowledge shared through
version control and usable by different AI agents.

Start with [MEMORY.md](MEMORY.md). It describes the knowledge categories and
helps agents select relevant files. The rules for verifying, updating, and
maintaining memory are in [the AI Memory Management section of
`AGENTS.md`](../../AGENTS.md#ai-memory-management).

Memory supplements, but does not replace, project instructions or authoritative
documentation. Store reusable facts and decisions rather than session
transcripts or temporary implementation status. Keep knowledge reviewable and
update it through normal pull requests.

## Memory lifecycle

### Adding knowledge

Add an entry when it is likely to help future development, is not already
adequately documented elsewhere, is verified, and has value beyond the current
task. Include references and limitations that help readers confirm when the
knowledge applies.

### Updating knowledge

When architecture or implementation changes, review memory entries about the
affected area. Verify their claims and source references, remove obsolete
instructions, and update category guides and `MEMORY.md` when files or topics
change.

### Removing knowledge

Remove information that is no longer valid, has been superseded, duplicates
authoritative documentation, is specific only to an individual task, or cannot
be verified and is no longer useful.

### Promoting knowledge

When a discovery becomes an established architectural rule or lasting project
convention, consider adding it to the relevant official project documentation.
Keep memory complementary to that documentation rather than using it as a
replacement.

## Review checklist

Use this checklist when completing a substantial PR:

- [ ] Are new discoveries worth retaining?
- [ ] Are existing memory entries still correct?
- [ ] Are references to source files still valid?
- [ ] Are there conflicting decisions?
- [ ] Has temporary information accidentally become permanent?
- [ ] Does `MEMORY.md` still point to the correct files?
- [ ] Can unnecessary or duplicated information be removed?
