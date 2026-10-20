#include <monkey_dust/ui/mygui_sdlgpu.h>
#include <monkey_dust/ui/kenshi_widgets.h>

#include <MyGUI.h>
#include <MyGUI_DataFileStream.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <stb_image.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace mdui {
namespace {

constexpr uint32_t kMaxVerts = 16384;  // per frame, all batches; 24 B each
constexpr int      kMaxBatches = 512;

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// '*' and '?' only, case-insensitive; MyGUI asks by exact name or by "*.ext"
bool Glob(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') return Glob(p + 1, s) || (*s && Glob(p, s + 1));
    return *s && (*p == '?' || *p == *s) && Glob(p + 1, s + 1);
}

// ── data: <KENSHI_DIR>/data/gui, all subfolders, looked up by file name ───────────────────────────────────────
class DataMgr : public MyGUI::DataManager {
public:
    void Init(const std::string& root) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file()) files_.emplace_back(Lower(it->path().filename().string()), it->path().string());
        }
    }
    MyGUI::IDataStream* getData(const std::string& name) override {
        const std::string& path = getDataPath(name);
        if (path.empty()) return nullptr;
        auto* st = new std::ifstream(path.c_str(), std::ios_base::binary);
        if (!st->is_open()) { delete st; return nullptr; }
        return new MyGUI::DataFileStream(st);
    }
    void freeData(MyGUI::IDataStream* d) override { delete d; }
    bool isDataExist(const std::string& name) override { return !getDataListNames(name).empty(); }
    const MyGUI::VectorString& getDataListNames(const std::string& pattern) override {
        names_.clear();
        std::string p = Lower(pattern);
        for (auto& f : files_) if (Glob(p.c_str(), f.first.c_str())) names_.push_back(f.second);
        return names_;
    }
    const std::string& getDataPath(const std::string& name) override {
        path_.clear();
        std::string p = Lower(name);
        for (auto& f : files_) if (f.first == p) { path_ = f.second; break; }
        return path_;
    }
private:
    std::vector<std::pair<std::string, std::string>> files_;
    MyGUI::VectorString names_;
    std::string path_;
};

// ── vertex buffer: CPU copy; MyGUI locks it while building a layer node ───────────────────────────────────────
class VBuf : public MyGUI::IVertexBuffer {
public:
    void setVertexCount(size_t n) override { v_.resize(n); }
    size_t getVertexCount() override { return v_.size(); }
    MyGUI::Vertex* lock() override { return v_.data(); }
    void unlock() override {}
private:
    std::vector<MyGUI::Vertex> v_;
};

// ── texture: RGBA8 on the CPU, re-created on unlock (fonts and canvases are written once) ───────────────────────
class Tex : public MyGUI::ITexture {
public:
    explicit Tex(const std::string& name) : name_(name) {}
    ~Tex() override { gpu_.Shutdown(); }
    const std::string& getName() const override { return name_; }
    void createManual(int w, int h, MyGUI::TextureUsage usage, MyGUI::PixelFormat fmt) override {
        w_ = w; h_ = h; usage_ = usage; fmt_ = fmt;
        px_.assign((size_t)w * h * 4, 0);
        Upload();
    }
    void loadFromFile(const std::string& file) override {
        const std::string& path = MyGUI::DataManager::getInstance().getDataPath(file);
        int w = 0, h = 0, n = 0;
        unsigned char* d = path.empty() ? nullptr : stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!d) { std::fprintf(stderr, "[MyGUI] texture '%s' not loaded\n", file.c_str()); return; }
        w_ = w; h_ = h; fmt_ = MyGUI::PixelFormat::R8G8B8A8; usage_ = MyGUI::TextureUsage::Static;
        px_.assign(d, d + (size_t)w * h * 4);
        stbi_image_free(d);
        Upload();
    }
    void saveToFile(const std::string&) override {}
    void destroy() override { gpu_.Shutdown(); px_.clear(); w_ = h_ = 0; }
    void* lock(MyGUI::TextureUsage) override { locked_ = true; return px_.data(); }
    void unlock() override { locked_ = false; Upload(); }
    bool isLocked() override { return locked_; }
    int getWidth() override { return w_; }
    int getHeight() override { return h_; }
    MyGUI::PixelFormat getFormat() override { return fmt_; }
    MyGUI::TextureUsage getUsage() override { return usage_; }
    size_t getNumElemBytes() override { return 4; }
    const GpuTexture& Gpu() const { return gpu_; }
