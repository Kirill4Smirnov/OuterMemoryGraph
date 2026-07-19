# Internal graph format v1

The preprocessing command converts CSV or SNAP-style edge lists into a static,
destination-sharded pull representation. The graph directory is considered
valid only after `manifest.json` has been written.

## Platform

Version 1 targets Linux on little-endian x86-64. Integer files are raw
little-endian arrays. All record sizes are asserted at compile time.

## Files

- `manifest.json`: version, counts, source path, and ordered shard metadata.
- `vertices.bin`: dense-ID order array of signed 32-bit original vertex IDs.
- `outdegrees.bin`: dense-ID order array of unsigned 32-bit outdegrees.
- `shard-NNNNN.bin`: sorted `DiskEdge` records owned by one destination range.

Each shard record is exactly eight bytes:

```text
uint32 little-endian destination
uint32 little-endian source
```

Records are sorted lexicographically by `(destination, source)`. Shard vertex
ranges are disjoint, contiguous, and cover `[0, vertex_count)`. This ownership
rule lets PageRank workers write separate `next_rank` ranges without atomics.

## Vertex semantics

Without a separate vertex file, the vertex set is the union of all edge
endpoints. Original signed int32 identifiers are sorted and mapped to dense IDs
in `[0, vertex_count)`. Duplicate edges are removed during the final merge;
self-loops are retained.

