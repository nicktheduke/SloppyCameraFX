// SPDX-License-Identifier: GPL-3.0-or-later
// SLOPPY CAMERA FX by nicktheduke: mod event API. Other mods trigger lens effects by name with a plain SKSE mod event,
// so they need none of our files to compile and nothing breaks when SLOPPY CAMERA FX is not installed:
//
//     SendModEvent("SloppyCameraFX_Trigger", "blood", 1.0)   ; one-shot effect, strength 0-1 (0 or omitted = 1)
//     SendModEvent("SloppyCameraFX_Hold", "frost", 0.8)      ; held effect at a level 0-1, 0 lets go
//     SendModEvent("SloppyCameraFX_ReleaseAll")              ; lets go of every held effect
//
// A hold lasts kHoldSeconds unless it is sent again, so a mod that forgets to let go (or gets uninstalled mid-effect)
// cannot leave something stuck on the lens. Everything goes through the user's own switches, strengths and
// photosensitive mode (ApiTrigger / ApiHold in the modules), and the "Other Mods" menu page can turn the lot off.
//
// Test without Papyrus: Data\SKSE\Plugins\SloppyCameraFX\ApiTest.txt, one "<seconds> <Trigger|Hold|ReleaseAll> [name]
// [value]" per line, is played after every save load through the same SKSE event source SendModEvent uses.
#include "Compat.h"
#include "Module.h"

