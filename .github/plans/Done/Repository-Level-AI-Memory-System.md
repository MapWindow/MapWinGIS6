# MapWinGIS6 – Repository-Level AI Memory System

## Implementation checklist

Copilot: Update this checklist as each phase is implemented. Only mark a phase complete when its acceptance criteria have been met and its PR is ready.

- [x] **Phase 1 – Memory infrastructure** (PR 1)
- [x] **Phase 2 – Initial knowledge population** (PR 2)
- [x] **Phase 3 – AI agent integration** (PR 3)
- [x] **Phase 4 – Memory maintenance and validation** (PR 4)

**Target branch:** `development`

**Primary objective:** Establish one shared, version-controlled memory system that allows different AI coding agents to share accumulated engineering knowledge about MapWinGIS6.

---

# 1. Objectives and design principles

Implement a repository-level memory system that can be used by:

- GitHub Copilot CLI
- GitHub Copilot coding agent
- Claude Code
- JetBrains Junie
- Pi.dev and local LLMs
- Future AI coding agents

The memory system must be independent of any particular AI provider.

## 1.1 Core principles

1. **Single source of truth**

   All shared project memory must reside in `.ai/memory/`.

2. **Markdown and Git**

   Use ordinary Markdown files. No database, vector database, embeddings, external memory server or additional infrastructure.

3. **Keep instructions separate from knowledge**

   `AGENTS.md` defines how agents should work.

   `.ai/memory/` contains what agents have learned about the project.

4. **Selective retrieval**

   Agents should initially read a small memory index and load only the knowledge relevant to their current task.

5. **Verified knowledge**

   Memory entries must be based on verified source code, documentation, successful experiments or established architectural decisions.

6. **Cross-agent compatibility**

   Knowledge must be reusable by all supported AI agents without requiring agent-specific memory implementations.

7. **Maintainability**

   Memory must be easy to review, update, correct and remove through normal pull requests.

8. **No automatic hooks initially**

   Do not implement Claude Code hooks, shell hooks, automatic memory injection or background processes in this project.

   Memory access and updates must initially be controlled through agent instructions.

## 1.2 What memory is and isn't

The memory system complements existing project documentation.

It must not replace:

- README.md
- AGENTS.md
- docs/ARCHITECTURE.md
- docs/BUILDING.md
- docs/CODING-STANDARDS.md
- docs/PORTING.md
- docs/TESTING.md

These documents remain the authoritative documentation for their respective subjects.

Memory should capture additional knowledge that is useful across development sessions and would otherwise be lost.

Examples:

- Why a particular architectural decision was made.
- Why an alternative implementation was rejected.
- A non-obvious dependency configuration requirement.
- A recurring build or testing problem and its verified solution.
- A technical limitation discovered during development.
- Important lessons learned while porting MapWinGIS5 functionality.

---

# 2. Phase 1 – Memory infrastructure (PR 1)

## Objective

Create the initial memory directory structure, its index and the rules governing memory usage.

Do not attempt to populate the complete knowledge base in this phase.

## 2.1 Directory structure

Create:

    .ai/
    └── memory/
        ├── MEMORY.md
        ├── README.md
        │
        ├── architecture/
        │   └── README.md
        │
        ├── build/
        │   └── README.md
        │
        ├── testing/
        │   └── README.md
        │
        ├── decisions/
        │   └── README.md
        │
        ├── lessons/
        │   └── README.md
        │
        └── tools/
            └── README.md

The individual README files explain the purpose and intended contents of each category.

Do not create numerous empty files.

Additional knowledge files will be introduced in Phase 2 as actual content becomes available.

## 2.2 MEMORY.md – Central index

Create `.ai/memory/MEMORY.md`.

This must remain a relatively small document, ideally below 150 lines.

It serves as the entry point for all AI agents.

It must contain:

- A brief description of the memory system.
- Instructions to load relevant knowledge selectively.
- Links to the available memory categories.
- A short description of what each category contains.
- Rules for determining which knowledge is relevant.
- Instructions for updating the index when files are added, renamed or removed.

Agents should not need to read every memory file at the beginning of every session.

## 2.3 Memory management instructions

Extend the existing `AGENTS.md` with a dedicated section called:

`## AI Memory Management`

Include instructions covering:

**At the beginning of a session:**

1. Read `AGENTS.md`.
2. Read `.ai/memory/MEMORY.md`.
3. Determine which memory files are relevant to the current task.
4. Read those files before making architectural or implementation decisions.
5. Verify important assumptions against the current repository.

**During development:**

Agents should identify potentially reusable knowledge.

Examples:

