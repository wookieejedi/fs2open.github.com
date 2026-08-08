//
//

#include "quad_draw_list.h"
#include "2d.h"
#include "material.h"
#include "render.h"
#include "tracing/tracing.h"

namespace graphics {

quad_draw_list::quad_batch* quad_draw_list::find_batch(layer_type layer, const batch_key& key)
{
	auto& batches = _batches[layer];
	auto& used = _batches_used[layer];

	// Linear search is the right thing here: a pass produces a handful of distinct batches, and keeping them in
	// first-seen order also keeps the draw order deterministic.
	for (size_t i = 0; i < used; ++i) {
		if (batches[i].key == key) {
			return &batches[i];
		}
	}

	if (used == batches.size()) {
		batches.emplace_back();
	}

	auto* batch = &batches[used];
	++used;

	batch->key = key;
	batch->vertices.clear();

	return batch;
}

void quad_draw_list::add_quad(layer_type layer,
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
	const color* clr)
{
	// Compared for inequality rather than range: the parameter is already typed, and an ordered comparison against the
	// end of an enum trips "expression is always true" warnings on some compilers.
	Assertion(layer != NUM_LAYERS, "NUM_LAYERS is not a drawable quad draw list layer!");

	if (bitmap < 0) {
		return;
	}

	// An alpha-mask bitmap has no colour of its own, so without an alphacolor there is nothing to draw it with. The
	// immediate path bails out the same way.
	if (tex_type == material::TEX_TYPE_AABITMAP && !clr->is_alphacolor) {
		return;
	}

	auto* batch = find_batch(layer, {bitmap, tex_type, blend_mode});

	vec4 vert_color;
	vert_color.xyzw.x = clr->red / 255.0f;
	vert_color.xyzw.y = clr->green / 255.0f;
	vert_color.xyzw.z = clr->blue / 255.0f;
	vert_color.xyzw.w = clr->is_alphacolor ? clr->alpha / 255.0f : 1.0f;

	const quad_vertex top_left     = {{x1, y1}, {u0, v0}, vert_color};
	const quad_vertex top_right    = {{x2, y1}, {u1, v0}, vert_color};
	const quad_vertex bottom_left  = {{x1, y2}, {u0, v1}, vert_color};
	const quad_vertex bottom_right = {{x2, y2}, {u1, v1}, vert_color};

	// Two triangles rather than a strip so that every quad in the batch can live in one draw call
	auto& verts = batch->vertices;
	verts.push_back(top_left);
	verts.push_back(bottom_left);
	verts.push_back(top_right);
	verts.push_back(bottom_left);
	verts.push_back(bottom_right);
	verts.push_back(top_right);
}

bool quad_draw_list::empty() const
{
	for (int layer = 0; layer < NUM_LAYERS; ++layer) {
		for (size_t i = 0; i < _batches_used[layer]; ++i) {
			if (!_batches[layer][i].vertices.empty()) {
				return false;
			}
		}
	}

	return true;
}

void quad_draw_list::render_batch(quad_batch& batch)
{
	if (batch.vertices.empty()) {
		return;
	}

	material render_mat;
	render_mat.set_blend_mode(batch.key.blend_mode);
	render_mat.set_depth_mode(ZBUFFER_TYPE_NONE);
	render_mat.set_texture_map(TM_BASE_TYPE, batch.key.bitmap);
	render_mat.set_texture_type(batch.key.tex_type);
	render_mat.set_cull_mode(false);
	render_mat.set_color(1.0f, 1.0f, 1.0f, 1.0f); // Colour is handled by the vertices

	vertex_layout layout;
	layout.add_vertex_component(vertex_format_data::POSITION2, sizeof(quad_vertex), offsetof(quad_vertex, position));
	layout.add_vertex_component(vertex_format_data::TEX_COORD2, sizeof(quad_vertex), offsetof(quad_vertex, tex_coord));
	layout.add_vertex_component(vertex_format_data::COLOR4F, sizeof(quad_vertex), offsetof(quad_vertex, color));

	gr_render_primitives_2d_immediate(&render_mat,
		PRIM_TYPE_TRIS,
		&layout,
		static_cast<int>(batch.vertices.size()),
		batch.vertices.data(),
		batch.vertices.size() * sizeof(quad_vertex));

	// Keep the capacity so a steady-state frame stops allocating after the first few frames
	batch.vertices.clear();
}

void quad_draw_list::flush()
{
	if (empty()) {
		// Nothing to do here...
		return;
	}

	GR_DEBUG_SCOPE("Quad draw list flush");
	TRACE_SCOPE(tracing::QuadDrawListFlush);

	for (int layer = 0; layer < NUM_LAYERS; ++layer) {
		for (size_t i = 0; i < _batches_used[layer]; ++i) {
			render_batch(_batches[layer][i]);
		}

		// Release the slots back to the pool without destroying them, so the next pass reuses their vertex storage
		_batches_used[layer] = 0;
	}
}

}
