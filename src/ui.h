// Visual style: flat dark theme, system font, small vector icons.
#pragma once

#include "imgui.h"

namespace ui {

constexpr float kFontSize = 16.0f;

namespace color {
constexpr ImVec4 bg(0.063f, 0.063f, 0.078f, 1.0f);           // window background
constexpr ImVec4 panel(0.094f, 0.094f, 0.114f, 1.0f);        // panels
constexpr ImVec4 card(0.122f, 0.122f, 0.149f, 1.0f);         // clip cards
constexpr ImVec4 cardHover(0.149f, 0.149f, 0.184f, 1.0f);
constexpr ImVec4 cardSelected(0.165f, 0.157f, 0.231f, 1.0f);
constexpr ImVec4 preview(0.043f, 0.043f, 0.055f, 1.0f);      // video area (matches mpv background)
constexpr ImVec4 track(0.153f, 0.153f, 0.188f, 1.0f);        // timeline track
constexpr ImVec4 accent(0.486f, 0.424f, 0.941f, 1.0f);       // #7C6CF0
constexpr ImVec4 accentHover(0.565f, 0.510f, 0.965f, 1.0f);
constexpr ImVec4 accentActive(0.420f, 0.353f, 0.878f, 1.0f);
constexpr ImVec4 text(0.910f, 0.910f, 0.929f, 1.0f);
constexpr ImVec4 textDim(0.553f, 0.553f, 0.604f, 1.0f);
constexpr ImVec4 danger(0.906f, 0.329f, 0.396f, 1.0f);
constexpr ImVec4 success(0.373f, 0.792f, 0.561f, 1.0f);
}  // namespace color

ImU32 u32(const ImVec4& c, float alphaMul = 1.0f);

void loadFonts();
ImFont* boldFont();
void applyTheme(float scale);

enum class Icon { Play, Pause, Stop, StepBack, StepForward, Plus, Close };
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 col);

// Square button with a vector icon.
bool iconButton(const char* id, Icon icon, const char* tooltip = nullptr, bool primary = false);
bool primaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
// Width a regular button with this label would take.
float buttonWidth(const char* label);
// Draws `text` clipped to maxWidth with a trailing "..." if needed.
void textEllipsis(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, float maxWidth);

}  // namespace ui
