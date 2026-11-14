#include "ui.h"

#include "process.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace ui {
namespace {

ImFont* g_bold = nullptr;

bool looksLikeFont(const std::string& path) {
    if (path.empty()) return false;
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return false;
    unsigned char m[4] = {0, 0, 0, 0};
    size_t n = SDL_ReadIO(io, m, 4);
    SDL_CloseIO(io);
    if (n != 4) return false;
    return (m[0] == 0 && m[1] == 1 && m[2] == 0 && m[3] == 0) || std::memcmp(m, "OTTO", 4) == 0 ||
           std::memcmp(m, "true", 4) == 0 || std::memcmp(m, "ttcf", 4) == 0;
}

#ifndef _WIN32
std::string fcMatch(const std::string& pattern) {
    std::string out;
    if (runCapture({"fc-match", "-f", "%{file}", pattern}, Process::Stderr::Discard, out, 4096) != 0) return std::string();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
#endif

void findFonts(std::string& regular, std::string& bold) {
#ifdef _WIN32
    const char* windir = SDL_getenv("WINDIR");
    const std::string dir = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    for (const char* f : {"segoeui.ttf", "arial.ttf"})
        if (looksLikeFont(dir + f)) {
            regular = dir + f;
            break;
        }
    for (const char* f : {"seguisb.ttf", "segoeuib.ttf", "arialbd.ttf"})
        if (looksLikeFont(dir + f)) {
            bold = dir + f;
            break;
        }
#else
    // Ask fontconfig for a modern UI font, preferring static (non-variable) files.
    const std::string families = "Inter,Noto Sans,Cantarell,Roboto,Ubuntu,DejaVu Sans,Liberation Sans,sans-serif";
    regular = fcMatch(families + ":style=Regular:variable=false");
    bold = fcMatch(families + ":weight=bold:variable=false");
    if (!looksLikeFont(regular)) {
        regular.clear();
        for (const char* f : {"/usr/share/fonts/noto/NotoSans-Regular.ttf",
                              "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                              "/usr/share/fonts/TTF/DejaVuSans.ttf",
                              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                              "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                              "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"})
            if (looksLikeFont(f)) {
                regular = f;
                break;
            }
    }
    if (!looksLikeFont(bold)) bold.clear();
#endif
}

ImFont* addFontWithFallback(const std::string& path) {
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = path.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(path.c_str(), kFontSize);
    if (!font) return nullptr;
    // Glyphs missing from the system font are taken from Dear ImGui's embedded font.
    ImFontConfig merge;
    merge.MergeMode = true;
    io.Fonts->AddFontDefaultVector(&merge);
    return font;
}

}  // namespace

ImU32 u32(const ImVec4& c, float alphaMul) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul)); }

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    std::string regular, bold;
    findFonts(regular, bold);
    ImFont* main = addFontWithFallback(regular);
    if (!main) main = io.Fonts->AddFontDefaultVector();
    if (!bold.empty() && bold != regular) g_bold = addFontWithFallback(bold);
    if (!g_bold) g_bold = main;
    io.FontDefault = main;
}

ImFont* boldFont() { return g_bold; }

void applyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();

    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(6, 6);
    s.WindowRounding = 10;
    s.ChildRounding = 10;
    s.FrameRounding = 7;
    s.PopupRounding = 8;
    s.GrabRounding = 7;
    s.ScrollbarRounding = 8;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.ScrollbarSize = 10;
    s.GrabMinSize = 14;
    s.DisabledAlpha = 0.4f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    ImVec4* c = s.Colors;
    const ImVec4 a = color::accent;
    c[ImGuiCol_Text] = color::text;
    c[ImGuiCol_TextDisabled] = color::textDim;
    c[ImGuiCol_WindowBg] = color::bg;
    c[ImGuiCol_ChildBg] = color::panel;
    c[ImGuiCol_PopupBg] = ImVec4(0.110f, 0.110f, 0.133f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(0.153f, 0.153f, 0.188f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.184f, 0.184f, 0.227f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.212f, 0.212f, 0.263f, 1);
    c[ImGuiCol_TitleBg] = ImVec4(0.110f, 0.110f, 0.133f, 1);
    c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBg];
    c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_TitleBg];
    c[ImGuiCol_MenuBarBg] = color::panel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.18f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.26f);
    c[ImGuiCol_CheckMark] = a;
    c[ImGuiCol_SliderGrab] = a;
    c[ImGuiCol_SliderGrabActive] = color::accentHover;
    c[ImGuiCol_Button] = ImVec4(0.161f, 0.161f, 0.200f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.200f, 0.200f, 0.247f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.235f, 0.235f, 0.294f, 1);
    c[ImGuiCol_Header] = ImVec4(a.x, a.y, a.z, 0.22f);
    c[ImGuiCol_HeaderHovered] = ImVec4(a.x, a.y, a.z, 0.32f);
    c[ImGuiCol_HeaderActive] = ImVec4(a.x, a.y, a.z, 0.42f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_SeparatorHovered] = a;
    c[ImGuiCol_SeparatorActive] = a;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_InputTextCursor] = color::text;
    c[ImGuiCol_PlotHistogram] = a;
    c[ImGuiCol_PlotHistogramHovered] = color::accentHover;
    c[ImGuiCol_TextSelectedBg] = ImVec4(a.x, a.y, a.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = color::accentHover;
    c[ImGuiCol_DragDropTargetBg] = ImVec4(a.x, a.y, a.z, 0.10f);
    c[ImGuiCol_NavCursor] = a;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);

    s.ScaleAllSizes(scale);
    s.FontSizeBase = kFontSize;
    s.FontScaleDpi = scale;
}

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float size, ImU32 col) {
    const float h = size * 0.5f;
    switch (icon) {
    case Icon::Play:
        // Shifted right a bit so the triangle looks optically centered.
        dl->AddTriangleFilled(ImVec2(c.x - h * 0.70f, c.y - h), ImVec2(c.x - h * 0.70f, c.y + h),
                              ImVec2(c.x + h * 0.95f, c.y), col);
        break;
    case Icon::Pause: {
        const float bw = size * 0.30f, gap = size * 0.22f;
        dl->AddRectFilled(ImVec2(c.x - gap * 0.5f - bw, c.y - h), ImVec2(c.x - gap * 0.5f, c.y + h), col, bw * 0.25f);
        dl->AddRectFilled(ImVec2(c.x + gap * 0.5f, c.y - h), ImVec2(c.x + gap * 0.5f + bw, c.y + h), col, bw * 0.25f);
        break;
    }
    case Icon::Stop:
        dl->AddRectFilled(ImVec2(c.x - h * 0.85f, c.y - h * 0.85f), ImVec2(c.x + h * 0.85f, c.y + h * 0.85f), col,
                          size * 0.12f);
        break;
    case Icon::StepBack: {
        const float bw = std::max(2.0f, size * 0.17f);
        dl->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x - h + bw, c.y + h), col, bw * 0.3f);
        dl->AddTriangleFilled(ImVec2(c.x + h, c.y - h), ImVec2(c.x + h, c.y + h), ImVec2(c.x - h + bw + size * 0.08f, c.y),
                              col);
        break;
    }
    case Icon::StepForward: {
        const float bw = std::max(2.0f, size * 0.17f);
        dl->AddRectFilled(ImVec2(c.x + h - bw, c.y - h), ImVec2(c.x + h, c.y + h), col, bw * 0.3f);
        dl->AddTriangleFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x - h, c.y + h), ImVec2(c.x + h - bw - size * 0.08f, c.y),
                              col);
        break;
    }
    case Icon::Plus: {
        const float t = std::max(2.0f, std::round(size * 0.14f));
        dl->AddRectFilled(ImVec2(c.x - h, c.y - t * 0.5f), ImVec2(c.x + h, c.y + t * 0.5f), col, t * 0.5f);
        dl->AddRectFilled(ImVec2(c.x - t * 0.5f, c.y - h), ImVec2(c.x + t * 0.5f, c.y + h), col, t * 0.5f);
        break;
    }
    case Icon::Close: {
        const float t = std::max(1.5f, size * 0.16f);
        dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, t);
        dl->AddLine(ImVec2(c.x + h, c.y - h), ImVec2(c.x - h, c.y + h), col, t);
        break;
    }
    }
}

bool iconButton(const char* id, Icon icon, const char* tooltip, bool primary) {
    const float sz = ImGui::GetFrameHeight();
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, color::button);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color::buttonHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, color::buttonActive);
    }
    const bool pressed = ImGui::Button(id, ImVec2(sz, sz));
    if (primary) ImGui::PopStyleColor(3);
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f), sz * 0.36f,
             primary ? ImGui::GetColorU32(ImVec4(1, 1, 1, 1)) : ImGui::GetColorU32(ImGuiCol_Text));
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

bool primaryButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, color::button);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color::buttonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color::buttonActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

float buttonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

std::string widestDigits(std::string text) {
    char widest = '0';
    float widestW = 0.0f;
    for (char d = '0'; d <= '9'; ++d) {
        const char glyph[2] = {d, '\0'};
        const float w = ImGui::CalcTextSize(glyph).x;
        if (w > widestW) {
            widestW = w;
            widest = d;
        }
    }
    for (char& ch : text)
        if (ch >= '0' && ch <= '9') ch = widest;
    return text;
}

void textEllipsis(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, float maxWidth) {
    if (ImGui::CalcTextSize(text).x <= maxWidth) {
        dl->AddText(pos, col, text);
        return;
    }
    const char* ellipsis = "...";
    const float ellipsisW = ImGui::CalcTextSize(ellipsis).x;
    std::string s(text);
    size_t len = s.size();
    while (len > 0) {
        do {
            --len;
        } while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80);  // UTF-8 boundary
        if (ImGui::CalcTextSize(s.data(), s.data() + len).x + ellipsisW <= maxWidth) break;
    }
    s.resize(len);
    s += ellipsis;
    dl->AddText(pos, col, s.c_str());
}

}  // namespace ui
