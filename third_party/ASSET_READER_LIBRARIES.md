# Asset preview readers

Vendored, offline source subsets with upstream license notices. No nested repositories.

| Library | Pinned revision | Purpose |
|---|---|---|
| [ufbx](https://github.com/ufbx/ufbx) | v0.20.0, `a63ff0a47485328880b3300e7bcdf01413343a45` | FBX geometry, transforms, normals, UVs and material metadata |
| [miniz](https://github.com/richgel999/miniz) | 3.1.1, `d10b03cc73475af673df40f06e5cefd1d5f940d9` | Bounded in-memory DOCX/ZIP package access |
| [pugixml](https://github.com/zeux/pugixml) | v1.15, `ee86beb30e4973f5feffe3ce63bfa4fbadf72f38` | Word OOXML paragraphs, headings and table text |

`miniz_export.h` supplies the empty export annotation for this static build.
The engine does not extract packages to disk, execute scripts, or follow FBX external
file references. Model GPU resources remain owned by the existing preview renderer.
