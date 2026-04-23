---
mode: agent
description: "Run a Karpathy-style review of pending changes or a target file."
---

# Karpathy Review

Review the target (current diff, selection, or file the user names) against the
four Karpathy guidelines and report findings.

For each principle, list concrete violations with file + line references and a
suggested minimal fix. Do not modify code unless the user asks.

1. **Think Before Coding** — hidden assumptions, missed alternatives, unclear
   scope.
2. **Simplicity First** — speculative abstractions, unused configurability,
   error handling for impossible cases, oversized solutions.
3. **Surgical Changes** — edits unrelated to the stated task, style drift,
   reformatting, deletion of pre-existing code.
4. **Goal-Driven Execution** — missing or unverifiable success criteria, no
   reproducing test for bug fixes.

End with a short summary: "Ship as-is", "Minor cleanup", or "Needs rework",
plus the single highest-impact change to make first.
