/*
Better Key - realtime chroma/luma keyer for OBS Studio
Copyright (C) 2026 Insanity Labs

Pure-GPU keying: chroma-distance coverage estimation with
Vlahos-style spill unmixing, luma assist, translucent color fix.
No inference, no CPU roundtrip - everything runs in the pixel shader.
*/

#include <obs-module.h>
#include <graphics/graphics.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define S_DESPILL "despill"
#define S_OUTPUT "output"
#define S_CHROMA "chroma_assist"
#define S_CHROMA_COLOR "chroma_color"
#define S_CHROMA_SIM "chroma_similarity"
#define S_CHROMA_SMOOTH "chroma_smoothness"
#define S_GAMMA "gamma"
#define S_CONTRAST "contrast"
#define S_BRIGHTNESS "brightness"
#define S_OPACITY "opacity"
#define S_LUMA "luma_mode"
#define S_LUMA_LEVEL "luma_level"
#define S_LUMA_SOFT "luma_softness"
#define S_TDESAT "trans_desat"
#define S_CALIB "calibrate"

struct bk_filter {
	obs_source_t *source = nullptr;
	gs_effect_t *effect = nullptr;
	gs_texrender_t *texrender = nullptr;
	gs_texrender_t *calib_tex = nullptr;
	gs_stagesurf_t *calib_surf = nullptr;
	bool calibrate_pending = false;
	uint64_t frame_idx = 0;

	gs_eparam_t *p_image = nullptr;
	gs_eparam_t *p_despill = nullptr;
	gs_eparam_t *p_output = nullptr;
	gs_eparam_t *p_chroma = nullptr;
	gs_eparam_t *p_chroma_color = nullptr;
	gs_eparam_t *p_chroma_sim = nullptr;
	gs_eparam_t *p_chroma_smooth = nullptr;
	gs_eparam_t *p_px = nullptr;
	gs_eparam_t *p_luma_mode = nullptr;
	gs_eparam_t *p_luma_level = nullptr;
	gs_eparam_t *p_luma_soft = nullptr;
	gs_eparam_t *p_tdesat = nullptr;
	gs_eparam_t *p_gamma = nullptr;
	gs_eparam_t *p_contrast = nullptr;
	gs_eparam_t *p_bright = nullptr;
	gs_eparam_t *p_opacity = nullptr;

	float despill = 0.5f;
	int output = 0;
	bool chroma = false;
	uint32_t chroma_color = 0xff00ff00;
	float chroma_sim = 0.35f;
	float chroma_smooth = 0.1f;
	float gamma = 1.f;
	float contrast = 1.f;
	float brightness = 0.f;
	float opacity = 1.f;
	int luma_mode = 0;
	float luma_level = 0.2f;
	float luma_soft = 0.15f;
	float trans_desat = 0.4f;
};

static const char *bk_get_name(void *)
{
	return obs_module_text("BetterKey");
}

static void *bk_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new (std::nothrow) bk_filter();
	if (!f)
		return nullptr;
	f->source = source;

	obs_enter_graphics();
	f->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	char *eff_path = obs_module_file("better_key.effect");
	char *err = nullptr;
	if (eff_path) {
		f->effect = gs_effect_create_from_file(eff_path, &err);
		if (!f->effect && err) {
			blog(LOG_ERROR, "[better-key] effect compile failed: %s", err);
			bfree(err);
		}
		bfree(eff_path);
	}
	obs_leave_graphics();

	if (!f->effect) {
		blog(LOG_ERROR, "[better-key] could not load better_key.effect");
		obs_enter_graphics();
		gs_texrender_destroy(f->texrender);
		obs_leave_graphics();
		delete f;
		return nullptr;
	}

	f->p_image = gs_effect_get_param_by_name(f->effect, "image");
	f->p_despill = gs_effect_get_param_by_name(f->effect, "despill");
	f->p_output = gs_effect_get_param_by_name(f->effect, "output_mode");
	f->p_chroma = gs_effect_get_param_by_name(f->effect, "chroma_enable");
	f->p_chroma_color = gs_effect_get_param_by_name(f->effect, "chroma_color");
	f->p_chroma_sim = gs_effect_get_param_by_name(f->effect, "chroma_similarity");
	f->p_chroma_smooth = gs_effect_get_param_by_name(f->effect, "chroma_smoothness");
	f->p_px = gs_effect_get_param_by_name(f->effect, "pixel_size");
	f->p_luma_mode = gs_effect_get_param_by_name(f->effect, "luma_mode");
	f->p_luma_level = gs_effect_get_param_by_name(f->effect, "luma_level");
	f->p_luma_soft = gs_effect_get_param_by_name(f->effect, "luma_soft");
	f->p_tdesat = gs_effect_get_param_by_name(f->effect, "trans_desat");
	f->p_gamma = gs_effect_get_param_by_name(f->effect, "gamma");
	f->p_contrast = gs_effect_get_param_by_name(f->effect, "contrast");
	f->p_bright = gs_effect_get_param_by_name(f->effect, "brightness");
	f->p_opacity = gs_effect_get_param_by_name(f->effect, "opacity");

	obs_source_update(source, settings);
	return f;
}

