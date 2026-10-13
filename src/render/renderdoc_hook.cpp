#include <monkey_dust/render/renderdoc_hook.h>
#include <renderdoc_app.h>
#include <dlfcn.h>
#include <cstdio>
#include <ctime>
#include <filesystem>

static RENDERDOC_API_1_1_2* s_api = nullptr;
static bool s_tried = false;
static bool s_armed = false;
static bool s_capturing = false;
static char s_path_template[512] = "";

// 2026-09-05: every diagnostic print here now fflush()es immediately.
// Root cause of a silent debugging session: stdout is fully-buffered (not
// line-buffered) when redirected to a file/pipe (the common case for any
// scripted/automated launch, not just this tool) -- these messages were
// sitting in libc's buffer and got lost whenever the caller SIGKILLed the
// process shortly after arming a capture (the exact automation pattern
// this hook exists for), making it look like RenderDocHook_Init() was
// never even called when it may have run and failed (or succeeded)
// silently. Same class of bug already documented for [PERF] logging
// (docs/GRANITE_P5_BASELINE.md's stdbuf -oL note) -- fixed at the source
// here instead of relying on every caller to remember stdbuf -oL.
bool RenderDocHook_Init() {
    if (s_tried) return s_api != nullptr;
    s_tried = true;

    void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (!mod) {
        fprintf(stderr, "[RenderDocHook] librenderdoc.so not loaded in this process (not running under renderdoccmd)\n");
        fflush(stderr);
        return false;
    }
    pRENDERDOC_GetAPI get_api = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
    if (!get_api) {
        fprintf(stderr, "[RenderDocHook] RENDERDOC_GetAPI symbol not found\n");
        fflush(stderr);
        return false;
    }
    int ok = get_api(eRENDERDOC_API_Version_1_1_2, (void**)&s_api);
    if (!ok || !s_api) {
        fprintf(stderr, "[RenderDocHook] RENDERDOC_GetAPI call failed\n");
        fflush(stderr);
        s_api = nullptr;
        return false;
    }
    fprintf(stdout, "[RenderDocHook] connected to RenderDoc API\n");
    fflush(stdout);
    return true;
}

void RenderDocHook_ArmCapture(const char* tag) {
    if (!RenderDocHook_Init()) {
        fprintf(stdout, "[RenderDocHook] ArmCapture: Init() failed, capture NOT armed\n");
        fflush(stdout);
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories("temp_/captures", ec);
    std::snprintf(s_path_template, sizeof(s_path_template), "temp_/captures/%s_%lld", (tag && *tag) ? tag : "capture", (long long)std::time(nullptr));
    s_api->SetCaptureFilePathTemplate(s_path_template);
    s_armed = true;
    fprintf(stdout, "[RenderDocHook] capture armed for next frame -> %s_frame*.rdc\n", s_path_template);
    fflush(stdout);
}

void RenderDocHook_BeginFrame() {
    if (!s_armed || !s_api) return;
    s_armed = false;
    s_api->StartFrameCapture(nullptr, nullptr);
    s_capturing = true;
    fprintf(stdout, "[RenderDocHook] StartFrameCapture() called\n");
    fflush(stdout);
}

void RenderDocHook_EndFrame() {
    if (!s_capturing || !s_api) return;
    s_capturing = false;
    uint32_t ok = s_api->EndFrameCapture(nullptr, nullptr);
    fprintf(stdout, "[RenderDocHook] EndFrameCapture() -> %s\n", ok ? "OK (.rdc written)" : "FAILED");
    fflush(stdout);
}