private:
    void Upload() {
        gpu_.Shutdown();
        if (w_ <= 0 || h_ <= 0) return;
        GpuSamplerDesc s;
        s.min_filter = s.mag_filter = GpuSamplerDesc::Filter::LINEAR;
        s.wrap_s = s.wrap_t = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        gpu_.InitFromMemory(px_.data(), w_, h_, s);
    }
    std::string name_;
    int w_ = 0, h_ = 0;
    MyGUI::TextureUsage usage_;
    MyGUI::PixelFormat fmt_ = MyGUI::PixelFormat::R8G8B8A8;
    std::vector<uint8_t> px_;
    GpuTexture gpu_;
    bool locked_ = false;
};

// ── render manager + render target ────────────────────────────────────────────────────────────────────────────
struct Batch { uint32_t first, count; const Tex* tex; };

class RenderMgr : public MyGUI::RenderManager, public MyGUI::IRenderTarget {
public:
    void Init() {
        GpuPipeline::Desc d;
        d.vert_path = "shaders/mygui.vert";
        d.frag_path = "shaders/mygui.frag";
        d.layout.stride = sizeof(MyGUI::Vertex);
        d.layout.count = 3;
        d.layout.attribs[0] = { 0, 0,  GpuAttribFmt::F3 };
        d.layout.attribs[1] = { 1, 12, GpuAttribFmt::U8x4_NORM };
        d.layout.attribs[2] = { 2, 16, GpuAttribFmt::F2 };
        d.frag_samplers = 1;
        d.has_depth_target = false;
        d.raster.blend_enable = true;
        d.raster.depth_test = false;
        d.raster.depth_write = false;
        d.raster.cull_back = false;
        pipe_.Create(d);
        vbuf_.Init(kMaxVerts, sizeof(MyGUI::Vertex));
        static const uint8_t white[4] = { 255, 255, 255, 255 };
        GpuSamplerDesc s;
        s.min_filter = s.mag_filter = GpuSamplerDesc::Filter::NEAREST;
        s.wrap_s = s.wrap_t = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        white_.InitFromMemory(white, 1, 1, s);
        last_ = std::chrono::steady_clock::now();
    }
    void Shutdown() {
        for (auto& t : tex_) delete t.second;
        tex_.clear();
        white_.Shutdown();
        vbuf_.Shutdown();
        pipe_.Destroy();
    }
    void SetViewSize(int w, int h) {
        view_.set(std::max(w, 1), std::max(h, 1));
        info_.maximumDepth = 1;
        info_.hOffset = 0;
        info_.vOffset = 0;
        info_.aspectCoef = float(view_.height) / float(view_.width);
        info_.pixScaleX = 1.0f / float(view_.width);
        info_.pixScaleY = 1.0f / float(view_.height);
        onResizeView(view_);
        update_ = true;
    }