static void bk_destroy(void *data)
{
	auto *f = static_cast<bk_filter *>(data);
	if (!f)
		return;
	obs_enter_graphics();
	gs_texrender_destroy(f->texrender);
	gs_texrender_destroy(f->calib_tex);
	gs_stagesurface_destroy(f->calib_surf);
	gs_effect_destroy(f->effect);
	obs_leave_graphics();
	delete f;
}

static void bk_update(void *data, obs_data_t *s)
{
	auto *f = static_cast<bk_filter *>(data);
	f->despill = (float)obs_data_get_double(s, S_DESPILL);
	f->output = (int)obs_data_get_int(s, S_OUTPUT);
	f->chroma = obs_data_get_bool(s, S_CHROMA);
	f->chroma_color = (uint32_t)obs_data_get_int(s, S_CHROMA_COLOR);
	f->chroma_sim = (float)obs_data_get_double(s, S_CHROMA_SIM);
	f->chroma_smooth = (float)obs_data_get_double(s, S_CHROMA_SMOOTH);
	f->gamma = (float)obs_data_get_double(s, S_GAMMA);
	f->contrast = (float)obs_data_get_double(s, S_CONTRAST);
	f->brightness = (float)obs_data_get_double(s, S_BRIGHTNESS);
	f->opacity = (float)obs_data_get_double(s, S_OPACITY);
	f->luma_mode = (int)obs_data_get_int(s, S_LUMA);
	f->luma_level = (float)obs_data_get_double(s, S_LUMA_LEVEL);
	f->luma_soft = (float)obs_data_get_double(s, S_LUMA_SOFT);
	f->trans_desat = (float)obs_data_get_double(s, S_TDESAT);
}

// Downscale the captured frame to 160x90, take the median color of the
// four corner blocks as the screen color, and derive similarity from
// the screen's own chroma spread. Writes back into the filter settings.
static void bk_run_calibration(bk_filter *f, gs_texture_t *frame_tex)
{
	if (!f->calib_tex) {
		f->calib_tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		f->calib_surf = gs_stagesurface_create(160, 90, GS_RGBA);
	}
	gs_texrender_reset(f->calib_tex);
	if (gs_texrender_begin(f->calib_tex, 160, 90)) {
		struct vec4 clear = {0.f, 0.f, 0.f, 0.f};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.f, 0);
		gs_ortho(0.f, 160.f, 0.f, 90.f, -100.f, 100.f);
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *ip = gs_effect_get_param_by_name(def, "image");
		gs_effect_set_texture(ip, frame_tex);
		gs_technique_t *tech = gs_effect_get_technique(def, "Draw");
		const size_t tp = gs_technique_begin(tech);
		for (size_t i = 0; i < tp; i++) {
			if (gs_technique_begin_pass(tech, i)) {
				gs_draw_sprite(frame_tex, 0, 160, 90);
				gs_technique_end_pass(tech);
			}
		}
		gs_technique_end(tech);
		gs_texrender_end(f->calib_tex);
	}
	gs_stage_texture(f->calib_surf, gs_texrender_get_texture(f->calib_tex));

	uint8_t *pix = nullptr;
	uint32_t pitch = 0;
	if (!gs_stagesurface_map(f->calib_surf, &pix, &pitch) || !pix)
		return;

	const int bw = 24, bh = 18;
	std::vector<uint8_t> rr, gg, bb;
	for (int by : {0, 90 - bh})
		for (int bx : {0, 160 - bw})
			for (int y = by; y < by + bh; y++)
				for (int x = bx; x < bx + bw; x++) {
					const uint8_t *q = pix + y * pitch + x * 4;
					rr.push_back(q[0]);
					gg.push_back(q[1]);
					bb.push_back(q[2]);
				}
	gs_stagesurface_unmap(f->calib_surf);

	auto med8 = [](std::vector<uint8_t> &v) {
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		return v[v.size() / 2];
	};
	const float R = med8(rr) / 255.f, G = med8(gg) / 255.f, B = med8(bb) / 255.f;

	// same srgb -> CbCr distance the shader uses
	auto nl = [](float u) {
		return u <= 0.0031308f ? 12.92f * u : 1.055f * powf(u, 1.f / 2.4f) - 0.055f;
	};
	auto cbcr = [&](float r, float g, float b, float &cb, float &cr) {
		r = nl(r), g = nl(g), b = nl(b);
		cb = -0.100644f * r - 0.338572f * g + 0.439216f * b + 0.501961f;
		cr = 0.439216f * r - 0.398942f * g - 0.040274f * b + 0.501961f;
	};
	float kcb, kcr;
	cbcr(R, G, B, kcb, kcr);
	std::vector<float> dist;
	dist.reserve(rr.size());
	for (size_t i = 0; i < rr.size(); i++) {
		float cb2, cr2;
		cbcr(rr[i] / 255.f, gg[i] / 255.f, bb[i] / 255.f, cb2, cr2);
		dist.push_back(std::hypot(cb2 - kcb, cr2 - kcr));
	}
	std::sort(dist.begin(), dist.end());
	float sim = dist[(size_t)(dist.size() * 0.9)] * 1.5f + 0.08f;
	sim = std::min(0.5f, std::max(0.05f, sim));

	uint32_t packed = 0xff000000u | ((uint32_t)(B * 255.f) << 16) | ((uint32_t)(G * 255.f) << 8) |
			  (uint32_t)(R * 255.f);
	obs_data_t *s2 = obs_source_get_settings(f->source);
	obs_data_set_int(s2, S_CHROMA_COLOR, packed);
	obs_data_set_bool(s2, S_CHROMA, true);
	obs_data_set_double(s2, S_CHROMA_SIM, (double)sim);
	obs_source_update(f->source, s2);
	obs_data_release(s2);
	blog(LOG_INFO, "[better-key] calibrated: rgb=(%.0f,%.0f,%.0f) similarity=%.2f", (double)(R * 255.f),
	     (double)(G * 255.f), (double)(B * 255.f), (double)sim);
}

