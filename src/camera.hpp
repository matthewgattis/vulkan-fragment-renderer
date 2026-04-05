#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace vfr {

class Camera {
public:
    void on_mouse_move(float dx, float dy, bool free_look, bool orbiting, bool panning);
    void on_scroll(float delta);
    void on_key_move(const glm::vec3& direction, float dt);
    void update(float dt);
    void reset();

    glm::mat4 view_matrix() const;
    glm::vec3 position() const;
    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::vec3 up() const;

    float pivot_distance() const { return pivot_distance_; }

private:
    // Default: identity. Standard right-handed: forward = -Z, up = +Y, right = +X.
    glm::quat orientation_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    float pivot_distance_ = 3.0f;
    glm::vec3 pivot_{0.0f};

    glm::vec3 velocity_{0.0f};

    static constexpr float ROTATE_SPEED = 0.003f;  // radians per pixel
    static constexpr float PAN_SPEED = 1.0f / 1024.0f;
    static constexpr float ZOOM_SPEED = 1.0f / 8.0f;
    static constexpr float MAX_VELOCITY = 1.0f;
    static constexpr float ACCELERATION = 16.0f;
    static constexpr float FRICTION = 8.0f;
};

} // namespace vfr
