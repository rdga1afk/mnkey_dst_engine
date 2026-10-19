#pragma once
// MyGUI 3.2 platform on SDL_GPU (#192 spike). Compiled only with -DMD_MYGUI=ON.
// Two-phase frame: Prepare(cmd) before the render pass runs MyGUI and uploads the batches in one vertex buffer;
// Draw(rp, cmd) inside the pass replays them. GUI data is read from <KENSHI_DIR>/data/gui at run time.
#include <monkey_dust/render/gpu_pipeline.h>
#include <SDL3/SDL_events.h>
#include <string>

namespace mdui {

bool MyGuiInit(int view_w, int view_h, const char* layout_name);
void MyGuiShutdown();
bool MyGuiActive();
void MyGuiResize(int w, int h);
void MyGuiMouse(float x, float y, bool left_down);
// keyboard / text events from the SDL event pump; returns true while a widget has keyboard focus (edit box)
bool MyGuiEvent(const SDL_Event& e);
void MyGuiPrepare(md::GpuCommandBufferHandle cmd);
void MyGuiDraw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd);
// last frame's CPU cost of MyGuiPrepare, ms; widget dump "name x y w h" of the loaded layout into a file
bool MyGuiColour(const char* name, float rgb[3]);  // Kenshi palette ("Main", "Good", ...) from kenshi_colours.xml
double MyGuiLastPrepareMs();
bool MyGuiDumpWidgets(const char* path);
// test hook: focuses the widget `name`, feeds `utf8` through MyGuiEvent as SDL text-input events (then Backspace `backspaces` times) and returns the widget caption as UTF-8
std::string MyGuiTypeInto(const char* name, const char* utf8, int backspaces);

} // namespace mdui
