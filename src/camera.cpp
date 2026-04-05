#include "camera.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace vfr {

// Default orientation: identity quaternion.
// Standard right-handed convention: forward = -Z, up = +Y, right = +X.
static const glm::quat DEFAULT_ORIENTATION = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

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

void Camera::on_scroll(float delta, float speed) {
    pivot_distance_ = std::max(0.1f, pivot_distance_ - delta * speed * pivot_distance_);
}

void Camera::adjust_pivot_distance(float delta, float speed) {
    // Change pivot distance without moving the camera — shift pivot to compensate.
    glm::vec3 eye = position();
    pivot_distance_ = std::max(0.1f, pivot_distance_ - delta * speed * pivot_distance_);
    pivot_ = eye + forward() * pivot_distance_;
}

void Camera::set_move_direction(const glm::vec3& local_direction) {
    move_direction_ = right() * local_direction.x + forward() * local_direction.y + up() * local_direction.z;
}

void Camera::set_move_direction_world(const glm::vec3& world_direction) {
    move_direction_ = world_direction;
}

void Camera::update(float dt) {
    float max_speed = MAX_VELOCITY * pivot_distance_;

    // Acceleration in move direction
    if (glm::length(move_direction_) > 0.0f) {
        velocity_ += glm::normalize(move_direction_) * ACCELERATION * pivot_distance_ * dt;
    }

    // Friction opposing current velocity
    float speed = glm::length(velocity_);
    float friction_decel = FRICTION * pivot_distance_ * dt;
    if (speed > friction_decel) {
        velocity_ -= glm::normalize(velocity_) * friction_decel;
    } else {
        velocity_ = glm::vec3(0.0f);
    }

    // Clamp to max speed
    speed = glm::length(velocity_);
    if (speed > max_speed) {
        velocity_ *= max_speed / speed;
    }

    pivot_ += velocity_ * dt;
    move_direction_ = glm::vec3(0.0f);
}

void Camera::reset() {
    orientation_ = DEFAULT_ORIENTATION;
    pivot_distance_ = 4.0f;
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
