# Generate Grafana Dashboard

Use `tools/grafanaDropdownGenerator.py` to generate a Grafana dashboard JSON from the DataLogger CSV schemas.

## Example

```powershell
python tools\grafanaDropdownGenerator.py ExampleApp\local\schemas ExampleApp\local\grafana_dropdown_dashboard.json
```

## Inputs

- `schema_directory`: directory containing DataLogger schema CSV files.
- `output`: path for the generated Grafana dashboard JSON.

## Optional Arguments

```powershell
python tools\grafanaDropdownGenerator.py ExampleApp\local\schemas ExampleApp\local\grafana_dropdown_dashboard.json --datasource-uid MSSQL --sql-schema dbo --title "AO Telemetry"
```

- `--datasource-uid`: Grafana SQL Server datasource UID. Default is `MSSQL`.
- `--sql-schema`: SQL Server schema name. Default is `dbo`.
- `--title`: dashboard title.

## Output

The generated JSON contains:

- one dropdown variable per schema table;
- one MSSQL time-series panel per schema table;
- queries ordered by `timestamp_ms`.

Import `ExampleApp\local\grafana_dropdown_dashboard.json` through Grafana's dashboard import UI.

## Import in Grafana UI

1. Open Grafana in your browser.
2. In the left navigation, select **Dashboards**.
3. Click **New** and choose **Import**.
4. Click **Upload dashboard JSON file**.
5. Select `ExampleApp\local\grafana_dropdown_dashboard.json`.
6. Confirm the dashboard name and folder.
7. Select the SQL Server datasource if Grafana asks for one.
8. Click **Import**.

After import, use the dropdowns at the top of the dashboard to choose which column each table panel displays.
