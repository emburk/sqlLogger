#!/usr/bin/env python3
"""Generate one Grafana dashboard JSON with table-specific column dropdowns.

The script reads DataLogger CSV schemas to discover SQL table names, creates one
multi-select Grafana query variable per table, and writes one timeseries panel
with a separate MSSQL query target for every table.

Example:
    python tools/grafanaDropdownGenerator.py examples/ExampleApp/local/schemas examples/ExampleApp/local/grafana_dropdown_dashboard.json
"""

import argparse
import csv
import json
import re
from dataclasses import dataclass
from pathlib import Path


TIMESTAMP_COLUMN = "timestamp_ms"
NO_SELECTION_TEXT = "(none)"
NO_SELECTION_VALUE = "CAST(NULL AS float) AS [No column selected]"
DEFAULT_PANEL_HEIGHT = 8
DEFAULT_PANEL_WIDTH = 24
DEFAULT_SCHEMA_VERSION = 41
IDENTIFIER_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass(frozen=True)
class TableSchema:
    """Store the SQL table name and expanded payload columns parsed from one CSV file."""
    table_name: str
    columns: list[str]


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse schema input, output JSON, and Grafana SQL dashboard settings."""
    parser = argparse.ArgumentParser(
        description="Generate one Grafana dashboard with a dropdown variable per DataLogger table."
    )
    parser.add_argument(
        "schema_directory",
        help="Directory containing DataLogger CSV schema files.",
    )
    parser.add_argument(
        "output",
        help="Path to write the generated Grafana dashboard JSON.",
    )
    parser.add_argument(
        "--datasource-uid",
        default="MSSQL",
        help="Grafana Microsoft SQL Server datasource UID/name used in dashboard queries.",
    )
    parser.add_argument(
        "--database",
        default="",
        help="Optional SQL Server database name to place in Grafana query metadata.",
    )
    parser.add_argument(
        "--sql-schema",
        default="dbo",
        help="SQL Server schema name used in generated queries.",
    )
    parser.add_argument(
        "--title",
        default="DataLogger Dropdown Dashboard",
        help="Dashboard title.",
    )
    parser.add_argument(
        "--hide-variables",
        action="store_true",
        help="Place table variables in Grafana's controls menu. This is the default.",
    )
    parser.add_argument(
        "--show-variables",
        action="store_false",
        dest="hide_variables",
        help="Show table variables in the main Grafana variable bar.",
    )
    parser.set_defaults(hide_variables=True)
    return parser.parse_args(argv)


def validate_identifier(value: str, label: str) -> None:
    """Reject table, column, and schema names that cannot be used safely in SQL text."""
    if not IDENTIFIER_PATTERN.fullmatch(value):
        raise ValueError(f"invalid {label} identifier: {value}")


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    """Read one DataLogger CSV schema file and trim every header and cell value."""
    with path.open(newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(line for line in source if not line.lstrip().startswith("#"))
        return [
            {(key or "").strip(): (value or "").strip() for key, value in row.items()}
            for row in reader
        ]


def natural_sort_key(path: Path) -> list[object]:
    """Sort schema filenames in human order so table_2 comes before table_10."""
    return [
        int(part) if part.isdigit() else part.lower()
        for part in re.split(r"(\d+)", path.stem)
    ]


def expanded_column_names(rows: list[dict[str, str]], path: Path) -> list[str]:
    """Expand DataLogger array columns into the final SQL payload column names."""
    columns: list[str] = []
    for line_number, row in enumerate(rows, start=2):
        column_name = row.get("column_name", "")
        if not column_name:
            raise ValueError(f"{path} line {line_number}: missing column_name")
        validate_identifier(column_name, "column")

        try:
            length = int(row.get("length", ""))
        except ValueError as exc:
            raise ValueError(
                f"{path} line {line_number}: invalid length '{row.get('length', '')}'"
            ) from exc

        if length == 1:
            columns.append(column_name)
        elif length > 1:
            columns.extend(f"{column_name}_{index}" for index in range(length))
        else:
            raise ValueError(f"{path} line {line_number}: length must be positive")

    return columns


def load_schema_directory(schema_directory: Path) -> list[TableSchema]:
    """Load every CSV schema and return deterministic table metadata."""
    if not schema_directory.is_dir():
        raise ValueError(f"schema directory does not exist: {schema_directory}")

    tables: list[TableSchema] = []
    for schema_file in sorted(schema_directory.glob("*.csv"), key=natural_sort_key):
        table_name = schema_file.stem
        validate_identifier(table_name, "table")

        rows = read_csv_rows(schema_file)
        columns = expanded_column_names(rows, schema_file)
        if not columns:
            raise ValueError(f"{schema_file}: schema has no payload columns")

        tables.append(TableSchema(table_name, columns))

    if not tables:
        raise ValueError(f"schema directory has no CSV files: {schema_directory}")

    return tables


def datasource_ref(datasource_uid: str) -> dict:
    """Build the Grafana datasource reference used by dashboard v2 query JSON."""
    return {"name": datasource_uid}


def ref_id(index: int) -> str:
    """Return spreadsheet-style query refIds: A through Z, then AA, AB, and so on."""
    result = ""
    value = index
    while True:
        result = chr(ord("A") + (value % 26)) + result
        value = value // 26 - 1
        if value < 0:
            return result


def variable_definition(sql_schema: str, table_name: str) -> str:
    """Create the SQL query Grafana uses to populate one table's column dropdown."""
    return (
        "SELECT\n"
        "  __text,\n"
        "  __value\n"
        "FROM\n"
        "(\n"
        "  SELECT\n"
        "    0 AS sort_order,\n"
        f"    '{NO_SELECTION_TEXT}' AS __text,\n"
        f"    '{NO_SELECTION_VALUE}' AS __value\n"
        "  UNION ALL\n"
        "  SELECT\n"
        "    ORDINAL_POSITION AS sort_order,\n"
        "    COLUMN_NAME AS __text,\n"
        "    QUOTENAME(COLUMN_NAME) AS __value\n"
        "  FROM INFORMATION_SCHEMA.COLUMNS\n"
        f"  WHERE TABLE_SCHEMA = '{sql_schema}'\n"
        f"    AND TABLE_NAME = '{table_name}'\n"
        f"    AND COLUMN_NAME <> '{TIMESTAMP_COLUMN}'\n"
        ") AS column_options\n"
        "ORDER BY sort_order;"
    )


