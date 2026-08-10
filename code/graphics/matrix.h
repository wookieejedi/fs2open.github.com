#pragma once

#include "graphics/2d.h"
#include "render/3d.h"

extern transform_stack gr_model_matrix_stack;
extern matrix4 gr_view_matrix;
extern matrix4 gr_model_view_matrix;
extern matrix4 gr_projection_matrix;
extern matrix4 gr_last_projection_matrix;
extern matrix4 gr_env_texture_matrix;
extern float gr_near_plane;

void gr_matrix_on_frame();

void gr_start_instance_matrix(const vec3d* offset, const matrix* rotation);
void gr_start_angles_instance_matrix(const vec3d* pos, const angles* rotation);
void gr_end_instance_matrix();

void gr_set_proj_matrix(fov_t fov, float aspect, float z_near, float z_far);
void gr_end_proj_matrix();

// Orthographic projection override... when active gr_set_proj_matrix substitutes an
// orthographic frustum sized to match the perspective view at the given camera distance.
void gr_activate_ortho_proj_override(float camera_distance);
void gr_deactivate_ortho_proj_override();

void gr_set_view_matrix(const vec3d* pos, const matrix* orient);
void gr_end_view_matrix();

void gr_set_2d_matrix(/*int x, int y, int w, int h*/);
void gr_end_2d_matrix();

void gr_push_scale_matrix(const vec3d *scale_factor);
void gr_pop_scale_matrix();

/**
 * @brief Rotates all subsequent screen-space drawing around a pivot point
 *
 * @details The rotation is folded into the model-view matrix by gr_matrix_set_uniforms(), so it covers everything
 * that draws through a shader taking that matrix -- which is all of the regular 2D drawing.  It deliberately does
 * *not* use the model matrix stack, because callers routinely render 3D content in between, which clears the stack.
 * For the same reason the rotation suspends itself for the duration of any 3D rendering, since a rotation expressed
 * in screen pixels is meaningless once a perspective projection is in play.
 *
 * The NanoVG paths do not use the model-view matrix at all and apply the rotation themselves via
 * gr_get_2d_rotation().
 *
 * Note that this does not affect clipping; the clip rectangle stays axis-aligned and still applies to the
 * *unrotated* coordinates.  Backends widen the scissor rectangle to the bounding box of the rotated clip region so
 * that rotated geometry is not cut off by it, but the CPU-side clipping in the 2D draw functions is left alone so
 * that gauges keep clipping their contents the way they intend to.
 *
 * Rotations cannot be nested.
 *
 * @param pivot_x The X coordinate of the pivot, in real (already resized) screen pixels
 * @param pivot_y The Y coordinate of the pivot, in real (already resized) screen pixels
 * @param angle The rotation angle in radians, positive being clockwise on screen
 */
void gr_push_2d_rotation(float pivot_x, float pivot_y, float angle);

/**
 * @brief Ends a rotation previously started with gr_push_2d_rotation()
 */
void gr_pop_2d_rotation();

/**
 * @brief Retrieves the currently active screen-space rotation
 *
 * @returns false (leaving the outputs untouched) if no rotation is active
 */
bool gr_get_2d_rotation(float* pivot_x, float* pivot_y, float* angle);

/**
 * @brief Grows a screen-space rectangle to the bounding box of its rotated self
 *
 * @details Used by the backends so that the scissor rectangle does not cut off geometry that the active screen-space
 * rotation has moved outside of the original, axis-aligned rectangle.  The result is clamped to the given bounds.
 *
 * @returns false (leaving the rectangle untouched) if no rotation is active
 */
bool gr_expand_rect_for_2d_rotation(int* x, int* y, int* w, int* h, int max_w, int max_h);

void gr_setup_viewport();

void gr_reset_matrices();

extern matrix4 gr_texture_matrix;

void gr_set_texture_panning(float u, float v, bool enable);

/**
 * @brief Set current matrix uniforms
 *
 * Use this before rendering with a shader requiring the matrix uniforms so that the matrix uniform block point has the
 * up-to-date data.
 */
void gr_matrix_set_uniforms();
