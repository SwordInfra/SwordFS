# Agent Instructions

## Build Environment

All compilation and building MUST be done inside the `swordfs:dev` Docker image, not directly on macOS. The project targets Linux and depends on Linux-specific libraries (libfuse3, etc.) that are unavailable on the host.

```bash
# Run cmake and build inside the container
docker run --rm -v $(pwd):/workspace -w /workspace swordfs:dev \
  bash -c "cmake --preset default && cmake --build build"
```

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

- **Section comments**: Use the following separator style when commenting on groups of related functions (e.g., "Public API", "Private helpers"):

  ```cpp
  // ────────────────────────────────────────────────────────────────
  // Public API
  // ────────────────────────────────────────────────────────────────
  ```
