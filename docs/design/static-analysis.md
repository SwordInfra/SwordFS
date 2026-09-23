# Static analysis

SwordFS uses static analysis as a correctness and engineering-quality gate, not
as a style-warning collection. The analysis is deliberately layered because a
single translation unit cannot answer repository-wide liveness questions.

## Analysis layers

The CI pipeline generates `build/compile_commands.json` from the current
checkout with the normal CMake configure step. The compilation database is a
build artifact and is never committed.

Two analysis layers consume that database:

1. **clang-tidy** checks translation-unit-local defects. The project-wide
   static-audit entry point runs a small high-signal subset over every
   production translation unit, while the existing changed-file clang-tidy
   gate continues to run the full project configuration.
2. **Semantic symbol audit** parses all `src/` and `tests/` translation units
   with libclang. Clang USRs identify declarations across translation units so
   production and test references are counted by symbol identity rather than
   by text spelling. Translation units are independent parse inputs, so the CI
   entry point parses them in a bounded process pool (up to four workers by
   default) and deterministically merges their USR/reference sets afterward.

The AST walk descends only into repository-owned source/header locations.
Dependency-cache, generated build-tree, vendored third-party, and system-header
subtrees are pruned because traversing them dominates CI cost. This deliberately
means that library-mediated calls instantiated in system headers are not treated
as direct repository reference evidence. Protocols with that shape must remain
outside blocking classification unless the analyzer has an explicit reliable
model for them; standard lock-protocol methods are the current example.

The stable entry point is:

```bash
scripts/static-analysis/run.sh --build-dir build --output build/static-audit.json
```

The caller must configure the build first. The script does not create or cache
a compilation database on its own.

## Confidence model

The semantic audit owns only repository declarations under `src/`. References
are classified by their source location as production (`src/`), test
(`tests/`), or other repository/external locations.

The following findings are **high confidence** and block CI:

- a normal free function with a repository definition and no semantic
  reference anywhere in the scanned repository;
- a non-virtual, non-operator method with a repository definition and no
  semantic reference anywhere in the scanned repository;
- a namespace/class type alias or typedef with no semantic reference anywhere
  in the scanned repository;
- a production symbol whose only semantic consumers are tests. Such a symbol
  must be removed or the production/test construction design must be reworked
  so tests exercise a legitimate production contract.

The following findings are **review candidates** and do not block CI:

- a production symbol whose only semantic consumers are outside `src/` and
  `tests/`.
- standard lock-protocol methods (`lock`, `unlock`, `try_lock`, and shared-lock
  variants) with no direct production reference. Standard-library lock wrappers
  instantiate these calls in system headers, which the repository AST walk
  intentionally prunes, so repository-only reference absence is not sufficient
  evidence for a blocking dead-code conclusion.

Test-only use is treated as evidence of an invalid production surface. Tests
must not preserve setters, getters, hooks, flags, or other production APIs
whose only genuine consumers are tests. If a useful test scenario is difficult
to construct through the current production API, refactor the production
construction/configuration/dependency boundary so the test can use a cleaner
real contract instead of adding or retaining a test-only seam.

Every finding reports the Clang USR, qualified symbol, declaration/definition
location, and production/test/other reference sites. JSON output is the
machine-readable contract; CI also prints concise diagnostics and adds a job
summary.

## Conservative exclusions

Normal source references are not always a complete liveness signal. The audit
therefore excludes these declarations from blocking classification:

- virtual methods and overrides, whose runtime dispatch is rooted in an
  interface rather than a direct call to the concrete override;
- constructors, destructors, conversion functions, and operator overloads,
  where C++ and standard-library templates can create implicit or
  system-header-mediated references that are less useful as a high-confidence
  API-liveness signal;
- function/class templates and members whose semantic parent is a template;
- the process `main` entry point.

Callbacks and registry functions are not blanket-excluded. Passing a function
or method by address creates a real Clang reference and therefore keeps the
symbol live. SwordFS FUSE callback tables and static engine-registration
objects follow this rule naturally.

