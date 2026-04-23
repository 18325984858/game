---
description: "Write the minimum code that solves the problem. Nothing speculative."
applyTo: "**"
---

# Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes,
simplify.

## Anti-pattern

Introducing strategy classes, dependency injection, plugin systems, or generic
config layers for code that has exactly one caller and one use case.

## Preferred behavior

Write a single function/file that solves today's request. Add complexity only
when a real second use case appears.

### Example: discount calculation

A request to "add a function to calculate discount" should produce one small
function, not an abstract `DiscountStrategy` hierarchy with a config object.
