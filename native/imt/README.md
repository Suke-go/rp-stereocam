# native/imt

`imt` is the Importance-Map Transport core library.

It is a small C99 library for turning a per-tile importance map into:

- normalized tile weights
- QP offset lookup values
- stable high-importance-first ordering
- XOR FEC groups
- byte-exact little-endian wire packets
- receiver-side frame assembly and FEC recovery
- fixed-point PI pacing state

The library is deliberately independent of VR, cameras, codecs, sockets, and
operating-system clocks.

The public API is the single header:

```c
#include "imt.h"
```

Heap allocation is restricted to `*_init` functions and released by matching
`*_destroy` functions. Steady-state code uses integer arithmetic and lookup
tables only; floating point is used only when building the QP LUT.

Wire packets use a fixed 40-byte little-endian header. The implementation writes
each field byte-by-byte and never sends C structs with `memcpy`.

Build standalone:

```sh
cmake -S native/imt -B build/imt
cmake --build build/imt --config Debug
ctest --test-dir build/imt --output-on-failure
```

The same directory can also be consumed with `add_subdirectory(native/imt)`.
That path provides the `imt` library target without modifying the repository
root CMake files.
