# Code Style

Conventions for new and edited code in this codebase (excluding `tools/`).
These describe how code should read going forward; they are not a task list.

## Naming

- Name file-local and class-local constants with a `k` prefix in camelCase:
  `kRulerExtent`, `kSquareShapeId`, `kCurveHitSamples`.
- Write shape IDs and other registry values in decimal.
- Use the type's own vocabulary for members and locals; prefer full words over
  abbreviations that only the author will recognize.

## Values

- Prefer a named constant over an inline literal.
- Name each default value once and reference that name everywhere it is needed,
  including a member initializer and any setter fallback for the same field.
- Prefer an implicit or declarative definition over an ambiguous manual cycle:
  reach for a named algorithm, a table, or a single expression before writing an
  index-driven loop that reconstructs one.

## Structure

- Prefer a named function over an inline block that is used in more than one
  place or that carries its own idea.
- Keep member definitions in the same order as their declarations, grouped by
  concern.
- Keep a setter in the canonical form: return early when the value is unchanged,
  assign, invalidate the affected caches, then request a repaint.
- Let one function express one idea; when a function accumulates several, give
  each part a name.
- Group declarations by type category, in both struct definitions and function
  bodies: Qt and other class/composite types first, then integral types, then
  floating-point types.
- Declare a function's precomputed locals — values derived once and reused
  below — as a single block at the top of the function, followed by one blank
  line.

## Formatting

- Attach the opening brace to the line that introduces it, function and method
  definitions included; a brace never sits alone on its own line.
- Precede a function's final `return` with a blank line, unless the line
  immediately above it is a closing brace.

## Comments

- A comment carries intent the code cannot. It does not restate the code, does
  not enumerate particular cases, and does not describe what the code avoids.
- Keep documentation in `docs/`; a comment that grows into a documentation
  paragraph belongs there instead.
