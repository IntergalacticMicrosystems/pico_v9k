# AGENTS.md

Instructions for any coding agent (Claude Code, Codex, Grok Build, ...) working in this repository.
Read at the start of every session, so keep it short: one line per fact.

Before the final message of any task that changed repo files or ran the project's
build/test/run commands, emit one `AGENTS.md proposal` block or the line
`AGENTS.md: no change proposed.` See "Continuous improvement" below.

## Project

- Purpose: <one sentence>
- Stack: <language / framework / target hardware>
- Build: `<command>`
- Test: `<command>`
- Run: `<command>`
- Layout: `<dir>` = <what lives there>

## Working rules

- Prefer the simplest solution that works. If a simpler approach exists, propose it first.
- Do not change behaviour outside the task's scope without saying so.
- Before reporting done: if Build/Test above are real commands (not placeholders), run them and
  quote any failure verbatim. Say which checks were not run.

## Lessons learned

- (none yet)

## Continuous improvement

**When:** after a top-level task (not a subagent, not a question, not search-only work) that
changed repo files or ran the project's build/test/run commands. Applying an approved AGENTS.md
change does not itself trigger another retrospective.

**Review** what this task taught about this project: a command, flag, path or setup step found
by trial and error; a tool, library or hardware quirk that cost time; a convention the user
stated or corrected; a line in this file that proved wrong, stale or redundant.

**Filter.** Propose only a fact that is useful on a future task in this repo, not already in
this file, and not obvious from the README, manifest or file names (a buried gotcha is fine).
A user correction is always eligible, even if it is now visible in code. Filling or fixing a
Project placeholder is eligible when the fact is known. Never record global style preferences,
secrets, or one-off task details. At most one proposal per task; prefer `change` or `remove`
over `add`. The first lesson replaces the "(none yet)" line.

**Propose, then stop.** End the final message with exactly one of:

```
AGENTS.md proposal
Section: <Project | Working rules | Lessons learned>
Action: <add | change | remove>
Replaces: <exact existing line; "(none)" for add>
Line: <exact new line; "(none)" for remove>
Why: <one sentence: what happened that makes this worth keeping>
```

or the single line `AGENTS.md: no change proposed.` Do not wait within the turn.

**Approval.** Edit this file only when the user directly answers the proposal (yes / apply /
revised wording). A new task, "ok" followed by other work, or silence means skip: do not edit
and do not re-ask. Apply exactly the approved text, in the named section (merge, do not append
at the bottom). Never re-propose a rejected item unless new evidence appears.

**Size.** Lessons learned holds at most 40 entries and Project at most 10 lines, one line each.
If a proposal would exceed that, include a removal in the same proposal. If a project needs
more, the overflow belongs in the README, not here.
