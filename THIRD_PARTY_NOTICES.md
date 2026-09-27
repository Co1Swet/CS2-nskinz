# Third-party notices

The following notices apply to bundled third-party components. They do not replace the project-wide AGPL-3.0-only terms in `LICENSE`.

## Dear ImGui

Dear ImGui v1.91.9 — <https://github.com/ocornut/imgui>

Copyright (c) 2014-2025 Omar Cornut and Dear ImGui contributors.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## JSON for Modern C++ (nlohmann/json)

nlohmann/json v3.11.3 — <https://github.com/nlohmann/json>

Copyright (c) 2013-2023 Niels Lohmann.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## MinHook and Hacker Disassembler Engine

MinHook and its bundled Hacker Disassembler Engine files retain their BSD-style terms and copyright notices in [`third_party/MinHook/LICENSE.txt`](third_party/MinHook/LICENSE.txt). That complete file is also included with the binary release package as `MINHOOK-LICENSE.txt`.

## source2-demo

The independent `Temporary Demo` implementation vendors and modifies
[`source2-demo`](https://github.com/Rupas1k/source2-demo) version 0.5.8. It is
used to parse Source 2 entity ownership, materialize knife/glove fields, update
`AnimAssetData`, and write the new `.dem` file. nSkinzCS2 does not call an
external skin client or service at runtime.

Upstream is dual-licensed under MIT or Apache License 2.0. The complete
upstream terms are preserved in:

- [`third_party/source2-demo/LICENSE-MIT`](third_party/source2-demo/LICENSE-MIT)
- [`third_party/source2-demo/LICENSE-APACHE`](third_party/source2-demo/LICENSE-APACHE)

The vendored source, including nSkinz's field-materialization and string-table
insertion changes, is distributed in [`third_party/source2-demo`](third_party/source2-demo).
