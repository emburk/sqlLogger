# AGENTS.md

## Project Summary
Schema-driven telemetry DataLogger for SQL Server.

## Rules
- Warn for prompts that obviously contradicts with the design documents
- All functions must be briefly expressed with 1-2 line comments for readability
- All Non legacy c++ implementations should have 1-2 line comments for readability
- DataLoggerCore and SqlServerBackend stay single-threaded unless there is a very good approved reason
- Thread/async work is allowed only in an outer shell such as AsyncLogger
- Use real ODBC
- Use parameter-array batching
- CSV schema files only
- Maintain readability over optimization
- Do not redesign architecture without approval (do not make high level assumptions, ask first)
- Ask for permission each time "git -c .." command is necessary

## Development Log

- At each implementation or verification try, append one paragraph to `docs/04_DEV_LOGS.md`.
- Include timestamp with YYYY-MM-DD hour:minute:second.
- Record the purpose, rough output/result, and feedback or next observation.

## Important Docs
- docs/01_REQUIREMENTS.md
- docs/02_ARCHITECTURE.md
- docs/03_DESIGN_DECISIONS.md
- docs/04_DEV_LOGS.md