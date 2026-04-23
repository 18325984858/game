---
description: "Define verifiable success criteria. Loop until verified."
applyTo: "**"
---

# Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:

- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:

```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it
work") require constant clarification.

## Anti-pattern

"I'll review the code and make improvements" — open-ended work with no test or
observable check that says when the task is done.

## Preferred behavior

Before changing code, write or identify the test/command/output that will prove
the change is correct. Run it before and after.

### Example: reproducing a bug first

For "the sorting breaks on duplicate scores," write a failing test that
demonstrates the non-deterministic order, then change the sort to make it pass.
