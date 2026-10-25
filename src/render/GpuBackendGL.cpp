// OpenGL 4.3 compute backend of the GPU render stage.
//
// No GL loader library and no load-time import: opengl32.dll / libEGL.so.1 are
// opened only when the stage is enabled and about 40 entry points are fetched
// by hand.  Windows creates a hidden-window WGL 4.3 core context; Linux uses a
// surfaceless (or 1x1 pbuffer) EGL context, so no X11/Wayland window is needed.

#include "render/GpuBackend.h"
#include "render/gpu/VoiceStageGLSL.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace svms {
namespace gpu {
namespace {

// ── Minimal GL declarations (no system GL headers needed) ───────────────
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLboolean = unsigned char;
using GLchar = char;
using GLuint64 = unsigned long long;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;
using GLsync = struct __GLsync*;

constexpr GLenum GL_COMPUTE_SHADER = 0x91B9;
constexpr GLenum GL_SHADER_STORAGE_BUFFER = 0x90D2;
constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLbitfield GL_SHADER_STORAGE_BARRIER_BIT = 0x2000;
constexpr GLbitfield GL_BUFFER_UPDATE_BARRIER_BIT = 0x0200;
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
constexpr GLenum GL_ALREADY_SIGNALED = 0x911A;
constexpr GLenum GL_TIMEOUT_EXPIRED = 0x911B;
constexpr GLenum GL_CONDITION_SATISFIED = 0x911C;
constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT = 0x1;
constexpr GLenum GL_MAJOR_VERSION = 0x821B;
constexpr GLenum GL_MINOR_VERSION = 0x821C;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_MAX_COMPUTE_SHARED_MEMORY_SIZE = 0x8262;
constexpr GLenum GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS = 0x90EB;
constexpr GLenum GL_MAX_SHADER_STORAGE_BLOCK_SIZE = 0x90DE;
constexpr GLenum GL_NO_ERROR = 0;
constexpr GLenum GL_TIME_ELAPSED = 0x88BF;
constexpr GLenum GL_EXTENSIONS = 0x1F03;
constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
constexpr GLenum GL_SUBGROUP_SIZE_KHR = 0x9532;
constexpr GLenum GL_SUBGROUP_SUPPORTED_STAGES_KHR = 0x9533;
constexpr GLenum GL_SUBGROUP_SUPPORTED_FEATURES_KHR = 0x9534;
constexpr GLenum GL_COPY_READ_BUFFER = 0x8F36;
constexpr GLenum GL_COPY_WRITE_BUFFER = 0x8F37;
constexpr GLbitfield GL_MAP_READ_BIT = 0x0001;
constexpr GLbitfield GL_MAP_PERSISTENT_BIT = 0x0040;
constexpr GLbitfield GL_MAP_COHERENT_BIT = 0x0080;
constexpr GLbitfield GL_CLIENT_STORAGE_BIT = 0x0200;
constexpr GLenum GL_QUERY_RESULT = 0x8866;
constexpr GLuint64 GL_TIMEOUT_IGNORED = 0xFFFFFFFFFFFFFFFFull;

#if defined(_WIN32)
#  define SVMS_GLAPI __stdcall
#else
#  define SVMS_GLAPI
#endif

struct GLFns {
    const unsigned char* (SVMS_GLAPI* GetString)(GLenum) = nullptr;
    const unsigned char* (SVMS_GLAPI* GetStringi)(GLenum, GLuint) = nullptr;
    void (SVMS_GLAPI* GetIntegerv)(GLenum, GLint*) = nullptr;
    GLenum (SVMS_GLAPI* GetError)() = nullptr;
    void (SVMS_GLAPI* Flush)() = nullptr;
    GLuint (SVMS_GLAPI* CreateShader)(GLenum) = nullptr;
    void (SVMS_GLAPI* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void (SVMS_GLAPI* CompileShader)(GLuint) = nullptr;
    void (SVMS_GLAPI* GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void (SVMS_GLAPI* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    GLuint (SVMS_GLAPI* CreateProgram)() = nullptr;
    void (SVMS_GLAPI* AttachShader)(GLuint, GLuint) = nullptr;
    void (SVMS_GLAPI* LinkProgram)(GLuint) = nullptr;
    void (SVMS_GLAPI* GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void (SVMS_GLAPI* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void (SVMS_GLAPI* UseProgram)(GLuint) = nullptr;
    void (SVMS_GLAPI* DeleteShader)(GLuint) = nullptr;
    void (SVMS_GLAPI* DeleteProgram)(GLuint) = nullptr;
    GLint (SVMS_GLAPI* GetUniformLocation)(GLuint, const GLchar*) = nullptr;
    void (SVMS_GLAPI* Uniform1ui)(GLint, GLuint) = nullptr;
    void (SVMS_GLAPI* GenBuffers)(GLsizei, GLuint*) = nullptr;
    void (SVMS_GLAPI* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void (SVMS_GLAPI* BindBuffer)(GLenum, GLuint) = nullptr;
    void (SVMS_GLAPI* BindBufferBase)(GLenum, GLuint, GLuint) = nullptr;
    void (SVMS_GLAPI* BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void (SVMS_GLAPI* BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;
    void (SVMS_GLAPI* GetBufferSubData)(GLenum, GLintptr, GLsizeiptr, void*) = nullptr;
    // GL_ARB_buffer_storage (optional): pinned, persistently mapped read-back.
    void (SVMS_GLAPI* BufferStorage)(GLenum, GLsizeiptr, const void*, GLbitfield) = nullptr;
    void* (SVMS_GLAPI* MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = nullptr;
    void (SVMS_GLAPI* CopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr) = nullptr;
    void (SVMS_GLAPI* DispatchCompute)(GLuint, GLuint, GLuint) = nullptr;
    void (SVMS_GLAPI* MemoryBarrier)(GLbitfield) = nullptr;
    GLsync (SVMS_GLAPI* FenceSync)(GLenum, GLbitfield) = nullptr;
    GLenum (SVMS_GLAPI* ClientWaitSync)(GLsync, GLbitfield, GLuint64) = nullptr;
    void (SVMS_GLAPI* DeleteSync)(GLsync) = nullptr;
    void (SVMS_GLAPI* GenQueries)(GLsizei, GLuint*) = nullptr;
    void (SVMS_GLAPI* BeginQuery)(GLenum, GLuint) = nullptr;
    void (SVMS_GLAPI* EndQuery)(GLenum) = nullptr;
    void (SVMS_GLAPI* GetQueryObjectui64v)(GLuint, GLenum, GLuint64*) = nullptr;
};

// ── Platform context ────────────────────────────────────────────────────
class GLContext {
public:
    ~GLContext() { Destroy(); }
    bool Create(std::string& error);
    bool MakeCurrent();
    void Release();
    void Destroy();
    // Remember / put back whichever context the calling thread had current, so
    // a host that renders with OpenGL on this thread keeps its own context.
    void Push();
    void Pop();
    void* Proc(const char* name) const;

private:
#if defined(_WIN32)
    HMODULE opengl32_ = nullptr;
    HWND window_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC rc_ = nullptr;
    PROC (WINAPI* getProc_)(LPCSTR) = nullptr;
    BOOL (WINAPI* makeCurrent_)(HDC, HGLRC) = nullptr;
    BOOL (WINAPI* deleteContext_)(HGLRC) = nullptr;
    HDC prevDc_ = nullptr;
    HGLRC prevRc_ = nullptr;
#else
    void* egl_ = nullptr;
    void* display_ = nullptr;
    void* context_ = nullptr;
    void* surface_ = nullptr;
    void* (*getProc_)(const char*) = nullptr;
    unsigned (*makeCurrent_)(void*, void*, void*, void*) = nullptr;
#endif
};

#if defined(_WIN32)

bool GLContext::Create(std::string& error) {
    opengl32_ = LoadLibraryW(L"opengl32.dll");
    HMODULE gdi32 = LoadLibraryW(L"gdi32.dll");
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (!opengl32_ || !gdi32 || !user32) {
        error = "opengl32.dll is not available";
        return false;
    }
    auto createContext = reinterpret_cast<HGLRC(WINAPI*)(HDC)>(
        GetProcAddress(opengl32_, "wglCreateContext"));
    deleteContext_ = reinterpret_cast<BOOL(WINAPI*)(HGLRC)>(
        GetProcAddress(opengl32_, "wglDeleteContext"));
    makeCurrent_ = reinterpret_cast<BOOL(WINAPI*)(HDC, HGLRC)>(
        GetProcAddress(opengl32_, "wglMakeCurrent"));
    getProc_ = reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(
        GetProcAddress(opengl32_, "wglGetProcAddress"));
    Push();
    auto choosePixelFormat = reinterpret_cast<int(WINAPI*)(HDC, const PIXELFORMATDESCRIPTOR*)>(
        GetProcAddress(gdi32, "ChoosePixelFormat"));
    auto setPixelFormat = reinterpret_cast<BOOL(WINAPI*)(HDC, int, const PIXELFORMATDESCRIPTOR*)>(
        GetProcAddress(gdi32, "SetPixelFormat"));
    auto createWindow = reinterpret_cast<HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID)>(
        GetProcAddress(user32, "CreateWindowExW"));
    auto getDC = reinterpret_cast<HDC(WINAPI*)(HWND)>(GetProcAddress(user32, "GetDC"));
    if (!createContext || !deleteContext_ || !makeCurrent_ || !getProc_ ||
        !choosePixelFormat || !setPixelFormat || !createWindow || !getDC) {
        error = "WGL entry points are missing";
        return false;
    }
    // A hidden STATIC window: the cheapest thing that owns a device context.
    window_ = createWindow(0, L"STATIC", L"SVMS GPU", WS_POPUP, 0, 0, 1, 1,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window_) { error = "CreateWindowEx failed"; return false; }
    dc_ = getDC(window_);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int format = choosePixelFormat(dc_, &pfd);
    if (format == 0 || !setPixelFormat(dc_, format, &pfd)) {
        error = "no usable pixel format";
        return false;
    }
    HGLRC legacy = createContext(dc_);
    if (!legacy || !makeCurrent_(dc_, legacy)) {
        error = "wglCreateContext failed";
        if (legacy) deleteContext_(legacy);
        return false;
    }
    using CreateAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
    auto createAttribs = reinterpret_cast<CreateAttribs>(
        getProc_("wglCreateContextAttribsARB"));
    if (!createAttribs) {
        makeCurrent_(nullptr, nullptr);
        deleteContext_(legacy);
        error = "WGL_ARB_create_context is not supported";
        return false;
    }
    const int attribs[] = {
        0x2091, 4,        // WGL_CONTEXT_MAJOR_VERSION_ARB
        0x2092, 3,        // WGL_CONTEXT_MINOR_VERSION_ARB
        0x9126, 0x1,      // WGL_CONTEXT_PROFILE_MASK_ARB = core
        0};
    rc_ = createAttribs(dc_, nullptr, attribs);
    makeCurrent_(nullptr, nullptr);
    deleteContext_(legacy);
    if (!rc_) {
        error = "OpenGL 4.3 core context creation failed";
        return false;
    }
    return makeCurrent_(dc_, rc_) != FALSE;
}

bool GLContext::MakeCurrent() {
    return rc_ && makeCurrent_(dc_, rc_) != FALSE;
}

void GLContext::Push() {
    if (!opengl32_) opengl32_ = LoadLibraryW(L"opengl32.dll");
    if (!opengl32_) return;
    auto getDc = reinterpret_cast<HDC(WINAPI*)()>(
        GetProcAddress(opengl32_, "wglGetCurrentDC"));
    auto getRc = reinterpret_cast<HGLRC(WINAPI*)()>(
        GetProcAddress(opengl32_, "wglGetCurrentContext"));
    prevDc_ = getDc ? getDc() : nullptr;
    prevRc_ = getRc ? getRc() : nullptr;
}

void GLContext::Pop() {
    if (!makeCurrent_) return;
    if (prevRc_ && prevRc_ != rc_) makeCurrent_(prevDc_, prevRc_);
    else makeCurrent_(nullptr, nullptr);
    prevDc_ = nullptr;
    prevRc_ = nullptr;
}

void GLContext::Release() {
    Pop();
}

void GLContext::Destroy() {
    if (rc_ && deleteContext_) {
        makeCurrent_(nullptr, nullptr);
        deleteContext_(rc_);
    }
    rc_ = nullptr;
    Pop();
    if (window_) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto destroy = reinterpret_cast<BOOL(WINAPI*)(HWND)>(
            user32 ? GetProcAddress(user32, "DestroyWindow") : nullptr);
        if (destroy) destroy(window_);
        window_ = nullptr;
    }
}

void* GLContext::Proc(const char* name) const {
    void* p = getProc_ ? reinterpret_cast<void*>(getProc_(name)) : nullptr;
    const auto v = reinterpret_cast<intptr_t>(p);
    if (p && v != 1 && v != 2 && v != 3 && v != -1) return p;
    return opengl32_ ? reinterpret_cast<void*>(GetProcAddress(opengl32_, name))
                     : nullptr;
}

#else  // Linux: EGL

namespace egl {
constexpr int kNone = 0x3038;
constexpr int kSurfaceType = 0x3033;
constexpr int kPbufferBit = 0x0001;
constexpr int kRenderableType = 0x3040;
constexpr int kOpenGLBit = 0x0008;
constexpr unsigned kOpenGLApi = 0x30A2;
constexpr int kContextMajor = 0x3098;
constexpr int kContextMinor = 0x30FB;
constexpr int kContextProfile = 0x30FD;
constexpr int kCoreProfileBit = 0x1;
constexpr int kWidth = 0x3057;
constexpr int kHeight = 0x3056;
}  // namespace egl

bool GLContext::Create(std::string& error) {
    egl_ = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!egl_) { error = "libEGL.so.1 is not available"; return false; }
    auto sym = [&](const char* name) { return dlsym(egl_, name); };
    using GetDisplay = void* (*)(void*);
    using Initialize = unsigned (*)(void*, int*, int*);
    using BindApi = unsigned (*)(unsigned);
    using ChooseConfig = unsigned (*)(void*, const int*, void**, int, int*);
    using CreateContext = void* (*)(void*, void*, void*, const int*);
    using CreatePbuffer = void* (*)(void*, void*, const int*);
    auto getDisplay = reinterpret_cast<GetDisplay>(sym("eglGetDisplay"));
    auto initialize = reinterpret_cast<Initialize>(sym("eglInitialize"));
    auto bindApi = reinterpret_cast<BindApi>(sym("eglBindAPI"));
    auto chooseConfig = reinterpret_cast<ChooseConfig>(sym("eglChooseConfig"));
    auto createContext = reinterpret_cast<CreateContext>(sym("eglCreateContext"));
    auto createPbuffer = reinterpret_cast<CreatePbuffer>(sym("eglCreatePbufferSurface"));
    getProc_ = reinterpret_cast<void* (*)(const char*)>(sym("eglGetProcAddress"));
    makeCurrent_ = reinterpret_cast<unsigned (*)(void*, void*, void*, void*)>(
        sym("eglMakeCurrent"));
    if (!getDisplay || !initialize || !bindApi || !chooseConfig ||
        !createContext || !getProc_ || !makeCurrent_) {
        error = "EGL entry points are missing";
        return false;
    }
    display_ = getDisplay(nullptr);
    int major = 0, minor = 0;
    if (!display_ || !initialize(display_, &major, &minor)) {
        error = "eglInitialize failed";
        return false;
    }
    if (!bindApi(egl::kOpenGLApi)) { error = "eglBindAPI(OpenGL) failed"; return false; }
    const int configAttribs[] = {egl::kSurfaceType, egl::kPbufferBit,
                                 egl::kRenderableType, egl::kOpenGLBit,
                                 egl::kNone};
    void* config = nullptr;
    int configCount = 0;
    if (!chooseConfig(display_, configAttribs, &config, 1, &configCount) ||
        configCount < 1) {
        error = "no EGL config for OpenGL";
        return false;
    }
    const int ctxAttribs[] = {egl::kContextMajor, 4, egl::kContextMinor, 3,
                              egl::kContextProfile, egl::kCoreProfileBit,
                              egl::kNone};
    context_ = createContext(display_, config, nullptr, ctxAttribs);
    if (!context_) { error = "OpenGL 4.3 core context creation failed"; return false; }
    // Surfaceless first (EGL_KHR_surfaceless_context); a 1x1 pbuffer otherwise.
    if (!makeCurrent_(display_, nullptr, nullptr, context_)) {
        if (!createPbuffer) { error = "eglMakeCurrent failed"; return false; }
        const int pbAttribs[] = {egl::kWidth, 1, egl::kHeight, 1, egl::kNone};
        surface_ = createPbuffer(display_, config, pbAttribs);
        if (!surface_ || !makeCurrent_(display_, surface_, surface_, context_)) {
            error = "eglMakeCurrent failed";
            return false;
        }
    }
    return true;
}

void GLContext::Push() {}
void GLContext::Pop() {}

bool GLContext::MakeCurrent() {
    return context_ &&
           makeCurrent_(display_, surface_, surface_, context_) != 0u;
}

void GLContext::Release() {
    if (makeCurrent_ && display_)
        makeCurrent_(display_, nullptr, nullptr, nullptr);
}

void GLContext::Destroy() {
    if (display_ && context_ && egl_) {
        Release();
        using DestroyContext = unsigned (*)(void*, void*);
        auto destroy = reinterpret_cast<DestroyContext>(
            dlsym(egl_, "eglDestroyContext"));
        if (destroy) destroy(display_, context_);
    }
    context_ = nullptr;
}

void* GLContext::Proc(const char* name) const {
    return getProc_ ? getProc_(name) : nullptr;
}

#endif

// ── The backend ─────────────────────────────────────────────────────────
class GLBackend final : public Backend {
public:
    ~GLBackend() override { Shutdown(); }
    bool Init(uint32_t adapterIndex, std::string& error) override;
    bool UploadStore(const int16_t* data, uint32_t frames) override;
    bool Reserve(uint32_t handleCapacity, uint32_t maxFrames) override;
    bool UploadRows(const UploadRecord* records, uint32_t count) override;
    bool UploadEnvRows(const EnvUploadRecord* records, uint32_t count) override;
    bool Submit(const uint32_t* list, uint32_t count,
                const uint32_t* envList, uint32_t envCount,
                uint32_t frames, float retireThreshold, bool last) override;
    void Abandon() override;
    Result Collect(uint64_t timeoutMicros, const float** mixLR,
                   const DynOut** dyn, const EnvDynOut** envDyn) override;
    void ProfileNs(uint64_t& voice, uint64_t& reduce,
                   uint64_t& env) const override {
        voice = profileNs_[0];
        reduce = profileNs_[1];
        env = profileNs_[2];
    }
    const char* Name() const override { return name_.c_str(); }
    uint32_t MaxVoices() const override { return handleCapacity_; }

private:
    enum Buf { kStore, kRows, kList, kPartial, kMix, kDyn, kUpload,
               kEnvRows, kEnvList, kEnvDyn, kEnvUpload, kState,
               kMixRead, kDynRead, kEnvDynRead, kBufCount };
    bool Bind();
    void BindAll();
    GLuint BuildProgram(const char* define, std::string& error);
    bool DetectSubgroup32();
    bool ResizeBuffer(Buf which, size_t bytes);
    bool ResizeReadback(Buf which, size_t bytes);
    void Shutdown();
    bool CheckError(const char* what);

    GLContext context_;
    GLFns gl_;
    bool created_ = false;
    bool bound_ = false;
    std::thread::id boundThread_;
    bool bindingsDirty_ = true;
    GLuint programs_[5] = {};        // voice, reduce, scatter, env, env scatter
    GLint loc_[5][5] = {};
    GLuint buf_[kBufCount] = {};
    size_t bufBytes_[kBufCount] = {};
    // A block is submitted in up to two parts; each has its own fence, mix
    // region and (when profiling) timer queries.
    struct Part {
        GLsync fence = nullptr;
        uint32_t count = 0u;
        uint32_t envCount = 0u;
        uint32_t frames = 0u;
        uint32_t listBase = 0u;
        uint32_t envBase = 0u;
        bool voiceTimed = false;
        bool envTimed = false;
    };
    Part parts_[kMaxParts];
    uint32_t partsSubmitted_ = 0u;
    uint32_t partsCollected_ = 0u;
    uint32_t listBase_ = 0u;
    uint32_t envBase_ = 0u;
    uint32_t groupBase_ = 0u;
    bool pinned_ = false;             // read-back through persistent mappings
    bool subgroup32_ = false;         // 32-wide subgroups with shuffles (KHR)
    void* mapped_[kBufCount] = {};
    std::vector<float> mixCopy_;      // fallback read-back storage
    std::vector<DynOut> dynCopy_;
    std::vector<EnvDynOut> envDynCopy_;
    bool profile_ = false;
    GLuint queries_[3 * kMaxParts] = {};
    uint64_t profileNs_[3] = {};
    uint32_t handleCapacity_ = 0u;
    uint32_t maxFrames_ = 0u;
    uint32_t group_ = 128u;   // voices per workgroup (shader GROUP)
    uint32_t tile_ = 4u;      // frames per reduction tile (shader TILE)
    uint32_t uploadCursor_ = 0u;
    uint32_t envUploadCursor_ = 0u;
    std::string name_ = "opengl";
};

template <typename F>
bool Fetch(const GLContext& ctx, F& slot, const char* name) {
    slot = reinterpret_cast<F>(ctx.Proc(name));
    return slot != nullptr;
}

bool GLBackend::CheckError(const char* what) {
    const GLenum e = gl_.GetError();
    if (e == GL_NO_ERROR) return true;
    name_ += std::string(" [") + what + " error " + std::to_string(e) + "]";
    return false;
}

bool GLBackend::Init(uint32_t, std::string& error) {
    if (!context_.Create(error)) return false;
    created_ = true;
    bool ok = true;
#define SVMS_FETCH(field, glname) ok = Fetch(context_, gl_.field, glname) && ok
    SVMS_FETCH(GetString, "glGetString");
    SVMS_FETCH(GetStringi, "glGetStringi");
    SVMS_FETCH(GetIntegerv, "glGetIntegerv");
    SVMS_FETCH(GetError, "glGetError");
    SVMS_FETCH(Flush, "glFlush");
    SVMS_FETCH(CreateShader, "glCreateShader");
    SVMS_FETCH(ShaderSource, "glShaderSource");
    SVMS_FETCH(CompileShader, "glCompileShader");
    SVMS_FETCH(GetShaderiv, "glGetShaderiv");
    SVMS_FETCH(GetShaderInfoLog, "glGetShaderInfoLog");
    SVMS_FETCH(CreateProgram, "glCreateProgram");
    SVMS_FETCH(AttachShader, "glAttachShader");
    SVMS_FETCH(LinkProgram, "glLinkProgram");
    SVMS_FETCH(GetProgramiv, "glGetProgramiv");
    SVMS_FETCH(GetProgramInfoLog, "glGetProgramInfoLog");
    SVMS_FETCH(UseProgram, "glUseProgram");
    SVMS_FETCH(DeleteShader, "glDeleteShader");
    SVMS_FETCH(DeleteProgram, "glDeleteProgram");
    SVMS_FETCH(GetUniformLocation, "glGetUniformLocation");
    SVMS_FETCH(Uniform1ui, "glUniform1ui");
    SVMS_FETCH(GenBuffers, "glGenBuffers");
    SVMS_FETCH(DeleteBuffers, "glDeleteBuffers");
    SVMS_FETCH(BindBuffer, "glBindBuffer");
    SVMS_FETCH(BindBufferBase, "glBindBufferBase");
    SVMS_FETCH(BufferData, "glBufferData");
    SVMS_FETCH(BufferSubData, "glBufferSubData");
    SVMS_FETCH(GetBufferSubData, "glGetBufferSubData");
    SVMS_FETCH(DispatchCompute, "glDispatchCompute");
    SVMS_FETCH(MemoryBarrier, "glMemoryBarrier");
    SVMS_FETCH(FenceSync, "glFenceSync");
    SVMS_FETCH(ClientWaitSync, "glClientWaitSync");
    SVMS_FETCH(DeleteSync, "glDeleteSync");
    SVMS_FETCH(GenQueries, "glGenQueries");
    SVMS_FETCH(BeginQuery, "glBeginQuery");
    SVMS_FETCH(EndQuery, "glEndQuery");
    SVMS_FETCH(GetQueryObjectui64v, "glGetQueryObjectui64v");
#undef SVMS_FETCH
    if (!ok) {
        error = "OpenGL 4.3 compute entry points are missing";
        return false;
    }
    pinned_ = Fetch(context_, gl_.BufferStorage, "glBufferStorage") &&
              Fetch(context_, gl_.MapBufferRange, "glMapBufferRange") &&
              Fetch(context_, gl_.CopyBufferSubData, "glCopyBufferSubData") &&
              std::getenv("SVMS_GPU_NO_PINNED") == nullptr;
    GLint major = 0, minor = 0;
    gl_.GetIntegerv(GL_MAJOR_VERSION, &major);
    gl_.GetIntegerv(GL_MINOR_VERSION, &minor);
    const char* renderer = reinterpret_cast<const char*>(gl_.GetString(GL_RENDERER));
    name_ = std::string("opengl ") + std::to_string(major) + "." +
            std::to_string(minor) + ": " + (renderer ? renderer : "?");
    if (major < 4 || (major == 4 && minor < 3)) {
        error = "OpenGL 4.3 is required (driver reports " +
                std::to_string(major) + "." + std::to_string(minor) + ")";
        return false;
    }
    GLint sharedBytes = 0, invocations = 0;
    gl_.GetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &sharedBytes);
    gl_.GetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &invocations);
    const GLint neededShared = static_cast<GLint>(group_ * tile_ * 2u * 4u + group_ * 4u);
    if (sharedBytes < neededShared || invocations < static_cast<GLint>(group_)) {
        error = "compute limits too small (shared " +
                std::to_string(sharedBytes) + " B, invocations " +
                std::to_string(invocations) + ")";
        return false;
    }
    static const char* const kDefines[5] = {"VOICE_CS", "REDUCE_CS", "SCATTER_CS",
                                            "ENV_CS", "ENV_SCATTER_CS"};
    // Warp-level reduction when the driver offers it; a driver that advertises
    // it but fails to compile the shader falls back to the shared-memory one.
    subgroup32_ = DetectSubgroup32();
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool built = true;
        for (int i = 0; i < 5 && built; ++i) {
            programs_[i] = BuildProgram(kDefines[i], error);
            built = programs_[i] != 0u;
        }
        if (built) break;
        for (GLuint& program : programs_) {
            if (program) gl_.DeleteProgram(program);
            program = 0u;
        }
        if (!subgroup32_) return false;
        subgroup32_ = false;
        error.clear();
    }
    if (std::getenv("SVMS_GPU_DUMP") != nullptr) {
        // Debug: write the linked program binaries (NVIDIA's carries the
        // assembly as text) next to the working directory.
        using GetBinary = void (SVMS_GLAPI*)(GLuint, GLsizei, GLsizei*, GLenum*, void*);
        GetBinary getBinary = reinterpret_cast<GetBinary>(context_.Proc("glGetProgramBinary"));
        if (getBinary) {
            for (int p = 0; p < 5; ++p) {
                GLint length = 0;
                gl_.GetProgramiv(programs_[p], 0x8741, &length);
                std::vector<char> blob(static_cast<size_t>(length > 0 ? length : 1));
                GLsizei written = 0;
                GLenum format = 0;
                getBinary(programs_[p], length, &written, &format, blob.data());
                std::string path = "gpu_program_" + std::to_string(p) + ".bin";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(blob.data(), 1, static_cast<size_t>(written), f);
                    std::fclose(f);
                }
            }
        }
    }
    if (subgroup32_) name_ += " [subgroup]";
    if (pinned_) name_ += " [pinned]";
    static const char* const kUniforms[5][5] = {
        {"uCount", "uFrames", "uListBase", "uGroupBase", nullptr},
        {"uFrames", "uGroups", "uGroupFirst", "uMixBase", nullptr},
        {"uCount", "uBase", nullptr, nullptr, nullptr},
        {"uCount", "uFrames", "uListBase", "uGroupBase", "uRetireBits"},
        {"uCount", "uBase", nullptr, nullptr, nullptr}};
    for (int p = 0; p < 5; ++p)
        for (int u = 0; u < 5; ++u)
            loc_[p][u] = kUniforms[p][u]
                ? gl_.GetUniformLocation(programs_[p], kUniforms[p][u]) : -1;
    gl_.GenBuffers(kBufCount, buf_);
    profile_ = std::getenv("SVMS_GPU_PROFILE") != nullptr;
    if (profile_) gl_.GenQueries(3 * kMaxParts, queries_);
    context_.Release();
    return true;
}

bool GLBackend::DetectSubgroup32() {
    if (std::getenv("SVMS_GPU_NO_SUBGROUP") != nullptr || !gl_.GetStringi ||
        (group_ % 32u) != 0u) {
        return false;
    }
    GLint count = 0;
    gl_.GetIntegerv(GL_NUM_EXTENSIONS, &count);
    bool basic = false;
    bool shuffle = false;
    for (GLint i = 0; i < count; ++i) {
        const char* name = reinterpret_cast<const char*>(
            gl_.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (!name) continue;
        // One API-level extension (GL_KHR_shader_subgroup) covers the GLSL
        // _basic and _shuffle extensions used below.
        const bool khr = std::strcmp(name, "GL_KHR_shader_subgroup") == 0;
        basic = basic || khr;
        shuffle = shuffle || khr;
    }
    if (!basic || !shuffle) return false;
    GLint size = 0, stages = 0, features = 0;
    gl_.GetIntegerv(GL_SUBGROUP_SIZE_KHR, &size);
    gl_.GetIntegerv(GL_SUBGROUP_SUPPORTED_STAGES_KHR, &stages);
    gl_.GetIntegerv(GL_SUBGROUP_SUPPORTED_FEATURES_KHR, &features);
    gl_.GetError();
    // Compute stage bit 0x20; basic 0x1 and shuffle 0x10 features.
    return size == 32 && (stages & 0x20) != 0 && (features & 0x11) == 0x11;
}

GLuint GLBackend::BuildProgram(const char* define, std::string& error) {
    std::string header = "#version 430\n";
    if (subgroup32_) {
        header += "#extension GL_KHR_shader_subgroup_basic : require\n"
                  "#extension GL_KHR_shader_subgroup_shuffle : require\n"
                  "#define SUBGROUP32 1\n";
    }
    header += std::string("#define ") + define + " 1\n";
    const std::string sizes = "#define GROUP " + std::to_string(group_) + char(10) + "#define TILE " + std::to_string(tile_) + char(10);
    const std::string full = header + sizes;
    const GLchar* sources[2] = {full.c_str(), kVoiceStageGLSL};
    GLuint shader = gl_.CreateShader(GL_COMPUTE_SHADER);
    gl_.ShaderSource(shader, 2, sources, nullptr);
    gl_.CompileShader(shader);
    GLint status = 0;
    gl_.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint length = 0;
        gl_.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1));
        gl_.GetShaderInfoLog(shader, length, nullptr, log.data());
        error = std::string(define) + " failed to compile: " + log.data();
        gl_.DeleteShader(shader);
        return 0u;
    }
    GLuint program = gl_.CreateProgram();
    gl_.AttachShader(program, shader);
    gl_.LinkProgram(program);
    gl_.DeleteShader(shader);
    gl_.GetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        GLint length = 0;
        gl_.GetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1));
        gl_.GetProgramInfoLog(program, length, nullptr, log.data());
        error = std::string(define) + " failed to link: " + log.data();
        gl_.DeleteProgram(program);
        return 0u;
    }
    return program;
}