    // phase 1: run MyGUI, collect batches (outside the render pass)
    void Prepare(md::GpuCommandBufferHandle cmd) {
        auto now = std::chrono::steady_clock::now();
        onFrameEvent(std::chrono::duration<float>(now - last_).count());
        last_ = now;
        nverts_ = 0; nbatch_ = 0;
        out_ = static_cast<MyGUI::Vertex*>(vbuf_.MapWrite());
        if (!out_) return;
        onRenderToTarget(this, update_);
        update_ = false;
        vbuf_.Unmap();
        if (nverts_ > 0) vbuf_.Upload(cmd);
        out_ = nullptr;
        static int s_log = 0;
        if (s_log++ % 120 == 0) std::fprintf(stdout, "[MyGUI] frame %d: %d batches, %u vertices\n", s_log, nbatch_, nverts_);
    }
    // phase 2: replay inside the pass
    void Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd) {
        if (nbatch_ == 0 || !pipe_.SDLPipeline()) return;
        GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
        pv.BindPipeline(&pipe_);
        pv.BindVertexBuffer(&vbuf_);
        for (int i = 0; i < nbatch_; ++i) {
            const GpuTexture& t = batch_[i].tex ? batch_[i].tex->Gpu() : white_;
            if (!t.SDLTexture()) continue;
            SDL_GPUTextureSamplerBinding b{ t.SDLTexture(), t.SDLSampler() };
            pv.BindFragmentSamplers(0, &b, 1);
            pv.Draw(batch_[i].count, 1, batch_[i].first, 0);
        }
    }

    // MyGUI::RenderManager
    MyGUI::IVertexBuffer* createVertexBuffer() override { return new VBuf(); }
    void destroyVertexBuffer(MyGUI::IVertexBuffer* b) override { delete b; }
    MyGUI::ITexture* createTexture(const std::string& name) override {
        auto* t = new Tex(name);
        tex_[name] = t;
        return t;
    }
    void destroyTexture(MyGUI::ITexture* t) override {
        if (!t) return;
        tex_.erase(t->getName());
        delete t;
    }
    MyGUI::ITexture* getTexture(const std::string& name) override {
        auto it = tex_.find(name);
        return it == tex_.end() ? nullptr : it->second;
    }
    const MyGUI::IntSize& getViewSize() const override { return view_; }
    MyGUI::VertexColourType getVertexFormat() override { return MyGUI::VertexColourType::ColourABGR; }
    bool isFormatSupported(MyGUI::PixelFormat f, MyGUI::TextureUsage) override { return f == MyGUI::PixelFormat::R8G8B8A8; }

    // MyGUI::IRenderTarget
    void begin() override {}
    void end() override {}
    void doRender(MyGUI::IVertexBuffer* buf, MyGUI::ITexture* tex, size_t count) override {
        if (!out_ || count == 0) return;
        if (nverts_ + count > kMaxVerts || nbatch_ >= kMaxBatches) {
            if (!warned_) { std::fprintf(stderr, "[MyGUI] frame limit reached (verts %u batches %d): the rest is skipped\n", nverts_, nbatch_); warned_ = true; }
            return;
        }
        std::memcpy(out_ + nverts_, static_cast<VBuf*>(buf)->lock(), count * sizeof(MyGUI::Vertex));
        batch_[nbatch_++] = { nverts_, (uint32_t)count, static_cast<const Tex*>(tex) };
        nverts_ += (uint32_t)count;
    }
    const MyGUI::RenderTargetInfo& getInfo() override { return info_; }

private:
    GpuPipeline pipe_;
    GpuVertexBuffer vbuf_;
    GpuTexture white_;
    std::map<std::string, Tex*> tex_;
    MyGUI::IntSize view_;
    MyGUI::RenderTargetInfo info_;
    Batch batch_[kMaxBatches];
    MyGUI::Vertex* out_ = nullptr;
    uint32_t nverts_ = 0;
    int nbatch_ = 0;
    bool update_ = true;
    bool warned_ = false;
    std::chrono::steady_clock::time_point last_;
};

// Kenshi's own resource type "Colour" (kenshi_colours.xml): a named palette, <Property key="Main" value="#afa68b"/>; read by exe code by name, not by layouts
std::map<std::string, MyGUI::Colour> g_palette;

void LoadColourPalette(MyGUI::xml::ElementPtr node, const std::string&, MyGUI::Version) {
    MyGUI::xml::ElementEnumerator e = node->getElementEnumerator();
    while (e.next())
        if (e->getName() == "Property") g_palette[e->findAttribute("key")] = MyGUI::Colour::parse(e->findAttribute("value"));
}

