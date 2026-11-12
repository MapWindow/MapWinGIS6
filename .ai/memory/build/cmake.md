# CMake status

There is no standalone CMake project in the current repository. The supported
solution build is `MapWinGIS6.slnx` through MSBuild. The `.cmake` files under
`build/triplets/` are vcpkg overlay triplets; they do not define a CMake build
for MapWinGIS6.

Use the repository's MSBuild and vcpkg instructions rather than treating the
overlay triplets as a project generator. Recheck the repository before relying
on this status in future work.

## References

- [Build guide](../../../docs/BUILDING.md#first-build)
- [Solution](../../../MapWinGIS6.slnx)
- [vcpkg overlay triplets](../../../build/triplets/)