bool GLBackend::Bind() {
    // The context belongs to one thread at a time.  The audio thread binds it
    // on its first block; a rebuilt audio thread (device change) rebinds it.
    const std::thread::id self = std::this_thread::get_id();
    if (bound_ && boundThread_ == self) return true;
    if (!context_.MakeCurrent()) return false;
    boundThread_ = self;
    bound_ = true;
    return true;
}

void GLBackend::BindAll() {
    if (!bindingsDirty_) return;
    for (GLuint binding = 0u; binding < 12u; ++binding) {
        // Binding point order matches the shader: store, rows, list, partial,
        // mix, dyn, upload, env rows, env list, env dyn, env upload.
        static const Buf kOrder[12] = {kStore, kRows, kList, kPartial, kMix,
                                       kDyn, kUpload, kEnvRows, kEnvList,
                                       kEnvDyn, kEnvUpload, kState};
        gl_.BindBufferBase(GL_SHADER_STORAGE_BUFFER, binding,
                           buf_[kOrder[binding]]);
    }
    bindingsDirty_ = false;
}

bool GLBackend::ResizeBuffer(Buf which, size_t bytes) {
    if (bytes == 0u) bytes = 16u;
    GLint maxBlock = 0;
    gl_.GetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxBlock);
    if (maxBlock > 0 && bytes > static_cast<size_t>(maxBlock)) return false;
    if (bufBytes_[which] == bytes) return true;
    gl_.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[which]);
    gl_.BufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes),
                   nullptr, GL_DYNAMIC_DRAW);
    bufBytes_[which] = bytes;
    return gl_.GetError() == GL_NO_ERROR;
}