// SDL scancode -> MyGUI key code (MyGUI uses the DirectInput numbering); 0 = not mapped
int KeyFromScancode(SDL_Scancode sc) {
    static const unsigned char row1[] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };   // A..Z
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return row1[sc - SDL_SCANCODE_A];
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return 0x02 + (sc - SDL_SCANCODE_1);
    switch (sc) {
        case SDL_SCANCODE_0: return 0x0B;
        case SDL_SCANCODE_ESCAPE: return 0x01;
        case SDL_SCANCODE_BACKSPACE: return 0x0E;
        case SDL_SCANCODE_TAB: return 0x0F;
        case SDL_SCANCODE_RETURN: return 0x1C;
        case SDL_SCANCODE_SPACE: return 0x39;
        case SDL_SCANCODE_LCTRL: return 0x1D;
        case SDL_SCANCODE_RCTRL: return 0x9D;
        case SDL_SCANCODE_LSHIFT: return 0x2A;
        case SDL_SCANCODE_RSHIFT: return 0x36;
        case SDL_SCANCODE_HOME: return 0xC7;
        case SDL_SCANCODE_UP: return 0xC8;
        case SDL_SCANCODE_PAGEUP: return 0xC9;
        case SDL_SCANCODE_LEFT: return 0xCB;
        case SDL_SCANCODE_RIGHT: return 0xCD;
        case SDL_SCANCODE_END: return 0xCF;
        case SDL_SCANCODE_DOWN: return 0xD0;
        case SDL_SCANCODE_PAGEDOWN: return 0xD1;
        case SDL_SCANCODE_INSERT: return 0xD2;
        case SDL_SCANCODE_DELETE: return 0xD3;
        default: return 0;
    }
}

// first code point of a UTF-8 string
MyGUI::Char Utf8First(const char* s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    MyGUI::Char r = c & (0x3F >> n);
    for (int i = 1; i <= n && s[i]; ++i) r = (r << 6) | ((unsigned char)s[i] & 0x3F);
    return r;
}

struct State {
    std::unique_ptr<MyGUI::LogManager> log;
    std::unique_ptr<RenderMgr> render;
    std::unique_ptr<DataMgr> data;
    std::unique_ptr<MyGUI::Gui> gui;
    MyGUI::VectorWidgetPtr roots;
    bool left_prev = false;
    double prepare_ms = 0;
} g;

void DumpWidget(FILE* f, MyGUI::Widget* w, int depth) {
    MyGUI::IntCoord a = w->getAbsoluteCoord();
    std::fprintf(f, "%*s%s\t%d\t%d\t%d\t%d\n", depth * 2, "", w->getName().c_str(), a.left, a.top, a.width, a.height);
    for (size_t i = 0; i < w->getChildCount(); ++i) DumpWidget(f, w->getChildAt(i), depth + 1);
}

} // namespace

bool MyGuiInit(int w, int h, const char* layout) {
    if (g.gui) return true;
    const char* kd = std::getenv("KENSHI_DIR");
    if (!kd || !*kd) { std::fprintf(stderr, "[MyGUI] KENSHI_DIR is not set\n"); return false; }
    std::string root = std::string(kd) + "/data/gui";
    if (!fs::exists(root)) { std::fprintf(stderr, "[MyGUI] no %s\n", root.c_str()); return false; }

    g.log = std::make_unique<MyGUI::LogManager>();
    g.log->createDefaultSource("temp_/mygui.log");
    g.render = std::make_unique<RenderMgr>();
    g.data = std::make_unique<DataMgr>();
    g.data->Init(root);
    g.render->Init();
    g.render->SetViewSize(w, h);
    g.gui = std::make_unique<MyGUI::Gui>();
    g.gui->initialise("");   // no core file: the managers exist now, the Kenshi types are registered, then the theme (it lists the templates that use them) is read
    mdui::RegisterKenshiWidgets();
    MyGUI::ResourceManager::getInstance().registerLoadXmlDelegate("Colour") = MyGUI::newDelegate(LoadColourPalette);   // Kenshi's named colours (kenshi_colours.xml)
    MyGUI::ResourceManager::getInstance().load("kenshi_theme.xml");
    std::fprintf(stdout, "[MyGUI] colour palette: %zu entries, Main = #%02x%02x%02x\n", g_palette.size(),
                 (int)(g_palette["Main"].red * 255.f + .5f), (int)(g_palette["Main"].green * 255.f + .5f), (int)(g_palette["Main"].blue * 255.f + .5f));
    if (layout && *layout) g.roots = MyGUI::LayoutManager::getInstance().loadLayout(layout);
    std::fprintf(stdout, "[MyGUI] init: view %dx%d layout '%s' roots %zu\n", w, h, layout ? layout : "", g.roots.size());
    return true;
}

