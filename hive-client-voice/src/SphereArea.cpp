#include "SphereArea.h"

#include <epoxy/gl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hv {

namespace {

constexpr int kPointCount = 4200;

// GLSL 330 core port of the GLES2 swarm vertex shader — same math,
// `attribute`→`in`, `varying`→out, uniforms unchanged.
const char* kSwarmVertexShader = R"GLSL(
#version 330 core

layout(location = 0) in vec4 aSeed0;  // xyz: base direction on unit sphere · w: speed factor
layout(location = 1) in vec4 aSeed1;  // x: shell radius · y: phase · z: dart rate · w: brightness

uniform mat4  uProj;
uniform float uTime;
uniform float uLevel;   // 0..1 smoothed voice level
uniform float uState;   // 0 idle, 1 listening, 2 thinking, 3 speaking, 4 processing
uniform float uPointScale;
uniform float uCamDist; // camera distance — widens for portrait screens

out vec4 vColor;

float hash1(float n){ return fract(sin(n) * 43758.5453123); }
vec3 hash3(float n){
    return fract(sin(vec3(n, n + 1.31, n + 2.62)) *
                 vec3(43758.5453123, 22578.1459123, 19642.3490434));
}

void main(){
    float listenAmt = clamp(uState, 0.0, 1.0) * clamp(2.0 - uState, 0.0, 1.0);
    float thinkAmt  = max(0.0, 1.0 - abs(uState - 2.0));
    float speakAmt  = clamp(uState - 2.0, 0.0, 1.0) * clamp(4.0 - uState, 0.0, 1.0);
    float procAmt   = clamp(uState - 3.0, 0.0, 1.0);
    float pulse = 0.5 + 0.5 * sin(uTime * 3.2);

    vec3  baseDir = normalize(aSeed0.xyz);
    float ph      = aSeed1.y;
    float speed   = 0.55 + aSeed0.w * 1.45;
    float t       = uTime * speed + ph * 113.0;

    float agit = clamp(uLevel + thinkAmt * 0.55 + procAmt * 0.45 * pulse, 0.0, 1.5);

    vec3 core = 0.07 * vec3(sin(uTime * 0.31 + 1.7),
                            sin(uTime * 0.23),
                            sin(uTime * 0.19 + 4.2));

    float wanderAmp = 0.08 + 0.10 * agit;
    float f1 = 1.9 + 1.3 * hash1(ph * 3.1 + 0.7);
    float f2 = 2.6 + 1.5 * hash1(ph * 5.3 + 2.9);
    float f3 = 3.4 + 1.7 * hash1(ph * 7.7 + 5.1);
    vec3 wander = vec3(sin(t * f1 + ph * 6.1),
                       sin(t * f2 + ph * 9.4),
                       sin(t * f3 + ph * 12.7));
    wander -= baseDir * dot(wander, baseDir) * 0.6;

    float dartRate = aSeed1.z * (1.0 + 1.6 * agit);
    float cell  = floor(t * dartRate);
    float cellH = hash1(cell * 17.77 + ph * 91.7);
    float dartOn = step(0.62, cellH);
    vec3  dartDir = normalize(hash3(cell * 3.31 + ph * 57.3) - 0.5 +
                              vec3(0.0013, -0.0007, 0.0011));
    float dartPhase = fract(t * dartRate);
    float dartEnv = dartOn * smoothstep(0.0, 0.18, dartPhase) *
                    (1.0 - smoothstep(0.35, 1.0, dartPhase));
    vec3 dart = dartDir * dartEnv * (0.03 + 0.18 * agit);

    float wingBeat = 7.0 + aSeed0.w * 13.0;
    float flutter = 0.022 * sin(t * wingBeat) +
                    0.012 * sin(t * wingBeat * 0.37 + ph);
    float bulge = 1.0 + 0.10 * uLevel + 0.05 * thinkAmt + 0.06 * procAmt * pulse;
    float radius = (aSeed1.x + flutter) * bulge;

    vec3 world = baseDir * radius + wander * wanderAmp + dart + core;

    float rotSpeed = 0.25 + 0.15 * listenAmt + 1.30 * thinkAmt
                   + 0.30 * speakAmt + 0.55 * procAmt;
    float a = uTime * rotSpeed + baseDir.y * (thinkAmt + 0.4 * procAmt) * 2.0;
    float ca = cos(a), sa = sin(a);
    world = vec3(world.x * ca - world.z * sa, world.y,
                 world.x * sa + world.z * ca);

    float tilt = 0.35;
    float ct = cos(tilt), st = sin(tilt);
    world = vec3(world.x, world.y * ct - world.z * st, world.y * st + world.z * ct);

    vec4 view = vec4(world.xy, world.z - uCamDist, 1.0);
    gl_Position = uProj * view;

    float sizeVar = 0.7 + 0.9 * hash1(ph * 13.7 + 0.3);
    float flicker = 0.75 + 0.25 * sin(t * wingBeat);
    float ps = uPointScale * sizeVar * (1.1 + 1.0 * agit) * flicker;
    gl_PointSize = clamp(ps / max(0.6, -view.z), 1.0, 48.0);

    vec3 cIdle   = vec3(0.14, 0.46, 0.58);
    vec3 cListen = vec3(0.20, 0.85, 0.95);
    vec3 cThink  = vec3(0.95, 0.70, 0.25);
    vec3 cSpeak  = vec3(0.25, 0.95, 0.60);
    vec3 cProc   = vec3(0.62, 0.22, 0.95);
    vec3 col = cIdle;
    col = mix(col, cListen, listenAmt);
    col = mix(col, cThink,  thinkAmt);
    col = mix(col, cSpeak,  speakAmt);
    col = mix(col, cProc,   procAmt);

    float bee = step(0.88, hash1(ph * 29.1 + 3.3));
    col = mix(col, vec3(1.0, 0.72, 0.25), bee * 0.55);

    float bright = aSeed1.w * (0.85 + 0.75 * agit);
    bright = mix(bright, 0.75 + 0.25 * dartOn + 0.30 * pulse, procAmt);

    float depthFade = 0.55 + 0.45 *
        smoothstep(-(uCamDist + 1.3), -(uCamDist - 1.3), view.z);
    vColor = vec4(col * max(0.30, bright) * 1.55, depthFade);
}
)GLSL";

