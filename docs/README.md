# SwordFS design documentation

These documents explain mechanisms, state ownership, concurrency, and recovery.
API signatures and routine implementation details belong in source comments.

## Reading path

| Document | Questions it answers |
| --- | --- |
| [Architecture](design/architecture.md) | What are the subsystem boundaries, key workflows, and correctness invariants? |
| [Data structures](design/data-structures.md) | How do records, handles, chunks, and reclaim work relate? Which state survives restart? |
| [Thread model](design/thread-model.md) | Which threads run which work? Where do fibers suspend, and what protects shared state? |
| [Chunk publication](design/chunk-publication.md) | How does a buffered write become authoritative? What happens on failure or conflict? |
| [Redis metadata](design/redis-metadata-schema.md) | How are logical records stored, and where are the transaction boundaries? |

## Documentation scope

Describe a key operation through its preconditions, steps, commit point, state
transitions, and failure handling. Use state machines and sequence diagrams
where they clarify ordering or concurrency. Keep each mechanism's detailed
explanation in one place and link to it from the overview.

Distinguish current behavior from limitations and future work. Avoid an
inventory of every method, Redis command, or wrapper in the implementation.
