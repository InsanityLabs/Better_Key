/*
Better Key - realtime chroma/luma keyer for OBS Studio
Copyright (C) 2026 Insanity Labs

Pure-GPU keying: chroma-distance coverage estimation with
color unmixing against a self-maintaining 2058-point sampled
screen grid, luma assist, particle color repair.
No inference - everything runs in the pixel shader.
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
#define S_VIVID "particle_vivid"
#define S_PSAT "particle_saturation"
#define S_TRACK "screen_tracking"
#define S_PREPAIR "particle_repair"
#define S_CALIB "calibrate"

// 49 x 42 = 2058 screen sample points
static const int GRID_W = 49;
static const int GRID_H = 42;
static const int GRID_N = GRID_W * GRID_H;
static const int TRACK_INTERVAL = 15;

struct bk_filter {
	obs_source_t *source = nullptr;
	gs_effect_t *effect = nullptr;
	gs_texrender_t *texrender = nullptr;
	gs_texrender_t *calib_tex = nullptr;
	gs_stagesurf_t *calib_surf = nullptr;
	gs_texture_t *grid_tex = nullptr;
	double coef[3][6] = {};
	bool model_valid = false;
	bool grid_valid = false;
	bool grid_reset = false;
	bool grid_logged = false;
	bool calibrate_pending = false;
	uint64_t frame_idx = 0;

	gs_eparam_t *p_image = nullptr;
	gs_eparam_t *p_field = nullptr;
	gs_eparam_t *p_field_valid = nullptr;
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
	gs_eparam_t *p_vivid = nullptr;
	gs_eparam_t *p_psat = nullptr;
	gs_eparam_t *p_prepair = nullptr;

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
	float trans_desat = 0.f;
	float vivid = 0.5f;
	float psat = 1.f;
	bool track = true;
	float particle_repair = 0.5f;
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
	f->p_field = gs_effect_get_param_by_name(f->effect, "field");
	f->p_field_valid = gs_effect_get_param_by_name(f->effect, "field_valid");
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
	f->p_vivid = gs_effect_get_param_by_name(f->effect, "particle_vivid");
	f->p_psat = gs_effect_get_param_by_name(f->effect, "particle_sat");
	f->p_prepair = gs_effect_get_param_by_name(f->effect, "particle_repair");

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
	gs_texture_destroy(f->grid_tex);
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
	const uint32_t color = (uint32_t)obs_data_get_int(s, S_CHROMA_COLOR);
	if (color != f->chroma_color)
		f->grid_reset = true; // learned grid belongs to the old key
	f->chroma_color = color;
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
	f->vivid = (float)obs_data_get_double(s, S_VIVID);
	f->psat = (float)obs_data_get_double(s, S_PSAT);
	f->track = obs_data_get_bool(s, S_TRACK);
	f->particle_repair = (float)obs_data_get_double(s, S_PREPAIR);
}

static inline float bk_nl(float u)
{
	return u <= 0.0031308f ? 12.92f * u : 1.055f * powf(u, 1.f / 2.4f) - 0.055f;
}

// same srgb -> CbCr mapping the shader uses
static inline void bk_cbcr(float r, float g, float b, float &cb, float &cr)
{
	r = bk_nl(r), g = bk_nl(g), b = bk_nl(b);
	cb = -0.100644f * r - 0.338572f * g + 0.439216f * b + 0.501961f;
	cr = 0.439216f * r - 0.398942f * g - 0.040274f * b + 0.501961f;
}

// 6x6 linear solve (Gaussian elimination, partial pivoting), 3 right-hand sides
static bool bk_solve6(double A[6][6], double rhs[3][6], double out[3][6])
{
	for (int c = 0; c < 6; c++) {
		int piv = c;
		for (int r = c + 1; r < 6; r++)
			if (std::fabs(A[r][c]) > std::fabs(A[piv][c]))
				piv = r;
		if (std::fabs(A[piv][c]) < 1e-12)
			return false;
		if (piv != c) {
			for (int k = 0; k < 6; k++)
				std::swap(A[piv][k], A[c][k]);
			for (int ch = 0; ch < 3; ch++)
				std::swap(rhs[ch][piv], rhs[ch][c]);
		}
		for (int r = 0; r < 6; r++) {
			if (r == c)
				continue;
			const double m = A[r][c] / A[c][c];
			for (int k = c; k < 6; k++)
				A[r][k] -= m * A[c][k];
			for (int ch = 0; ch < 3; ch++)
				rhs[ch][r] -= m * rhs[ch][c];
		}
	}
	for (int ch = 0; ch < 3; ch++)
		for (int c = 0; c < 6; c++)
			out[ch][c] = rhs[ch][c] / A[c][c];
	return true;
}

// Samples the frame on a 49x42 lattice (2058 points). Points belonging
// to the screen's chroma cluster are fitted with a smooth quadratic
// surface per channel, which the shader samples as the local screen
// color. With calibrate=true the key color itself is
// found first (dominant saturated chroma cluster) and written back to
// the filter settings together with a similarity value.
static void bk_measure_grid(bk_filter *f, gs_texture_t *frame_tex, bool calibrate)
{
	if (!f->calib_tex) {
		f->calib_tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		f->calib_surf = gs_stagesurface_create(GRID_W, GRID_H, GS_RGBA);
	}
	gs_texrender_reset(f->calib_tex);
	if (gs_texrender_begin(f->calib_tex, GRID_W, GRID_H)) {
		struct vec4 clear = {0.f, 0.f, 0.f, 0.f};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.f, 0);
		gs_ortho(0.f, (float)GRID_W, 0.f, (float)GRID_H, -100.f, 100.f);
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *ip = gs_effect_get_param_by_name(def, "image");
		gs_effect_set_texture(ip, frame_tex);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		gs_technique_t *tech = gs_effect_get_technique(def, "Draw");
		const size_t tp = gs_technique_begin(tech);
		for (size_t i = 0; i < tp; i++) {
			if (gs_technique_begin_pass(tech, i)) {
				gs_draw_sprite(frame_tex, 0, GRID_W, GRID_H);
				gs_technique_end_pass(tech);
			}
		}
		gs_technique_end(tech);
		gs_blend_state_pop();
		gs_texrender_end(f->calib_tex);
	}
	gs_stage_texture(f->calib_surf, gs_texrender_get_texture(f->calib_tex));

	uint8_t *pix = nullptr;
	uint32_t pitch = 0;
	if (!gs_stagesurface_map(f->calib_surf, &pix, &pitch) || !pix)
		return;

	const size_t n = (size_t)GRID_N;
	std::vector<float> sr(n), sg(n), sb(n), scb(n), scr(n);
	std::vector<uint8_t> ok(n);
	for (int y = 0; y < GRID_H; y++)
		for (int x = 0; x < GRID_W; x++) {
			const uint8_t *q = pix + (size_t)y * pitch + (size_t)x * 4;
			const size_t i = (size_t)y * GRID_W + x;
			sr[i] = q[0] / 255.f;
			sg[i] = q[1] / 255.f;
			sb[i] = q[2] / 255.f;
			ok[i] = q[3] > 200;
			bk_cbcr(sr[i], sg[i], sb[i], scb[i], scr[i]);
		}
	gs_stagesurface_unmap(f->calib_surf);

	float kr = (f->chroma_color & 0xff) / 255.f;
	float kg = ((f->chroma_color >> 8) & 0xff) / 255.f;
	float kb = ((f->chroma_color >> 16) & 0xff) / 255.f;

	// center of the screen's chroma cluster; inliers are measured
	// against this, not against the picker color
	float ccb = 0.f, ccr = 0.f;
	float cluster_peak = 0.f; // brightest channel of the cluster mean
	{
		// dominant saturated chroma cluster = the screen
		const int HB = 48;
		std::vector<int> hist((size_t)HB * HB, 0);
		for (size_t i = 0; i < n; i++) {
			if (!ok[i] || std::hypot(scb[i] - 0.502f, scr[i] - 0.502f) < 0.08f)
				continue;
			const int bx = std::min(HB - 1, std::max(0, (int)(scb[i] * HB)));
			const int by = std::min(HB - 1, std::max(0, (int)(scr[i] * HB)));
			hist[(size_t)by * HB + bx]++;
		}
		const auto best = std::max_element(hist.begin(), hist.end());
		if (*best < 20) {
			if (calibrate)
				blog(LOG_WARNING, "[better-key] calibrate: no screen-colored region found");
			return;
		}
		const size_t bi = (size_t)(best - hist.begin());
		ccb = ((float)(bi % HB) + 0.5f) / HB, ccr = ((float)(bi / HB) + 0.5f) / HB;
		double ar = 0, ag = 0, ab = 0;
		size_t cnt = 0;
		for (int it = 0; it < 3; it++) {
			double acb = 0, acr = 0;
			ar = ag = ab = 0;
			cnt = 0;
			for (size_t i = 0; i < n; i++) {
				if (!ok[i] || std::hypot(scb[i] - ccb, scr[i] - ccr) > 0.06f)
					continue;
				acb += scb[i], acr += scr[i];
				ar += sr[i], ag += sg[i], ab += sb[i];
				cnt++;
			}
			if (!cnt)
				return;
			ccb = (float)(acb / cnt), ccr = (float)(acr / cnt);
		}
		cluster_peak = (float)(std::max(ar, std::max(ag, ab)) / cnt);
		if (calibrate) {
			kr = (float)(ar / cnt), kg = (float)(ag / cnt), kb = (float)(ab / cnt);
			f->grid_reset = true;
		} else {
			// live tracking only follows a cluster that is plausibly
			// the key the user chose
			float kcb, kcr;
			bk_cbcr(kr, kg, kb, kcb, kcr);
			if (std::hypot(ccb - kcb, ccr - kcr) > 0.25f)
				return;
		}
	}

	if (f->grid_reset) {
		f->grid_reset = false;
		f->model_valid = false;
		f->grid_valid = false;
	}

	// Anything translucent in front of the screen (particles, haze)
	// pulls a sample away from the pure key, never toward it. The fit
	// is therefore reweighted toward the purest samples, so it follows
	// the bare screen and ignores particles drifting across it. A
	// quadratic surface can follow lighting falloff but cannot bend
	// around an individual particle.
	const int kc = (kr >= kg && kr >= kb) ? 0 : (kg >= kb ? 1 : 2);
	auto purity = [kc](const double *c) {
		return c[kc] - std::max(c[(kc + 1) % 3], c[(kc + 2) % 3]);
	};
	auto basis = [](size_t i, double *b) {
		const double x = (double)(i % GRID_W) / (GRID_W - 1) * 2.0 - 1.0;
		const double y = (double)(i / GRID_W) / (GRID_H - 1) * 2.0 - 1.0;
		b[0] = 1.0, b[1] = x, b[2] = y, b[3] = x * x, b[4] = x * y, b[5] = y * y;
	};

	std::vector<size_t> idx;
	std::vector<float> dist;
	for (size_t i = 0; i < n; i++) {
		if (!ok[i])
			continue;
		const float d = std::hypot(scb[i] - ccb, scr[i] - ccr);
		// same hue but clearly darker than the screen = subject or
		// shadowed edge, not a clean screen sample
		if (d > 0.12f || std::max(sr[i], std::max(sg[i], sb[i])) < 0.8f * cluster_peak)
			continue;
		idx.push_back(i);
		dist.push_back(d);
	}
	const size_t inliers = idx.size();

	if (inliers >= 60) {
		std::vector<double> wgt(inliers, 1.0);
		double coef[3][6] = {};
		bool solved = false;
		for (int it = 0; it < 10; it++) {
			double A[6][6] = {}, rhs[3][6] = {};
			for (size_t k = 0; k < inliers; k++) {
				const size_t i = idx[k];
				double b[6];
				basis(i, b);
				const double smp[3] = {sr[i], sg[i], sb[i]};
				for (int r = 0; r < 6; r++) {
					for (int c = 0; c < 6; c++)
						A[r][c] += wgt[k] * b[r] * b[c];
					for (int ch = 0; ch < 3; ch++)
						rhs[ch][r] += wgt[k] * b[r] * smp[ch];
				}
			}
			for (int r = 0; r < 6; r++)
				A[r][r] += 1e-6;
			solved = bk_solve6(A, rhs, coef);
			if (!solved)
				break;
			for (size_t k = 0; k < inliers; k++) {
				const size_t i = idx[k];
				double b[6], m[3] = {0.0, 0.0, 0.0};
				basis(i, b);
				for (int ch = 0; ch < 3; ch++)
					for (int c = 0; c < 6; c++)
						m[ch] += coef[ch][c] * b[c];
				const double smp[3] = {sr[i], sg[i], sb[i]};
				const double e = purity(smp) - purity(m);
				wgt[k] = e >= 0.0 ? 8.0 : 1.0 / (1.0 + (e / 0.004) * (e / 0.004));
			}
		}
		if (solved) {
			const double t = (calibrate || !f->model_valid) ? 1.0 : 0.2;
			for (int ch = 0; ch < 3; ch++)
				for (int c = 0; c < 6; c++)
					f->coef[ch][c] += (coef[ch][c] - f->coef[ch][c]) * t;
			f->model_valid = true;
		}
	}

	if (f->model_valid) {
		std::vector<uint8_t> bytes(n * 4);
		for (size_t i = 0; i < n; i++) {
			double b[6];
			basis(i, b);
			for (int ch = 0; ch < 3; ch++) {
				double v = 0.0;
				for (int c = 0; c < 6; c++)
					v += f->coef[ch][c] * b[c];
				bytes[i * 4 + ch] = (uint8_t)std::lround(std::min(1.0, std::max(0.0, v)) * 255.0);
			}
			bytes[i * 4 + 3] = 255;
		}
		if (!f->grid_tex) {
			const uint8_t *init = bytes.data();
			f->grid_tex = gs_texture_create(GRID_W, GRID_H, GS_RGBA, 1, &init, GS_DYNAMIC);
		} else {
			gs_texture_set_image(f->grid_tex, bytes.data(), GRID_W * 4, false);
		}
		f->grid_valid = f->grid_tex != nullptr;
	}

	if (!f->grid_logged || calibrate) {
		f->grid_logged = true;
		blog(LOG_INFO, "[better-key] screen grid: %zu of %d points on screen, key rgb=(%.0f,%.0f,%.0f)%s",
		     inliers, GRID_N, (double)(kr * 255.f), (double)(kg * 255.f), (double)(kb * 255.f),
		     calibrate ? " (calibrated)" : "");
	}

	if (!calibrate)
		return;

	// similarity is how far from the screen a pixel still counts as
	// screen-mixed; the screen's own spread only sets a lower bound
	float sim = 0.3f;
	if (!dist.empty()) {
		std::sort(dist.begin(), dist.end());
		sim = dist[(size_t)(dist.size() * 0.9)] * 1.5f + 0.3f;
	}
	sim = std::min(0.5f, std::max(0.3f, sim));

	const uint32_t packed = 0xff000000u | ((uint32_t)std::lround(kb * 255.f) << 16) |
				((uint32_t)std::lround(kg * 255.f) << 8) | (uint32_t)std::lround(kr * 255.f);
	f->chroma_color = packed; // keep the grid we just measured
	obs_data_t *s2 = obs_source_get_settings(f->source);
	obs_data_set_int(s2, S_CHROMA_COLOR, packed);
	obs_data_set_bool(s2, S_CHROMA, true);
	obs_data_set_double(s2, S_CHROMA_SIM, (double)sim);
	obs_source_update(f->source, s2);
	obs_data_release(s2);
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
		bk_measure_grid(f, frame_tex, true);
	} else if (f->chroma && f->track && (!f->grid_valid || f->grid_reset || f->frame_idx % TRACK_INTERVAL == 0)) {
		bk_measure_grid(f, frame_tex, false);
	}

	const bool use_grid = f->grid_valid && f->grid_tex && !f->grid_reset;
	gs_effect_set_texture(f->p_image, frame_tex);
	gs_effect_set_texture(f->p_field, use_grid ? f->grid_tex : frame_tex);
	gs_effect_set_float(f->p_field_valid, use_grid ? 1.f : 0.f);
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
	gs_effect_set_float(f->p_vivid, f->vivid);
	gs_effect_set_float(f->p_psat, f->psat);
	gs_effect_set_float(f->p_prepair, f->particle_repair);
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
	obs_properties_add_bool(p, S_TRACK, obs_module_text("ScreenTracking"));

	obs_property_t *lm = obs_properties_add_list(p, S_LUMA, obs_module_text("LumaMode"), OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(lm, obs_module_text("LumaMode.Off"), 0);
	obs_property_list_add_int(lm, obs_module_text("LumaMode.Darks"), 1);
	obs_property_list_add_int(lm, obs_module_text("LumaMode.Brights"), 2);
	obs_properties_add_float_slider(p, S_LUMA_LEVEL, obs_module_text("LumaLevel"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, S_LUMA_SOFT, obs_module_text("LumaSoft"), 0.01, 1.0, 0.01);

	obs_properties_add_float_slider(p, S_DESPILL, obs_module_text("Despill"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, S_VIVID, obs_module_text("ParticleVivid"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, S_PSAT, obs_module_text("ParticleSat"), 0.0, 4.0, 0.01);
	obs_properties_add_float_slider(p, S_PREPAIR, obs_module_text("ParticleRepair"), 0.0, 1.0, 0.01);
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
	obs_property_list_add_int(o, obs_module_text("Output.Grid"), 4);
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
	obs_data_set_default_double(s, S_TDESAT, 0.0);
	obs_data_set_default_double(s, S_VIVID, 0.5);
	obs_data_set_default_double(s, S_PSAT, 1.0);
	obs_data_set_default_bool(s, S_TRACK, true);
	obs_data_set_default_double(s, S_PREPAIR, 0.5);
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
