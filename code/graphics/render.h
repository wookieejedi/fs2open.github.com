#pragma once

#include "globalincs/pstypes.h"
#include "graphics/2d.h"

namespace graphics {
class quad_draw_list;
}

/**
 * @brief Redirects 2D bitmap and string drawing into a batch list until gr_end_quad_batch() is called
 *
 * While a batch is active, gr_bitmap()/gr_aabitmap()/gr_string() and their variants queue quads into @p draw_list
 * instead of issuing a draw call each. Bitmaps land in the list's lower layer and text in its upper one, so text
 * still ends up over a bitmap queued alongside it even though the two are drawn in separate passes. Quads that
 * overlap but use different textures within the same layer may swap z-order relative to unbatched rendering.
 *
 * State the list does not capture -- the clip rectangle, the 2D matrix, entering a 3D frame -- flushes it
 * automatically before it changes, as does any primitive that cannot be batched, so the batch never draws under the
 * wrong state and un-batched primitives keep their z-order. Batches cannot be nested.
 *
 * @param draw_list The list to queue quads into
 */
void gr_begin_quad_batch(graphics::quad_draw_list* draw_list);

/**
 * @brief Draws everything queued into the active batch list so far
 *
 * Batching stays active afterwards. The engine already calls this wherever state the batch depends on is about to
 * change, so callers rarely need it directly. Does nothing if no batch is active, and is safe to call from within a
 * flush -- it returns immediately rather than recursing.
 */
void gr_flush_quad_batch();

/**
 * @brief Flushes the active batch list and stops redirecting 2D drawing into it
 */
void gr_end_quad_batch();

/**
 * @brief Flashes the screen with the specified color
 * @param r The red color value
 * @param g The green color value
 * @param b The blue color value
 */
void gr_flash( int r, int g, int b );
/**
 * @brief Flashes the screen with the specified color with an alpha value
 * @param r The red color value
 * @param g The green color value
 * @param b The blue color value
 * @param a The alpha value of the flash
 */
void gr_flash_alpha(int r, int g, int b, int a);

/**
 * @brief Draws a grey-scale bitmap multiplied with the current color
 * @param x The x-coordinate of the draw call
 * @param y The y-coordinate of the draw call
 * @param resize_mode The resize mode for translating the coordinated
 * @param mirror @c true to mirror the image
 * @param scale_factor a multiplier for the width and height of the bitmap
 */
void gr_aabitmap(int x, int y, int resize_mode = GR_RESIZE_FULL, bool mirror = false, float scale_factor = 1.0f);
/**
 * @brief Draws a grey-scale bitmap multiplied with the current color
 * @param x The x-coordinate of the draw call
 * @param y The y-coordinate of the draw call
 * @param w Resolves to the x axis right clip plane, usually <= bitmap width
 * @param h Resolves to the y axis right clip plane, usually <= bitmap height
 * @param sx The bitmap source x coordinate to start rendering from at the x 0 coordinate
 * @param sy The bitmap source y coordinate to start rendering from at the y 0 coordinate
 * @param resize_mode The resize mode for translating the coordinated
 * @param mirror @c true to mirror the image
 * @param scale_factor a multipler for the width and height of the bitmap. Note that w, h, sx, and sy should be unscaled source bitmap values
 */
void gr_aabitmap_ex(int x, int y, int w, int h, int sx, int sy, int resize_mode = GR_RESIZE_FULL, bool mirror = false, float scale_factor = 1.0f);
/**
 * @brief Draws a normal-colored bitmap to the screen
 * @param x The x-coordinate of the draw call
 * @param y The y-coordinate of the draw call
 *@param w Resolves to the x axis right clip plane, usually <= bitmap width
 * @param h Resolves to the y axis right clip plane, usually <= bitmap height
 * @param sx The bitmap source x coordinate to start rendering from at the x 0 coordinate
 * @param sy The bitmap source y coordinate to start rendering from at the y 0 coordinate
 * @param resize_mode The resize mode for translating the coordinated
 * @param mirror @c true to mirror the image
 * @param scale_factor a multipler for the width and height of the bitmap. Note that w, h, sx, and sy should be unscaled source bitmap values
 */
