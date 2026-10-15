# Slice Chunk ordered overlay model (#483)

The common FileMetadata mapping remains fixed-size `ChunkIndex -> ChunkID`. Only
the Slice mechanism interprets the ordered Slice records inside one `ChunkID`;
the COW mechanism and its full-object head stay independent.

## V1 model (based on JuiceFS)

A contiguous **logical Slice** has a nonzero `SliceID` and may grow through
sequential tail appends while it is still writable. Each history record has
five fields `(pos, id, size, off, len)`: chunk-local position, Slice identity,
the Slice length **at the time of the record**, offset within the Slice, and
covered logical length. The same `SliceID` may therefore occur with different
`size` values, which must not be rejected merely for being different. An
unchanged Slice prefix retains its offsets and identity across append records;
clipping a visible record also preserves these references.

**Append eligibility:** an append extends the same Slice's contiguous logical
range at its tail. Once a newer *different* Slice (or an explicit zero overlay)
has overwritten any of its existing range, that Slice is frozen against
further appends; later writes require a new Slice. A read-only range planner
interprets already committed records, while the Slice Writer (#485) and the
cross-mount authoritative publication check (#271/#424) must enforce this
write-time rule, including races with other mounts.

Logical Slice growth must not be mistaken for portable, in-place modification
of an S3 object. The actual object upload/append representation and safe
visibility to existing readers are design responsibilities of #485/#271;
#483 defines neither an S3 append operation nor an object-key mutation policy.

`SliceID == 0` is an explicit zero-cover record with `size == off == 0`.
Uncovered ranges are implicitly zero. Apply records in **committed append
order**: a later record wins every overlapping byte, regardless of its
`SliceID` allocation order. The derived read view is a complete sequence of
nonoverlapping, bounded object-reference or zero ranges; it is not persisted.

Chunk-local fields are 32-bit while one chunk's configured size fits in a
`uint32_t`; `SliceID` is a nonzero 64-bit number within the signed Redis
counter range. Check `pos/len` and `off/len` by subtraction to avoid overflow.
Reject zero-length records, malformed Slice references, noncanonical zeros,
or ranges beyond the fixed-size chunk.

## Scope boundary

#483 defines and tests the pure records, validation and range planner only.
Redis codec/index, S3 I/O, authoritative cross-mount publication, truncate,
GC and compaction remain #484/#485/#486/#424/#272/#273/#274 respectively.
The planner has no metadata I/O or global state, and its results must match a
byte-array oracle applying the same records in append order.

JuiceFS peer model: [slice.go](https://github.com/juicedata/juicefs/blob/main/pkg/meta/slice.go)
(`slice`, `buildSlice`).
