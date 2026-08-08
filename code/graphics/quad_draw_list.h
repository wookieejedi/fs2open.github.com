#pragma once

#include "globalincs/pstypes.h"
#include "2d.h"
#include "material.h"

namespace graphics {

/**
 * @brief A class for batching textured 2D quads together
 *
 * Each quad carries its own vertex colour, so draws that differ only by colour still merge. Quads are grouped by the
 * state that actually has to be applied to the GPU -- bitmap, texture type and blend mode -- and every group becomes a
 * single draw call at flush() time. This replaces a per-quad material application plus a per-quad vertex sub-upload,
 * which is where the cost of drawing many small 2D elements actually goes.
 *
 * Quads are submitted into a @em layer. All of layer 0 is drawn before any of layer 1, so a caller can guarantee that
 * one class of element ends up on top of another (the HUD uses this to keep text above gauge backgrounds) without
 * giving up batching within each layer. Within a layer, groups are drawn in the order their state was first seen, and
 * quads within a group in submission order; quads that overlap but land in different groups of the same layer may
 * therefore swap z-order relative to unbatched rendering.
 *
 * Positions are expected in final screen pixels with the clip offset already applied -- the same values the immediate
 * path would have handed to the GPU. Because the scissor rectangle set by gr_set_clip() is real GPU state that is
 * @em not captured per quad, the caller must flush() before changing the clip.
 */
class quad_draw_list {
 public:
	enum layer_type {
		LAYER_BACKGROUND = 0,	//!< drawn first
		LAYER_FOREGROUND = 1,	//!< drawn over LAYER_BACKGROUND
		NUM_LAYERS
	};

 private:
	struct quad_vertex {
		vec2d position;
		vec2d tex_coord;
		vec4 color;
	};

	/**
	 * @brief The GPU state a run of quads has in common
	 *
	 * Two quads can only share a draw call if all of these match. The bitmap handle is the exact frame rather than the
	 * animation's base frame because the array slice is applied as a uniform by the material, not per vertex.
	 */
	struct batch_key {
		int bitmap;
		material::texture_type tex_type;
		gr_alpha_blend blend_mode;

		bool operator==(const batch_key& other) const
		{
			return bitmap == other.bitmap && tex_type == other.tex_type && blend_mode == other.blend_mode;
		}
	};

	struct quad_batch {
		batch_key key;
		SCP_vector<quad_vertex> vertices;
	};

	// A pool rather than a plain list: flush() resets the in-use count instead of destroying the entries, so the
	// vertex storage is reused and a steady-state frame stops allocating. Entries past the in-use count are stale and
	// must not be matched against, or the list would grow a slot for every bitmap ever drawn and the per-quad lookup
	// below would keep getting slower for the rest of the session.
	SCP_vector<quad_batch> _batches[NUM_LAYERS];
	size_t _batches_used[NUM_LAYERS] = {};

	/**
	 * @brief Finds the batch for this state in the layer, claiming a pool entry if this state has not been seen yet
	 */
	quad_batch* find_batch(layer_type layer, const batch_key& key);

	void render_batch(quad_batch& batch);

 public:
	quad_draw_list() = default;

	/**
	 * @brief Adds a textured quad to this draw list
	 *
	 * The coordinates are final screen pixels and the UVs are normalised texture coordinates, matching what the
	 * immediate rendering path computes just before handing vertices to the GPU.
	 *
	 * @param layer Which layer to draw this quad in
	 * @param bitmap The bitmap handle to texture the quad with
	 * @param tex_type How the bitmap should be sampled (an alpha mask, opaque, or with an alpha channel)
	 * @param blend_mode The alpha blending mode for this quad
	 * @param x1 Left edge
	 * @param y1 Top edge
	 * @param x2 Right edge
	 * @param y2 Bottom edge
	 * @param u0 Texture coordinate of the left edge
	 * @param v0 Texture coordinate of the top edge
	 * @param u1 Texture coordinate of the right edge
	 * @param v1 Texture coordinate of the bottom edge
	 * @param clr The colour to modulate the quad with
	 */
	void add_quad(layer_type layer,
		int bitmap,
		material::texture_type tex_type,
		gr_alpha_blend blend_mode,
		float x1,
		float y1,
		float x2,
		float y2,
		float u0,
		float v0,
		float u1,
		float v1,
		const color* clr);

	/**
	 * @brief Whether anything has been added since the last flush
	 *
	 * Lets a caller skip the bookkeeping around a flush when there is nothing pending.
	 */
	bool empty() const;

	/**
	 * @brief Draws everything stored so far and empties the list
	 *
	 * Layers are drawn in order, and the vertex storage is kept for reuse so a steady-state frame does no allocation.
	 */
	void flush();
};

}
