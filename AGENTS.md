# Agent Instructions

## Dependency and Library Reuse

- **Prefer existing libraries over reimplementing functionality.** Before writing custom parsing, conversion, randomization, synchronization, string, container, or utility code, check whether Folly or an existing SwordFS utility already provides the required capability.
- **Use Folly wherever it is a suitable fit.** SwordFS already depends heavily on Folly; avoid introducing hand-written helpers that duplicate Folly functionality. Keep custom logic only for SwordFS-specific semantics or when the Folly API cannot express the required behavior.
- When reviewing code, explicitly look for opportunities to replace custom utility code with existing Folly/SwordFS facilities.

## Naming and Semantic Rules

- **Function names**: `PascalCase` (e.g., `DoSomething()`)
- **Variable names**: `snake_case` (e.g., `table_name`)
- **Class/struct names**: `PascalCase` (e.g., `MyClass`)
- **Type aliases**: Use a `Fn` suffix when the alias represents a function type (e.g., `EncodeEntryFn`), and a `Cb` suffix when it represents a callback type (e.g., `CompletionCb`).
- **Enum values**: `kCamelCase` (e.g., `kMyEnumValue`) — not `MACRO_CASE`
- **Macros**: `MACRO_CASE` (e.g., `MY_MACRO`)
- **Namespace names**: `snake_case`
- **`InodeID` variables**: MUST end with `_ino` (e.g., `parent_ino`, `child_ino`, `dir_ino`)
- **`SwordFsInode` variables**: MUST NOT end with `_ino`, `_inode`, or any similar suffix. Use plain names like `parent`, `child`, `dir`, `inode`.
- **`Status` return value variables**: MUST be named `status` — never `s`, `st`, `rc`, `ret`, or any abbreviation. A function returning `utils::Status` or similar status type must be captured as:
  ```cpp
  auto status = DoSomething();
  if (!status.ok()) { ... }
  ```

- **Class member sections**: Keep private methods and private data members in separate `private:` sections. Do not mix private methods and variables in the same section. Prefer separate `private:` labels over comment-based section markers.
- **Section comments**: Use the following separator style when commenting on groups of related functions (e.g., "Public API", "Private helpers"):

  ```cpp
  // ────────────────────────────────────────────────────────────────
  // Public API
  // ────────────────────────────────────────────────────────────────
  ```

## Git Commit Rules

- **Prefer one commit per PR.** For follow-up fixes, review feedback, CI fixes, formatting, or other changes that do not represent a significant independent unit of work, reuse the PR's existing commit with `git commit --amend` instead of creating additional commits.
- Create a new commit within the same PR only when the change is significant and logically independent enough to deserve separate review or rollback.
- Before committing follow-up work on an existing PR, inspect the PR's commit history and amend the appropriate existing commit whenever practical. Avoid accumulating fixup, cleanup, formatting, or "address review" commits.

## Editing Rules

- **NEVER use `sed`, `python`, `awk`, or any external command for direct file editing.** All file modifications MUST go through the `replace_string_in_file` tool so every change is visible and reviewable. This includes bulk find-and-replace operations — use `multi_replace_string_in_file` instead.
