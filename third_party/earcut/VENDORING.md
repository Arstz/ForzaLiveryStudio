# Vendored earcut.hpp

Source: earcut.hpp v2.2.4 (https://github.com/mapbox/earcut.hpp), a C++ port of
the Mapbox earcut polygon triangulation library.

Only the single header `include/mapbox/earcut.hpp` is vendored, unchanged, along
with the upstream `LICENSE`.

Build (see the `earcut` interface target in ../../CMakeLists.txt): header only;
`polygon_mesh.cpp` includes it as `<earcut.hpp>`.

## Licensing

earcut.hpp is ISC licensed, which is permissive and compatible with this
project's AGPLv3. No source changes were made. See LICENSE.