// Host-visible read-back buffer: pinned and persistently mapped when the driver
// has GL_ARB_buffer_storage (the GPU then DMAs straight into it), a plain
// buffer plus a host copy otherwise.
bool GLBackend::ResizeReadback(Buf which, size_t bytes) {
    if (bytes == 0u) bytes = 16u;
    if (bufBytes_[which] == bytes) return true;
    if (!pinned_) {
        bufBytes_[which] = bytes;
        return true;
    }
    if (mapped_[which] != nullptr) {
        // Deleting the buffer unmaps it; storage is immutable, so a new size
        // needs a new buffer object.
        gl_.DeleteBuffers(1, &buf_[which]);
        gl_.GenBuffers(1, &buf_[which]);
        mapped_[which] = nullptr;
    }
    const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT |
                             GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT;
    gl_.BindBuffer(GL_COPY_WRITE_BUFFER, buf_[which]);
    gl_.BufferStorage(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(bytes),
                      nullptr, flags);
    mapped_[which] = gl_.MapBufferRange(
        GL_COPY_WRITE_BUFFER, 0, static_cast<GLsizeiptr>(bytes),
        GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    if (mapped_[which] == nullptr || gl_.GetError() != GL_NO_ERROR) {
        pinned_ = false;       // fall back to glGetBufferSubData
        mapped_[which] = nullptr;
    }
    bufBytes_[which] = bytes;
    return true;
}

bool GLBackend::UploadStore(const int16_t* data, uint32_t frames) {
    if (!Bind()) return false;
    // frames + 8 elements are readable (the sample-store padding contract);
    // whole 32-bit words only.
    const size_t words = (static_cast<size_t>(frames) + 8u) / 2u;
    const size_t bytes = words * 4u;
    // One spare word: the kernel reads the word after the last sample.
    const size_t allocBytes = bytes + 16u;
    GLint maxBlock = 0;
    gl_.GetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxBlock);
    if (maxBlock > 0 && bytes > static_cast<size_t>(maxBlock)) return false;
    gl_.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[kStore]);
    gl_.BufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(allocBytes),
                   nullptr, GL_STATIC_DRAW);
    gl_.BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(bytes),
                      data);
    bufBytes_[kStore] = allocBytes;
    bindingsDirty_ = true;
    return gl_.GetError() == GL_NO_ERROR;
}