def dashboard_variable(table: TableSchema,
                       datasource_uid: str,
                       sql_schema: str,
                       hide_variables: bool) -> dict:
    """Build one multi-select Grafana query variable for a table's payload columns."""
    definition = variable_definition(sql_schema, table.table_name)
    return {
        "kind": "QueryVariable",
        "spec": {
            "allowCustomValue": True,
            "current": {"text": [NO_SELECTION_TEXT], "value": [NO_SELECTION_VALUE]},
            "definition": definition,
            "hide": "inControlsMenu" if hide_variables else "dontHide",
            "includeAll": False,
            "label": table.table_name,
            "multi": True,
            "name": table.table_name,
            "options": [],
            "query": {
                "datasource": datasource_ref(datasource_uid),
                "group": "mssql",
                "kind": "DataQuery",
                "spec": {"__legacyStringValue": definition},
                "version": "v0",
            },
            "refresh": "onDashboardLoad",
            "regex": "",
            "regexApplyTo": "value",
            "skipUrlSync": False,
            "sort": "disabled",
        },
    }


def table_query_sql(table: TableSchema, sql_schema: str) -> str:
    """Create one MSSQL time-series query that uses the matching table variable."""
    return (
        "SELECT\n"
        f"  [{TIMESTAMP_COLUMN}] AS time,\n"
        f"  ${{{table.table_name}:csv}}\n"
        "FROM\n"
        f"  [{sql_schema}].[{table.table_name}]\n"
        "ORDER BY time;"
    )


def query_spec(table: TableSchema,
               query_index: int,
               datasource_uid: str,
               database: str,
               sql_schema: str) -> dict:
    """Build one visible panel target for a single SQL table."""
    spec = {
        "editorMode": "code",
        "format": "time_series",
        "rawQuery": True,
        "rawSql": table_query_sql(table, sql_schema),
        "sql": {
            "columns": [{"parameters": [], "type": "function"}],
            "groupBy": [{"property": {"type": "string"}, "type": "groupBy"}],
            "limit": 50,
        },
        "table": f"{sql_schema}.{table.table_name}",
    }
    if database:
        spec["dataset"] = database

    return {
        "kind": "PanelQuery",
        "spec": {
            "hidden": False,
            "query": {
                "datasource": datasource_ref(datasource_uid),
                "group": "mssql",
                "kind": "DataQuery",
                "spec": spec,
                "version": "v0",
            },
            "refId": ref_id(query_index),
        },
    }