void MyGuiShutdown() {
    if (!g.gui) return;
    g.gui->shutdown();
    g.gui.reset();
    g.render->Shutdown();
    g.render.reset();
    g.data.reset();
    g.log.reset();
}

bool MyGuiActive() { return g.gui != nullptr; }

void MyGuiResize(int w, int h) { if (g.render) g.render->SetViewSize(w, h); }

void MyGuiMouse(float x, float y, bool left) {
    if (!g.gui) return;
    auto& im = MyGUI::InputManager::getInstance();
    im.injectMouseMove((int)x, (int)y, 0);
    if (left && !g.left_prev) im.injectMousePress((int)x, (int)y, MyGUI::MouseButton::Left);
    if (!left && g.left_prev) im.injectMouseRelease((int)x, (int)y, MyGUI::MouseButton::Left);
    g.left_prev = left;
}

bool MyGuiEvent(const SDL_Event& e) {
    if (!g.gui) return false;
    auto& im = MyGUI::InputManager::getInstance();
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN: if (int k = KeyFromScancode(e.key.scancode)) im.injectKeyPress((MyGUI::KeyCode::Enum)k); break;
        case SDL_EVENT_KEY_UP:   if (int k = KeyFromScancode(e.key.scancode)) im.injectKeyRelease((MyGUI::KeyCode::Enum)k); break;
        case SDL_EVENT_TEXT_INPUT: {   // printable characters (any language) arrive here, not as key codes
            for (const char* p = e.text.text; *p;) {
                im.injectKeyPress(MyGUI::KeyCode::None, Utf8First(p));
                unsigned char c = (unsigned char)*p; p += c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            }
            break;
        }
        default: break;
    }
    return im.isFocusKey();   // true while an edit box has keyboard focus: the caller keeps the key away from the game
}

void MyGuiPrepare(md::GpuCommandBufferHandle cmd) {
    if (!g.gui) return;
    auto t0 = std::chrono::steady_clock::now();
    g.render->Prepare(cmd);
    g.prepare_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static int n = 0; static double sum = 0, mx = 0;
    sum += g.prepare_ms; mx = std::max(mx, g.prepare_ms);
    if (++n % 300 == 0) { std::fprintf(stdout, "[MyGUI] Prepare CPU ms: mean %.3f max %.3f over %d frames\n", sum / 300, mx, n); sum = 0; mx = 0; }
}

void MyGuiDraw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd) {
    if (g.gui) g.render->Draw(rp, cmd);
}

bool MyGuiColour(const char* name, float rgb[3]) {
    auto it = g_palette.find(name);
    if (it == g_palette.end()) return false;
    rgb[0] = it->second.red; rgb[1] = it->second.green; rgb[2] = it->second.blue;
    return true;
}

std::string MyGuiTypeInto(const char* name, const char* utf8, int backspaces) {
    if (!g.gui) return "";
    MyGUI::Widget* w = nullptr;
    for (auto* r : g.roots) if ((w = r->findWidget(name))) break;
    if (!w) return "<widget not found>";
    MyGUI::InputManager::getInstance().setKeyFocusWidget(w);
    SDL_Event ev = {};
    ev.type = SDL_EVENT_TEXT_INPUT;
    ev.text.text = utf8;
    MyGuiEvent(ev);
    for (int i = 0; i < backspaces; ++i) {
        SDL_Event k = {};
        k.type = SDL_EVENT_KEY_DOWN; k.key.scancode = SDL_SCANCODE_BACKSPACE; MyGuiEvent(k);
        k.type = SDL_EVENT_KEY_UP; MyGuiEvent(k);
    }
    return w->castType<MyGUI::TextBox>()->getCaption().asUTF8();
}

double MyGuiLastPrepareMs() { return g.prepare_ms; }

bool MyGuiDumpWidgets(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fprintf(f, "# widget\tleft\ttop\twidth\theight (absolute, px)\n");
    for (auto* w : g.roots) DumpWidget(f, w, 0);
    std::fclose(f);
    return true;
}

} // namespace mdui
