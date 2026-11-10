// SPDX-License-Identifier: GPL-3.0-or-later
// SLOPPY CAMERA FX by nicktheduke: recorded self test (see SelfTest.h).
#include "SelfTest.h"

#include "Combat.h"
#include "Weather.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string_view>

namespace SloppyCameraFX::SelfTest
{
	namespace
	{
		constexpr auto kFlag = L"Data\\SKSE\\Plugins\\SloppyCameraFX\\SelfTest.txt";
		constexpr double kStartDelay = 12.0;  // seconds after the load, so the load screen fade is over
		constexpr double kGap = 2.5;           // clean lens between steps

		struct Step
		{
			bool combat;
			std::string_view name;
			double seconds;
		};

		constexpr Step kSteps[] = {
			{ true, "blood", 9.0 },
			{ true, "ichor", 7.0 },
			{ true, "oil", 7.0 },
			{ true, "draugr", 7.0 },
			{ true, "ghost", 7.0 },
			{ true, "venom", 7.0 },
			{ true, "web", 9.0 },
			{ true, "ice", 7.0 },
			{ true, "fire", 7.0 },
			{ true, "grit", 6.0 },
			{ true, "ash_puff", 6.0 },
			{ true, "steam", 7.0 },
			{ true, "flash_slash", 5.0 },
			{ true, "shout", 6.0 },
			{ true, "stomp", 6.0 },
			{ true, "hit_shake", 8.0 },
			{ true, "poison", 10.0 },
			{ true, "dragonfire", 9.0 },
			{ true, "rime", 9.0 },
			{ true, "shock_hit", 7.0 },
			{ true, "shock_lasting", 8.0 },
			{ true, "stamina_breath", 10.0 },
			{ false, "frost", 15.0 },
			{ false, "rain", 9.0 },
			{ false, "water_sheet", 7.0 },
			{ false, "sun", 8.0 },
			{ false, "moon", 8.0 },
			{ false, "heat_haze", 8.0 },
			{ false, "ash", 8.0 },
			{ false, "dust", 8.0 },
			{ false, "fog", 8.0 },
			{ false, "embers", 8.0 },
			{ false, "cold_breath", 10.0 },
			{ false, "entry_fog", 8.0 },
		};
		constexpr int kCount = static_cast<int>(std::size(kSteps));

		// weather-only take: with SelfTestWeather.txt present, every loading screen (a save load or coc) plays only the
		// weather steps, 10 s after the loading screen closes, right where you arrive
		constexpr auto kWeatherFlag = L"Data\\SKSE\\Plugins\\SloppyCameraFX\\SelfTestWeather.txt";
		std::atomic<bool> weatherMode{ false };
		constexpr double kWeatherDelay = 10.0;
		bool begun = false;

		int FirstWeather()
		{
			for (int i = 0; i < kCount; ++i)
				if (!kSteps[i].combat) return i;
			return kCount;
		}

		std::atomic<bool> armed{ false };
		bool running = false;
		double startAt = 0.0;
		int index = -1;
		bool inStep = false;
		double stepStart = 0.0;
		float lastT = -1.0f;

		double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

		void Call(const Step& a_step, float a_t, float a_prev, bool a_end)
		{
			if (a_step.combat)
				globals::features::screenAction.SelfTestTick(a_step.name, a_t, a_prev, a_end);
			else
				globals::features::screenWeather.SelfTestTick(a_step.name, a_t, a_prev, a_end);
		}
	}

	namespace
	{
		// weather mode: arms on every loading screen close (save load or coc)
		struct LoadingSink final : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && !a_event->opening && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
					std::error_code error;
					if (std::filesystem::exists(kWeatherFlag, error)) {
						weatherMode.store(true, std::memory_order_release);
						armed.store(true, std::memory_order_release);
						logger::info("[selftest] weather mode armed: {} weather steps start {:.0f} s after this loading screen", kCount - FirstWeather(), kWeatherDelay);
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		static LoadingSink sink;
		static bool done = false;
		if (done)
			return;
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&sink);
			done = true;
		}
	}

	void OnGameLoaded()
	{
		std::error_code error;
		if (std::filesystem::exists(kFlag, error)) {
			armed.store(true, std::memory_order_release);
			logger::info("[selftest] armed: {} steps start {:.0f} s after the load", kCount, kStartDelay);
		}
	}

	void Tick()
	{
		if (armed.exchange(false, std::memory_order_acq_rel)) {
			if (running && inStep)
				Call(kSteps[index], 0.0f, 0.0f, true);
			running = true;
			const bool weather = weatherMode.exchange(false, std::memory_order_acq_rel);
			startAt = Now() + (weather ? kWeatherDelay : kStartDelay);
			index = weather ? FirstWeather() - 1 : -1;
			begun = false;
			inStep = false;
		}
		if (!running)
			return;
		const double now = Now();
		if (!inStep) {
			const double nextAt = !begun ? startAt : stepStart + kSteps[index].seconds + kGap;
			if (now < nextAt)
				return;
			if (++index >= kCount) {
				running = false;
				logger::info("[selftest] DONE");
				return;
			}
			inStep = true;
			begun = true;
			stepStart = now;
			lastT = -1.0f;
			logger::info("[selftest] BEGIN {} ({}/{}, {:.1f} s)", kSteps[index].name, index + 1, kCount, kSteps[index].seconds);
		}
		const auto& step = kSteps[index];
		const float t = static_cast<float>(now - stepStart);
		if (t >= step.seconds) {
			Call(step, t, lastT, true);
			inStep = false;
			stepStart = now - step.seconds;  // gap counts from here
			logger::info("[selftest] END {}", step.name);
			return;
		}
		Call(step, t, lastT, false);
		lastT = t;
	}
}