bool GLBackend::Reserve(uint32_t handleCapacity, uint32_t maxFrames) {
    if (!Bind()) return false;
    if (handleCapacity == handleCapacity_ && maxFrames == maxFrames_) return true;
    const size_t cap = handleCapacity;
    // Sustained and envelope rows each round their last group up.
    // (each part rounds two kernels' last groups up).
    const size_t groups = (cap + group_ - 1u) / group_ + 2u * kMaxParts;
    bool ok = ResizeBuffer(kEnvRows, cap * kEnvRowWords * 4u) &&
              ResizeBuffer(kState, cap * kDynWords * 4u) &&
              ResizeBuffer(kEnvList, cap * sizeof(uint32_t)) &&
              ResizeBuffer(kEnvDyn, cap * sizeof(EnvDynOut)) &&
              ResizeBuffer(kEnvUpload, cap * sizeof(EnvUploadRecord)) &&
              ResizeBuffer(kRows, cap * kRowWords * 4u) &&
              ResizeBuffer(kList, cap * sizeof(uint32_t)) &&
              ResizeBuffer(kDyn, cap * sizeof(DynOut)) &&
              ResizeBuffer(kPartial, groups * maxFrames * 2u * 4u) &&
              ResizeBuffer(kMix, static_cast<size_t>(maxFrames) * 2u * kMaxParts * 4u) &&
              ResizeReadback(kMixRead, static_cast<size_t>(maxFrames) * 2u * kMaxParts * 4u) &&
              ResizeReadback(kDynRead, cap * sizeof(DynOut)) &&
              ResizeReadback(kEnvDynRead, cap * sizeof(EnvDynOut)) &&
              ResizeBuffer(kUpload, cap * sizeof(UploadRecord));
    if (!ok) return false;
    bindingsDirty_ = true;
    handleCapacity_ = handleCapacity;
    maxFrames_ = maxFrames;
    uploadCursor_ = 0u;
    envUploadCursor_ = 0u;
    if (!pinned_) {
        mixCopy_.assign(static_cast<size_t>(maxFrames) * 2u * kMaxParts, 0.0f);
        dynCopy_.assign(cap, DynOut{});
        envDynCopy_.assign(cap, EnvDynOut{});
    }
    Abandon();
    return gl_.GetError() == GL_NO_ERROR;
}

