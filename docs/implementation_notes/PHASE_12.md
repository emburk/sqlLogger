# Phase 12: Static Library Example Solution

## Phase objective

Add a separate Visual Studio solution that validates library-style consumption
without removing the retained monolithic solution.

The phase adds:

- `DataLoggerCore/DataLoggerCore.vcxproj` as a static library;
- `SqlServerBackend/SqlServerBackend.vcxproj` as a static library;
- `ExampleAppLib/ExampleAppLib.sln` as the library-based example solution;
- `ExampleAppLib/ExampleAppLib.vcxproj` as the executable that reuses
  `ExampleApp/main.cpp`.

## Design notes

- No open design decision was found that blocks this phase.
- `DEC-029` originally chose one project for the first implementation, but also
  allowed splitting the folder boundaries into separate projects later.
- `DEC-032` records the new resolved decision to keep the monolithic solution
  while adding this library-based example solution.

## Ownership split

- `DataLoggerCore` owns schema loading, validation, decoding, buffering, and the
  `IDBBackend` interface.
- `SqlServerBackend` owns the concrete SQL Server ODBC backend and depends on
  `DataLoggerCore`.
- `ExampleAppLib` owns only the executable project and links both static
  libraries plus `odbc32.lib`.

## Files changed

- `DataLoggerCore/DataLoggerCore.vcxproj`
- `DataLoggerCore/DataLoggerCore.vcxproj.filters`
- `SqlServerBackend/SqlServerBackend.vcxproj`
- `SqlServerBackend/SqlServerBackend.vcxproj.filters`
- `ExampleAppLib/ExampleAppLib.sln`
- `ExampleAppLib/ExampleAppLib.vcxproj`
- `ExampleAppLib/ExampleAppLib.vcxproj.filters`
- `README.md`
- `docs/03_DESIGN_DECISIONS.md`
- `docs/05_IMPLEMENTATION_PLAN.md`
