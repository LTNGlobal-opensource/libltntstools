# Review Focus

## Task
Review the codebase for the following, in order of priority:

1. **Memory safety**
   - Out-of-bounds reads/writes (buffers, arrays, pointer arithmetic)
   - Use-after-free / double-free
   - Uninitialized memory reads
   - Integer overflow/underflow that affects allocation sizes or offsets

2. **Null pointer problems**
   - Missing null checks before dereference
   - Functions that can return NULL/nullptr without callers checking
   - Assumptions that a pointer is valid without validating it at the boundary

3. **Excessive memory use**
   - Unnecessary copies of large buffers
   - Allocations that scale poorly with input size
   - Data held longer than needed (should be freed/scoped tighter)

4. **Memory leaks**
   - Missing frees/deletes on all exit paths (including early returns and error paths)
   - Leaks on exception/error handling paths specifically — check these first,
     they're the most commonly missed
   - Ownership ambiguity (unclear who is responsible for freeing what)

5. **Function argument consistency**
   - Inconsistent parameter ordering across similar functions (e.g. buffer,
     length vs length, buffer)
   - Inconsistent naming/types for the same concept across the API
   - Inconsistent const-correctness

6. **Result codes**
   - Every failure path returns a distinct, meaningful code (no silent
     `-1`/`0` overloads that collide with valid values)
   - Result codes are documented and consistent across the module
   - Callers actually check and propagate result codes rather than ignoring them

## Output format
For each issue found:
- File and line number(s)
- Category (from the list above)
- Severity (critical / moderate / minor)
- Short explanation of the risk
- Suggested fix (describe it; don't apply it — see workflow below)

## Workflow rules
- **Do not stage or commit any changes.** This is a review pass, not a fix pass,
  unless I explicitly ask you to apply a specific fix.
- If asked to apply a fix, make the edit but do not run `git add` or `git commit`.
- I will review all changes myself before anything is committed.
- Prefer flagging issues over "fixing while reviewing" — keep review and
  modification as separate steps.
- Always create unit tests for any proposed changes
