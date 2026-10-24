# Editor UI dependencies

Vendored source subsets, including upstream license notices. No nested Git repositories.

| Library | Revision | Used for |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.91.9b-docking, `4806a1924ff6181180bf5e4b8b79ab4394118875` | Docking, tables, trees, inputs and official Win32/DX11/DX12/Vulkan backends |
| [HelloImGui](https://github.com/pthom/hello_imgui) | v1.6.3, `4271e5f631a4d992f7499b7ab22c5f07319d9fd2` | Standalone upstream theme module, with its explicit reuse permission |
| [ImPlot](https://github.com/epezent/implot) | v0.16, `18c72431f8265e2b0b5378a3a73d8a883b2175ff` | Interactive profiler charts |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | `105e4fde45e4e74aae8973d98750820ef7aca7e2` | Scene grid and transform manipulators |
| [Microsoft Codicons](https://github.com/microsoft/vscode-codicons) | `@vscode/codicons` 0.0.36, `f82216e8ddcdf0c31eb1e5074a87bb88b3cf7517` | DPI rasterized toolbar, hierarchy and inspector font icons; CC BY 4.0 icons, MIT code |

Versions are pinned together for the existing native renderer APIs. Configuration uses
the checked-in sources and does not require a network connection. The application owns
the engine loop, commands and assets; HelloImGui's window/application runner is not used.
