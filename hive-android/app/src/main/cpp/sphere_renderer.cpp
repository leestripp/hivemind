// HiveMind Voice — Jarvis sphere renderer (OpenGL ES 2.0).
//
// A point-cloud sphere (Fibonacci lattice) whose radius is displaced by
// 3D simplex noise. The displacement amplitude follows the voice level
// (mic RMS while listening, synthetic envelope while the agent speaks),
// so the sphere visibly "moves with the voice signal".
//
// States: 0 idle · 1 listening · 2 thinking · 3 speaking.
// Rendered additively on a near-black background.

#include <GLES2/gl2.h>
#include <jni.h>
#include <android/log.h>
#include <cmath>
#include <cstdint>
#include <vector>

#define LOG_TAG "HiveSphere"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kPointCount = 1600;

const char* kVertexShader = R"GLSL(
precision highp float;

attribute vec3 aPos;

uniform mat4  uProj;
uniform float uTime;
uniform float uLevel;   // 0..1 smoothed voice level
uniform float uState;   // 0 idle, 1 listening, 2 thinking, 3 speaking
uniform float uPointScale;
uniform float uCamDist; // camera distance — widens for portrait screens

varying vec4 vColor;

// -- Ashima simplex noise 3D (MIT) --------------------------------------
vec3 mod289(vec3 x){ return x - floor(x * (1.0/289.0)) * 289.0; }
vec4 mod289(vec4 x){ return x - floor(x * (1.0/289.0)) * 289.0; }
vec4 permute(vec4 x){ return mod289(((x*34.0)+1.0)*x); }
vec4 taylorInvSqrt(vec4 r){ return 1.79284291400159 - 0.85373472095314 * r; }

float snoise(vec3 v){
    const vec2 C = vec2(1.0/6.0, 1.0/3.0);
    const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);

    vec3 i  = floor(v + dot(v, C.yyy));
    vec3 x0 = v - i + dot(i, C.xxx);

    vec3 g = step(x0.yzx, x0.xyz);
    vec3 l = 1.0 - g;
    vec3 i1 = min(g.xyz, l.zxy);
    vec3 i2 = max(g.xyz, l.zxy);

    vec3 x1 = x0 - i1 + C.xxx;
    vec3 x2 = x0 - i2 + C.yyy;
    vec3 x3 = x0 - D.yyy;

    i = mod289(i);
    vec4 p = permute(permute(permute(
              i.z + vec4(0.0, i1.z, i2.z, 1.0))
            + i.y + vec4(0.0, i1.y, i2.y, 1.0))
            + i.x + vec4(0.0, i1.x, i2.x, 1.0));

    float n_ = 0.142857142857;
    vec3 ns = n_ * D.wyz - D.xzx;

    vec4 j = p - 49.0 * floor(p * ns.z * ns.z);

    vec4 x_ = floor(j * ns.z);
    vec4 y_ = floor(j - 7.0 * x_);

    vec4 x = x_ * ns.x + ns.yyyy;
    vec4 y = y_ * ns.x + ns.yyyy;
    vec4 h = 1.0 - abs(x) - abs(y);

    vec4 b0 = vec4(x.xy, y.xy);
    vec4 b1 = vec4(x.zw, y.zw);

    vec4 s0 = floor(b0)*2.0 + 1.0;
    vec4 s1 = floor(b1)*2.0 + 1.0;
    vec4 sh = -step(h, vec4(0.0));

    vec4 a0 = b0.xzyw + s0.xzyw*sh.xxyy;
    vec4 a1 = b1.xzyw + s1.xzyw*sh.zzww;

    vec3 p0 = vec3(a0.xy, h.x);
    vec3 p1 = vec3(a0.zw, h.y);
    vec3 p2 = vec3(a1.xy, h.z);
    vec3 p3 = vec3(a1.zw, h.w);

    vec4 norm = taylorInvSqrt(vec4(dot(p0,p0), dot(p1,p1), dot(p2,p2), dot(p3,p3)));
    p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;

    vec4 m = max(0.6 - vec4(dot(x0,x0), dot(x1,x1), dot(x2,x2), dot(x3,x3)), 0.0);
    m = m * m;
    return 42.0 * dot(m*m, vec4(dot(p0,x0), dot(p1,x1), dot(p2,x2), dot(p3,x3)));
}
// ------------------------------------------------------------------------

void main(){
    float listenAmt = clamp(uState, 0.0, 1.0) * clamp(2.0 - uState, 0.0, 1.0);
    float thinkAmt  = max(0.0, 1.0 - abs(uState - 2.0));
    float speakAmt  = clamp(uState - 2.0, 0.0, 1.0);

    // Rotation: idle drift, listening a touch faster, thinking spins hard.
    float speed = 0.25 + 0.15 * listenAmt + 1.30 * thinkAmt + 0.30 * speakAmt;
    float a = uTime * speed + aPos.y * thinkAmt * 2.0;  // think: vertical shear
    float ca = cos(a), sa = sin(a);
    vec3 p = vec3(aPos.x * ca - aPos.z * sa, aPos.y, aPos.x * sa + aPos.z * ca);

    // Slight fixed tilt for depth.
    float tilt = 0.35;
    float ct = cos(tilt), st = sin(tilt);
    p = vec3(p.x, p.y * ct - p.z * st, p.y * st + p.z * ct);

    // Noise displacement — amplitude follows the voice level.
    float n1 = snoise(p * 2.3 + vec3(0.0, uTime * 0.45, 0.0));
    float n2 = snoise(p * 5.0 - vec3(uTime * 0.8, 0.0, 0.0));
    float amp = 0.045 + uLevel * 0.40 + thinkAmt * 0.10;
    float radius = 1.0 + n1 * amp + n2 * uLevel * 0.18;
    vec3 world = p * radius;

    vec4 view = vec4(world.xy, world.z - uCamDist, 1.0);
    gl_Position = uProj * view;

    float ps = uPointScale * (1.6 + uLevel * 2.6 + n1 * 0.9 + thinkAmt * 0.6);
    gl_PointSize = clamp(ps / max(0.6, -view.z), 1.0, 64.0);

    // Per-state palette.
    vec3 cIdle   = vec3(0.14, 0.46, 0.58);
    vec3 cListen = vec3(0.20, 0.85, 0.95);
    vec3 cThink  = vec3(0.95, 0.70, 0.25);
    vec3 cSpeak  = vec3(0.25, 0.95, 0.60);
    vec3 col = cIdle;
    col = mix(col, cListen, listenAmt);
    col = mix(col, cThink,  thinkAmt);
    col = mix(col, cSpeak,  speakAmt);

    float bright = 0.55 + 0.45 * n1 + uLevel * 0.9;
    vColor = vec4(col * max(0.30, bright), 1.0);
}
)GLSL";