- A previously unknown technical limitation.
- A verified solution to a difficult build problem.
- An architectural decision.
- A dependency or tooling requirement.
- A lesson from a failed implementation.

**At the end of a task:**

Agents should:

1. Review whether new reusable knowledge was discovered.
2. Check whether the information already exists.
3. Verify the information against the source code or other reliable evidence.
4. Update the appropriate memory file.
5. Update `MEMORY.md` when necessary.
6. Include relevant memory changes in the implementation PR.

Do not create memory entries for every small implementation detail.

## 2.4 Memory quality rules

Add the following requirements:

- Never store guesses as established facts.
- Distinguish verified facts from assumptions and proposals.
- Include references to relevant source files, documentation or PRs where useful.
- Prefer concise explanations over long transcripts.
- Avoid duplicating existing documentation.
- Remove obsolete information when it is no longer relevant.
- Never store credentials, secrets, access tokens or personal information.
- Do not record temporary implementation status as permanent knowledge.
- Do not turn an unsuccessful experiment into an architectural rule.

## 2.5 Acceptance criteria

- [x] Directory structure created.
- [x] Central MEMORY.md created.
- [x] README files explain the memory categories.
- [x] AGENTS.md contains memory management instructions.
- [x] Existing instructions remain intact.
- [x] No agent-specific memory system introduced.
- [x] No hooks or automatic injection implemented.
- [x] Documentation reviewed for duplication and consistency.

### Remaining limitations

- The category guides are in place, but the memory system does not yet contain
  project-specific knowledge. Initial knowledge population is deferred to
  Phase 2.

---

# 3. Phase 2 – Initial knowledge population (PR 2)

## Objective

Populate the memory system with useful existing knowledge about MapWinGIS6.

The initial memory should be based on the actual repository, not assumptions or outdated information from previous development sessions.

Review:

- Existing documentation.
- Source code.
- CMake configuration.
- vcpkg configuration.
- GitHub Actions workflows.
- Existing tests.
- Merged pull requests.
- Established architectural decisions.

## 3.1 Architecture memory

Create relevant files under:

`.ai/memory/architecture/`

Suggested topics:

### overview.md

Document the overall MapWinGIS6 architecture.

Include:

- Responsibilities of MapWinGIS.Core.dll.
- Responsibilities of the ActiveX control.
- Relationships between the different components.
- Important architectural boundaries.
- Where new functionality should generally be implemented.

Pay particular attention to the principle that the ActiveX control should remain relatively slim, with GIS functionality implemented in the core library where appropriate.

### activex.md

Document important ActiveX-specific knowledge.

Include:

- COM architecture.
- Interface design.
- Interaction between ActiveX and the core library.
- Important compatibility considerations.
- Technical limitations and implementation conventions.

### core-library.md

Document knowledge specific to the core library.

Include:

- Component responsibilities.
- Public and internal API conventions.
- Dependencies.
- Important design considerations.

Only create these files if the actual source structure supports the separation.

## 3.2 Build memory

Create relevant files under:

`.ai/memory/build/`

Suggested topics:

### windows.md

Document the Windows development environment.

Include:

- Supported configurations.
- Visual Studio build requirements.
- Windows SDK requirements.
- ATL/MFC dependencies.
- Known platform-specific issues.

### cmake.md

Document the CMake implementation.

Include:

- Project structure.
- Configuration options.
- Build targets.
- Generator requirements.
- CLion compatibility.
- Relationship between CMake and the existing Visual Studio solution.

### dependencies.md

Document:

- vcpkg configuration.
- GDAL integration.
- Important third-party dependencies.
- Dependency version requirements.
- Known dependency-related problems.

Do not duplicate the complete build documentation. Record additional technical details and non-obvious discoveries.

## 3.3 Testing memory

Create:

`.ai/memory/testing/testing.md`

Document:

- GoogleTest configuration.
- Test project organization.
- How tests are executed.
- Important initialization requirements.
- Known testing limitations.
- Relationship between unit tests and the C# ActiveX test application.

Include only verified procedures.

## 3.4 Architectural decisions

Create individual decision records under:

`.ai/memory/decisions/`

Use a simple naming convention:

    YYYY-MM-DD-short-description.md

Each decision record should contain:

- Title.
- Date, if known.
- Status: Proposed, Accepted, Superseded or Rejected.
- Context.
- Decision.
- Consequences.
- Relevant PRs or source references.

Initial decisions should cover established principles such as:

- Modern architecture over a line-by-line MapWinGIS5 port.
- Prioritizing internal structure and API design over strict backward compatibility.
- Keeping ActiveX relatively slim.
- Implementing GIS functionality in the core library where appropriate.

Do not invent historical dates or decisions.

