---
description: "Touch only what you must. Match existing style. No drive-by refactors."
applyTo: "**"
---

# Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:

- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.

When your changes create orphans:

- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

## Anti-pattern

While fixing one bug, also: reformatting quotes, adding type hints, rewriting
docstrings, "tightening" unrelated validation, or restructuring control flow.

## Preferred behavior

Produce a diff where every hunk is justified by the stated task. Surface
unrelated issues as notes for the user, not as silent edits.
