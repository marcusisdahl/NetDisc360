// SPDX-License-Identifier: MIT
#pragma once
struct ImDrawData;
// Call after nb::InitUi(), once the device exists. Builds the font texture.
bool ImGuiXbox360Init(IDirect3DDevice9* device);
// Draws ImGui::GetDrawData() into the current render target.
void ImGuiXbox360Render(ImDrawData* data);