Where the original decision cannot be established, identify it as a currently documented architectural principle rather than pretending it was formally decided on a particular date.

Do not copy existing formal ADRs if they already exist.

## 3.5 Lessons learned

Create:

`.ai/memory/lessons/lessons.md`

Initially, populate this only with verified, reusable discoveries from merged PRs and development history.

Examples:

- Non-obvious C++ implementation constraints.
- GDAL integration problems.
- COM-specific pitfalls.
- Build and test problems that have already been solved.

Each lesson should explain:

1. What the problem was.
2. Why it occurred.
3. How it was resolved.
4. Whether the solution has any limitations.

Avoid turning this file into a chronological development diary.

## 3.6 Tools memory

Create relevant files under:

`.ai/memory/tools/`

Document tool-specific information only when it is useful beyond the tool's normal documentation.

Potential subjects:

- Copilot CLI.
- CLion.
- Pi.dev and local models.

Do not store personal configuration, credentials or machine-specific secrets.

## 3.7 Acceptance criteria

- [x] Existing repository documentation reviewed.
- [x] Relevant source code and configuration inspected.
- [x] Architecture memory populated.
- [x] Build memory populated.
- [x] Testing memory populated.
- [x] Established architectural decisions recorded.
- [x] Reusable lessons documented.
- [x] MEMORY.md updated.
- [x] No duplicated documentation.
- [x] All technical claims verified against the current repository.
- [x] Uncertain or historical information clearly identified.

### Remaining limitations

- The repository has no standalone CMake project; the build memory records
  this status rather than inventing CMake instructions.
- No tool-specific memory file was added because existing tool documentation
  was sufficient.
- The original dates of the architectural principles were not established;
  decision records identify them as current repository guidance.

---

# 4. Phase 3 – AI agent integration (PR 3)

## Objective

Ensure different AI coding agents use the same repository-level memory.

The shared knowledge must live exclusively in `.ai/memory/`.

Agent-specific configuration files may explain how to access the shared memory but must not contain separate copies of the knowledge.

## 4.1 GitHub Copilot

Review:

`.github/copilot-instructions.md`

And the existing `AGENTS.md`.

Ensure that Copilot:

- Reads the central memory index.
- Loads relevant memory files before implementation.
- Updates memory when useful knowledge is discovered.
- Includes memory changes in appropriate PRs.

Avoid repeating the complete memory instructions in multiple files.

## 4.2 Claude Code

Check the current Claude Code project-instruction conventions.

If necessary, introduce a small `CLAUDE.md` that points to:

- `AGENTS.md`
- `.ai/memory/MEMORY.md`

It should not contain a second copy of the project knowledge.

Do not implement Claude Code hooks or automatic memory injection.

## 4.3 JetBrains Junie

Check the current Junie project-guideline conventions.

If a dedicated configuration file is necessary, make it a lightweight reference to the shared instructions and memory.

Avoid creating an independent knowledge base.

## 4.4 Pi.dev

Review the existing Pi.dev project configuration and system prompt conventions.

Ensure Pi.dev can access:

- `AGENTS.md`
- `.ai/memory/MEMORY.md`
- Relevant memory files.

Local LLMs should follow the same memory management principles as cloud-based agents.

Avoid requiring a particular model or provider.

## 4.5 Cross-agent compatibility

All agent-specific configuration must follow these principles:

- `.ai/memory/` is authoritative.
- No separate agent-specific project memory.
- No duplicate architectural documentation.
- No automatic hooks.
- No hardcoded absolute paths.
- Use repository-relative paths.
- Keep agent-specific instructions minimal.

Verify the current capabilities and supported configuration conventions of each tool before adding integration files.

If a tool does not support a particular instruction mechanism, document the limitation rather than introducing an unreliable workaround.

## 4.6 Acceptance criteria

- [x] Copilot integration verified.
- [x] Claude Code integration verified or documented if unsupported.
- [x] Junie integration verified or documented if unsupported.
- [x] Pi.dev integration verified or documented if unsupported.
- [x] All agents reference the same memory.
- [x] No duplicated knowledge.
- [x] No hooks or automatic injection.
- [x] Existing agent configurations preserved.

### Remaining limitations

- Claude Code reads `AGENTS.md` directly by default in v2.1.277 and later.
  Older versions or sessions with AGENTS.md support disabled need a
  `CLAUDE.md` import; none was added because the current supported default
  needs no separate project instructions.
- Junie CLI and Pi load `AGENTS.md` directly, so no separate `.junie` or
  `.pi` instruction files are needed.

---

# 5. Phase 4 – Memory maintenance and validation (PR 4)

## Objective

