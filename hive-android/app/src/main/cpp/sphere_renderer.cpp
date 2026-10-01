// HiveMind Voice — Jarvis sphere renderer (OpenGL ES 2.0).
//
// A swarm of "insects" holding a sphere shape — the Hive Mind look.
//
// Behaviour model (grounded in swarm research, not bird flocking):
//   - Attanasi et al. 2014 (PLOS Comput Biol): insect swarms have NO
//     collective order — unlike boids, insects never align. Each flies
//     its own erratic path; only cohesion keeps the swarm together.
//   - Individual flight = tangled quasi-periodic loops (incommensurate
//     sine sums per axis) + sharp "saccade" darts (fruit-fly turn-and-go).
//   - The swarm centre itself wanders (Sci Rep 2016, srep30515).
// Every insect is O(1) in the vertex shader from a static seed VBO —
// no CPU simulation, no neighbour search, ~zero battery cost.
//
// Voice level drives *agitation*: wander amplitude, dart frequency and
// radial bulge all rise with the signal, so the swarm moves with the voice.
//
// Background: a faint procedural honeycomb hex grid (fullscreen quad,
// drawn under the additive points) — amber, vignetted, slow drift.
//
// States: 0 idle · 1 listening · 2 thinking · 3 speaking · 4 processing.

#include <GLES2/gl2.h>
#include <jni.h>
#include <android/log.h>
#include <cmath>
#include <cstdint>
#include <vector>

#define LOG_TAG "HiveSphere"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kPointCount = 2600;

// ---------------------------------------------------------------- swarm --
const char* kSwarmVertexShader = R"GLSL(
precision highp float;

attribute vec4 aSeed0;  // xyz: base direction on unit sphere · w: speed factor
attribute vec4 aSeed1;  // x: shell radius · y: phase · z: dart rate · w: brightness

uniform mat4  uProj;
uniform float uTime;
uniform float uLevel;   // 0..1 smoothed voice level
uniform float uState;   // 0 idle, 1 listening, 2 thinking, 3 speaking, 4 processing
uniform float uPointScale;
uniform float uCamDist; // camera distance — widens for portrait screens

