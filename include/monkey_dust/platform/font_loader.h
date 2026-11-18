#pragma once
// font_loader.h — Loads embedded project fonts into ImGui atlas.
// All 4 TTF fonts are compiled into engine lib (embedded_fonts.cpp).
// Call MdFonts::Load() once after ImGui::CreateContext().

#include "imgui.h"
#include <monkey_dust/platform/embedded_fonts.h>

namespace MdFonts {

// Loaded font pointers — valid after Load(), null before.
inline ImFont* regular         = nullptr;
inline ImFont* bold            = nullptr;
inline ImFont* italic          = nullptr;
inline ImFont* bold_italic     = nullptr;

// Load the 4 embedded Arimo fonts. Call once after ImGui::CreateContext().
// ui_size  = pixel size for Arimo (UI text)
inline void Load(float ui_size = 14.f) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // static arrays — ImGui must NOT free them

    regular         = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::Arimo_Regular,         (int)md_font::Arimo_Regular_size,         ui_size,   &cfg);
    bold            = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::Arimo_Bold,            (int)md_font::Arimo_Bold_size,            ui_size,   &cfg);
    italic          = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::Arimo_Italic,          (int)md_font::Arimo_Italic_size,          ui_size,   &cfg);
    bold_italic     = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::Arimo_BoldItalic,      (int)md_font::Arimo_BoldItalic_size,      ui_size,   &cfg);

    io.FontDefault = regular;  // Arimo Regular as global default
}

} // namespace MdFonts
