# AGENTS.md

## Repository workflow

- Do not commit generated artifacts, crash reports, build output, or local
  configuration unless explicitly requested.

## Build and test

- Use the repository build wrapper: `./build.sh` on Unix and `.\build.bat` on Windows.
- Do not invoke `scons` directly; direct invocations can cause avoidable full
  rebuild churn.
- Run the smallest relevant build or test first, then the required verification
  gate.
- Keep test artifacts isolated and identify the binary, command line, and
  source revision used to produce them.
- Preserve diagnostics and logs when a test crashes or fails.

## Code changes

- Touch only files within the requested scope and code owned by the current
  task. Preserve unrelated edits.
- Prefer simple code, explicit constants, fixed layouts, and compile-time
  configuration where appropriate.
- Follow existing formatting and naming. Trust repository formatter and style
  tooling.
- Do not add speculative abstractions or unrelated cleanup.
- Do not use web search for repository work.