varying vec4 vColor;

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
    float speed   = 0.55 + aSeed0.w * 1.45;          // per-insect pace
    float t       = uTime * speed + ph * 113.0;      // per-insect clock

    // Agitation 0..~1.5: voice + state energy. Drives everything below.
    float agit = clamp(uLevel + thinkAmt * 0.55 + procAmt * 0.45 * pulse, 0.0, 1.5);

    // Wandering swarm core — the whole ball of insects drifts (srep30515).
    vec3 core = 0.07 * vec3(sin(uTime * 0.31 + 1.7),
                            sin(uTime * 0.23),
                            sin(uTime * 0.19 + 4.2));

    // Loopy quasi-periodic wander: three incommensurate sines per axis,
    // per-insect frequencies — each insect traces its own tangled loop.
    // Tangential bias keeps the sphere silhouette readable.
    float wanderAmp = 0.08 + 0.10 * agit;
    float f1 = 1.9 + 1.3 * hash1(ph * 3.1 + 0.7);
    float f2 = 2.6 + 1.5 * hash1(ph * 5.3 + 2.9);
    float f3 = 3.4 + 1.7 * hash1(ph * 7.7 + 5.1);
    vec3 wander = vec3(sin(t * f1 + ph * 6.1),
                       sin(t * f2 + ph * 9.4),
                       sin(t * f3 + ph * 12.7));
    wander -= baseDir * dot(wander, baseDir) * 0.6;

    // Saccade darts: sharp turn-and-go events, occasional per time cell.
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

    // Radial: shell position + wingbeat flutter + voice bulge.
    float wingBeat = 7.0 + aSeed0.w * 13.0;
    float flutter = 0.022 * sin(t * wingBeat) +
                    0.012 * sin(t * wingBeat * 0.37 + ph);
    float bulge = 1.0 + 0.10 * uLevel + 0.05 * thinkAmt + 0.06 * procAmt * pulse;
    float radius = (aSeed1.x + flutter) * bulge;

    vec3 world = baseDir * radius + wander * wanderAmp + dart + core;

    // Rotation: idle drift, listening a touch faster, thinking spins hard,
    // processing a steady working spin. Shear while thinking/processing.
    float rotSpeed = 0.25 + 0.15 * listenAmt + 1.30 * thinkAmt
                   + 0.30 * speakAmt + 0.55 * procAmt;
    float a = uTime * rotSpeed + baseDir.y * (thinkAmt + 0.4 * procAmt) * 2.0;
    float ca = cos(a), sa = sin(a);
    world = vec3(world.x * ca - world.z * sa, world.y,
                 world.x * sa + world.z * ca);

    // Slight fixed tilt for depth.
    float tilt = 0.35;
    float ct = cos(tilt), st = sin(tilt);
    world = vec3(world.x, world.y * ct - world.z * st, world.y * st + world.z * ct);

    vec4 view = vec4(world.xy, world.z - uCamDist, 1.0);
    gl_Position = uProj * view;

    // Per-insect varied size + wingbeat shimmer; agitation inflates.
    float sizeVar = 0.7 + 0.9 * hash1(ph * 13.7 + 0.3);
    float flicker = 0.75 + 0.25 * sin(t * wingBeat);
    float ps = uPointScale * sizeVar * (1.1 + 1.0 * agit) * flicker;
    gl_PointSize = clamp(ps / max(0.6, -view.z), 1.0, 48.0);

    // Per-state palette.
    vec3 cIdle   = vec3(0.14, 0.46, 0.58);
    vec3 cListen = vec3(0.20, 0.85, 0.95);
    vec3 cThink  = vec3(0.95, 0.70, 0.25);
    vec3 cSpeak  = vec3(0.25, 0.95, 0.60);
    vec3 cProc   = vec3(0.62, 0.22, 0.95);   // violet = STT decode
    vec3 col = cIdle;
    col = mix(col, cListen, listenAmt);
    col = mix(col, cThink,  thinkAmt);
    col = mix(col, cSpeak,  speakAmt);
    col = mix(col, cProc,   procAmt);

    // Amber "bee" flecks tie the swarm to the honeycomb backdrop.
    float bee = step(0.88, hash1(ph * 29.1 + 3.3));
    col = mix(col, vec3(1.0, 0.72, 0.25), bee * 0.55);

    float bright = aSeed1.w * (0.85 + 0.75 * agit);
    // Processing owns brightness outright: luminous pulsing violet,
    // guaranteed readable at zero mic level, never clamping to white.
    bright = mix(bright, 0.75 + 0.25 * dartOn + 0.30 * pulse, procAmt);

    // Far side of the swarm dims — reads as a 3D ball, not a disc.
    // Window tracks the camera: the swarm spans ±1.3 around z = -uCamDist.
    float depthFade = 0.40 + 0.60 *
        smoothstep(-(uCamDist + 1.3), -(uCamDist - 1.3), view.z);
    vColor = vec4(col * max(0.30, bright), depthFade);
}
)GLSL";

// -------------------------------------------------------------- backdrop --
const char* kHoneyVertexShader = R"GLSL(
precision highp float;
attribute vec2 aPos;   // fullscreen quad, NDC
void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }
)GLSL";

const char* kHoneyFragmentShader = R"GLSL(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

uniform vec2  uRes;
uniform float uTime;
uniform float uLevel;

// Distance to the border of a pointy-top hexagon centred at the origin.
float hexDist(vec2 p){
    p = abs(p);
    return max(dot(p, vec2(0.5, 0.8660254)), p.x);
}

void main(){
    vec2 px  = gl_FragCoord.xy;
    vec2 ndc = (px - 0.5 * uRes) / uRes.y;
    // Faint at the edges, a touch stronger behind the swarm.
    float vign = smoothstep(1.15, 0.25, length(ndc));

    // Slow drift so the hive feels alive.
    vec2 uv = px / 34.0 + vec2(uTime * 0.010, uTime * 0.006);

    const vec2 kHex = vec2(1.0, 1.7320508);   // flat-top hex tiling
    vec2 h  = kHex * 0.5;
    vec2 ga = mod(uv, kHex) - h;
    vec2 gb = mod(uv - h, kHex) - h;
    vec2 gv = dot(ga, ga) < dot(gb, gb) ? ga : gb;
    float line = 1.0 - smoothstep(0.46, 0.5, hexDist(gv));

    // Gentle per-cell brightness variation so the hive feels organic.
    vec2 cell = uv - gv;
    float cellHash = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);

    // Dark void behind the swarm — a large blurred black disc so the dim
    // idle insects read against near-black instead of fighting the grid.
    // (The sphere projects to NDC 0, the centre of this quad.)
    float voidMask = 1.0 - 0.90 * smoothstep(0.55, 0.20, length(ndc));

    // Honey amber, faint; breathes with the voice. Straight alpha — an
    // earlier premultiplied form squared the effect to invisibility.
    float a = line * (0.05 + 0.07 * uLevel) * (0.25 + 0.75 * vign) *
              (0.85 + 0.25 * cellHash) * voidMask;
    gl_FragColor = vec4(vec3(1.0, 0.72, 0.25), a);
}
)GLSL";

