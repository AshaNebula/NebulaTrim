// Visual style: flat dark theme, system font, small vector icons.
#pragma once

#include "imgui.h"

#include <string>

namespace ui {

constexpr float kFontSize = 16.0f;

namespace color {
constexpr ImVec4 bg(0.063f, 0.063f, 0.078f, 1.0f);           // window background
constexpr ImVec4 panel(0.094f, 0.094f, 0.114f, 1.0f);        // panels
constexpr ImVec4 card(0.122f, 0.122f, 0.149f, 1.0f);         // clip cards
constexpr ImVec4 cardHover(0.149f, 0.149f, 0.184f, 1.0f);
constexpr ImVec4 cardSelected(0.200f, 0.133f, 0.180f, 1.0f);  // pink-tinted
constexpr ImVec4 preview(0.043f, 0.043f, 0.055f, 1.0f);      // video area (matches mpv background)
constexpr ImVec4 track(0.153f, 0.153f, 0.188f, 1.0f);        // timeline track
// Pink accent: bright for strokes on the dark panels, deeper for filled buttons so that
// white labels keep a 4.6:1 contrast (WCAG AA).
constexpr ImVec4 accent(0.925f, 0.282f, 0.600f, 1.0f);       // #EC4899 frames, handles, slider
constexpr ImVec4 accentHover(0.957f, 0.447f, 0.714f, 1.0f);  // #F472B6
constexpr ImVec4 button(0.859f, 0.153f, 0.467f, 1.0f);       // #DB2777 filled buttons and badges
constexpr ImVec4 buttonHover(0.890f, 0.231f, 0.537f, 1.0f);  // #E33B89
constexpr ImVec4 buttonActive(0.745f, 0.094f, 0.365f, 1.0f); // #BE185D
constexpr ImVec4 text(0.910f, 0.910f, 0.929f, 1.0f);
constexpr ImVec4 textDim(0.553f, 0.553f, 0.604f, 1.0f);
constexpr ImVec4 danger(0.949f, 0.376f, 0.290f, 1.0f);       // red-orange, distinct from the pink accent
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
// `text` with every digit replaced by the font's widest digit. Its width is an upper bound for
// any text with the same layout, so it sizes slots that must not move when numbers change.
std::string widestDigits(std::string text);
// Draws `text` clipped to maxWidth with a trailing "..." if needed.
void textEllipsis(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, float maxWidth);

}  // namespace ui
