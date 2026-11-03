// SPDX-License-Identifier: GPL-3.0-or-later
// nicktheduke: game-data stand-ins for the Bottled Shaders pieces Screen Weather used to lean on
// (SkySync, ProceduralSun, Moon utils). Works the same under vanilla, Community Shaders and Bottled Shaders.
#pragma once

#include "Compat.h"

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>

namespace WeatherCompat
{
	enum class Caster : uint8_t { Sun = 0, Masser = 1, Secunda = 2 };

	inline RE::NiPoint3 Zenith() { return { 0.0f, 0.0f, 1.0f }; }

	inline RE::NiPoint3 MoonDirection(const RE::Moon* a_moon)
	{
		if (!a_moon || !a_moon->root)
			return Zenith();
		auto dir = a_moon->root->world.rotate.GetVectorY();
		return dir.Unitize() > FLT_EPSILON ? dir : Zenith();
	}

	/** @brief World-space direction from the camera toward the sun or a moon (unit length, zenith if unknown). */
	inline RE::NiPoint3 CelestialDirection(const RE::Sky* a_sky, Caster a_caster)
	{
		if (!a_sky || !a_sky->root)
			return Zenith();
		RE::NiPoint3 dir{};
		if (a_caster == Caster::Sun) {
			// The sun's local translate is set every frame; the world one can lag a frame, which is invisible here.
			if (a_sky->sun && a_sky->sun->root)
				dir = a_sky->sun->root->world.translate - a_sky->root->world.translate;
		} else {
			return MoonDirection(a_caster == Caster::Masser ? a_sky->masser : a_sky->secunda);
		}
		return dir.Unitize() > FLT_EPSILON ? dir : Zenith();
	}

	/** @brief How visible the sun disc is (0-1), from the sky shader's own fade. */
	inline float SunVisibility(const RE::Sky* a_sky)
	{
		const auto* sun = a_sky ? a_sky->sun : nullptr;
		if (!sun || !sun->root || !sun->sunBaseNode || !sun->sunBase)
			return 0.0f;
		if (sun->root->GetFlags().any(RE::NiAVObject::Flag::kHidden) || sun->sunBaseNode->GetFlags().any(RE::NiAVObject::Flag::kHidden))
			return 0.0f;
		const auto* prop = skyrim_cast<RE::BSSkyShaderProperty*>(sun->sunBase->GetGeometryRuntimeData().shaderProperty.get());
		if (!prop)
			return 0.0f;
		const float alpha = prop->kBlendColor.alpha;
		return alpha > 0.0f ? std::min(alpha, 1.0f) : 0.0f;
	}

	/** @brief Moon phase from the moon texture name (vanilla and Moon and Stars naming). */
	inline RE::Moon::Phases::Phase PhaseFromTexture(const char* a_name)
	{
		using P = RE::Moon::Phases::Phase;
		if (!a_name)
			return P::kFull;
		std::string lower(a_name);
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		static const std::pair<const char*, P> lookup[] = {
			{ "full", P::kFull },
			{ "three_wan", P::kWaningGibbous },
			{ "half_wan", P::kWaningQuarter },
			{ "one_wan", P::kWaningCrescent },
			{ "new", P::kNewMoon },
			{ "one_wax", P::kWaxingCrescent },
			{ "half_wax", P::kWaxingQuarter },
			{ "three_wax", P::kWaxingGibbous },
		};
		for (const auto& [suffix, id] : lookup)
			if (lower.find(suffix) != std::string::npos)
				return id;
		return P::kFull;
	}
}

namespace WeatherCompat
{
	/** @brief Wide-char path to UTF-8 for log lines. */
	inline std::string Narrow(const wchar_t* a_text)
	{
		if (!a_text)
			return {};
		const int length = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
		if (length <= 1)
			return {};
		std::string result(static_cast<size_t>(length - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, a_text, -1, result.data(), length, nullptr, nullptr);
		return result;
	}

	/** @brief Camera position in world space (the main world camera). */
	inline bool CameraPosition(RE::NiPoint3& o_position)
	{
		auto* camera = RE::Main::WorldRootCamera();
		if (!camera)
			return false;
		o_position = camera->world.translate;
		return std::isfinite(o_position.x) && std::isfinite(o_position.y) && std::isfinite(o_position.z);
	}

	/** @brief Screen uv of a direction (sun, moon) seen from the camera. False when it is behind the camera. */
	inline bool DirectionToScreen(const RE::NiPoint3& a_direction, float& o_u, float& o_v)
	{
		RE::NiPoint3 eye;
		if (!CameraPosition(eye))
			return false;
		return Camera::WorldToScreen(eye + a_direction * 100000.0f, o_u, o_v);
	}

	/**
	 * @brief Device depth of a world point, the same way the GPU wrote the depth buffer (unjittered main camera view-proj
	 * with the engine's camera-relative position adjust). The engine stores the matrix for either row- or column-vector
	 * maths depending on the build, so the layout is detected from the projection. False when unavailable or when the
	 * renderer's current camera is not the main camera (then the caller skips the depth test).
	 */
	inline bool WorldDepth(const RE::NiPoint3& a_world, float& o_depth)
	{
		auto* shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
		if (!shadowState)
			return false;
		auto& runtime = shadowState->GetRuntimeData();
		const auto& view = runtime.cameraData.getEye();
		const RE::NiPoint3 adjust = runtime.posAdjust.getEye();
		RE::NiPoint3 eye;
		if (!CameraPosition(eye) || eye.GetDistance(adjust) > 4000.0f)
			return false;
		const float* proj = reinterpret_cast<const float*>(&view.projMatrixUnjittered);
		const float* m = reinterpret_cast<const float*>(&view.viewProjMatrixUnjittered);
		const bool rowVector = std::abs(std::abs(proj[2 * 4 + 3]) - 1.0f) < 1e-3f;
		const bool colVector = std::abs(std::abs(proj[3 * 4 + 2]) - 1.0f) < 1e-3f;
		if (rowVector == colVector)
			return false;
		const float v[4]{ a_world.x - adjust.x, a_world.y - adjust.y, a_world.z - adjust.z, 1.0f };
		auto at = [&](int r, int c) { return m[r * 4 + c]; };
		float z = 0.0f, w = 0.0f;
		for (int i = 0; i < 4; ++i) {
			z += rowVector ? v[i] * at(i, 2) : at(2, i) * v[i];
			w += rowVector ? v[i] * at(i, 3) : at(3, i) * v[i];
		}
		if (!(w > 1.0f) || !std::isfinite(z))
			return false;
		o_depth = z / w;
		return true;
	}
}