const char* kHoneyVertexShader = R"GLSL(
#version 330 core

layout(location = 0) in vec2 aPos;   // fullscreen quad, NDC
void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }
)GLSL";

const char* kHoneyFragmentShader = R"GLSL(
#version 330 core

uniform vec2  uRes;
uniform float uTime;
uniform float uLevel;

out vec4 fragColor;

float hexDist(vec2 p){
    p = abs(p);
    return max(dot(p, vec2(0.5, 0.8660254)), p.x);
}

void main(){
    vec2 px  = gl_FragCoord.xy;
    vec2 ndc = (px - 0.5 * uRes) / uRes.y;
    float vign = smoothstep(1.15, 0.25, length(ndc));

    vec2 uv = px / 34.0 + vec2(uTime * 0.010, uTime * 0.006);

    const vec2 kHex = vec2(1.0, 1.7320508);
    vec2 h  = kHex * 0.5;
    vec2 ga = mod(uv, kHex) - h;
    vec2 gb = mod(uv - h, kHex) - h;
    vec2 gv = dot(ga, ga) < dot(gb, gb) ? ga : gb;
    float line = 1.0 - smoothstep(0.46, 0.5, hexDist(gv));

    vec2 cell = uv - gv;
    float cellHash = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);

    float voidMask = 1.0 - 0.90 * smoothstep(0.55, 0.20, length(ndc));

    float a = line * (0.06 + 0.08 * uLevel) * (0.25 + 0.75 * vign) *
              (0.85 + 0.25 * cellHash) * voidMask;
    fragColor = vec4(vec3(1.0, 0.72, 0.25), a);
}
)GLSL";

