#include <monkey_dust/ui/mygui_sdlgpu.h>

#include <MyGUI.h>
#include <MyGUI_DataFileStream.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <stb_image.h>

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
        d.vert_uniform_bufs = 1;
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
    }
    // phase 2: replay inside the pass
    void Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd) {
        if (nbatch_ == 0 || !pipe_.SDLPipeline()) return;
        GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
        pv.BindPipeline(&pipe_);
        pv.BindVertexBuffer(&vbuf_);
        float inv[4] = { info_.pixScaleX, info_.pixScaleY, 0.f, 0.f };
        pv.PushVertexUniforms(0, inv, sizeof(inv));
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
    g.gui->initialise("kenshi_theme.xml");
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

void MyGuiPrepare(md::GpuCommandBufferHandle cmd) {
    if (!g.gui) return;
    auto t0 = std::chrono::steady_clock::now();
    g.render->Prepare(cmd);
    g.prepare_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void MyGuiDraw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd) {
    if (g.gui) g.render->Draw(rp, cmd);
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
