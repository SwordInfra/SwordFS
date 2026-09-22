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
| [Static analysis](design/static-analysis.md) | Which automated code-quality gates run, what is considered high confidence, and how are exceptions handled? |
| [CI merge policy](design/ci-merge-policy.md) | Which CI results are mandatory before merge, and how is the repository merge gate enforced? |
| [POSIX conformance](design/posix-conformance.md) | How does pjdfstest define the supported regression contract, known semantic debt, and public conformance status? |
| [fstests conformance](design/fstests-conformance.md) | How does the broader Linux/FUSE fstests baseline select upstream tests, classify known gaps, and prevent silent coverage loss? |

## Documentation scope

Describe a key operation through its preconditions, steps, commit point, state
transitions, and failure handling. Use state machines and sequence diagrams
where they clarify ordering or concurrency. Keep each mechanism's detailed
explanation in one place and link to it from the overview.

Distinguish current behavior from limitations and future work. Avoid an
inventory of every method, Redis command, or wrapper in the implementation.
