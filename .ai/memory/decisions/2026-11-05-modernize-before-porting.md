# Modernize the architecture before porting

- Date: 2026-11-05 (recorded here; original decision date is not recorded)
- Status: Accepted as current repository guidance

## Context

MapWinGIS 5 is a large MFC/ATL ActiveX project. Reusing its behavior can be
valuable, but reproducing its structure would constrain the new native Core
API and its separation from COM.

## Decision

Design MapWinGIS6 as maintainable modern C++ first. Use MapWinGIS 5 as a
behavioral reference, not as the architecture to transliterate.

## Consequences

New functionality should follow the v6 component boundaries and API
conventions. Legacy code and tests can inform expected behavior, but a legacy
class hierarchy or implementation pattern is not required.

## References

- [Mission and ground rules](../../../AGENTS.md)
- [Porting guidance](../../../docs/PORTING.md#what-port-means-here)
- [Architecture documentation](../../../docs/ARCHITECTURE.md)
