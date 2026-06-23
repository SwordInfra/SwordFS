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
- **Braces**: opening brace on the same line; closing brace on its own line
