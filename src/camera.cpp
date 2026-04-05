#include "camera.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace vfr {

// Default orientation: camera looks along world +Y with Z up.
// Camera convention: forward = -Z, up = +Y, right = +X.
// So we need: camera -Z → world +Y, camera +Y → world +Z, camera +X → world +X.
// This is a 90° rotation around X.
static const glm::quat DEFAULT_ORIENTATION =
    glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));

void Camera::on_mouse_move(float dx, float dy, bool free_look, bool orbiting, bool panning) {
    if (free_look) {
        glm::vec3 eye = position();

        // Both axes rotate around camera-local directions — no gimbal lock
        glm::quat yaw_rot = glm::angleAxis(-dx * ROTATE_SPEED, up());
        glm::quat pitch_rot = glm::angleAxis(-dy * ROTATE_SPEED, right());
        orientation_ = glm::normalize(yaw_rot * pitch_rot * orientation_);

        // Recompute pivot so eye position is preserved
        pivot_ = eye + forward() * pivot_distance_;
    }
    if (orbiting) {
        glm::quat yaw_rot = glm::angleAxis(-dx * ROTATE_SPEED, up());
        glm::quat pitch_rot = glm::angleAxis(-dy * ROTATE_SPEED, right());
        orientation_ = glm::normalize(yaw_rot * pitch_rot * orientation_);
    }
    if (panning) {
        float scale = pivot_distance_ * PAN_SPEED;
        pivot_ -= right() * dx * scale;
        pivot_ += up() * dy * scale;
    }
}

void Camera::on_scroll(float delta) {
    pivot_distance_ = std::max(0.1f, pivot_distance_ - delta * ZOOM_SPEED * pivot_distance_);
}

void Camera::on_key_move(const glm::vec3& direction, float dt) {
    glm::vec3 world_dir = right() * direction.x + forward() * direction.y + up() * direction.z;
    velocity_ += world_dir * ACCELERATION * dt * pivot_distance_;
}

void Camera::update(float dt) {
    float speed = glm::length(velocity_);
    if (speed > MAX_VELOCITY * pivot_distance_) {
        velocity_ *= (MAX_VELOCITY * pivot_distance_) / speed;
    }

    pivot_ += velocity_ * dt;
    velocity_ *= std::max(0.0f, 1.0f - FRICTION * dt);
}

void Camera::reset() {
    orientation_ = DEFAULT_ORIENTATION;
    pivot_distance_ = 3.0f;
    pivot_ = glm::vec3(0.0f);
    velocity_ = glm::vec3(0.0f);
}

glm::vec3 Camera::forward() const {
    return glm::normalize(orientation_ * glm::vec3(0.0f, 0.0f, -1.0f));
}

glm::vec3 Camera::right() const {
    return glm::normalize(orientation_ * glm::vec3(1.0f, 0.0f, 0.0f));
}

glm::vec3 Camera::up() const {
    return glm::normalize(orientation_ * glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::vec3 Camera::position() const {
    return pivot_ - forward() * pivot_distance_;
}

glm::mat4 Camera::view_matrix() const {
    glm::vec3 eye = position();
    // Build view matrix directly from quaternion — no lookAt, no gimbal lock.
    // view = R_inv * T(-eye), where R = mat3(orientation_)
    return glm::translate(glm::mat4_cast(glm::conjugate(orientation_)), -eye);
}

} // namespace vfr
