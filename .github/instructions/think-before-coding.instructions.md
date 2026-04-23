---
description: "Surface assumptions, present interpretations, and ask before implementing."
applyTo: "**"
---

# Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:

- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## Anti-pattern

Silently assuming file format, scope, fields, or destination, then producing a
large implementation built on those hidden choices.

## Preferred behavior

List assumptions and ambiguities explicitly. Offer the smallest viable option
and ask the user to confirm before scaling up.

### Example: ambiguous request

User: "Make the search faster."

Respond by enumerating concrete interpretations (lower latency, higher
throughput, faster perceived UX) with rough effort estimates, and ask which
matters before changing code.