const char* kFragmentShader = R"GLSL(
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
    GLuint program = 0;
    GLuint vbo = 0;
    GLint aPos = -1;
    GLint uProj = -1, uTime = -1, uLevel = -1, uState = -1, uPointScale = -1;
    GLint uCamDist = -1;
    float proj[16] = {0};
    float pointScale = 8.0f;
    float camDist = 3.1f;
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

void perspective(float* m, float fovyRad, float aspect, float zn, float zf) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1.0f;
    m[14] = (2.0f * zf * zn) / (zn - zf);
}

}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_com_hivemind_voice_SphereNative_nativeInit(JNIEnv*, jclass) {
    if (g.ready) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) { LOGE("shader build failed"); return; }

    g.program = glCreateProgram();
    glAttachShader(g.program, vs);
    glAttachShader(g.program, fs);
    glLinkProgram(g.program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(g.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char buf[1024];
        glGetProgramInfoLog(g.program, sizeof(buf), nullptr, buf);
        LOGE("program link failed: %s", buf);
        glDeleteProgram(g.program);
        g.program = 0;
        return;
    }

    g.aPos        = glGetAttribLocation(g.program, "aPos");
    g.uProj       = glGetUniformLocation(g.program, "uProj");
    g.uTime       = glGetUniformLocation(g.program, "uTime");
    g.uLevel      = glGetUniformLocation(g.program, "uLevel");
    g.uState      = glGetUniformLocation(g.program, "uState");
    g.uPointScale = glGetUniformLocation(g.program, "uPointScale");
    g.uCamDist    = glGetUniformLocation(g.program, "uCamDist");

    // Fibonacci lattice — even point distribution over the sphere.
    std::vector<float> pts(kPointCount * 3);
    const float ga = 3.14159265358979f * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < kPointCount; ++i) {
        float y = 1.0f - (static_cast<float>(i) / (kPointCount - 1)) * 2.0f;
        float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        float t = ga * static_cast<float>(i);
        pts[i * 3 + 0] = std::cos(t) * r;
        pts[i * 3 + 1] = y;
        pts[i * 3 + 2] = std::sin(t) * r;
    }
    glGenBuffers(1, &g.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(pts.size() * sizeof(float)),
                 pts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    g.ready = true;
}

JNIEXPORT void JNICALL
Java_com_hivemind_voice_SphereNative_nativeResize(JNIEnv*, jclass,
                                                  jint w, jint h) {
    glViewport(0, 0, w, h);
    float aspect = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
    perspective(g.proj, 0.832f /* ~47.7deg */, aspect, 0.1f, 20.0f);
    // Visible half-extent of the sphere must stay < ~75% of the smaller
    // frustum half-extent at the camera plane. aspect >= 0.75 is fine at
    // the default distance; narrower pulls the camera back.
    g.camDist = 3.1f / std::min(1.0f, aspect / 0.75f);
    g.pointScale = static_cast<float>(w < h ? w : h) * 0.011f *
                   (g.camDist / 3.1f);
}

JNIEXPORT void JNICALL
Java_com_hivemind_voice_SphereNative_nativeRender(JNIEnv*, jclass,
                                                  jfloat timeSec, jfloat level,
                                                  jfloat state) {
    if (!g.ready) return;

    glClearColor(0.02f, 0.03f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    glUseProgram(g.program);
    glUniformMatrix4fv(g.uProj, 1, GL_FALSE, g.proj);
    glUniform1f(g.uTime, timeSec);
    glUniform1f(g.uLevel, level < 0.f ? 0.f : (level > 1.f ? 1.f : level));
    glUniform1f(g.uState, state);
    glUniform1f(g.uPointScale, g.pointScale);
    glUniform1f(g.uCamDist, g.camDist);

    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glEnableVertexAttribArray(static_cast<GLuint>(g.aPos));
    glVertexAttribPointer(static_cast<GLuint>(g.aPos), 3, GL_FLOAT, GL_FALSE,
                          0, nullptr);
    glDrawArrays(GL_POINTS, 0, kPointCount);
    glDisableVertexAttribArray(static_cast<GLuint>(g.aPos));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

JNIEXPORT void JNICALL
Java_com_hivemind_voice_SphereNative_nativeDestroy(JNIEnv*, jclass) {
    if (g.vbo) { glDeleteBuffers(1, &g.vbo); g.vbo = 0; }
    if (g.program) { glDeleteProgram(g.program); g.program = 0; }
    g.ready = false;
}

}  // extern "C"