#include "Combat.h"
#include "Weather.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr std::string_view kTriggerEvent = "SloppyCameraFX_Trigger";
	constexpr std::string_view kHoldEvent = "SloppyCameraFX_Hold";
	constexpr std::string_view kReleaseAllEvent = "SloppyCameraFX_ReleaseAll";
	constexpr double kHoldSeconds = 30.0;
	constexpr size_t kMaxQueued = 64;
	constexpr auto kTestFile = L"Data\\SKSE\\Plugins\\SloppyCameraFX\\ApiTest.txt";

	enum class Kind
	{
		kTrigger,
		kHold,
		kReleaseAll
	};

	struct Request
	{
		Kind kind = Kind::kTrigger;
		std::string name;
		float value = 1.0f;
	};

	struct TestLine
	{
		double at = 0.0;
		std::string event;
		std::string name;
		float value = 0.0f;
		bool sent = false;
	};

	double Now()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	bool SameName(std::string_view a_left, std::string_view a_right)
	{
		return a_left.size() == a_right.size() && _strnicmp(a_left.data(), a_right.data(), a_left.size()) == 0;
	}

	std::string Lower(std::string_view a_text)
	{
		std::string out(a_text);
		for (auto& c : out)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}

	class ApiModule final : public SloppyCameraFX::Module, public RE::BSTEventSink<SKSE::ModCallbackEvent>
	{
	public:
		const char* Name() const override { return "Other Mods"; }
		const char* LegacyName() const override { return "SLOPPY CAMERA FX Other Mods"; }  // never existed in Community Shaders
		bool IsEnabled() const override { return allow.load(std::memory_order_relaxed); }

		void OnDataLoaded() override
		{
			if (auto* source = SKSE::GetModCallbackEventSource()) {
				source->AddEventSink(this);
				logger::info("[api] listening for {} / {} / {}", kTriggerEvent, kHoldEvent, kReleaseAllEvent);
			} else {
				logger::warn("[api] SKSE mod event source not available: other mods cannot trigger effects");
			}
		}

		void OnGameLoaded() override
		{
			{
				std::scoped_lock lock(mutex);
				queued.clear();
				queued.push_back({ Kind::kReleaseAll, {}, 0.0f });  // nothing held carries over from the last save
			}
			LoadTest();
		}

		// Any thread the sender is on (Papyrus's SendModEvent arrives on the main thread).
		RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
		{
			if (!a_event || !a_event->eventName.c_str())
				return RE::BSEventNotifyControl::kContinue;
			const std::string_view event = a_event->eventName.c_str();
			Request request;
			if (SameName(event, kTriggerEvent))
				request.kind = Kind::kTrigger;
			else if (SameName(event, kHoldEvent))
				request.kind = Kind::kHold;
			else if (SameName(event, kReleaseAllEvent))
				request.kind = Kind::kReleaseAll;
			else
				return RE::BSEventNotifyControl::kContinue;
			received.fetch_add(1, std::memory_order_relaxed);
			if (!allow.load(std::memory_order_relaxed))
				return RE::BSEventNotifyControl::kContinue;
			request.name = Lower(a_event->strArg.c_str() ? a_event->strArg.c_str() : "");
			request.value = std::isfinite(a_event->numArg) ? a_event->numArg : 0.0f;
			std::scoped_lock lock(mutex);
			if (queued.size() < kMaxQueued)
				queued.push_back(std::move(request));
			return RE::BSEventNotifyControl::kContinue;
		}

		// Render thread, once per frame (also while switched off, so held effects are let go).
		void Update() override
		{
			std::vector<Request> batch;
			{
				std::scoped_lock lock(mutex);
				batch.swap(queued);
			}
			const double now = Now();
			if (!allow.load(std::memory_order_relaxed)) {
				batch.clear();
				ReleaseAll();
			}
			for (const auto& request : batch) {
				switch (request.kind) {
				case Kind::kTrigger:
					Trigger(request.name, request.value);
					break;
				case Kind::kHold:
					Hold(request.name, request.value, now);
					break;
				case Kind::kReleaseAll:
					ReleaseAll();
					break;
				}
			}
			for (auto it = holds.begin(); it != holds.end();) {
				if (now >= it->second) {
					Apply(it->first, 0.0f);
					it = holds.erase(it);
				} else {
					++it;
				}
			}
			TickTest(now);
		}

		void Render(uint32_t) override {}

		void DrawMenu() override
		{
			ImGui::Text("SLOPPY CAMERA FX - Other Mods by nicktheduke");
			ImGui::Separator();
			bool on = allow.load(std::memory_order_relaxed);
			if (ImGui::Checkbox("Let Other Mods Trigger Lens Effects", &on))
				allow.store(on, std::memory_order_relaxed);
			ImGui::Text("Other mods can ask for a lens effect by name (a blood splat, frost, a shake...).\n"
						"It always goes through your own switches, strengths and Photosensitive Safe Mode:\n"
						"an effect you turned off stays off. Untick this to ignore every request.");
			ImGui::Separator();
			ImGui::Text("Requests received this session: %u   shown: %u   unknown names: %u", received.load(std::memory_order_relaxed),
				shown.load(std::memory_order_relaxed), unknown.load(std::memory_order_relaxed));
			ImGui::Text("Held right now: %u", static_cast<unsigned>(heldCount.load(std::memory_order_relaxed)));
			ImGui::Separator();
			ImGui::Text("For mod authors (Papyrus, no dependency needed):\n"
						"  SendModEvent(\"SloppyCameraFX_Trigger\", \"blood\", 1.0)\n"
						"  SendModEvent(\"SloppyCameraFX_Hold\", \"frost\", 0.8)   ; 0 lets go, lasts 30 s unless sent again\n"
						"  SendModEvent(\"SloppyCameraFX_ReleaseAll\")");
			ImGui::Text("Trigger: blood, vampire_blood, ichor, oil, draugr_dust, bone_dust, sap, atronach_embers,\n"
						"  atronach_ice, atronach_sparks, ghost_mist, venom, web, ice, fire, shock, grit, ash_puff, steam,\n"
						"  stomp, shout, flash, splash, shake, shake_power, shake_bash, shake_arrow, shake_blocked,\n"
						"  shake_big, shake_stagger");
			ImGui::Text("Hold: poison, dragonfire, rime, shock_lasting, stamina_breath, frost, rain, sun, moon,\n"
						"  heat_haze, ash, dust, fog, embers, cold_breath, entry_fog");
		}

		void LoadSettings(nlohmann::json& a_json) override
		{
			bool on = true;
			if (a_json.is_object() && a_json.contains("AllowModEvents") && a_json["AllowModEvents"].is_boolean())
				on = a_json["AllowModEvents"].get<bool>();
			allow.store(on, std::memory_order_relaxed);
		}

		void SaveSettings(nlohmann::json& a_json) override { a_json["AllowModEvents"] = allow.load(std::memory_order_relaxed); }

	private:
		void Note(std::string_view a_what, std::string_view a_name, float a_value, bool a_known)
		{
			if (a_known) {
				shown.fetch_add(1, std::memory_order_relaxed);
				if (logged < 40) {
					++logged;
					logger::info("[api] {} {} {:.2f}", a_what, a_name, a_value);
				}
			} else {
				unknown.fetch_add(1, std::memory_order_relaxed);
				if (unknownLogged.size() < 32 && unknownLogged.insert(std::string(a_name)).second)
					logger::warn("[api] {}: no effect called '{}' (see the Other Mods menu page for the list)", a_what, a_name);
			}
		}

		void Trigger(const std::string& a_name, float a_strength)
		{
			const float strength = a_strength > 0.001f ? std::min(a_strength, 1.0f) : 1.0f;
			const bool known = globals::features::screenAction.ApiTrigger(a_name, strength) || globals::features::screenWeather.ApiTrigger(a_name, strength);
			Note("trigger", a_name, strength, known);
		}

		bool Apply(const std::string& a_name, float a_level)
		{
			return globals::features::screenAction.ApiHold(a_name, a_level) || globals::features::screenWeather.ApiHold(a_name, a_level);
		}

		void Hold(const std::string& a_name, float a_level, double a_now)
		{
			const float level = std::clamp(a_level, 0.0f, 1.0f);
			const bool known = Apply(a_name, level);
			Note("hold", a_name, level, known);
			if (!known)
				return;
			if (level > 0.001f)
				holds[a_name] = a_now + kHoldSeconds;
			else
				holds.erase(a_name);
			heldCount.store(holds.size(), std::memory_order_relaxed);
		}

		void ReleaseAll()
		{
			for (const auto& [name, until] : holds)
				Apply(name, 0.0f);
			holds.clear();
			heldCount.store(0, std::memory_order_relaxed);
		}

		// ---- ApiTest.txt: plays mod events on a timer after a save load (render thread reads, main thread sends) ----

		void LoadTest()
		{
			std::vector<TestLine> lines;
			std::ifstream file(kTestFile);
			std::string text;
			while (file && std::getline(file, text)) {
				if (text.empty() || text[0] == ';' || text[0] == '#')
					continue;
				std::istringstream in(text);
				TestLine line;
				if (!(in >> line.at >> line.event))
					continue;
				in >> line.name >> line.value;
				lines.push_back(std::move(line));
			}
			std::scoped_lock lock(mutex);
			test = std::move(lines);
			testStart = 0.0;
			if (!test.empty())
				logger::info("[api] ApiTest.txt: {} events will be sent after the load", test.size());
		}

		void TickTest(double a_now)
		{
			std::scoped_lock lock(mutex);
			if (test.empty() || globals::state->IsMainOrLoadingMenuOpen())
				return;
			if (testStart <= 0.0)
				testStart = a_now;
			for (auto& line : test) {
				if (line.sent || a_now - testStart < line.at)
					continue;
				line.sent = true;
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([event = "SloppyCameraFX_" + line.event, name = line.name, value = line.value]() {
						if (auto* source = SKSE::GetModCallbackEventSource()) {
							const SKSE::ModCallbackEvent modEvent{ event.c_str(), name.c_str(), value, nullptr };
							source->SendEvent(&modEvent);
							logger::info("[api] ApiTest sent {} '{}' {:.2f}", event, name, value);
						}
					});
				}
			}
		}

		std::atomic<bool> allow{ true };
		std::atomic<uint32_t> received{ 0 };
		std::atomic<uint32_t> shown{ 0 };
		std::atomic<uint32_t> unknown{ 0 };
		std::atomic<size_t> heldCount{ 0 };

		std::mutex mutex;  // queued + test
		std::vector<Request> queued;
		std::vector<TestLine> test;
		double testStart = 0.0;

		// render thread only
		std::unordered_map<std::string, double> holds;
		std::unordered_set<std::string> unknownLogged;
		int logged = 0;
	};

	struct Registrar
	{
		Registrar()
		{
			static ApiModule module;
			SloppyCameraFX::RegisterModule(&module, 300);  // after Weather (100) and Combat (200): draws nothing itself
		}
	} registrar;
}