static void bk_video_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<bk_filter *>(data);
	obs_source_t *parent = obs_filter_get_parent(f->source);

	if (!f->effect || !parent) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	const uint32_t w = obs_source_get_width(parent);
	const uint32_t h = obs_source_get_height(parent);
	if (!w || !h) {
		obs_source_skip_video_filter(f->source);
		return;
	}
	f->frame_idx++;

	// render parent into our texrender (same branch as obs's own
	// filter path: default render for plain sync sources)
	obs_source_t *target = obs_filter_get_target(f->source);
	const uint32_t pflags = obs_source_get_output_flags(parent);
	const bool custom_draw = (pflags & OBS_SOURCE_CUSTOM_DRAW) != 0;
	const bool async = (pflags & OBS_SOURCE_ASYNC) != 0;

	gs_texrender_reset(f->texrender);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	if (gs_texrender_begin(f->texrender, w, h)) {
		struct vec4 clear = {0.f, 0.f, 0.f, 0.f};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.f, 0);
		gs_ortho(0.f, (float)w, 0.f, (float)h, -100.f, 100.f);
		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);
		gs_texrender_end(f->texrender);
	}
	gs_blend_state_pop();

	gs_texture_t *frame_tex = gs_texrender_get_texture(f->texrender);
	if (!frame_tex) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	if (f->calibrate_pending) {
		f->calibrate_pending = false;
		bk_run_calibration(f, frame_tex);
	}

	gs_effect_set_texture(f->p_image, frame_tex);
	gs_effect_set_float(f->p_despill, f->despill);
	gs_effect_set_float(f->p_output, (float)f->output);
	gs_effect_set_float(f->p_chroma, f->chroma ? 1.f : 0.f);
	gs_effect_set_float(f->p_chroma_sim, f->chroma_sim);
	gs_effect_set_float(f->p_chroma_smooth, f->chroma_smooth);
	struct vec2 px = {1.f / (float)w, 1.f / (float)h};
	gs_effect_set_vec2(f->p_px, &px);
	gs_effect_set_float(f->p_luma_mode, (float)f->luma_mode);
	gs_effect_set_float(f->p_luma_level, f->luma_level);
	gs_effect_set_float(f->p_luma_soft, f->luma_soft);
	gs_effect_set_float(f->p_tdesat, f->trans_desat);
	gs_effect_set_float(f->p_gamma, f->gamma);
	gs_effect_set_float(f->p_contrast, f->contrast);
	gs_effect_set_float(f->p_bright, f->brightness);
	gs_effect_set_float(f->p_opacity, f->opacity);
	const float cr = (f->chroma_color & 0xff) / 255.f;
	const float cg = ((f->chroma_color >> 8) & 0xff) / 255.f;
	const float cb = ((f->chroma_color >> 16) & 0xff) / 255.f;
	struct vec4 cc = {cr, cg, cb, 0.f};
	gs_effect_set_vec4(f->p_chroma_color, &cc);

	// premultiplied-alpha output, matching the obs filter pipeline
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_technique_t *tech = gs_effect_get_technique(f->effect, "Draw");
	const size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		if (gs_technique_begin_pass(tech, i)) {
			gs_draw_sprite(frame_tex, 0, w, h);
			gs_technique_end_pass(tech);
		}
	}
	gs_technique_end(tech);
	gs_blend_state_pop();
}

