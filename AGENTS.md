# Agent Instructions

## Dependency and Library Reuse

- **Prefer existing libraries over reimplementing functionality.** Before writing custom parsing, conversion, randomization, synchronization, string, container, or utility code, check whether Folly or an existing SwordFS utility already provides the required capability.
- **Use Folly wherever it is a suitable fit.** SwordFS already depends heavily on Folly; avoid introducing hand-written helpers that duplicate Folly functionality. Keep custom logic only for SwordFS-specific semantics or when the Folly API cannot express the required behavior.
- When reviewing code, explicitly look for opportunities to replace custom utility code with existing Folly/SwordFS facilities.

## Editing Rules

- **NEVER use `sed`, `python`, `awk`, or any external command for direct file editing.** All file modifications MUST go through the `replace_string_in_file` tool so every change is visible and reviewable. This includes bulk find-and-replace operations — use `multi_replace_string_in_file` instead.

## Code Style

All C++ code MUST follow the **Google C++ Style Guide**. Key rules include:

- **Function names**: `PascalCase` (e.g., `DoSomething()`)
- **Variable names**: `snake_case` (e.g., `table_name`)
- **Class/struct names**: `PascalCase` (e.g., `MyClass`)
- **Enum values**: `kCamelCase` (e.g., `kMyEnumValue`) — not `MACRO_CASE`
- **Macros**: `MACRO_CASE` (e.g., `MY_MACRO`)
- **Pointers/references**: `type* ptr` — asterisk/ampersand adjacent to the type, not the variable name
- **Namespace names**: `snake_case`
- **`InodeID` variables**: MUST end with `_ino` (e.g., `parent_ino`, `child_ino`, `dir_ino`)

- **`SwordFsInode` variables**: MUST NOT end with `_ino`, `_inode`, or any similar suffix. Use plain names like `parent`, `child`, `dir`, `inode`.

- **`Status` return value variables**: MUST be named `status` — never `s`, `st`, `rc`, `ret`, or any abbreviation. A function returning `utils::Status` or similar status type must be captured as:
  ```cpp
  auto status = DoSomething();
  if (!status.ok()) { ... }
  ```

- **Section comments**: Use the following separator style when commenting on groups of related functions (e.g., "Public API", "Private helpers"):

  ```cpp
  // ────────────────────────────────────────────────────────────────
  // Public API
  // ────────────────────────────────────────────────────────────────
  ```