void gr_bitmap_ex(int x, int y, int w, int h, int sx, int sy, int resize_mode = GR_RESIZE_FULL, bool mirror = false, float scale_factor = 1.0f);

/**
 * @brief Renders the specified string to the screen using the current font and color
 * @param x The x-coordinate
 * @param y The y-coordinate
 * @param string The string to draw to the screen
 * @param resize_mode The mode for translating the screen positions
 * @param scaleMultiplier The scale to use to apply scaling in addition to user settings
 * @param length The number of bytes in the string to render. -1 will render the whole string.
 */
void gr_string(float x, float y, const char* string, int resize_mode = GR_RESIZE_FULL, float scaleMultiplier = 1.0f, size_t length = std::string::npos);
/**
 * @brief Renders the specified string to the screen using the current font and color
 * @param x The x-coordinate
 * @param y The y-coordinate
 * @param string The string to draw to the screen
 * @param resize_mode The mode for translating the screen positions
 * @param scaleMultiplier The scale to use to apply scaling in addition to user settings
 * @param length The number of bytes in the string to render. -1 will render the whole string.
 */
inline void gr_string(int x, int y, const char* string, int resize_mode = GR_RESIZE_FULL, float scaleMultiplier = 1.0f, size_t length = std::string::npos)
{
	gr_string(i2fl(x), i2fl(y), string, resize_mode, scaleMultiplier, length);
}

/**
 * @brief Draws outlined text at the given position
 *
 * @details Renders the text string with an outline by drawing the text
 * at surrounding offsets in the outline color, then drawing the main text on top
 * in the foreground color.
 *
 * @param x The x-coordinate
 * @param y The y-coordinate
 * @param text The text to draw
 * @param foreground Color for the main text
 * @param outline Color for the outline
 * @param offset Pixel offset for the outline (default 1)
 * @param resize_mode The mode for translating the screen positions
 * @param scaleMultiplier The scale to use to apply scaling in addition to user settings
 * @param length The number of bytes in the string to render. -1 will render the whole string.
 */
void gr_string_outlined(int x, int y, const char* text, const color* foreground, const color* outline, int offset = 1, int resize_mode = GR_RESIZE_FULL, float scaleMultiplier = 1.0f, size_t length = std::string::npos);

/**
 * @brief Draws a single line segment to the screen.
 * @param x1 The starting x-coordinate
 * @param y1 The starting y-coordinate
 * @param x2 The end x-coordinate
 * @param y2 The end y-coordinate
 * @param resize_mode The resize mode for translating screen positions
 */
void gr_line(int x1, int y1, int x2, int y2, int resize_mode = GR_RESIZE_FULL);
/**
 * @brief Draws an antialiased line is the current color is an alphacolor, otherwise just draws a fast line.
 * This gets called internally by g3_draw_line. This assumes he vertex's are already clipped, so call g3_draw_line
 * not this if you have two 3d points.
 * @param v1 The starting position
 * @param v2 The end position
 */
void gr_aaline(vertex *v1, vertex *v2);
/**
 * @brief Draw a gradient line... x1,y1 is bright, x2,y2 is transparent.
 * @param x1 The starting x-coordinate
 * @param y1 The starting y-coordinate
 * @param x2 The end x-coordinate
 * @param y2 The end y-coordinate
 * @param resize_mode The resize mode for translating screen positions
 */
void gr_gradient(int x1, int y1, int x2, int y2, int resize_mode = GR_RESIZE_FULL);
/**
 * @brief Sets the specified pixel to the current color
 * @param x The x-coordinate
 * @param y The y-coordinate
 * @param resize_mode The mode for translating the screen positions
 */
void gr_pixel(int x, int y, int resize_mode = GR_RESIZE_FULL);

/**
 * @brief Draws a filled rectangle with the current color
 * @param x The x-coordinate
 * @param y The y-coordinate
 * @param w The width of the rectangle
 * @param h The height of the rectangle
 * @param resize_mode The mode for translating the screen positions
 * @param angle The angle (in radians) for rotating the rectangle around its center.
 */
void gr_rect(int x, int y, int w, int h, int resize_mode = GR_RESIZE_FULL, float angle = 0);
/**
 * @brief Draws a filled rectangle with the current shading color
 * @param x The x-coordinate
 * @param y The y-coordinate
 * @param w The width of the rectangle
 * @param h The height of the rectangle
 * @param resize_mode The mode for translating the screen positions
 */
