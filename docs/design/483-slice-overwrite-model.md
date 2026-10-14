# Slice Chunk ordered overlay model (#483)

The common FileMetadata mapping remains fixed-size `ChunkIndex -> ChunkID`. Only
the Slice mechanism interprets the ordered Slice records inside one `ChunkID`;
the COW mechanism and its full-object head stay independent.

## V1 model (based on JuiceFS)

An immutable object corresponds to a contiguous Slice (`SliceID`, nonzero).
Each appended record has five logical fields `(pos, id, size, off, len)`:
chunk-local position, object identity, full object byte size, offset within the
object, and covered logical length. A clipped record retains its physical
identity and increments `off`: clipping does not upload new objects.

`SliceID == 0` is an explicit zero-cover record with `size == off == 0`.
Uncovered ranges are implicitly zero. Apply records in **committed append
order**: a later record wins every overlapping byte, regardless of its
`SliceID` allocation order. The derived read view is a complete sequence of
nonoverlapping, bounded object-reference or zero ranges; it is not persisted.

Chunk-local fields are 32-bit while one chunk's configured size fits in a
`uint32_t`; `SliceID` is a nonzero 64-bit number within the signed Redis
counter range. Check `pos/len` and `off/len` by subtraction to avoid overflow.
Reject zero-length records, malformed object references, noncanonical zeros,
or ranges beyond the fixed-size chunk.

## Scope boundary

#483 defines and tests the pure records, validation and range planner only.
Redis codec/index, S3 I/O, authoritative cross-mount publication, truncate,
GC and compaction remain #484/#485/#486/#424/#272/#273/#274 respectively.
The planner has no metadata I/O or global state, and its results must match a
byte-array oracle applying the same records in append order.

JuiceFS peer model: [slice.go](https://github.com/juicedata/juicefs/blob/main/pkg/meta/slice.go)
(`slice`, `buildSlice`).
