#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace vfr {

class Camera {
public:
    void on_mouse_move(float dx, float dy, bool free_look, bool orbiting, bool panning);
    void on_scroll(float delta, float speed);
    void adjust_pivot_distance(float delta, float speed);
    void set_move_direction(const glm::vec3& local_direction);
    void set_move_direction_world(const glm::vec3& world_direction);
    void update(float dt);
    void reset();

    glm::mat4 view_matrix() const;
    glm::vec3 position() const;
    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::vec3 up() const;

    float pivot_distance() const { return pivot_distance_; }

    static constexpr float ZOOM_MOUSE_SPEED = 1.0f / 128.0f;  // left+right mouse zoom speed
    static constexpr float ZOOM_SCROLL_SPEED = 1.0f / 32.0f;  // scroll wheel zoom speed

private:
    // Default: identity. Standard right-handed: forward = -Z, up = +Y, right = +X.
    glm::quat orientation_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    float pivot_distance_ = 4.0f;
    glm::vec3 pivot_{0.0f};

    glm::vec3 velocity_{0.0f};
    glm::vec3 move_direction_{0.0f};  // world-space, normalized or zero

    static constexpr float ROTATE_SPEED = 1.0f / 1024.0f;  // radians per pixel
    static constexpr float PAN_SPEED = 1.0f / 2048.0f;
    static constexpr float MAX_VELOCITY = 1.0f;
    static constexpr float ACCELERATION = 16.0f;
    static constexpr float FRICTION = 8.0f;
};

} // namespace vfr