static bool bk_calibrate_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<bk_filter *>(data);
	f->calibrate_pending = true;
	return true;
}

static obs_properties_t *bk_properties(void *data)
{
	auto *f = static_cast<bk_filter *>(data);
	obs_properties_t *p = obs_properties_create();

	obs_properties_add_bool(p, S_CHROMA, obs_module_text("ChromaAssist"));
	obs_properties_add_color(p, S_CHROMA_COLOR, obs_module_text("ChromaColor"));
	obs_properties_add_float_slider(p, S_CHROMA_SIM, obs_module_text("ChromaSim"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, S_CHROMA_SMOOTH, obs_module_text("ChromaSmooth"), 0.01, 1.0, 0.01);
	obs_properties_add_button2(p, S_CALIB, obs_module_text("Calibrate"), bk_calibrate_clicked, f);

	obs_property_t *lm = obs_properties_add_list(p, S_LUMA, obs_module_text("LumaMode"), OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(lm, obs_module_text("LumaMode.Off"), 0);
	obs_property_list_add_int(lm, obs_module_text("LumaMode.Darks"), 1);
	obs_property_list_add_int(lm, obs_module_text("LumaMode.Brights"), 2);
	obs_properties_add_float_slider(p, S_LUMA_LEVEL, obs_module_text("LumaLevel"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, S_LUMA_SOFT, obs_module_text("LumaSoft"), 0.01, 1.0, 0.01);

	obs_properties_add_float_slider(p, S_DESPILL, obs_module_text("Despill"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, S_TDESAT, obs_module_text("TransDesat"), 0.0, 1.0, 0.01);

	obs_properties_add_float_slider(p, S_GAMMA, obs_module_text("Gamma"), 0.2, 3.0, 0.01);
	obs_properties_add_float_slider(p, S_CONTRAST, obs_module_text("Contrast"), 0.0, 4.0, 0.01);
	obs_properties_add_float_slider(p, S_BRIGHTNESS, obs_module_text("Brightness"), -1.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, S_OPACITY, obs_module_text("Opacity"), 0.0, 1.0, 0.01);

	obs_property_t *o = obs_properties_add_list(p, S_OUTPUT, obs_module_text("Output"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(o, obs_module_text("Output.Composite"), 0);
	obs_property_list_add_int(o, obs_module_text("Output.FinalMatte"), 2);
	obs_property_list_add_int(o, obs_module_text("Output.Raw"), 3);
	return p;
}

static void bk_defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, S_DESPILL, 0.5);
	obs_data_set_default_bool(s, S_CHROMA, true);
	obs_data_set_default_int(s, S_CHROMA_COLOR, 0xff00ff00);
	obs_data_set_default_double(s, S_CHROMA_SIM, 0.35);
	obs_data_set_default_double(s, S_CHROMA_SMOOTH, 0.1);
	obs_data_set_default_double(s, S_GAMMA, 1.0);
	obs_data_set_default_double(s, S_CONTRAST, 1.0);
	obs_data_set_default_double(s, S_BRIGHTNESS, 0.0);
	obs_data_set_default_double(s, S_OPACITY, 1.0);
	obs_data_set_default_int(s, S_LUMA, 0);
	obs_data_set_default_double(s, S_LUMA_LEVEL, 0.2);
	obs_data_set_default_double(s, S_LUMA_SOFT, 0.15);
	obs_data_set_default_double(s, S_TDESAT, 0.4);
	obs_data_set_default_int(s, S_OUTPUT, 0);
}

// NOTE: the id string is kept as "infinity_key_filter" so existing
// scene collections keep working across the rename to Better Key.
struct obs_source_info infinity_key_filter = {
	.id = "infinity_key_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = bk_get_name,
	.create = bk_create,
	.destroy = bk_destroy,
	.get_defaults = bk_defaults,
	.get_properties = bk_properties,
	.update = bk_update,
	.video_render = bk_video_render,
};
