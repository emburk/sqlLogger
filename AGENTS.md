# AGENTS.md

## Project Summary
Schema-driven telemetry DataLogger for SQL Server.

## Rules
- Warn for prompts that obviously contradicts with the design documents
- All functions must be briefly expressed with 1-2 line comments for readability
- All Non legacy c++ implementations should have 1-2 line comments for readability
- Single-threaded only
- No async/concurrency
- Use real ODBC
- Use parameter-array batching
- CSV schema files only
- Maintain readability over optimization
- Do not redesign architecture without approval (do not make high level assumptions, ask first)


## Important Docs
- docs/01_REQUIREMENTS.md
- docs/02_ARCHITECTURE.md
- docs/03_DESIGN_DECISIONS.md