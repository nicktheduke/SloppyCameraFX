// SLOPPY CAMERA FX by nicktheduke.
// One SKSE plugin, switchable modules (Combat, Weather). Draws right after the game's tone mapping and before the HUD
// by chaining onto the same call Community Shaders and Bottled Shaders use, so it works with either one or neither.
#include "Compat.h"
#include "LensShared.h"
#include "Module.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include <psapi.h>

namespace
{
	constexpr auto kSettingsPath = L"Data\\SKSE\\Plugins\\SloppyCameraFX\\Settings.json";
	constexpr auto kLegacySettingsPath = L"Data\\SKSE\\Plugins\\CommunityShaders\\SettingsUser.json";
	constexpr auto kVersionText = "1.0.0";

	std::multimap<int, SloppyCameraFX::Module*>& Modules()
	{
		static std::multimap<int, SloppyCameraFX::Module*> modules;
		return modules;
	}

	// -----------------------------------------------------------------------------------------------------------------
	// Settings: one json file, one object per module. First run imports the module's section from Community Shaders'
	// / Bottled Shaders' SettingsUser.json, so nothing you already tuned there is lost.

	std::mutex settingsMutex;
	std::string lastSaved;
	double lastSaveCheck = 0.0;

	double Now()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	nlohmann::json ReadJson(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path);
		if (!file)
			return nlohmann::json();
		try {
			return nlohmann::json::parse(file, nullptr, true, true);
		} catch (const std::exception& e) {
			logger::warn("[core] could not read {}: {}", a_path.string(), e.what());
			return nlohmann::json();
		}
	}

	void LoadSettings()
	{
		std::scoped_lock lock(settingsMutex);
		auto settings = ReadJson(kSettingsPath);
		nlohmann::json legacy;
		bool legacyRead = false;
		for (auto& [order, module] : Modules()) {
			nlohmann::json section;
			if (settings.is_object() && settings.contains(module->Name()) && settings[module->Name()].is_object()) {
				section = settings[module->Name()];
			} else {
				if (!legacyRead) {
					legacy = ReadJson(kLegacySettingsPath);
					legacyRead = true;
				}
				if (legacy.is_object() && legacy.contains(module->LegacyName()) && legacy[module->LegacyName()].is_object()) {
					section = legacy[module->LegacyName()];
					logger::info("[core] {}: imported your settings from Community Shaders' '{}' section", module->Name(), module->LegacyName());
				} else {
					section = nlohmann::json::object();
					logger::info("[core] {}: no saved settings, using defaults", module->Name());
				}
			}
			try {
				module->LoadSettings(section);
			} catch (const std::exception& e) {
				logger::warn("[core] {}: settings unreadable ({}), using defaults", module->Name(), e.what());
				nlohmann::json empty = nlohmann::json::object();
				module->LoadSettings(empty);
			}
		}
	}

	/** @brief Writes Settings.json when anything changed (checked at most twice a second while the menu is open). */
	void SaveSettingsIfChanged(bool a_force)
	{
		const double now = Now();
		if (!a_force && now - lastSaveCheck < 0.5)
			return;
		lastSaveCheck = now;
		std::scoped_lock lock(settingsMutex);
		nlohmann::json all = nlohmann::json::object();
		for (auto& [order, module] : Modules()) {
			nlohmann::json section;
			module->SaveSettings(section);
			all[module->Name()] = section;
		}
		std::string text = all.dump(1, '\t');
		if (text == lastSaved)
			return;
		std::error_code error;
		std::filesystem::create_directories(std::filesystem::path(kSettingsPath).parent_path(), error);
		std::ofstream out(kSettingsPath, std::ios::trunc);
		if (out) {
			out << text;
			lastSaved = std::move(text);
		}
	}

	// -----------------------------------------------------------------------------------------------------------------
	// Menu state (main, loading, map, stats) for the render thread.

	class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuSink* GetSingleton()
		{
			static MenuSink singleton;
			return &singleton;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (!a_event)
				return RE::BSEventNotifyControl::kContinue;
			auto& s = globals::gameState;
			const std::string_view name = a_event->menuName.c_str() ? a_event->menuName.c_str() : "";
			if (name == RE::MainMenu::MENU_NAME)
				s.mainMenu = a_event->opening;
			else if (name == RE::LoadingMenu::MENU_NAME) {
				s.loadingMenu = a_event->opening;
				s.isLoadingMenuOpen = a_event->opening;
			} else if (name == RE::MapMenu::MENU_NAME)
				s.isMapMenuOpen = a_event->opening;
			else if (name == RE::StatsMenu::MENU_NAME)
				s.isStatsMenuOpen = a_event->opening;
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// -----------------------------------------------------------------------------------------------------------------
	// The draw hook: the call to the HDR tone mapping pass in the image space manager. Community Shaders and Bottled
	// Shaders replace this same call; installing after them wraps theirs, so their pass runs first and ours after.

	uintptr_t HookSite(bool a_second)
	{
		return REL::RelocationID(99023, 105674).address() + (a_second ? REL::Relocate(0x230, 0x178) : REL::Relocate(0x1EA, 0x178));
	}

	bool resourcesReady = false;
	double lastUpdate = 0.0;
	bool firstDrawLogged = false;

	void AfterTonemap(uint32_t a_output);

	struct TonemapPass
	{
		static void thunk(RE::ImageSpaceManager* a1, RE::ImageSpaceEffect* a2, uint32_t a_input, uint32_t a_output, RE::ImageSpaceShaderParam* a5)
		{
			func(a1, a2, a_input, a_output, a5);
			AfterTonemap(a_output);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief SE only: the engine's second call site, with its own original (it may differ from the first's). */
	struct TonemapPassSecond
	{
		static void thunk(RE::ImageSpaceManager* a1, RE::ImageSpaceEffect* a2, uint32_t a_input, uint32_t a_output, RE::ImageSpaceShaderParam* a5)
		{
			func(a1, a2, a_input, a_output, a5);
			AfterTonemap(a_output);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	uint64_t lastRenderedFrame = UINT64_MAX;

	void AfterTonemap(uint32_t a_output)
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return;
		if (!resourcesReady) {
			globals::d3d::device = reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder);
			globals::d3d::context = reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
			if (!globals::d3d::device || !globals::d3d::context)
				return;
			for (auto& [order, module] : Modules())
				module->SetupResources();
			resourcesReady = true;
			logger::info("[core] D3D11 ready, modules set up");
		}
		// Once per frame even where the engine calls the pass twice.
		const double now = Now();
		if (now - lastUpdate > 0.002) {
			lastUpdate = now;
			globals::gameState.frameCountAtomic.fetch_add(1, std::memory_order_relaxed);
			// Every module, on or off: Update is where a module that was just switched off gives back what it borrowed
			// (Combat restores the game's own screen blood there) and clears its shared lens coverage.
			for (auto& [order, module] : Modules())
				module->Update();
		}
		// Draw once per frame even when SE runs the pass from both call sites.
		const uint64_t frame = globals::gameState.frameCountAtomic.load(std::memory_order_relaxed);
		if (frame == lastRenderedFrame)
			return;
		lastRenderedFrame = frame;
		for (auto& [order, module] : Modules())
			if (module->IsEnabled())
				module->Render(a_output);
		if (!firstDrawLogged) {
			firstDrawLogged = true;
			logger::info("[core] first frame through the hook (output target {})", a_output);
		}
	}

	uintptr_t CallTarget(uintptr_t a_site)
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>(a_site);
		if (bytes[0] != 0xE8)
			return 0;
		return a_site + 5 + *reinterpret_cast<const int32_t*>(a_site + 1);
	}

	/** @brief Follows an SKSE trampoline stub (FF 25 00000000 <abs64>) to where it really goes. */
	uintptr_t ResolveStub(uintptr_t a_address)
	{
		if (!a_address)
			return 0;
		const auto* bytes = reinterpret_cast<const uint8_t*>(a_address);
		if (bytes[0] == 0xFF && bytes[1] == 0x25 && *reinterpret_cast<const int32_t*>(a_address + 2) == 0)
			return *reinterpret_cast<const uintptr_t*>(a_address + 6);
		return a_address;
	}

	bool InModule(uintptr_t a_address, const wchar_t* a_module)
	{
		HMODULE module = GetModuleHandleW(a_module);
		if (!module || !a_address)
			return false;
		MODULEINFO info{};
		if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
			return false;
		const auto base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
		return a_address >= base && a_address < base + info.SizeOfImage;
	}

	bool hookInstalled = false;

	/** @brief True once installed. Waits (returns false) while Community Shaders is loaded but has not hooked yet. */
	bool InstallHook(bool a_force)
	{
		if (hookInstalled)
			return true;
		const auto site = HookSite(false);
		const auto target = CallTarget(site);
		if (!target) {
			logger::critical("[core] the tone mapping call is not where it should be (game version?): nothing will draw");
			return true;
		}
		const bool csLoaded = GetModuleHandleW(L"CommunityShaders.dll") != nullptr;
		const bool csHooked = InModule(target, L"CommunityShaders.dll") || InModule(ResolveStub(target), L"CommunityShaders.dll");
		if (csLoaded && !csHooked && !a_force) {
			logger::info("[core] Community Shaders has not hooked the tone mapping pass yet; waiting for it");
			return false;
		}
		auto& trampoline = SKSE::GetTrampoline();
		TonemapPass::func = trampoline.write_call<5>(site, TonemapPass::thunk);
		if (REL::Module::IsSE()) {
			const auto second = HookSite(true);
			if (CallTarget(second))
				TonemapPassSecond::func = trampoline.write_call<5>(second, TonemapPassSecond::thunk);
		}
		hookInstalled = true;
		logger::info("[core] drawing after the tone mapping pass ({})", csHooked ? "chained after Community Shaders / Bottled Shaders" : (csLoaded ? "Community Shaders loaded but not hooked here" : "no Community Shaders"));
		return true;
	}

	// -----------------------------------------------------------------------------------------------------------------
	// Menu (SKSE Menu Framework): "SLOPPY CAMERA FX" with one page per module.

	template <size_t I>
	void __stdcall RenderModulePage()
	{
		size_t index = 0;
		for (auto& [order, module] : Modules()) {
			if (index++ == I) {
				module->DrawMenu();
				break;
			}
		}
		SaveSettingsIfChanged(false);
	}

	void RegisterMenu()
	{
		if (!GetModuleHandleW(L"SKSEMenuFramework")) {
			logger::info("[core] SKSE Menu Framework not installed: settings are read from Settings.json only");
			return;
		}
		SKSEMenuFramework::SetSection("SLOPPY CAMERA FX");
		static constexpr SKSEMenuFramework::Model::RenderFunction pages[] = { &RenderModulePage<0>, &RenderModulePage<1>, &RenderModulePage<2>, &RenderModulePage<3> };
		size_t index = 0;
		for (auto& [order, module] : Modules()) {
			if (index >= std::size(pages))
				break;
			SKSEMenuFramework::AddSectionItem(module->Name(), pages[index++]);
		}
		logger::info("[core] menu registered in SKSE Menu Framework ({} pages)", index);
	}

	// -----------------------------------------------------------------------------------------------------------------

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostPostLoad:
			for (auto& [order, module] : Modules())
				module->OnPostPostLoad();
			InstallHook(false);
			break;
		case SKSE::MessagingInterface::kInputLoaded:
			InstallHook(true);
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			if (auto* ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
				// Menus already open before the sink existed (the main menu, usually).
				auto& s = globals::gameState;
				s.mainMenu = ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
				s.loadingMenu = s.isLoadingMenuOpen = ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
				s.isMapMenuOpen = ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
				s.isStatsMenuOpen = ui->IsMenuOpen(RE::StatsMenu::MENU_NAME);
			}
			for (auto& [order, module] : Modules())
				module->OnDataLoaded();
			RegisterMenu();
			SaveSettingsIfChanged(true);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			for (auto& [order, module] : Modules())
				module->OnGameLoaded();
			SaveSettingsIfChanged(true);
			break;
		case SKSE::MessagingInterface::kSaveGame:
			SaveSettingsIfChanged(true);  // catches tweaks made in the last half second before the menu closed
			break;
		default:
			break;
		}
	}

	void InitLog()
	{
		auto path = logger::log_directory();
		if (!path)
			return;
		*path /= "SloppyCameraFX.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}
}

namespace SloppyCameraFX
{
	void RegisterModule(Module* a_module, int a_drawOrder)
	{
		Modules().emplace(a_drawOrder, a_module);
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 1, 0, 0, 0 },
	.Name = "SloppyCameraFX"sv,
	.Author = "nicktheduke"sv,
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, false);
	InitLog();
	logger::info("SLOPPY CAMERA FX {} by nicktheduke", kVersionText);
	SKSE::AllocTrampoline(64);
	for (auto& [order, module] : Modules())
		logger::info("[core] module: {}", module->Name());
	LoadSettings();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
