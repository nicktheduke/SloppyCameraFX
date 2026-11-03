// SPDX-License-Identifier: GPL-3.0-or-later
// nicktheduke: state shared by the Weather and Combat modules of SLOPPY CAMERA FX so the two lens effects never fight.
// Either feature works alone; when both run they share one breathing rhythm and one lens budget.
#pragma once

#include <atomic>
#include <cmath>

namespace LensShared
{
	/** @brief Total lens coverage (0-1) Screen Weather and Screen Action together aim to stay under. */
	inline constexpr float kLensBudget = 0.32f;
	/** @brief Radius (screen fraction from the centre) that stays clear of opaque lens effects. */
	inline constexpr float kSafeZone = 0.2f;

	struct State
	{
		// Written by Screen Weather each frame it renders (0 when off).
		std::atomic<float> weatherCoverage{ 0.0f };  // rough share of the lens Screen Weather is covering
		std::atomic<float> weatherFrost{ 0.0f };     // current frost amount on the lens
		std::atomic<float> coldBreathVisible{ 0.0f };  // how strongly the cold-breath vapor is showing
		// Written by Screen Action each frame it renders (0 when off).
		std::atomic<float> actionCoverage{ 0.0f };

		// One breathing clock for both features.
		std::atomic<float> exertion{ 0.0f };     // 0 calm .. 1 completely winded
		std::atomic<uint64_t> breathFrame{ 0 };  // frame the clock last advanced
		float breathTimer = 0.0f;                // seconds until the next exhale
		float exhaleAge = 99.0f;                 // seconds since the last exhale began
		float exhaleStrength = 0.0f;             // how big the last exhale was (0-1)
		uint32_t exhaleCount = 0;
		float baseBreathsPerMinute = 14.0f;      // calm rate (Cold Breath NG's ini sets this when installed)
	};

	inline State& Get()
	{
		static State state;
		return state;
	}

	inline float Clamp01(float a_value) { return std::isfinite(a_value) ? (a_value < 0.0f ? 0.0f : (a_value > 1.0f ? 1.0f : a_value)) : 0.0f; }

	/**
	 * @brief How winded the actor is (0 calm .. 1 spent), from stamina, sprinting, combat and power attacks.
	 * Reads plain stamina, so any stamina system works; Blade and Blunt drains stamina much harder on every
	 * swing, so with it loaded only deeper drops count as winded. Main thread only.
	 */
	inline float ComputeExertion(RE::Actor* a_actor, bool a_bladeAndBlunt)
	{
		if (!a_actor)
			return 0.0f;
		float exertion = 0.0f;
		if (auto* values = a_actor->AsActorValueOwner()) {
			const float maximum = values->GetPermanentActorValue(RE::ActorValue::kStamina);
			const float current = values->GetActorValue(RE::ActorValue::kStamina);
			if (std::isfinite(maximum) && maximum > 1.0f && std::isfinite(current)) {
				const float pct = Clamp01(current / maximum);
				const float lo = a_bladeAndBlunt ? 0.08f : 0.15f;
				const float hi = a_bladeAndBlunt ? 0.55f : 0.7f;
				const float t = Clamp01((pct - lo) / (hi - lo));
				exertion = 1.0f - t * t * (3.0f - 2.0f * t);
			}
		}
		if (auto* state = a_actor->AsActorState(); state && state->IsSprinting())
			exertion = exertion > 0.6f ? exertion : 0.6f;
		if (a_actor->IsInCombat())
			exertion = exertion > 0.3f ? exertion : 0.3f;
		if (a_actor->IsPowerAttacking())
			exertion = exertion > 0.75f ? exertion : 0.75f;
		return Clamp01(exertion);
	}

	/**
	 * @brief Advances the shared breathing clock once per frame, whichever feature calls first.
	 * Exertion (stamina, sprinting, combat) only changes the rhythm and size of each breath.
	 */
	inline void AdvanceBreath(uint64_t a_frame, float a_delta, float a_exertion)
	{
		auto& s = Get();
		uint64_t previous = s.breathFrame.load(std::memory_order_relaxed);
		if (previous == a_frame || !s.breathFrame.compare_exchange_strong(previous, a_frame))
			return;
		if (!std::isfinite(a_delta) || a_delta < 0.0f)
			a_delta = 0.0f;
		const float exertion = Clamp01(a_exertion);
		s.exertion.store(exertion, std::memory_order_relaxed);
		s.exhaleAge += a_delta;
		s.breathTimer -= a_delta;
		if (s.breathTimer <= 0.0f) {
			const float calmPeriod = 60.0f / (s.baseBreathsPerMinute > 4.0f ? s.baseBreathsPerMinute : 14.0f);
			const float period = calmPeriod + (1.3f - calmPeriod) * exertion;  // winded: about 46 breaths a minute
			// Natural variation: no two breaths exactly the same length.
			++s.exhaleCount;
			const float jitter = 0.85f + 0.3f * std::fmod(static_cast<float>(s.exhaleCount) * 0.6180339f, 1.0f);
			s.breathTimer = period * jitter;
			s.exhaleAge = 0.0f;
			s.exhaleStrength = 0.55f + 0.45f * exertion;
		}
	}
}
