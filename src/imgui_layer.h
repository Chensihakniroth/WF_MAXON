#pragma once
#include <windows.h>
#include <string>

namespace wfmaxon {

bool InitImGui(HWND hwnd);
void ShutdownImGui();
bool IsImGuiInitialized();
void RenderImGuiFrame();
bool HandleImGuiMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

} // namespace wfmaxon