// ------------------------------------------------------------ soft point --
const char* kPointFragmentShader = R"GLSL(
precision mediump float;
varying vec4 vColor;
void main(){
    vec2 d = gl_PointCoord - vec2(0.5);
    float r = length(d) * 2.0;
    float alpha = exp(-r * r * 4.0);
    if (alpha < 0.015) discard;
    gl_FragColor = vec4(vColor.rgb, vColor.a * alpha);
}
)GLSL";

struct Renderer {
    // swarm program
    GLuint swarmProgram = 0;
    GLuint swarmVbo = 0;
    GLint aSeed0 = -1, aSeed1 = -1;
    GLint uProj = -1, uTime = -1, uLevel = -1, uState = -1;
    GLint uPointScale = -1, uCamDist = -1;
    // honeycomb backdrop program
    GLuint honeyProgram = 0;
    GLuint honeyVbo = 0;
    GLint aHoneyPos = -1;
    GLint uHoneyRes = -1, uHoneyTime = -1, uHoneyLevel = -1;

    float proj[16] = {0};
    float pointScale = 8.0f;
    float camDist = 3.1f;
    float viewportW = 1.0f, viewportH = 1.0f;
    bool ready = false;
};

Renderer g;

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char buf[1024];
        glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        LOGE("shader compile failed: %s", buf);
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
        LOGE("program link failed: %s", buf);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

void perspective(float* m, float fovyRad, float aspect, float zn, float zf) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1.0f;
    m[14] = (2.0f * zf * zn) / (zn - zf);
}