bool GLBackend::UploadRows(const UploadRecord* records, uint32_t count) {
    if (count == 0u) return true;
    if (!Bind() || uploadCursor_ + count > handleCapacity_) return false;
    BindAll();
    gl_.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[kUpload]);
    gl_.BufferSubData(GL_SHADER_STORAGE_BUFFER,
                      static_cast<GLintptr>(uploadCursor_) * sizeof(UploadRecord),
                      static_cast<GLsizeiptr>(count) * sizeof(UploadRecord),
                      records);
    gl_.UseProgram(programs_[2]);
    gl_.Uniform1ui(loc_[2][0], count);
    gl_.Uniform1ui(loc_[2][1], uploadCursor_);
    gl_.DispatchCompute((count + 63u) / 64u, 1u, 1u);
    // Rows are read by the voice pass; later scatters touch other handles.
    gl_.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    uploadCursor_ += count;
    return true;
}

bool GLBackend::UploadEnvRows(const EnvUploadRecord* records, uint32_t count) {
    if (count == 0u) return true;
    if (!Bind() || envUploadCursor_ + count > handleCapacity_) return false;
    BindAll();
    gl_.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[kEnvUpload]);
    gl_.BufferSubData(GL_SHADER_STORAGE_BUFFER,
                      static_cast<GLintptr>(envUploadCursor_) * sizeof(EnvUploadRecord),
                      static_cast<GLsizeiptr>(count) * sizeof(EnvUploadRecord),
                      records);
    gl_.UseProgram(programs_[4]);
    gl_.Uniform1ui(loc_[4][0], count);
    gl_.Uniform1ui(loc_[4][1], envUploadCursor_);
    gl_.DispatchCompute((count + 63u) / 64u, 1u, 1u);
    gl_.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    envUploadCursor_ += count;
    return true;
}

