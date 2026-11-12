// SPDX-License-Identifier: GPL-3.0-or-later
// nicktheduke: lens frost and lens water, driven by survival cold, weather and location.
#pragma once

#include "Compat.h"
#include "Module.h"

#include <array>
#include <chrono>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include <future>

#include <unordered_set>

/** @brief Frost creeping in from the screen edges and water on the lens (rain drops, a water sheet after swimming). */
struct ScreenWeather
{
	struct Settings
	{
		bool Enabled = true;

		// Frost look
		bool FrostEnabled = true;
		float FrostOpacity = 1.0f;
		float FrostCoverage = 0.8f;
		float FrostScale = 1.0f;
		float FrostRefraction = 0.0f;  // off by default (the frost does not bend the view); raise it to let the ice act like a lens
		float FrostBlur = 0.0f;
		std::array<float, 3> FrostTint{ 0.84f, 0.92f, 1.0f };
		float FrostGrowSeconds = 30.0f;
		float FrostMeltSeconds = 10.0f;
		float FrostEdgeSoftness = 0.6f;
		float FrostGlow = 1.0f;
		float FrostDayIntensity = 1.0f;
		float FrostNightIntensity = 0.6f;  // nicktheduke: was 0.1, which left the frost almost invisible at night
		float FrostBreathRate = 0.12f;
		float FrostBreathDepth = 0.2f;
		// nicktheduke: relief and cold tint so the ice reads against white snow and sky (0 = additive only, as before)
		float FrostContrast = 1.0f;
		// nicktheduke: frost shape, 0 = hugs the frame (rectangle) .. 1 = round. Roundness also grows with the frost.
		float FrostShape = 0.35f;

		// Frost drivers
		bool UseSurvival = true;
		float SurvivalAmbientWeight = 0.6f;
		float WeatherSnowFrost = 0.8f;
		float BlizzardFrost = 1.0f;
		int BlizzardWindSpeed = 150;
		float SnowGroundFrost = 0.25f;
		float WarmthProtection = 0.6f;

		// Water
		bool WaterEnabled = true;
		float RainDropAmount = 1.0f;
		float RainDropSize = 1.0f;
		float RainLookUpBias = 0.7f;
		float DropLifetimeSeconds = 4.0f;
		float WaterRefraction = 1.0f;
		float SurfaceSheetAmount = 1.0f;
		float SheetDrySeconds = 5.0f;

		// nicktheduke: low sun shining into the lens on clear days
		bool SunLensEnabled = true;     // master switch for everything below
		bool SunGlintsEnabled = true;   // frost crystals and water on the lens catch the sun
		float SunGlintStrength = 1.0f;
		bool LensDirtEnabled = true;    // dust and smudges that only show in sun glare
		float LensDirtAmount = 0.35f;
		float SunLensStrength = 1.0f;
		float SunMaxHeight = 0.6f;      // sun elevation (sine) where the effect has faded out; low sun is strongest
		float SunCloudyAmount = 0.35f;  // how much cloudy (not rainy) weathers still allow it
		bool MoonLensEnabled = true;    // nicktheduke: a full or nearly full moon on a clear night, much gentler than the sun
		float MoonLensStrength = 1.0f;

		// nicktheduke: atmosphere layers (lower priority than frost and water, in this order)
		bool HeatHazeEnabled = true;  // fires, forges, lava; fumaroles and hot springs more lightly
		float HeatHazeStrength = 1.0f;
		float HeatRadiusScale = 1.0f;
		bool AshEnabled = true;  // ash weathers (Solstheim, ash pits) settle grey ash on the lens edges
		float AshStrength = 1.0f;
		bool EmbersEnabled = true;  // glowing embers drifting past in ash and burning weathers
		float EmberStrength = 1.0f;
		bool DustEnabled = true;  // dust storms, Helgen/Alduin smoke, and dry gales
		float DustStrength = 1.0f;
		int DustWindSpeed = 200;  // dry weathers at or above this wind speed (0-255) raise dust
		bool FogLensEnabled = true;  // halos around lights and condensation in dense fog
		float FogHaloStrength = 1.0f;
		float FogCondensation = 0.5f;