// Deterministic PRNG (xorshift32) for per-insect seeds.
uint32_t prngNext(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
float prngFloat(uint32_t& s) {  // 0..1
    return (prngNext(s) >> 8) * (1.0f / 16777216.0f);
}

}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_com_hivemind_voice_SphereNative_nativeInit(JNIEnv*, jclass) {
    // onSurfaceCreated means a NEW GL context (first run, activity
    // re-entered after back, or context loss). The previous context's
    // programs/VBOs died with it — always rebuild for THIS context.
    // (Do not glDelete* the stale handles; they belong to a dead
    // context. g.ready must NOT gate this: a second VoiceActivity got
    // an early-return here and drew with dead handles -> black sphere.)
    g = Renderer{};

    g.swarmProgram = linkProgram(kSwarmVertexShader, kPointFragmentShader);
    g.honeyProgram = linkProgram(kHoneyVertexShader, kHoneyFragmentShader);
    if (!g.swarmProgram || !g.honeyProgram) {
        LOGE("shader build failed");
        return;
    }

    g.aSeed0       = glGetAttribLocation(g.swarmProgram, "aSeed0");
    g.aSeed1       = glGetAttribLocation(g.swarmProgram, "aSeed1");
    g.uProj        = glGetUniformLocation(g.swarmProgram, "uProj");
    g.uTime        = glGetUniformLocation(g.swarmProgram, "uTime");
    g.uLevel       = glGetUniformLocation(g.swarmProgram, "uLevel");
    g.uState       = glGetUniformLocation(g.swarmProgram, "uState");
    g.uPointScale  = glGetUniformLocation(g.swarmProgram, "uPointScale");
    g.uCamDist     = glGetUniformLocation(g.swarmProgram, "uCamDist");
    g.aHoneyPos    = glGetAttribLocation(g.honeyProgram, "aPos");
    g.uHoneyRes    = glGetUniformLocation(g.honeyProgram, "uRes");
    g.uHoneyTime   = glGetUniformLocation(g.honeyProgram, "uTime");
    g.uHoneyLevel  = glGetUniformLocation(g.honeyProgram, "uLevel");

    // Per-insect seeds: base directions from a Fibonacci lattice (even
    // sphere coverage -> the swarm always reads as a ball) plus random
    // speed / shell depth / phase / dart rate / brightness.
    uint32_t rng = 0x9e3779b9u;
    std::vector<float> seeds(kPointCount * 8);
    const float ga = 3.14159265358979f * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < kPointCount; ++i) {
        float y = 1.0f - (static_cast<float>(i) / (kPointCount - 1)) * 2.0f;
        float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        float th = ga * static_cast<float>(i);
        // small jitter so insects aren't pinned to the lattice
        float jy = (prngFloat(rng) - 0.5f) * 0.12f;
        float jt = (prngFloat(rng) - 0.5f) * 0.12f;
        float yy = y + jy;
        float rr = std::sqrt(std::max(0.0f, 1.0f - yy * yy));
        float tt = th + jt;
        seeds[i * 8 + 0] = std::cos(tt) * rr;
        seeds[i * 8 + 1] = yy;
        seeds[i * 8 + 2] = std::sin(tt) * rr;
        seeds[i * 8 + 3] = prngFloat(rng);                          // speed
        float shellHash = prngFloat(rng);
        seeds[i * 8 + 4] = 0.50f + 0.55f * std::pow(shellHash, 1.5f); // shell
        seeds[i * 8 + 5] = prngFloat(rng) * 1000.0f;                // phase
        seeds[i * 8 + 6] = 0.25f + 0.75f * prngFloat(rng);          // dart rate
        seeds[i * 8 + 7] = 0.45f + 0.55f * prngFloat(rng);          // brightness
    }
    glGenBuffers(1, &g.swarmVbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.swarmVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(seeds.size() * sizeof(float)),
                 seeds.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Fullscreen quad for the honeycomb backdrop (NDC triangle strip).
    const float quad[8] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    glGenBuffers(1, &g.honeyVbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.honeyVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    g.ready = true;
}

JNIEXPORT void JNICALL
Java_com_hivemind_voice_SphereNative_nativeResize(JNIEnv*, jclass,
                                                  jint w, jint h) {
    glViewport(0, 0, w, h);
    g.viewportW = static_cast<float>(w);
    g.viewportH = static_cast<float>(h);
    float aspect = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
    perspective(g.proj, 0.832f /* ~47.7deg */, aspect, 0.1f, 20.0f);
    // Keep the whole swarm (max radius ~1.3 world units, see the vertex
    // shader) inside the frustum *width* on narrow portrait screens:
    // half-width at the swarm = tan(fovy/2) * camDist * aspect, so below
    // ~aspect 1.0 the old 3.1 baseline underscans and the swarm spills
    // past the screen edges (2026-10-02: dots edge-to-edge, no ball).
    g.camDist = std::max(3.1f, 1.30f / (0.4414f * aspect));
    g.pointScale = static_cast<float>(w < h ? w : h) * 0.0125f *
                   (g.camDist / 3.1f);
}

JNIEXPORT void JNICALL
Java_com_hivemind_voice_SphereNative_nativeRender(JNIEnv*, jclass,
                                                  jfloat timeSec, jfloat level,
                                                  jfloat state) {
    if (!g.ready) return;
    float lvl = level < 0.f ? 0.f : (level > 1.f ? 1.f : level);

    glClearColor(0.02f, 0.03f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);

    // 1) Honeycomb backdrop — normal alpha blending, under everything.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g.honeyProgram);
    glUniform2f(g.uHoneyRes, g.viewportW, g.viewportH);
    glUniform1f(g.uHoneyTime, timeSec);
    glUniform1f(g.uHoneyLevel, lvl);
    glBindBuffer(GL_ARRAY_BUFFER, g.honeyVbo);
    glEnableVertexAttribArray(static_cast<GLuint>(g.aHoneyPos));
    glVertexAttribPointer(static_cast<GLuint>(g.aHoneyPos), 2, GL_FLOAT,
                          GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(static_cast<GLuint>(g.aHoneyPos));

    // 2) The swarm — additive glow points.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glUseProgram(g.swarmProgram);
    glUniformMatrix4fv(g.uProj, 1, GL_FALSE, g.proj);
    glUniform1f(g.uTime, timeSec);
    glUniform1f(g.uLevel, lvl);
    glUniform1f(g.uState, state);
    glUniform1f(g.uPointScale, g.pointScale);
    glUniform1f(g.uCamDist, g.camDist);

    glBindBuffer(GL_ARRAY_BUFFER, g.swarmVbo);
    glEnableVertexAttribArray(static_cast<GLuint>(g.aSeed0));
    glVertexAttribPointer(static_cast<GLuint>(g.aSeed0), 4, GL_FLOAT,
                          GL_FALSE, 8 * sizeof(float), nullptr);
    glEnableVertexAttribArray(static_cast<GLuint>(g.aSeed1));
    glVertexAttribPointer(static_cast<GLuint>(g.aSeed1), 4, GL_FLOAT,
                          GL_FALSE, 8 * sizeof(float),
                          reinterpret_cast<void*>(4 * sizeof(float)));
    glDrawArrays(GL_POINTS, 0, kPointCount);
    glDisableVertexAttribArray(static_cast<GLuint>(g.aSeed0));
    glDisableVertexAttribArray(static_cast<GLuint>(g.aSeed1));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // extern "C"