void gr_shade(int x, int y, int w, int h, int resize_mode = GR_RESIZE_FULL);

/**
 * @brief Draws a filled circle with the current color
 * @param xc The center x-position of the circle
 * @param yc The center y-position of the circle
 * @param d The diameter of the circle
 * @param resize_mode The mode for translating the screen positions
 */
void gr_circle(int xc, int yc, int d, int resize_mode = GR_RESIZE_FULL);
/**
 * @brief Draws an unfilled circle with the current color
 * @param xc The center x-position of the circle
 * @param yc The center y-position of the circle
 * @param d The diameter of the circle
 * @param resize_mode The mode for translating the screen positions
 */
void gr_unfilled_circle(int xc, int yc, int d, int resize_mode = GR_RESIZE_FULL);
/**
 * @brief Draws a limited circle arc from the specified start to the end angle
 * @param xc The center x-position
 * @param yc The center y-position
 * @param r The radius of the arc
 * @param angle_start The starting angle of the arc
 * @param angle_end The end angle of the arc
 * @param fill @c true to fill the arc segment
 * @param resize_mode The mode for translating the screen positions
 */
void gr_arc(int xc, int yc, float r, float angle_start, float angle_end, bool fill, int resize_mode = GR_RESIZE_FULL);
/**
 * @brief Draws a 90° curve with the center at the specified coordinates
 *
 * The direction specified in which direction the curve should be drawn
 *
 * @param x The center x-position of the curve
 * @param y The center y-position of the curve
 * @param r The radius of the curve
 * @param direction The direction to draw the curve
 * @param resize_mode The mode for translating the screen positions
 */
void gr_curve(int x, int y, int r, int direction, int resize_mode);

/**
 * @brief Start buffering 2D rendering operations
 *
 * This will defer rendering 2D interface elements until gr_2d_stop_buffer is called. This can improve performance when
 * doing a lot of 2D operations since the actual drawing will only be done once.
 *
 * @warning This will only affect a few rendering operations and might change the drawing order if incompatible rendering
 * commands are executed while the buffering mechanism is active.
 */
void gr_2d_start_buffer();

/**
 * @brief Stop buffering 2D rendering operations
 *
 * This will stop the 2D buffering mechanism and also flush all previous render commands.
 */
void gr_2d_stop_buffer();

/**
 * @brief The buffer object holding the data for immediate draws
 */
extern gr_buffer_handle gr_immediate_buffer_handle;

/**
 * @brief Adds data to the immediate buffer for use by draw operations
 *
 * @warning The data is only available in the buffer for one frame.
 *
 * @param size The size of the data buffer
 * @param data The pointer to the data
 * @return The offset into the immediate buffer where this data starts at
 */
size_t gr_add_to_immediate_buffer(size_t size, void *data);

/**
 * @brief Resets the immediate buffer for reuse
 */
void gr_reset_immediate_buffer();

/**
 * @brief Renders some vertex data from an immediate memory buffer
 * @param material_info The material information for rendering the data
 * @param prim_type The primitive type of the data
 * @param layout The vertex layout of the data
 * @param n_verts How many vertices are in the data
 * @param data The pointer to the data to render
 * @param size The size of the data
 */
void gr_render_primitives_immediate(material* material_info, primitive_type prim_type, vertex_layout* layout, int n_verts, void* data, size_t size);

/**
 * @brief Renders some vertex data from an immediate memory buffer with a 2D projection matrix
 * @param material_info The material information for rendering the data
 * @param prim_type The primitive type of the data
 * @param layout The vertex layout of the data
 * @param n_verts How many vertices are in the data
 * @param data The pointer to the data to render
 * @param size The size of the data
 */
void gr_render_primitives_2d_immediate(material* material_info, primitive_type prim_type, vertex_layout* layout, int n_verts, void* data, size_t size);

void gr_bitmap_list(bitmap_rect_list* list, int n_bm, int resize_mode, float angle = 0.f);

void gr_aabitmap_list(bitmap_rect_list* list, int n_bm, int resize_mode, float angle = 0.f);
