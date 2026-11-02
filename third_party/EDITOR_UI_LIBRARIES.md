# Editor UI dependencies

Fixed official archives with SHA-256 checksums in [dependencies.json](dependencies.json).
Sources and font assets live in the ignored build cache; see [dependency policy](README.md).

| Library | Revision | Used for |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.91.9b-docking, `4806a1924ff6181180bf5e4b8b79ab4394118875` | Docking, tables, trees, inputs and official Win32/DX11/DX12/Vulkan backends |
| [HelloImGui](https://github.com/pthom/hello_imgui) | v1.6.3, `4271e5f631a4d992f7499b7ab22c5f07319d9fd2` | Standalone upstream theme module, with its explicit reuse permission |
| [ImPlot](https://github.com/epezent/implot) | v0.16, `18c72431f8265e2b0b5378a3a73d8a883b2175ff` | Interactive profiler charts |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | `105e4fde45e4e74aae8973d98750820ef7aca7e2` | Scene grid and transform manipulators |
| [Microsoft Codicons](https://github.com/microsoft/vscode-codicons) | `@vscode/codicons` 0.0.36, `f82216e8ddcdf0c31eb1e5074a87bb88b3cf7517` | DPI rasterized toolbar, hierarchy and inspector font icons; CC BY 4.0 icons, MIT code |

Versions are pinned together for the existing native renderer APIs. Configuration uses
the downloaded cache; first configuration requires network access. The application owns
the engine loop, commands and assets; HelloImGui's window/application runner is not used.

Local backend integration patches are recorded in `patches/imgui-backends.patch` and applied at configure time (preserve when updating ImGui):

- DX12 exposes borrowed viewport queue/fence/backbuffer handles through
  `ImGui_ImplDX12_GetViewportSync`. The engine inserts GPU waits between its
  world queue and upstream's per-window queues; capture reads the acquired
  rendered buffer before upstream presents it.
- Vulkan exposes the acquired secondary backbuffer for opt-in GPU capture,
  requests `TRANSFER_SRC` usage only when the surface supports it, and keys
  render-complete semaphores by acquired image index. The latter follows
  [Khronos present-semaphore reuse guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
  Acquire semaphores continue to rotate independently.

These accessors transfer no ownership. UI controls, docking, native platform
window creation and normal presentation remain upstream implementations.