const char* kPointFragmentShader = R"GLSL(
#version 330 core

in vec4 vColor;
out vec4 fragColor;

void main(){
    vec2 d = gl_PointCoord - vec2(0.5);
    float r = length(d) * 2.0;
    float alpha = exp(-r * r * 2.2);
    if (alpha < 0.015) discard;
    fragColor = vec4(vColor.rgb, vColor.a * alpha);
}
)GLSL";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char buf[1024];
        glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        fprintf(stderr, "hive-voice: shader compile failed: %s\n", buf);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint linkProgram(const char* vsSrc, const char* fsSrc) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char buf[1024];
        glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        fprintf(stderr, "hive-voice: program link failed: %s\n", buf);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Deterministic PRNG (xorshift32) for per-insect seeds.
uint32_t prngNext(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
float prngFloat(uint32_t& s) {
    return (prngNext(s) >> 8) * (1.0f / 16777216.0f);
}

}  // namespace

// static
std::string SphereArea::glInfo_;

SphereArea::SphereArea() {
    set_required_version(3, 3);
    probeEnabled_ = std::getenv("HIVE_VOICE_GL_PROBE") != nullptr;
}

SphereArea::SphereArea(BaseObjectType* cobject,
                       const Glib::RefPtr<Gtk::Builder>&)
    : Gtk::GLArea(cobject) {
    set_required_version(3, 3);
    probeEnabled_ = std::getenv("HIVE_VOICE_GL_PROBE") != nullptr;
}

SphereArea::~SphereArea() {
    if (tickId_ != 0) remove_tick_callback(tickId_);
}

void SphereArea::setLevel(float level) {
    targetLevel_ = std::clamp(level, 0.0f, 1.0f);
}

void SphereArea::setState(SphereState state) {
    stateTarget_ = static_cast<float>(state);
}

void SphereArea::on_realize() {
    Gtk::GLArea::on_realize();
    start_ = std::chrono::steady_clock::now();
    tickId_ = add_tick_callback(sigc::mem_fun(*this, &SphereArea::tick));
}

void SphereArea::on_unrealize() {
    if (tickId_ != 0) {
        remove_tick_callback(tickId_);
        tickId_ = 0;
    }
    Gtk::GLArea::on_unrealize();
}

void SphereArea::on_resize(int width, int height) {
    Gtk::GLArea::on_resize(width, height);
    viewportW_ = static_cast<float>(width);
    viewportH_ = static_cast<float>(height);
    float aspect = height > 0 ? static_cast<float>(width) /
                                    static_cast<float>(height)
                              : 1.0f;
    perspective(proj_, 0.832f /* ~47.7deg */, aspect, 0.1f, 20.0f);
    // Keep the whole swarm inside the frustum WIDTH on narrow screens
    // (same derivation as the Android nativeResize).
    camDist_ = std::max(3.1f, 1.30f / (0.4414f * aspect));
    pointScale_ = static_cast<float>(width < height ? width : height) *
                  0.024f * (camDist_ / 3.1f);
}

void SphereArea::perspective(float* m, float fovyRad, float aspect, float zn,
                             float zf) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    const float f = 1.0f / std::tan(fovyRad * 0.5f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1.0f;
    m[14] = (2.0f * zf * zn) / (zn - zf);
}