		// nicktheduke: temperature, breath and heat shimmer (own JSON group: the settings macro tops out at 64 fields)
		struct Temperature
		{
			bool ThawEnabled = true;            // frost near a fire or lava melts into drips instead of just fading
			float ThawStrength = 1.0f;
			bool WarmAreasBlockFrost = true;    // Survival Mode Improved: warm areas keep snowfall frost light
			bool ColdBreathEnabled = true;      // first person: your breath shows as vapor in the cold
			float ColdBreathStrength = 1.0f;
			float ColdBreathInterval = 6.0f;     // nicktheduke: shortest time between two cold-breath puffs (seconds)
			float ColdBreathHeight = 0.12f;      // nicktheduke: how far up the screen the vapor rises (screen heights above the old spot)
			bool HideColdBreathNGFirstPerson = true;  // Cold Breath NG: hide its puff on you in first person (third person and NPCs keep it)
			bool EntryFogEnabled = true;        // stepping into a warm interior from the cold fogs the lens
			float EntryFogStrength = 1.0f;
			float EntryFogSeconds = 10.0f;
			bool HeatShimmerEnabled = true;     // the air just above lava and big fires bends and rises (replaces Hot Lava)
			float HeatShimmerStrength = 1.0f;
			float HeatShimmerRange = 1.0f;
		} Temp;
	};

	void SetupResources();
	void DataLoaded();
	void GameLoaded();
	void Reset();
	void DrawSettings();
	void SaveSettings(json& o_json);
	void LoadSettings(json& o_json);
	void RestoreDefaultSettings();
	/** @brief nicktheduke: recorded self test. Called every frame while step a_name runs (a_prev = a_t of the last call,
	 * -1 on the first), and once with a_end = true when it stops. */
	void SelfTestTick(std::string_view a_name, float a_t, float a_prev, bool a_end);
	/** @brief Mod event API (core\Api.cpp), render thread. One-shot effect by name; a_strength 0-1 is scaled by the
	 * user's own strength settings and respects their on/off switches. True when the name is one of this module's. */
	bool ApiTrigger(std::string_view a_name, float a_strength);
	/** @brief Mod event API: sets a held effect's level (0 lets go). True when the name is one of this module's. */
	bool ApiHold(std::string_view a_name, float a_value);
	void ClearShaderCache();

	/** @brief Draws frost and lens water over the tone-mapped image. */
	void RenderPostProcess(RE::RENDER_TARGET a_output);

	Settings settings;
	bool loaded = true;

private:
	// Menu-only preview sliders; deliberately not part of Settings so they are never saved.
	float previewFrost = 0.0f;
	float previewRain = 0.0f;
	float previewSun = 0.0f;
	float previewMoon = 0.0f;
	float previewHeat = 0.0f;
	float previewAsh = 0.0f;
	float previewDust = 0.0f;
	float previewFog = 0.0f;
	float previewEmbers = 0.0f;
	float previewBreath = 0.0f;
	float previewEntryFog = 0.0f;
	float lastNearestHeat = -1.0f;

	struct GameSample
	{
		float frostTarget = 0.0f;
		float rainTarget = 0.0f;
		float lookUp = 0.0f;
		bool underwater = false;
		bool exterior = false;
		// For the settings readout
		float bodyCold = -1.0f;
		float ambientCold = -1.0f;
		float snowWeather = 0.0f;
		float rainWeather = 0.0f;
		float warmth = -1.0f;
		bool blizzard = false;
		float clearSky = 0.0f;  // 1 = clear/pleasant weather, 0 = rain, snow or full overcast
		RE::FormID weatherNow = 0, weatherLast = 0;  // log: current and previous weather
		float weatherPct = 0.0f;
		float heatTarget = 0.0f;
		float ashTarget = 0.0f;
		float dustTarget = 0.0f;
		float fogTarget = 0.0f;
		float emberTarget = 0.0f;
		float nearestHeat = -1.0f;  // readout: distance to the closest heat source
		float groundSnow = 0.0f;    // readout: how much of the ground around you is snow or ice (0..1)
		// nicktheduke: temperature
		float heatRaw = 0.0f;       // heat before the lens priority rules (thaws frost)
		float breathCold = 0.0f;    // how cold the air is for visible breath (0 warm .. 1 freezing)
		bool firstPerson = false;
		bool coldInterior = false;  // an interior that is still cold (ice caves): no entry fog, breath continues
		float exertion = 0.0f;
		uint32_t shimmerCount = 0;
		std::array<float4, 8> shimmerSources{};   // world top-centre xyz, horizontal radius
		std::array<float, 8> shimmerStrength{};
	};

