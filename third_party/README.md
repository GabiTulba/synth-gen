# Vendored third-party code

## imgui/ — Dear ImGui v1.91.8

Source: https://github.com/ocornut/imgui, tag `v1.91.8`, MIT license
(see `imgui/LICENSE.txt`). Trimmed to the core library plus the two
backends the dev app uses (`imgui_impl_sdl2`, `imgui_impl_sdlrenderer2`);
docs, examples, demo and unused backends are removed.

To upgrade: re-clone the desired tag and apply the same trim, keeping
`imgui*.h/.cpp`, `imconfig.h`, `imstb_*.h`, `LICENSE.txt`, and
`backends/imgui_impl_sdl2.*` + `backends/imgui_impl_sdlrenderer2.*`.
