# Bundled spdlog

- Upstream: https://github.com/gabime/spdlog/tree/v1.16.0
- Version: 1.16.0
- Source: https://codeload.github.com/gabime/spdlog/zip/refs/tags/v1.16.0
- Archive SHA256: `3d25808d2fc4db86621a46855800c99ab5734999b61c4cbf9470edf631555397`
- Files: unmodified upstream `include/` and `LICENSE`, including bundled fmt.
- License: MIT; bundled fmt retains its license in its headers.

CMake falls back to these header-only sources when no installed spdlog target
is found. No network access or optional dependency fetch is needed for logging.