Establish a sustainable process for keeping memory accurate and useful as MapWinGIS6 evolves.

## 5.1 Memory maintenance rules

Document the following rules in `.ai/memory/README.md`.

### Adding knowledge

A new memory entry should be added when information:

- Is likely to be useful in future development sessions.
- Is not already adequately documented elsewhere.
- Is sufficiently verified.
- Has value beyond the immediate implementation task.

### Updating knowledge

When architecture or implementation changes:

- Update affected memory files.
- Check references to changed source files.
- Remove obsolete instructions.
- Update the central index if necessary.

### Removing knowledge

Remove information that:

- Is no longer valid.
- Has been superseded.
- Duplicates authoritative documentation.
- Is too specific to an individual task.
- Cannot be verified and is no longer useful.

### Promoting knowledge

When a discovery becomes an established architectural rule or an important permanent project convention, consider promoting it into the appropriate official documentation.

Memory must not become a replacement for maintained engineering documentation.

## 5.2 Memory review procedure

Add a memory review checklist that agents can use when completing substantial PRs.

Checklist:

- [ ] Are new discoveries worth retaining?
- [ ] Are existing memory entries still correct?
- [ ] Are references to source files still valid?
- [ ] Are there conflicting decisions?
- [ ] Has temporary information accidentally become permanent?
- [ ] Does MEMORY.md still point to the correct files?
- [ ] Can unnecessary or duplicated information be removed?

## 5.3 Validation

Perform a complete review of the memory system.

Verify:

1. All links between memory files work.
2. All files are relevant and contain meaningful information.
3. No sensitive information is present.
4. No outdated technical assumptions remain.
5. Existing project documentation has not been unnecessarily duplicated.
6. The central index remains concise.
7. Agent-specific configuration points to the shared memory.
8. Memory instructions are consistent across all agents.

This is primarily a documentation and configuration change. Run appropriate repository checks, but a full C++ build should not be necessary unless implementation changes require it.

## 5.4 Acceptance criteria

- [x] Memory maintenance rules documented.
- [x] Memory review checklist added.
- [x] Obsolete knowledge removal process documented.
- [x] Cross-reference validation completed.
- [x] Agent configurations reviewed.
- [x] Complete memory system reviewed for consistency.
- [x] Final documentation checks passed.

### Remaining limitations

- The plan file is outside the Git repository, so this status update is not
  included in the implementation PR.

---

# 6. General implementation instructions for Copilot

These instructions apply to all four phases.

## 6.1 Repository inspection

Before implementing each phase:

1. Inspect the current repository structure.
2. Read the existing AGENTS.md.
3. Read the existing documentation.
4. Check the current agent configuration files.
5. Identify existing documentation that should be reused rather than duplicated.

Do not assume that the proposed directory structure or file names already exist.

Adapt the plan where necessary while preserving the architectural principles.

## 6.2 Pull request strategy

Create four separate PRs targeting `development`.

Each PR should:

- Have a focused scope.
- Include an appropriate description.
- Explain the changes made.
- Include a testing/validation summary.
- Identify any deviations from this plan.

Do not wait for a previous PR to be merged before starting work on the next phase, provided the implementation can be developed safely against the intended structure.

Avoid unrelated source-code changes.

## 6.3 Documentation quality

Write documentation in clear English.

Prefer concise, structured Markdown.

Use relative links within the repository.

Avoid unnecessary abstractions and complicated memory-management conventions.

The system should remain understandable to developers who do not use AI coding agents.

## 6.4 Scope exclusions

The following are explicitly outside the scope of this implementation:

- Claude Code hooks.
- Automatic memory injection.
- Background memory collection.
- External memory servers.
- Vector databases.
- Embedding generation.
- RAG infrastructure.
- Cloud-hosted project memory.
- Separate knowledge bases for individual AI agents.
- Automatic modification of source code based on memory entries.

These may be considered separately in the future, but they are not required for the initial system.

---

# 7. Final expected result

After all four PRs are completed, MapWinGIS6 should have:

1. One authoritative, version-controlled repository memory system.
2. A concise central memory index.
3. Structured knowledge covering architecture, build environment, testing, decisions, lessons and tools.
4. Shared memory management instructions for AI coding agents.
5. Lightweight integrations for supported agents.
6. A documented process for maintaining and validating memory.
7. No dependency on a particular AI provider or model.

**Success criterion:**

A new AI coding agent should be able to start working on MapWinGIS6, read the central instructions and memory index, retrieve the relevant project knowledge, and avoid repeating previously solved engineering problems or proposing approaches that conflict with established architectural decisions.

The memory system should grow organically with MapWinGIS6 and remain useful regardless of which AI coding tools are used in the future.