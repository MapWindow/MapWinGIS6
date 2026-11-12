# Preserve compatibility selectively

- Date: 2026-11-05 (recorded here; original decision date is not recorded)
- Status: Accepted as current repository guidance

## Context

MapWinGIS has a long-lived COM API, but the v6 Core library is a new native
API. Requiring full legacy parity inside Core would import COM constraints and
legacy structure into unrelated internal design.

## Decision

Preserve established legacy behavior when it fits the v6 design and provides
value. Treat backward compatibility as a deliberate boundary-level concern,
not an absolute constraint on Core structure or APIs. Document intentional
differences and the reasons for them.

## Consequences

Check the contract of each legacy behavior before preserving it. In COM,
retain published interface identifiers and signatures when extending a
compatible interface; do not claim v5 compatibility for a new v6 interface
without verifying the contract.

## References

- [Porting and compatibility guidance](../../../docs/PORTING.md#compatibility-decisions)
- [Current ActiveX compatibility boundary](../../../docs/PORTING.md#current-activex-compatibility-boundary)
- [ActiveX interface definitions](../../../src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.idl)