bool GLBackend::Submit(const uint32_t* list, uint32_t count,
                       const uint32_t* envList, uint32_t envCount,
                       uint32_t frames, float retireThreshold, bool last) {
    if (partsSubmitted_ != 0u && partsCollected_ == partsSubmitted_) {
        // The previous block is fully collected: this starts a new one.
        partsSubmitted_ = partsCollected_ = 0u;
        listBase_ = envBase_ = groupBase_ = 0u;
    }
    if (!Bind() || listBase_ + count > handleCapacity_ ||
        envBase_ + envCount > handleCapacity_ || frames == 0u ||
        frames > maxFrames_ || partsSubmitted_ >= kMaxParts) {
        return false;
    }
    BindAll();
    const uint32_t part = partsSubmitted_;
    Part& p = parts_[part];
    const uint32_t groups = (count + group_ - 1u) / group_;
    const uint32_t envGroups = (envCount + group_ - 1u) / group_;
    if (count != 0u) {
        gl_.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[kList]);
        gl_.BufferSubData(GL_SHADER_STORAGE_BUFFER,
                          static_cast<GLintptr>(listBase_) * sizeof(uint32_t),
                          static_cast<GLsizeiptr>(count) * sizeof(uint32_t), list);
    }
    if (envCount != 0u) {
        gl_.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[kEnvList]);
        gl_.BufferSubData(GL_SHADER_STORAGE_BUFFER,
                          static_cast<GLintptr>(envBase_) * sizeof(uint32_t),
                          static_cast<GLsizeiptr>(envCount) * sizeof(uint32_t),
                          envList);
    }
    p.voiceTimed = profile_ && count != 0u;
    p.envTimed = profile_ && envCount != 0u;
    if (count != 0u) {
        gl_.UseProgram(programs_[0]);
        gl_.Uniform1ui(loc_[0][0], count);
        gl_.Uniform1ui(loc_[0][1], frames);
        gl_.Uniform1ui(loc_[0][2], listBase_);
        gl_.Uniform1ui(loc_[0][3], groupBase_);
        if (p.voiceTimed) gl_.BeginQuery(GL_TIME_ELAPSED, queries_[part * 3u]);
        gl_.DispatchCompute(groups, 1u, 1u);
        if (p.voiceTimed) gl_.EndQuery(GL_TIME_ELAPSED);
    }
    if (envCount != 0u) {
        uint32_t retireBits = 0u;
        std::memcpy(&retireBits, &retireThreshold, sizeof(retireBits));
        gl_.UseProgram(programs_[3]);
        gl_.Uniform1ui(loc_[3][0], envCount);
        gl_.Uniform1ui(loc_[3][1], frames);
        gl_.Uniform1ui(loc_[3][2], envBase_);
        gl_.Uniform1ui(loc_[3][3], groupBase_ + groups);
        gl_.Uniform1ui(loc_[3][4], retireBits);
        if (p.envTimed) gl_.BeginQuery(GL_TIME_ELAPSED, queries_[part * 3u + 2u]);
        gl_.DispatchCompute(envGroups, 1u, 1u);
        if (p.envTimed) gl_.EndQuery(GL_TIME_ELAPSED);
    }
    gl_.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    gl_.UseProgram(programs_[1]);
    gl_.Uniform1ui(loc_[1][0], frames);
    gl_.Uniform1ui(loc_[1][1], groups + envGroups);
    gl_.Uniform1ui(loc_[1][2], groupBase_);
    gl_.Uniform1ui(loc_[1][3], part * frames * 2u);
    if (profile_) gl_.BeginQuery(GL_TIME_ELAPSED, queries_[part * 3u + 1u]);
    gl_.DispatchCompute((frames * 2u + 63u) / 64u, 1u, 1u);
    if (profile_) gl_.EndQuery(GL_TIME_ELAPSED);
    gl_.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    if (pinned_ && mapped_[kMixRead] != nullptr) {
        // The GPU writes the results straight into pinned host memory.
        auto copy = [&](Buf from, Buf to, size_t offset, size_t bytes) {
            gl_.BindBuffer(GL_COPY_READ_BUFFER, buf_[from]);
            gl_.BindBuffer(GL_COPY_WRITE_BUFFER, buf_[to]);
            gl_.CopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                                  static_cast<GLintptr>(offset),
                                  static_cast<GLintptr>(offset),
                                  static_cast<GLsizeiptr>(bytes));
        };
        copy(kMix, kMixRead, static_cast<size_t>(part) * frames * 8u,
             static_cast<size_t>(frames) * 8u);
        if (count != 0u)
            copy(kDyn, kDynRead, static_cast<size_t>(listBase_) * sizeof(DynOut),
                 static_cast<size_t>(count) * sizeof(DynOut));
        if (envCount != 0u)
            copy(kEnvDyn, kEnvDynRead,
                 static_cast<size_t>(envBase_) * sizeof(EnvDynOut),
                 static_cast<size_t>(envCount) * sizeof(EnvDynOut));
    }
    p.fence = gl_.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0u);
    gl_.Flush();
    p.count = count;
    p.envCount = envCount;
    p.frames = frames;
    p.listBase = listBase_;
    p.envBase = envBase_;
    listBase_ += count;
    envBase_ += envCount;
    groupBase_ += groups + envGroups;
    ++partsSubmitted_;
    if (last) {
        uploadCursor_ = 0u;
        envUploadCursor_ = 0u;
    }
    return p.fence != nullptr && gl_.GetError() == GL_NO_ERROR;
}