An entry point invoked only through a mechanism invisible to the C++ AST, such
as a string-based dynamic symbol lookup or a required external ABI symbol, must
use an exact suppression if SwordFS introduces such a surface.

The analyzer intentionally does not attempt to prove semantic redundancy,
authority duplication, or compatibility residue. It also cannot prove that a
self-recursive function or an isolated cycle of mutually-referencing symbols
is unreachable; those cases still require compiler diagnostics or engineering
review.

## clang-tidy contract

The project configuration keeps the existing `bugprone-*`, `performance-*`,
and `modernize-use-override` checks. The static-audit layer additionally
enforces a focused local-dead-code set across all production translation units:

- `clang-analyzer-deadcode.DeadStores`;
- `misc-unused-using-decls`.

`misc-unused-parameters` is not enabled globally. Filesystem, callback, and
interface implementations legitimately receive parameters required by an
external contract, so a project-wide parameter warning would create pressure
to distort interfaces or add noise rather than identify dead API surface.

New checks must have a clear failure mode, low false-positive rate on the
current repository, and a reason they belong in an enforced gate. Large check
families are not enabled merely to increase warning count.

## Suppressions

There is no permanent broad baseline. A suppression is allowed only for a
specific semantic finding and must contain:

- the exact finding category;
- the exact Clang USR;
- the reported qualified symbol for reviewability;
- a non-empty engineering reason that explains why the symbol is live despite
  the normal reference model.

Suppressions live in `scripts/static-analysis/suppressions.json`. A suppression
that no longer matches a current finding is an error so obsolete exceptions
are removed instead of accumulating.

`test-only-production-symbol` findings are not suppressible as intentional
test seams. The exact suppression mechanism remains available for categories
where the analyzer cannot see a real production reference (for example an
external ABI/dynamic lookup that is invisible to the C++ AST), but it must not
be used to retain production code solely for test construction or inspection.

This rule is semantic rather than name-based. Moving a test-only setter/hook
behind an optional parameter of a production-used function does not make the
test seam legitimate when that parameter path still has no production
consumer. Tests should construct state through the same production
configuration, factory/registry, dependency, or lifecycle path used by the
runtime. If those paths are too hard to use in a focused test, refactor the
production design so the real contract is cleaner instead of hiding the test
dependency inside an otherwise live API.

## CI contract

The `dead-code-audit` job performs these steps on the current checkout:

The job name intentionally reflects its scope: its repository-wide enforced
analysis is focused on dead/unused production code. Broader `bugprone-*`,
`performance-*`, and `modernize-use-override` clang-tidy checks run separately
on changed production files in the Debug `build-and-test` job.

```text
CMake configure
    -> build/compile_commands.json
    -> analyzer fixture verification
    -> full high-signal clang-tidy scan
    -> repository semantic symbol audit
    -> JSON artifact + GitHub summary/annotations
```

Any high-confidence unsuppressed finding, including a production symbol used
only by tests, fails the job. Review-only candidates remain visible in the JSON
artifact and job summary but do not block until a rule has demonstrated enough
precision to be promoted.

The dedicated fixture covers production use, true zero-reference symbols,
test-only use, virtual dispatch exclusions, callback/registration references,
type aliases, overloaded names, operator exclusions, and standard-library
lock-protocol indirection. It uses its own empty suppression set so repository
exceptions cannot mask fixture findings or be misreported as stale while the
fixture is analyzed in isolation.
This protects the semantic classification rules independently of whatever
symbols happen to exist in SwordFS production code.

The analyzer fixture is the functional regression contract for the Python
tooling. Python static-analysis scripts are intentionally outside the C++
per-file Codecov patch-coverage gate; their correctness is established by the
semantic fixture, the real-repository scan, and the CI gate itself. The shell
entry point contains orchestration only and is exercised end-to-end by the
`dead-code-audit` job.