def timeseries_panel(tables: list[TableSchema],
                     datasource_uid: str,
                     database: str,
                     sql_schema: str) -> dict:
    """Create the single Grafana timeseries panel containing one query per table."""
    return {
        "kind": "Panel",
        "spec": {
            "data": {
                "kind": "QueryGroup",
                "spec": {
                    "queries": [
                        query_spec(table, index, datasource_uid, database, sql_schema)
                        for index, table in enumerate(tables)
                    ],
                    "queryOptions": {},
                    "transformations": [],
                },
            },
            "description": "",
            "id": 1,
            "links": [],
            "title": "Telemetry",
            "vizConfig": {
                "group": "timeseries",
                "kind": "VizConfig",
                "spec": {
                    "fieldConfig": {
                        "defaults": {
                            "color": {"mode": "palette-classic"},
                            "custom": {
                                "axisBorderShow": False,
                                "axisCenteredZero": False,
                                "axisColorMode": "text",
                                "axisLabel": "",
                                "axisPlacement": "auto",
                                "barAlignment": 0,
                                "barWidthFactor": 0.6,
                                "drawStyle": "line",
                                "fillOpacity": 0,
                                "gradientMode": "none",
                                "hideFrom": {"legend": False, "tooltip": False, "viz": False},
                                "insertNulls": False,
                                "lineInterpolation": "linear",
                                "lineWidth": 1,
                                "pointSize": 5,
                                "scaleDistribution": {"type": "linear"},
                                "showPoints": "auto",
                                "showValues": False,
                                "spanNulls": False,
                                "stacking": {"group": "A", "mode": "none"},
                                "thresholdsStyle": {"mode": "off"},
                            },
                            "thresholds": {
                                "mode": "absolute",
                                "steps": [
                                    {"color": "green", "value": 0},
                                    {"color": "red", "value": 80},
                                ],
                            },
                        },
                        "overrides": [],
                    },
                    "options": {
                        "annotations": {"clustering": -1, "multiLane": False},
                        "legend": {
                            "calcs": [],
                            "displayMode": "list",
                            "placement": "bottom",
                            "showLegend": True,
                        },
                        "tooltip": {"hideZeros": False, "mode": "single", "sort": "none"},
                    },
                },
                "version": "13.0.1+security-01",
            },
        },
    }


def annotations() -> list[dict]:
    """Create Grafana's default annotation query block."""
    return [
        {
            "kind": "AnnotationQuery",
            "spec": {
                "builtIn": True,
                "enable": True,
                "hide": True,
                "iconColor": "rgba(0, 211, 255, 1)",
                "name": "Annotations & Alerts",
                "query": {
                    "datasource": datasource_ref("-- Grafana --"),
                    "group": "grafana",
                    "kind": "DataQuery",
                    "spec": {},
                    "version": "v0",
                },
            },
        }
    ]


def build_dashboard(tables: list[TableSchema],
                    title: str,
                    datasource_uid: str,
                    database: str,
                    sql_schema: str,
                    hide_variables: bool) -> dict:
    """Assemble the complete Grafana dashboard JSON document."""
    return {
        "annotations": annotations(),
        "cursorSync": "Off",
        "editable": True,
        "elements": {"panel-1": timeseries_panel(tables, datasource_uid, database, sql_schema)},
        "layout": {
            "kind": "GridLayout",
            "spec": {
                "items": [
                    {
                        "kind": "GridLayoutItem",
                        "spec": {
                            "element": {"kind": "ElementReference", "name": "panel-1"},
                            "height": DEFAULT_PANEL_HEIGHT,
                            "width": DEFAULT_PANEL_WIDTH,
                            "x": 0,
                            "y": 0,
                        },
                    }
                ],
            },
        },
        "links": [],
        "liveNow": False,
        "preferences": {"layout": {"kind": "GridLayout", "spec": {"items": []}}},
        "preload": False,
        "schemaVersion": DEFAULT_SCHEMA_VERSION,
        "tags": [],
        "timeSettings": {
            "autoRefresh": "",
            "autoRefreshIntervals": ["5s", "10s", "30s", "1m", "5m", "15m", "30m", "1h", "2h", "1d"],
            "fiscalYearStartMonth": 0,
            "from": "now-6h",
            "hideTimepicker": False,
            "timezone": "browser",
            "to": "now",
        },
        "title": title,
        "variables": [
            dashboard_variable(table, datasource_uid, sql_schema, hide_variables)
            for table in tables
        ],
    }


def write_dashboard(output_path: Path, dashboard: dict) -> None:
    """Write the generated dashboard JSON with stable indentation."""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as target:
        json.dump(dashboard, target, indent=2)
        target.write("\n")


def main(argv: list[str] | None = None) -> int:
    """Load schemas, generate the dashboard JSON, and report the result."""
    args = parse_args(argv)
    validate_identifier(args.sql_schema, "SQL schema")

    tables = load_schema_directory(Path(args.schema_directory))
    dashboard = build_dashboard(
        tables,
        args.title,
        args.datasource_uid,
        args.database,
        args.sql_schema,
        args.hide_variables,
    )
    write_dashboard(Path(args.output), dashboard)
    print(f"generated one Grafana panel with {len(tables)} table queries: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