void GLBackend::Abandon() {
    for (Part& p : parts_) {
        if (p.fence) gl_.DeleteSync(p.fence);
        p = Part{};
    }
    partsSubmitted_ = partsCollected_ = 0u;
    listBase_ = envBase_ = groupBase_ = 0u;
    uploadCursor_ = envUploadCursor_ = 0u;
}

Result GLBackend::Collect(uint64_t timeoutMicros, const float** mixLR,
                          const DynOut** dyn, const EnvDynOut** envDyn) {
    if (partsCollected_ >= partsSubmitted_) return Result::Idle;
    const uint32_t part = partsCollected_;
    Part& p = parts_[part];
    if (!p.fence) return Result::Idle;
    const GLuint64 nanos = timeoutMicros >= (GL_TIMEOUT_IGNORED / 1000ull)
        ? GL_TIMEOUT_IGNORED : timeoutMicros * 1000ull;
    const GLenum wait = gl_.ClientWaitSync(p.fence, GL_SYNC_FLUSH_COMMANDS_BIT, nanos);
    if (wait == GL_TIMEOUT_EXPIRED) return Result::Late;   // caller abandons
    gl_.DeleteSync(p.fence);
    p.fence = nullptr;
    if (wait != GL_ALREADY_SIGNALED && wait != GL_CONDITION_SATISFIED)
        return Result::Lost;
    if (profile_) {
        for (uint32_t q = 0; q < 3u; ++q) {
            if ((q == 0u && !p.voiceTimed) || (q == 2u && !p.envTimed)) continue;
            GLuint64 ns = 0;
            gl_.GetQueryObjectui64v(queries_[part * 3u + q], GL_QUERY_RESULT, &ns);
            profileNs_[q] += ns;
        }
    }
    ++partsCollected_;
    const size_t mixOffset = static_cast<size_t>(part) * p.frames * 2u;
    if (pinned_ && mapped_[kMixRead] != nullptr) {
        *mixLR = static_cast<const float*>(mapped_[kMixRead]) + mixOffset;
        *dyn = static_cast<const DynOut*>(mapped_[kDynRead]);
        *envDyn = static_cast<const EnvDynOut*>(mapped_[kEnvDynRead]);
        return gl_.GetError() == GL_NO_ERROR ? Result::Ok : Result::Lost;
    }
    gl_.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[kMix]);
    gl_.GetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                         static_cast<GLintptr>(mixOffset * 4u),
                         static_cast<GLsizeiptr>(p.frames) * 8,
                         mixCopy_.data() + mixOffset);
    if (p.count != 0u) {
        gl_.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[kDyn]);
        gl_.GetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                             static_cast<GLintptr>(p.listBase) * sizeof(DynOut),
                             static_cast<GLsizeiptr>(p.count) * sizeof(DynOut),
                             dynCopy_.data() + p.listBase);
    }
    if (p.envCount != 0u) {
        gl_.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[kEnvDyn]);
        gl_.GetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                             static_cast<GLintptr>(p.envBase) * sizeof(EnvDynOut),
                             static_cast<GLsizeiptr>(p.envCount) * sizeof(EnvDynOut),
                             envDynCopy_.data() + p.envBase);
    }
    *mixLR = mixCopy_.data() + mixOffset;
    *dyn = dynCopy_.data();
    *envDyn = envDynCopy_.data();
    return gl_.GetError() == GL_NO_ERROR ? Result::Ok : Result::Lost;
}

void GLBackend::Shutdown() {
    if (!created_) return;
    context_.Push();
    if (context_.MakeCurrent()) {
        Abandon();
        for (GLuint program : programs_)
            if (program) gl_.DeleteProgram(program);
        gl_.DeleteBuffers(kBufCount, buf_);
    }
    context_.Destroy();
    created_ = false;
}

}  // namespace

std::unique_ptr<Backend> CreateOpenGLBackend() {
    return std::unique_ptr<Backend>(new GLBackend());
}

}  // namespace gpu
}  // namespace svms