bool SphereArea::initGL() {
    make_current();
    if (glInfo_.empty()) {
        const auto* rend = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const auto* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const auto* vend = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        glInfo_ = std::string(vend ? vend : "?") + " | " +
                  (rend ? rend : "?") + " | " + (ver ? ver : "?");
        fprintf(stderr, "hive-voice: GL: %s\n", glInfo_.c_str());
    }
    swarmProgram_ = linkProgram(kSwarmVertexShader, kPointFragmentShader);
    honeyProgram_ = linkProgram(kHoneyVertexShader, kHoneyFragmentShader);
    if (!swarmProgram_ || !honeyProgram_) return false;

    uProj_ = glGetUniformLocation(swarmProgram_, "uProj");
    uTime_ = glGetUniformLocation(swarmProgram_, "uTime");
    uLevel_ = glGetUniformLocation(swarmProgram_, "uLevel");
    uState_ = glGetUniformLocation(swarmProgram_, "uState");
    uPointScale_ = glGetUniformLocation(swarmProgram_, "uPointScale");
    uCamDist_ = glGetUniformLocation(swarmProgram_, "uCamDist");
    uHoneyRes_ = glGetUniformLocation(honeyProgram_, "uRes");
    uHoneyTime_ = glGetUniformLocation(honeyProgram_, "uTime");
    uHoneyLevel_ = glGetUniformLocation(honeyProgram_, "uLevel");

    buildSeeds();

    // Core profile (what GtkGLArea hands us on desktop Mesa) requires a
    // bound VAO — GLES2 didn't, which is why the Android original worked
    // without one. glGenVertexArrays via epoxy covers both.
    glGenVertexArrays(1, &vao_);
    // ...and in core profile the shader's gl_PointSize is IGNORED unless
    // GL_PROGRAM_POINT_SIZE is enabled — without it every point clamps to
    // 1px and the swarm reads as sparse dim stars. GLES2 applied shader
    // point size implicitly.
    glEnable(GL_PROGRAM_POINT_SIZE);

    const float quad[8] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    glGenBuffers(1, &honeyVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, honeyVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void SphereArea::buildSeeds() {
    std::vector<float> seeds(kPointCount * 8);
    const float ga = 3.14159265358979f * (3.0f - std::sqrt(5.0f));
    uint32_t rng = 0x9e3779b9u;
    for (int i = 0; i < kPointCount; ++i) {
        const float y = 1.0f - (static_cast<float>(i) / (kPointCount - 1)) * 2.0f;
        const float th = ga * static_cast<float>(i);
        const float jy = (prngFloat(rng) - 0.5f) * 0.12f;
        const float jt = (prngFloat(rng) - 0.5f) * 0.12f;
        const float yy = y + jy;
        const float rr = std::sqrt(std::max(0.0f, 1.0f - yy * yy));
        const float tt = th + jt;
        seeds[i * 8 + 0] = std::cos(tt) * rr;
        seeds[i * 8 + 1] = yy;
        seeds[i * 8 + 2] = std::sin(tt) * rr;
        seeds[i * 8 + 3] = prngFloat(rng);                          // speed
        const float shellHash = prngFloat(rng);
        seeds[i * 8 + 4] = 0.50f + 0.55f * std::pow(shellHash, 1.5f);  // shell
        seeds[i * 8 + 5] = prngFloat(rng) * 1000.0f;                // phase
        seeds[i * 8 + 6] = 0.25f + 0.75f * prngFloat(rng);          // dart rate
        seeds[i * 8 + 7] = 0.45f + 0.55f * prngFloat(rng);          // brightness
    }
    glGenBuffers(1, &swarmVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, swarmVbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(seeds.size() * sizeof(float)),
                 seeds.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool SphereArea::tick(const Glib::RefPtr<Gdk::FrameClock>&) {
    queue_render();
    return true;
}

bool SphereArea::on_render(const Glib::RefPtr<Gdk::GLContext>& context) {
    Gtk::GLArea::on_render(context);
    if (!swarmProgram_) {
        if (!initGL()) return false;
    }
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                                 start_).count();
    // Smooth level + state so the sphere eases instead of snapping
    // (SphereView.onDrawFrame); idle breathing when nothing drives it.
    level_ += (targetLevel_ - level_) * 0.25f;
    state_ += (stateTarget_ - state_) * 0.08f;
    if (std::abs(stateTarget_ - state_) < 0.01f) state_ = stateTarget_;
    const float breathing = targetLevel_ < 0.02f
        ? 0.06f + 0.05f * std::sin(t * 1.7f)
        : 0.0f;
    const float lvl = std::clamp(level_ + breathing, 0.0f, 1.0f);

    glViewport(0, 0, static_cast<GLsizei>(viewportW_),
               static_cast<GLsizei>(viewportH_));
    glClearColor(0.02f, 0.03f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBindVertexArray(vao_);

    // 1) Honeycomb backdrop — normal alpha blending, under everything.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(honeyProgram_);
    glUniform2f(uHoneyRes_, viewportW_, viewportH_);
    glUniform1f(uHoneyTime_, t);
    glUniform1f(uHoneyLevel_, lvl);
    glBindBuffer(GL_ARRAY_BUFFER, honeyVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);

    // 2) The swarm — additive glow points.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glUseProgram(swarmProgram_);
    glUniformMatrix4fv(uProj_, 1, GL_FALSE, proj_);
    glUniform1f(uTime_, t);
    glUniform1f(uLevel_, lvl);
    glUniform1f(uState_, state_);
    glUniform1f(uPointScale_, pointScale_);
    glUniform1f(uCamDist_, camDist_);

    glBindBuffer(GL_ARRAY_BUFFER, swarmVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                          nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                          reinterpret_cast<void*>(4 * sizeof(float)));
    glDrawArrays(GL_POINTS, 0, kPointCount);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // Queued readback screenshot — runs inside on_render while the GLArea
    // framebuffer is still bound.
    if (!pendingShot_.empty()) {
        const int w = static_cast<int>(viewportW_),
                  h = static_cast<int>(viewportH_);
        if (w > 0 && h > 0) {
            std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            // glReadPixels is bottom-up — flip rows for the texture.
            std::vector<unsigned char> flip(px.size());
            const size_t stride = static_cast<size_t>(w) * 4;
            for (int y = 0; y < h; ++y)
                std::memcpy(flip.data() + static_cast<size_t>(y) * stride,
                            px.data() + static_cast<size_t>(h - 1 - y) * stride,
                            stride);
            GBytes* bytes = g_bytes_new(flip.data(), flip.size());
            GdkTexture* tex = gdk_memory_texture_new(
                w, h, GDK_MEMORY_R8G8B8A8, bytes, stride);
            gdk_texture_save_to_png(tex, pendingShot_.c_str());
            g_object_unref(tex);
            g_bytes_unref(bytes);
        }
        pendingShot_.clear();
    }

    // Periodic readback probe: the definitive answer to "did the GPU
    // draw anything?" when the display path (HDR/compositor) is suspect.
    if (probeEnabled_ && (renderCount_ % 60) == 0) {
        const int w = static_cast<int>(viewportW_),
                  h = static_cast<int>(viewportH_);
        if (w > 0 && h > 0) {
            std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            float best = 0.0f;
            for (size_t i = 0; i + 2 < px.size(); i += 4) {
                const float l = 0.2126f * px[i] + 0.7152f * px[i + 1] +
                                0.0722f * px[i + 2];
                if (l > best) best = l;
            }
            maxLum_ = best;
        }
        lastGlError_ = glGetError();
    }
    ++renderCount_;

    return true;
}

std::string SphereArea::debugStats() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "renders=%u viewport=%.0fx%.0f max_lum=%.1f/255 "
                  "gl_error=0x%x programs=%u/%u vao=%u level=%.2f state=%.2f",
                  renderCount_, static_cast<double>(viewportW_),
                  static_cast<double>(viewportH_), static_cast<double>(maxLum_),
                  lastGlError_, swarmProgram_, honeyProgram_, vao_,
                  static_cast<double>(level_), static_cast<double>(state_));
    return buf;
}

}  // namespace hv
