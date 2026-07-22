---
name: update-changelog
description: Update the project changelog. Use whenever the user says "update changelog", "update the changelog", "add to changelog", or asks to record recent changes in a changelog.
---

# Update Changelog

Update `CHANGELOG.md` with a concise, user-focused summary of relevant changes.

## Process

1. **Inspect current changes and history**
   - Run `git status --short`.
   - Review relevant diffs with `git diff --stat` and targeted `git diff -- <path>` as needed.
   - If there are staged changes, also check `git diff --cached --stat` and targeted staged diffs.
   - Review recent history with `git log --oneline --decorate --max-count=30`.
   - Release tags are expected to use the form `Vx.x.x` (for example `V1.2.3`).
   - Use release-tag boundaries to inspect changes release by release. Prefer the latest `Vx.x.x` tag as the changelog boundary for `[Unreleased]`, for example `git log --oneline V1.2.3..HEAD` and `git diff --stat V1.2.3..HEAD`.
   - When preparing or reviewing a specific release, compare the previous `Vx.x.x` tag to the target release tag, for example `git log --oneline V1.2.2..V1.2.3`.
   - Use `git show --stat --oneline <commit>` or targeted commit diffs as needed to understand notable changes.

2. **Find or create the changelog**
   - Prefer `CHANGELOG.md` at the repository root.
   - If it does not exist, create it using Keep a Changelog-style headings with an `## [Unreleased]` section.

3. **Choose entries**
   - Prefer entries based on recent git history plus any current unstaged/staged changes.
   - Record notable user-visible, API, behavior, documentation, example, test, build, or integration changes.
   - Consolidate related commits into a single concise changelog bullet instead of listing commit subjects verbatim.
   - Avoid noisy entries for tiny refactors, formatting-only edits, or generated files unless they matter to users/contributors.

4. **Place entries under `## [Unreleased]`**
   - Use these sections when appropriate: `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`.
   - Create missing subsections only when needed.
   - Keep bullets concise and understandable without requiring code context.

5. **Preserve existing content**
   - Do not rewrite historical release entries unless explicitly asked.
   - Avoid duplicate bullets.
   - Preserve existing changelog style if it is already established.

6. **Summarize the update**
   - Tell the user which changelog file was updated and list the added bullets briefly.

## Entry guidance for this repository

Track changes such as:

- Public API updates in `include/ipc.h`.
- Actor lifecycle, registry, dispatch, message ID, send/error-path, and payload behavior changes.
- Platform port behavior, especially POSIX or Zephyr integration.
- Examples, build/test infrastructure, and contributor-facing test improvements.

Do not expose implementation details unnecessarily; write changelog entries from a user or maintainer perspective.
