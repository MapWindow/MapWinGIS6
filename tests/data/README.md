# Test data

Small, hand-written datasets used by the C++ tests. Everything here is
generated from the text sources in `source/`, so the binary files can be
recreated at any time and reviewed through their sources.

## `provinces`

Four adjacent 1 km² squares in **EPSG:28992** (Amersfoort / RD New), available
as a shapefile and as a GeoPackage:

| File set | Notes |
|---|---|
| `provinces.shp` + `.shx` + `.dbf` + `.prj` + `.cpg` | `.cpg` says UTF-8 |
| `provinces.gpkg` | Same layer, same attributes |

Layer extent: `(100000, 400000)` – `(104000, 401000)`.

| ID | NAME | AREA_KM2 | SURVEYED | ACTIVE | X range |
|---|---|---|---|---|---|
| 1 | Noord | 1.0 | 2020-01-15 | 1 | 100000 – 101000 |
| 2 | Oost | 1.0 | 2021-06-30 | 1 | 101000 – 102000 |
| 3 | Fryslân | 1.0 | 2022-11-01 | 0 | 102000 – 103000 |
| 4 | *(null)* | 1.0 | *(null)* | 1 | 103000 – 104000 |

It is deliberately small but covers the things that tend to break:

- all five MapWinGIS field types, including `Date` and a boolean;
- a **null** string and a **null** date, so "null" and "empty" can be told apart;
- a **non-ASCII** value (`Fryslân`), so encoding problems show up;
- a **projected** CRS with a datum shift, rather than plain WGS84;
- round coordinates, so tests can assert exact values.

## Regenerating

Only needed when `source/provinces.csv` or `source/provinces.vrt` changes:

```powershell
pwsh -File build\scripts\generate-testdata.ps1 -GdalBin <folder containing ogr2ogr.exe>
```

Tests never write to this folder. Anything that modifies data copies it to a
temporary directory first.
