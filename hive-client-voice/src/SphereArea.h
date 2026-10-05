#pragma once

#include <gtkmm/glarea.h>
#include <gtkmm/builder.h>

#include <chrono>

namespace hv {

// Visual states — mirror the shader's uState values (SphereState.kt).
enum class SphereState { Idle = 0, Listening, Thinking, Speaking, Processing };

// Jarvis sphere (Gtk::GLArea edition): a swarm of "insects" holding a
// sphere shape, plus the honeycomb backdrop — the GLSL 330 core port of
// the Android GLES2 sphere_renderer.cpp. All animation state lives in
// the vertex shader from a static seed VBO; the CPU only feeds time,
// level and state. Continuously redrawn from the frame clock.
class SphereArea : public Gtk::GLArea {
 public:
    SphereArea();
    SphereArea(BaseObjectType* cobject, const Glib::RefPtr<Gtk::Builder>&);
    ~SphereArea() override;

    void setLevel(float level);        // 0..1 instantaneous target
    void setState(SphereState state);  // eased toward in the render loop

 protected:
    void on_realize() override;
    void on_unrealize() override;
    bool on_render(const Glib::RefPtr<Gdk::GLContext>& context) override;
    void on_resize(int width, int height) override;

 private:
    bool tick(const Glib::RefPtr<Gdk::FrameClock>& clock);
    bool initGL();
    void buildSeeds();
    void perspective(float* m, float fovyRad, float aspect, float zn,
                     float zf);

    // Render state (GL names only valid between realize/unrealize).
    unsigned int swarmProgram_ = 0, honeyProgram_ = 0;
    unsigned int swarmVbo_ = 0, honeyVbo_ = 0;
    int aSeed0_ = -1, aSeed1_ = -1;
    int uProj_ = -1, uTime_ = -1, uLevel_ = -1, uState_ = -1;
    int uPointScale_ = -1, uCamDist_ = -1;
    int aHoneyPos_ = -1, uHoneyRes_ = -1, uHoneyTime_ = -1, uHoneyLevel_ = -1;

    float proj_[16] = {};
    float pointScale_ = 8.0f, camDist_ = 3.1f;
    float viewportW_ = 1.0f, viewportH_ = 1.0f;

    // Animation state (UI thread only).
    float targetLevel_ = 0.0f, level_ = 0.0f;
    float state_ = 0.0f, stateTarget_ = 0.0f;
    std::chrono::steady_clock::time_point start_;
    unsigned int tickId_ = 0;
};

}  // namespace hv
