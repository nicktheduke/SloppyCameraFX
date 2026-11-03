// SLOPPY CAMERA FX by nicktheduke.
// One plugin, switchable modules. Each module (Combat, Weather) implements this and registers itself with core;
// core owns the hook, the menu, the settings file and the shared lens state (LensShared.h).
#pragma once

#include <nlohmann/json.hpp>

namespace SloppyCameraFX
{
	class Module
	{
	public:
		virtual ~Module() = default;

		/** @brief Key in Settings.json and the menu section name ("Combat", "Weather"). */
		virtual const char* Name() const = 0;
		/** @brief The section name this module had inside Community Shaders / Bottled Shaders, for the one-time import. */
		virtual const char* LegacyName() const = 0;
		/** @brief Off: no events reacted to, nothing updated or drawn. */
		virtual bool IsEnabled() const = 0;

		virtual void OnPostPostLoad() {}
		virtual void OnDataLoaded() {}
		virtual void OnGameLoaded() {}
		/** @brief Render thread, once the D3D11 device exists (first frame). */
		virtual void SetupResources() {}
		/** @brief Render thread, once per frame before Render. Called even while disabled, so the module can tidy up
		 * (restore game settings, zero its LensShared coverage) and return early. */
		virtual void Update() {}
		/** @brief Render thread, right after the game's tone mapping (and Community Shaders' passes), before the HUD. */
		virtual void Render(uint32_t a_outputTarget) = 0;
		/** @brief Inside the SKSE Menu Framework window. */
		virtual void DrawMenu() = 0;

		virtual void LoadSettings(nlohmann::json& a_json) = 0;
		virtual void SaveSettings(nlohmann::json& a_json) = 0;
	};

	/** @brief Called by each module's translation unit at static init time. Order of registration is draw order. */
	void RegisterModule(Module* a_module, int a_drawOrder);
}