	void QueryGame();
	bool PrepareShaders();
	bool PrepareInput(ID3D11Texture2D* a_output, ID3D11RenderTargetView* a_view);
	void ResolveSurvivalGlobals();
	void LoadLensWeathers();
	float ScanHeat(GameSample& a_sample);  // main thread
	void UpdateColdBreathNG(bool a_hide);  // main thread
	void LoadColdBreathNGRate();
	float SampleGroundSnow(RE::PlayerCharacter* a_player);  // main thread: share of snow/ice ground around the player
	float groundSnow = 0.0f;       // eased value used for frost and breath
	float groundSnowRaw = 0.0f;    // last sample
	double groundSnowSampledAt = -1.0;
	RE::TESObjectCELL* groundSnowCell = nullptr;
	std::unordered_map<const RE::TESLandTexture*, bool> snowTextureCache;  // main thread only
	float statusLogTimer = 0.0f;
	std::chrono::steady_clock::time_point groundSnowCellSince{};

	// LensWeathers.ini: weather form ID -> strength, per layer
	std::unordered_map<RE::FormID, float> ashWeathers;
	std::unordered_map<RE::FormID, float> dustWeathers;
	std::unordered_map<RE::FormID, float> emberWeathers;
	std::unordered_map<RE::FormID, uint8_t> heatBaseCache;  // 0 none, 1 small fire, 2 large fire, 3 volcanic

	RE::TESGlobal* survivalEnabled = nullptr;
	RE::TESGlobal* coldNeedValue = nullptr;
	RE::TESGlobal* coldNeedMax = nullptr;
	RE::TESGlobal* coldStage1 = nullptr;
	RE::TESGlobal* coldStage2 = nullptr;
	RE::TESGlobal* coldStage4 = nullptr;
	RE::TESGlobal* coldStage3 = nullptr;
	RE::TESGlobal* coldStage5 = nullptr;
	RE::TESGlobal* smiAmbient = nullptr;

	std::mutex sampleMutex;
	GameSample sample;
	GameSample frameSample;
	std::atomic<bool> queryPending{ false };
	float queryElapsed = 0.0f;

	float frost = 0.0f;
	float rain = 0.0f;
	float sheet = 0.0f;
	bool wasUnderwater = false;
	float elapsed = 0.0f;
	float sunLight = 0.0f;  // smoothed sun-on-lens strength
	float heat = 0.0f;
	float ash = 0.0f;
	float dust = 0.0f;
	float fogLens = 0.0f;
	float embers = 0.0f;
	float sunHeight = 0.0f;
	// nicktheduke: temperature
	float thaw = 0.0f;           // drips while frost melts near heat
	float entryFog = 0.0f;       // lens condensation after coming in from the cold (1 = fresh)
	float lingerCold = 0.0f;     // breath keeps showing for a few breaths after going inside
	float lastExteriorCold = 0.0f;
	bool wasExterior = true;
	float breathCold = 0.0f;     // smoothed
	float exhaleAge = 99.0f;
	float coldPuffAge = 99.0f;     // nicktheduke: seconds since the current cold-breath puff began
	float sinceColdPuff = 99.0f;   // nicktheduke: seconds since the last puff started (one puff per exhale, at most every ColdBreathInterval s)
	uint32_t breathShape = 0;      // nicktheduke: cold breath shape for the current puff (0-5: three shapes, each either way)
	float moonLight = 0.0f;        // nicktheduke: moon on the lens, eased
	float moonRadius = 0.03f;      // nicktheduke: apparent radius of the moon on screen (screen heights)
	uint8_t moonCaster = 1;        // nicktheduke: WeatherCompat::Caster of the moon in use (Masser or Secunda)
	float lastExhaleAge = 99.0f;   // nicktheduke: to spot a new exhale on the shared breathing clock
	float exhaleStrength = 0.0f;
	uint64_t frameCounter = 0;
	std::unordered_set<RE::FormID> coldBreathNGArt;  // Cold Breath NG art objects
	bool coldBreathNGHidden = false;
	bool bladeAndBlunt = false;

	winrt::com_ptr<ID3D11VertexShader> vertexShader;
	winrt::com_ptr<ID3D11PixelShader> pixelShader;
	winrt::com_ptr<ID3D11Texture2D> inputTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> inputView;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	winrt::com_ptr<ID3D11SamplerState> wrapSampler;
	winrt::com_ptr<ID3D11ShaderResourceView> frostMaskView;
	winrt::com_ptr<ID3D11ShaderResourceView> dropAtlasView;
	winrt::com_ptr<ID3D11ShaderResourceView> lensDirtView;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	std::unique_ptr<ConstantBuffer> parameters;
	D3D11_TEXTURE2D_DESC inputDescription{};
	DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN;
	bool reloadShaders = true;
	// nicktheduke: lens shaders compile on a worker thread so a shader cache reset never stalls the main thread
	std::future<std::pair<ID3D11VertexShader*, ID3D11PixelShader*>> pendingShaders;
};

namespace globals::features
{
	extern ScreenWeather screenWeather;
}
