// SPDX-License-Identifier: GPL-3.0-or-later
// nicktheduke: lens frost and lens water, driven by survival cold, weather and location.
#include "Weather.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>

#include "LensShared.h"
#include "WeatherCompat.h"

#include <DDSTextureLoader.h>
// DirectXTK (DDS loading) is linked by CMakeLists.txt.
#include <fstream>

#define I18N_KEY_PREFIX "feature.screen_weather."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenWeather::Settings::Temperature,
	ThawEnabled,
	ThawStrength,
	WarmAreasBlockFrost,
	ColdBreathEnabled,
	ColdBreathStrength,
	ColdBreathHeight,
	ColdBreathInterval,
	HideColdBreathNGFirstPerson,
	EntryFogEnabled,
	EntryFogStrength,
	EntryFogSeconds,
	HeatShimmerEnabled,
	HeatShimmerStrength,
	HeatShimmerRange);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenWeather::Settings,
	Enabled,
	FrostEnabled,
	FrostOpacity,
	FrostCoverage,
	FrostScale,
	FrostRefraction,
	FrostBlur,
	FrostTint,
	FrostGrowSeconds,
	FrostMeltSeconds,
	FrostEdgeSoftness,
	FrostGlow,
	FrostDayIntensity,
	FrostNightIntensity,
	FrostBreathRate,
	FrostBreathDepth,
	FrostContrast,
	FrostShape,
	UseSurvival,
	SurvivalAmbientWeight,
	WeatherSnowFrost,
	BlizzardFrost,
	BlizzardWindSpeed,
	SnowGroundFrost,
	WarmthProtection,
	WaterEnabled,
	RainDropAmount,
	RainDropSize,
	RainLookUpBias,
	DropLifetimeSeconds,
	WaterRefraction,
	SurfaceSheetAmount,
	SheetDrySeconds,
	SunLensEnabled,
	SunGlintsEnabled,
	SunGlintStrength,
	LensDirtEnabled,
	LensDirtAmount,
	SunLensStrength,
	SunMaxHeight,
	SunCloudyAmount,
	MoonLensEnabled,
	MoonLensStrength,
	HeatHazeEnabled,
	HeatHazeStrength,
	HeatRadiusScale,
	AshEnabled,
	AshStrength,
	EmbersEnabled,
	EmberStrength,
	DustEnabled,
	DustStrength,
	DustWindSpeed,
	FogLensEnabled,
	FogHaloStrength,
	FogCondensation,
	Temp);

namespace
{
	constexpr auto kShaderPath = L"Data\\SKSE\\Plugins\\SloppyCameraFX\\Shaders\\Weather.hlsl";
	// FrostMask.dds: a user-installed crystal photo (not shipped). FrostMask_nicktheduke.dds: nicktheduke's own procedural frost, free to redistribute.
	// LensDrops.dds: optional user replacement. LensDrops_nicktheduke.dds: nicktheduke's own 8x8 droplet atlas, free to redistribute.
	constexpr std::array<const wchar_t*, 2> kDropAtlasPaths{ L"Data\\SKSE\\Plugins\\SloppyCameraFX\\Weather\\LensDrops.dds", L"Data\\SKSE\\Plugins\\SloppyCameraFX\\Weather\\LensDrops_nicktheduke.dds" };
	constexpr std::array<const wchar_t*, 2> kFrostMaskPaths{ L"Data\\SKSE\\Plugins\\SloppyCameraFX\\Weather\\FrostMask.dds", L"Data\\SKSE\\Plugins\\SloppyCameraFX\\Weather\\FrostMask_nicktheduke.dds" };
	// LensDirt.dds: optional user replacement. LensDirt_nicktheduke.dds: nicktheduke's own procedural lens dirt, free to redistribute.
	constexpr std::array<const wchar_t*, 2> kLensDirtPaths{ L"Data\\SKSE\\Plugins\\SloppyCameraFX\\Weather\\LensDirt.dds", L"Data\\SKSE\\Plugins\\SloppyCameraFX\\Weather\\LensDirt_nicktheduke.dds" };
	// Survival warmth rating at which clothing gives its full protection against weather frost.
	constexpr float kFullWarmth = 200.0f;
	constexpr float kQueryInterval = 0.2f;
	constexpr float kMaximumFrameStep = 0.1f;
	constexpr float kRainFadeSeconds = 3.0f;

	struct alignas(16) RenderData
	{
		float4 BufferDim;    // 1/w, 1/h, time, float input
		float4 Frost;        // amount, coverage, scale, opacity
		float4 FrostLook;    // refraction, scene blur, glow, crystal intensity (day/night applied)
		float4 FrostTint;    // rgb, edge softness
		float4 Water;        // rain, sheet, drop size, refraction
		float4 WaterTiming;  // drop lifetime, frost mask texture present, unused, unused
		float4 HDRData;
		float4 FrostBreath;  // rate (Hz), depth, contrast, unused
		float4 SunLens;      // sun screen uv xy, strength, lens dirt amount
		float4 SunLook;      // glint strength, unused x3
		float4 Atmos;        // heat haze, ash, dust, fog
		float4 AtmosLook;    // fog halo, fog condensation, embers, cold breath shape (0-5)
		float4 Thermal;      // thaw drips, cold breath vapor, seconds since the exhale began, entry fog
		float4 ThermalLook;  // exhale strength, shimmer source count, depth sign (+1 larger = farther), depth present
		float4 Shimmer[8];   // per heat source: screen uv xy, radius (screen heights), strength
		float4 ShimmerDepth[2];  // per heat source: device depth of the source top
		float4 MoonLens;         // nicktheduke: moon screen uv xy, strength, apparent radius (screen heights)
		float4 FrostShape;       // nicktheduke: x = frost shape 0..1, yzw unused
	};
	static_assert(sizeof(RenderData) == 416);

	float Smooth(float a_edge0, float a_edge1, float a_x);

	/** @brief 1 by day, 0 at night, easing over dawn (5-8 h) and dusk (18-21 h) like ENB's night/day factor. */
	float DayFactor()
	{
		auto* sky = RE::Sky::GetSingleton();
		const float hour = sky && std::isfinite(sky->currentGameHour) ? sky->currentGameHour : 12.0f;
		return Smooth(5.0f, 8.0f, hour) * (1.0f - Smooth(18.0f, 21.0f, hour));
	}

	float Smooth(float a_edge0, float a_edge1, float a_x)
	{
		if (a_edge1 <= a_edge0)
			return a_x >= a_edge1 ? 1.0f : 0.0f;
		const float t = std::clamp((a_x - a_edge0) / (a_edge1 - a_edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	float Clamp01(float a_value) { return std::isfinite(a_value) ? std::clamp(a_value, 0.0f, 1.0f) : 0.0f; }

	float WeatherFlagAmount(const RE::Sky* a_sky, RE::TESWeather::WeatherDataFlag a_flag)
	{
		auto has = [a_flag](const RE::TESWeather* a_weather) {
			return a_weather && a_weather->data.flags.any(a_flag) ? 1.0f : 0.0f;
		};
		const float pct = Clamp01(a_sky->currentWeatherPct);
		return has(a_sky->currentWeather) * pct + has(a_sky->lastWeather) * (1.0f - pct);
	}

	/** @brief Survival warmth rating of the actor's worn gear (Actor::GetWarmthRating, not built for VR-enabled CommonLib). */
	float PlayerWarmth(const RE::Actor* a_actor)
	{
		if (REL::Module::IsVR())
			return -1.0f;
		using func_t = float(const RE::Actor*);
		static REL::Relocation<func_t> func{ RELOCATION_ID(25834, 26394) };
		return func(a_actor);
	}

		float GlobalValue(const RE::TESGlobal* a_global, float a_fallback)
	{
		return a_global && std::isfinite(a_global->value) ? a_global->value : a_fallback;
	}

	class PostProcessState
	{
	public:
		explicit PostProcessState(ID3D11DeviceContext* a_context) :
			context(a_context)
		{
			context->OMGetRenderTargets(1, &renderTarget, &depthStencil);
			context->OMGetBlendState(&blendState, blendFactor, &sampleMask);
			context->OMGetDepthStencilState(&depthState, &stencilRef);
			context->RSGetState(&rasterizerState);
			context->RSGetViewports(&viewportCount, nullptr);
			if (viewportCount > 0) {
				viewportCount = std::min<UINT>(viewportCount, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
				context->RSGetViewports(&viewportCount, viewports.data());
			}
			context->IAGetInputLayout(&inputLayout);
			context->IAGetPrimitiveTopology(&topology);
			context->VSGetShader(&vertexShader, nullptr, nullptr);
			context->PSGetShader(&pixelShader, nullptr, nullptr);
			context->PSGetConstantBuffers(0, 1, &constantBuffer);
			context->PSGetSamplers(0, 2, samplerStates);
			context->PSGetShaderResources(0, 5, resources);
		}

		~PostProcessState()
		{
			context->OMSetRenderTargets(1, &renderTarget, depthStencil);
			context->OMSetBlendState(blendState, blendFactor, sampleMask);
			context->OMSetDepthStencilState(depthState, stencilRef);
			context->RSSetState(rasterizerState);
			if (viewportCount > 0)
				context->RSSetViewports(viewportCount, viewports.data());
			context->IASetInputLayout(inputLayout);
			context->IASetPrimitiveTopology(topology);
			context->VSSetShader(vertexShader, nullptr, 0);
			context->PSSetShader(pixelShader, nullptr, 0);
			context->PSSetConstantBuffers(0, 1, &constantBuffer);
			context->PSSetSamplers(0, 2, samplerStates);
			context->PSSetShaderResources(0, 5, resources);
			for (IUnknown* object : std::initializer_list<IUnknown*>{ renderTarget, depthStencil, blendState, depthState, rasterizerState, inputLayout,
					 vertexShader, pixelShader, constantBuffer, samplerStates[0], samplerStates[1], resources[0], resources[1], resources[2], resources[3], resources[4] }) {
				if (object)
					object->Release();
			}
		}

		PostProcessState(const PostProcessState&) = delete;
		PostProcessState& operator=(const PostProcessState&) = delete;

	private:
		ID3D11DeviceContext* context;
		ID3D11RenderTargetView* renderTarget = nullptr;
		ID3D11DepthStencilView* depthStencil = nullptr;
		ID3D11BlendState* blendState = nullptr;
		FLOAT blendFactor[4]{};
		UINT sampleMask = 0;
		ID3D11DepthStencilState* depthState = nullptr;
		UINT stencilRef = 0;
		ID3D11RasterizerState* rasterizerState = nullptr;
		UINT viewportCount = 0;
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
		ID3D11InputLayout* inputLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11VertexShader* vertexShader = nullptr;
		ID3D11PixelShader* pixelShader = nullptr;
		ID3D11Buffer* constantBuffer = nullptr;
		ID3D11SamplerState* samplerStates[2]{};
		ID3D11ShaderResourceView* resources[5]{};
	};
}

void ScreenWeather::SetupResources()
{
	auto* device = globals::d3d::device;

	parameters = std::make_unique<ConstantBuffer>(ConstantBufferDesc<RenderData>(), "ScreenWeather::Parameters");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	if (SUCCEEDED(device->CreateSamplerState(&samplerDesc, sampler.put())))
		Util::SetResourceName(sampler.get(), "ScreenWeather::Sampler");
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	if (SUCCEEDED(device->CreateSamplerState(&samplerDesc, wrapSampler.put())))
		Util::SetResourceName(wrapSampler.get(), "ScreenWeather::WrapSampler");

	// Optional frost crystal mask (R8, tileable); the shader falls back to procedural crystals without it.
	for (const auto* path : kFrostMaskPaths) {
		if (!std::filesystem::exists(path))
			continue;
		if (SUCCEEDED(DirectX::CreateDDSTextureFromFile(device, path, nullptr, frostMaskView.put()))) {
			Util::SetResourceName(frostMaskView.get(), "ScreenWeather::FrostMask SRV");
			logger::info("[ScreenWeather] Frost crystals: {}", WeatherCompat::Narrow(path));
			break;
		}
		logger::warn("[ScreenWeather] Could not load {}", WeatherCompat::Narrow(path));
	}
	for (const auto* path : kLensDirtPaths) {
		if (!std::filesystem::exists(path))
			continue;
		if (SUCCEEDED(DirectX::CreateDDSTextureFromFile(device, path, nullptr, lensDirtView.put()))) {
			Util::SetResourceName(lensDirtView.get(), "ScreenWeather::LensDirt SRV");
			logger::info("[ScreenWeather] Lens dirt: {}", WeatherCompat::Narrow(path));
			break;
		}
		logger::warn("[ScreenWeather] Could not load {}", WeatherCompat::Narrow(path));
	}
	for (const auto* path : kDropAtlasPaths) {
		if (!std::filesystem::exists(path))
			continue;
		if (SUCCEEDED(DirectX::CreateDDSTextureFromFile(device, path, nullptr, dropAtlasView.put()))) {
			Util::SetResourceName(dropAtlasView.get(), "ScreenWeather::DropAtlas SRV");
			logger::info("[ScreenWeather] Lens droplets: {}", WeatherCompat::Narrow(path));
			break;
		}
		logger::warn("[ScreenWeather] Could not load {}", WeatherCompat::Narrow(path));
	}

	D3D11_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (SUCCEEDED(device->CreateBlendState(&blendDesc, blendState.put())))
		Util::SetResourceName(blendState.get(), "ScreenWeather::BlendState");

	D3D11_RASTERIZER_DESC rasterizerDesc{};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.DepthClipEnable = true;
	if (SUCCEEDED(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put())))
		Util::SetResourceName(rasterizerState.get(), "ScreenWeather::Rasterizer");

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	if (SUCCEEDED(device->CreateDepthStencilState(&depthDesc, depthStencilState.put())))
		Util::SetResourceName(depthStencilState.get(), "ScreenWeather::DepthState");

	if (!sampler || !blendState || !rasterizerState || !depthStencilState)
		logger::error("[ScreenWeather] Failed to create post-process states");

	PrepareShaders();
}

bool ScreenWeather::PrepareShaders()
{
	// nicktheduke: the lens pixel shader is large; compiling it here blocked the main thread for a minute
	// after every shader cache reset (game freeze). Compile on a worker instead and keep drawing with
	// the previous shaders (or skip the effect) until the new pair is ready.
	if (pendingShaders.valid() && pendingShaders.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		auto [rawVertex, rawPixel] = pendingShaders.get();
		winrt::com_ptr<ID3D11VertexShader> newVertex;
		winrt::com_ptr<ID3D11PixelShader> newPixel;
		newVertex.attach(rawVertex);
		newPixel.attach(rawPixel);
		if (newVertex && newPixel) {
			Util::SetResourceName(newVertex.get(), "ScreenWeather::FullscreenVS");
			Util::SetResourceName(newPixel.get(), "ScreenWeather::LensPS");
			vertexShader = std::move(newVertex);
			pixelShader = std::move(newPixel);
		} else {
			logger::error("[ScreenWeather] Shader compilation failed; the effect stays off");
		}
	}
	if (reloadShaders && !pendingShaders.valid()) {
		reloadShaders = false;
		pendingShaders = std::async(std::launch::async, []() {
			auto* newVertex = static_cast<ID3D11VertexShader*>(Util::CompileShader(kShaderPath, { { "VSHADER", "1" } }, "vs_5_0", "mainVS"));
			auto* newPixel = static_cast<ID3D11PixelShader*>(Util::CompileShader(kShaderPath, { { "PSHADER", "1" } }, "ps_5_0", "mainPS"));
			return std::pair{ newVertex, newPixel };
		});
	}
	return vertexShader && pixelShader;
}

void ScreenWeather::ClearShaderCache()
{
	reloadShaders = true;
}

bool ScreenWeather::PrepareInput(ID3D11Texture2D* a_output, ID3D11RenderTargetView* a_view)
{
	D3D11_TEXTURE2D_DESC description{};
	D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
	a_output->GetDesc(&description);
	a_view->GetDesc(&viewDescription);
	if (description.SampleDesc.Count != 1 || description.ArraySize != 1 || viewDescription.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || viewDescription.Texture2D.MipSlice != 0)
		return false;
	if (std::memcmp(&description, &inputDescription, sizeof(description)) == 0 && inputFormat == viewDescription.Format)
		return inputTexture && inputView;

	auto* device = globals::d3d::device;
	auto copyDescription = description;
	copyDescription.Usage = D3D11_USAGE_DEFAULT;
	copyDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	copyDescription.CPUAccessFlags = 0;
	copyDescription.MiscFlags = 0;

	winrt::com_ptr<ID3D11Texture2D> texture;
	winrt::com_ptr<ID3D11ShaderResourceView> view;
	auto result = device->CreateTexture2D(&copyDescription, nullptr, texture.put());
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription{};
	srvDescription.Format = viewDescription.Format;
	srvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDescription.Texture2D.MipLevels = 1;
	if (SUCCEEDED(result))
		result = device->CreateShaderResourceView(texture.get(), &srvDescription, view.put());
	if (FAILED(result)) {
		logger::error("[ScreenWeather] Scene copy failed: 0x{:08X}, {}x{}, format {}", static_cast<unsigned int>(result), description.Width, description.Height, static_cast<unsigned int>(description.Format));
		return false;
	}

	Util::SetResourceName(texture.get(), "ScreenWeather::SceneCopy");
	Util::SetResourceName(view.get(), "ScreenWeather::SceneCopy SRV");
	inputDescription = description;
	inputFormat = viewDescription.Format;
	inputTexture = std::move(texture);
	inputView = std::move(view);
	return true;
}

void ScreenWeather::ResolveSurvivalGlobals()
{
	auto find = [](const char* a_editorID) { return RE::TESForm::LookupByEditorID<RE::TESGlobal>(a_editorID); };
	survivalEnabled = find("Survival_ModeEnabled");
	coldNeedValue = find("Survival_ColdNeedValue");
	coldNeedMax = find("Survival_ColdNeedMaxValue");
	coldStage1 = find("Survival_ColdStage1Value");
	coldStage2 = find("Survival_ColdStage2Value");
	coldStage4 = find("Survival_ColdStage4Value");
	coldStage3 = find("Survival_ColdStage3Value");
	coldStage5 = find("Survival_ColdStage5Value");
	smiAmbient = find("SMI_CurrentAmbientTemp");
	logger::info("[ScreenWeather] Survival Mode {}, Survival Mode Improved ambient temperature {}",
		survivalEnabled && coldNeedValue ? "found" : "not found", smiAmbient ? "found" : "not found");
}

void ScreenWeather::DataLoaded()
{
	ResolveSurvivalGlobals();
	LoadLensWeathers();

	// nicktheduke: optional integrations, detected by plugin name (no ESP of our own).
	coldBreathNGArt.clear();
	if (auto* handler = RE::TESDataHandler::GetSingleton()) {
		bladeAndBlunt = handler->LookupLoadedModByName("BladeAndBlunt.esp") != nullptr || handler->LookupLoadedLightModByName("BladeAndBlunt.esp") != nullptr;
		if (handler->LookupLoadedModByName("ColdBreathNG.esp") || handler->LookupLoadedLightModByName("ColdBreathNG.esp")) {
			for (RE::FormID local = 0x800; local <= 0x813; ++local) {
				if (auto* art = handler->LookupForm<RE::BGSArtObject>(local, "ColdBreathNG.esp"))
					coldBreathNGArt.insert(art->GetFormID());
			}
		}
	}
	LoadColdBreathNGRate();
	logger::info("[ScreenWeather] Cold Breath NG {} ({} art objects), Blade and Blunt {}", coldBreathNGArt.empty() ? "not found" : "found",
		coldBreathNGArt.size(), bladeAndBlunt ? "found" : "not found");
}

void ScreenWeather::LoadColdBreathNGRate()
{
	// Match Cold Breath NG's calm breathing pace (breaths per minute) so first person agrees with third person.
	std::ifstream file("Data\\SKSE\\Plugins\\ColdBreathNG.ini");
	if (!file.is_open())
		return;
	float minimum = 0.0f, maximum = 0.0f;
	std::string line;
	while (std::getline(file, line)) {
		const auto eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = line.substr(0, eq);
		key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }), key.end());
		try {
			if (key == "minBreathCycleRate")
				minimum = std::stof(line.substr(eq + 1));
			else if (key == "maxBreathCycleRate")
				maximum = std::stof(line.substr(eq + 1));
		} catch (...) {}
	}
	if (minimum > 4.0f && maximum >= minimum && maximum < 60.0f)
		LensShared::Get().baseBreathsPerMinute = (minimum + maximum) * 0.5f;
}

void ScreenWeather::UpdateColdBreathNG(bool a_hide)
{
	// Cold Breath NG hangs its puff on the third-person head. In first person (especially with Improved Camera,
	// which keeps the body but tucks the head away) that puff floats at the top of the screen, so we hide it
	// there and draw first-person breath ourselves. Third person and every NPC keep Cold Breath NG's effect.
	if (coldBreathNGArt.empty() || (!a_hide && !coldBreathNGHidden))
		return;
	auto* lists = RE::ProcessLists::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!lists || !player)
		return;
	const auto playerHandle = player->GetHandle();
	lists->ForEachModelEffect([&](RE::ModelReferenceEffect* a_effect) {
		if (a_effect && a_effect->artObject && a_effect->target == playerHandle && coldBreathNGArt.contains(a_effect->artObject->GetFormID())) {
			if (auto* object = a_effect->artObject3D.get())
				object->SetAppCulled(a_hide);
		}
		return RE::BSContainer::ForEachResult::kContinue;
	});
	coldBreathNGHidden = a_hide;
}

void ScreenWeather::LoadLensWeathers()
{
	// Data\SKSE\Plugins\SloppyCameraFX\Weather\LensWeathers.ini, sections [Ash] [Dust] [Embers], lines "Plugin.esp|localID = strength".
	ashWeathers.clear();
	dustWeathers.clear();
	emberWeathers.clear();
	std::ifstream file("Data\\SKSE\\Plugins\\SloppyCameraFX\\Weather\\LensWeathers.ini");
	auto* handler = RE::TESDataHandler::GetSingleton();
	if (!file.is_open() || !handler)
		return;
	std::unordered_map<RE::FormID, float>* target = nullptr;
	std::string line;
	size_t loadedCount = 0;
	auto trim = [](std::string& a_text) {
		a_text.erase(0, a_text.find_first_not_of(" \t\r"));
		a_text.erase(a_text.find_last_not_of(" \t\r") + 1);
	};
	while (std::getline(file, line)) {
		trim(line);
		if (line.empty() || line[0] == ';' || line[0] == '#')
			continue;
		if (line.front() == '[') {
			std::string section = line.substr(1, line.find(']') - 1);
			std::transform(section.begin(), section.end(), section.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			target = section == "ash" ? &ashWeathers : section == "dust" ? &dustWeathers : section == "embers" ? &emberWeathers : nullptr;
			continue;
		}
		const auto bar = line.find('|');
		if (!target || bar == std::string::npos)
			continue;
		std::string plugin = line.substr(0, bar);
		std::string rest = line.substr(bar + 1);
		trim(plugin);
		float strength = 1.0f;
		if (const auto eq = rest.find('='); eq != std::string::npos) {
			try {
				strength = std::stof(rest.substr(eq + 1));
			} catch (...) {}
			rest = rest.substr(0, eq);
		}
		trim(rest);
		try {
			const auto local = static_cast<RE::FormID>(std::stoul(rest, nullptr, 16)) & 0x00FFFFFF;
			if (auto* weather = handler->LookupForm<RE::TESWeather>(local, plugin)) {
				(*target)[weather->GetFormID()] = Clamp01(strength);
				++loadedCount;
			}
		} catch (...) {}
	}
	logger::info("[ScreenWeather] LensWeathers.ini: {} weathers (ash {}, dust {}, embers {})", loadedCount, ashWeathers.size(), dustWeathers.size(), emberWeathers.size());
}

float ScreenWeather::ScanHeat(GameSample& a_sample)
{
	auto* tes = RE::TES::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!tes || !player)
		return 0.0f;
	const float scale = std::clamp(settings.HeatRadiusScale, 0.25f, 4.0f);
	const auto eye = player->GetPosition();
	float best = 0.0f;
	float nearest = -1.0f;
	auto classify = [](RE::TESBoundObject* a_base) -> uint8_t {
		switch (a_base->GetFormType()) {
		case RE::FormType::Static:
		case RE::FormType::MovableStatic:
		case RE::FormType::Activator:
		case RE::FormType::Furniture:
			break;
		default:
			return 0;
		}
		auto* model = a_base->As<RE::TESModel>();
		const char* path = model ? model->GetModel() : nullptr;
		if (!path || !*path)
			return 0;
		std::string p(path);
		std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		auto has = [&p](std::initializer_list<const char*> a_words) {
			for (const char* word : a_words)
				if (p.find(word) != std::string::npos)
					return true;
			return false;
		};
		if (has({ "firewood", "firefl", "unlit", "extinguish", "sconce", "torch", "candle", "lantern", "icon" }))
			return 0;
		// nicktheduke heat tiers: 1 hearth/oven/cookpot (gentle), 2 forge/smelter/kiln, 3 fumarole/hot spring,
		// 4 open fire (campfire, brazier, fire FX), 5 big fire (bonfire, pyre, heavy fire), 6 lava/magma.
		if (has({ "fumarole", "volcanic", "hotspring", "sulfur", "geyser", "steamvent" }))
			return 3;
		if (has({ "lava", "magma" }))
			return 6;
		if (has({ "forge", "smelter", "furnace", "kiln" }))
			return 2;
		if (has({ "bonfire", "pyre", "firelg", "fireheavy", "embersheavy" }))
			return 5;
		if (has({ "hearth", "fireplace", "oven", "cookpot", "spit" }))
			return 1;
		if (has({ "campfire", "firepit", "brazier", "fxfire", "firewithembers", "firesm", "firemed" }))
			return 4;
		return 0;
	};
	// nicktheduke: heat shimmer sources (the air above lava and big fires), kept up to 8, strongest first.
	const float shimmerRange = settings.Temp.HeatShimmerEnabled ? 2600.0f * std::clamp(settings.Temp.HeatShimmerRange, 0.25f, 3.0f) : 0.0f;
	struct ShimmerCandidate
	{
		float4 source;
		float strength;
	};
	std::vector<ShimmerCandidate> shimmer;
	const float lensRange = 1400.0f * scale;
	tes->ForEachReferenceInRange(player, std::max(lensRange, shimmerRange), [&](RE::TESObjectREFR* a_ref) {
		if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Is3DLoaded())
			return RE::BSContainer::ForEachResult::kContinue;
		auto* base = a_ref->GetBaseObject();
		if (!base)
			return RE::BSContainer::ForEachResult::kContinue;
		uint8_t kind;
		if (const auto it = heatBaseCache.find(base->GetFormID()); it != heatBaseCache.end())
			kind = it->second;
		else
			kind = heatBaseCache.emplace(base->GetFormID(), classify(base)).first->second;
		if (kind == 0)
			return RE::BSContainer::ForEachResult::kContinue;
		const float distance = eye.GetDistance(a_ref->GetPosition());
		if (shimmerRange > 0.0f && distance < shimmerRange) {
			constexpr float kShimmerByKind[7]{ 0.0f, 0.25f, 0.8f, 0.3f, 0.45f, 0.8f, 1.0f };
			const float baseStrength = kShimmerByKind[std::min<uint8_t>(kind, 6)];
			const float strength = baseStrength * (1.0f - Smooth(shimmerRange * 0.6f, shimmerRange, distance));
			if (strength > 0.02f) {
				const auto position = a_ref->GetPosition();
				const auto boundMin = a_ref->GetBoundMin();
				const auto boundMax = a_ref->GetBoundMax();
				float radius = 0.5f * std::max(std::abs(boundMax.x - boundMin.x), std::abs(boundMax.y - boundMin.y));
				if (!std::isfinite(radius))
					radius = 60.0f;
				radius = std::clamp(radius, 40.0f, 1500.0f);
				float top = std::isfinite(boundMax.z) ? boundMax.z : 30.0f;
				top = std::clamp(top, 0.0f, 400.0f);
				const float cx = std::isfinite(boundMin.x + boundMax.x) ? 0.5f * (boundMin.x + boundMax.x) : 0.0f;
				const float cy = std::isfinite(boundMin.y + boundMax.y) ? 0.5f * (boundMin.y + boundMax.y) : 0.0f;
				shimmer.push_back({ float4(position.x + cx, position.y + cy, position.z + top, radius), strength });
			}
		}
		if (distance > lensRange)
			return RE::BSContainer::ForEachResult::kContinue;
		// nicktheduke: peak lens haze and reach per tier. A shop hearth is a faint wobble up close; a forge or lava is strong.
		constexpr float kPeak[7]{ 0.0f, 0.3f, 0.75f, 0.5f, 0.55f, 0.85f, 1.0f };
		constexpr float kNear[7]{ 0.0f, 90.0f, 160.0f, 300.0f, 110.0f, 200.0f, 220.0f };
		constexpr float kFar[7]{ 1.0f, 260.0f, 520.0f, 1300.0f, 340.0f, 680.0f, 760.0f };
		const uint8_t tier = std::min<uint8_t>(kind, 6);
		const float amount = kPeak[tier] * (1.0f - Smooth(kNear[tier] * scale, kFar[tier] * scale, distance));
		if (nearest < 0.0f || distance < nearest)
			nearest = distance;
		best = std::max(best, amount);
		return RE::BSContainer::ForEachResult::kContinue;
	});
	lastNearestHeat = nearest;
	std::sort(shimmer.begin(), shimmer.end(), [](const ShimmerCandidate& a, const ShimmerCandidate& b) { return a.strength > b.strength; });
	a_sample.shimmerCount = static_cast<uint32_t>(std::min<size_t>(shimmer.size(), a_sample.shimmerSources.size()));
	for (uint32_t i = 0; i < a_sample.shimmerCount; ++i) {
		a_sample.shimmerSources[i] = shimmer[i].source;
		a_sample.shimmerStrength[i] = shimmer[i].strength;
	}
	return Clamp01(best);
}

void ScreenWeather::GameLoaded()
{
	frost = 0.0f;
	rain = 0.0f;
	sheet = 0.0f;
	wasUnderwater = false;
	heat = ash = dust = fogLens = embers = 0.0f;
	thaw = entryFog = lingerCold = breathCold = lastExteriorCold = 0.0f;
	wasExterior = true;
	coldBreathNGHidden = false;
}

// nicktheduke 2026-11-03: snow lying on the ground, without Bottled's Snow Cover. The game knows the material of the
// terrain at any point (the same lookup that picks footstep sounds). Sample the ground under the player and a ring of
// 8 points about 6 m out, so a rock or a cleared road underfoot does not switch it off in the middle of a snowfield.
float ScreenWeather::SampleGroundSnow(RE::PlayerCharacter* a_player)
{
	auto* tes = RE::TES::GetSingleton();
	if (!a_player || !tes || tes->interiorCell || !tes->gridCells || globals::state->isLoadingMenuOpen.load(std::memory_order_relaxed))
		return -1.0f;
	// Wait a few seconds after any cell change (coc, doors, fast travel): only read land the game has finished attaching.
	auto* parent = a_player->GetParentCell();
	const auto now = std::chrono::steady_clock::now();
	if (parent != groundSnowCell) {
		groundSnowCell = parent;
		groundSnowCellSince = now;
		return -1.0f;
	}
	if (now - groundSnowCellSince < std::chrono::seconds(3) || !parent || !parent->IsAttached())
		return -1.0f;

	const auto* grid = tes->gridCells;
	const std::uint32_t length = std::min<std::uint32_t>(grid->length, 8);
	// The terrain itself says what is painted where: each quarter of a cell has a base texture and up to 6 blended layers,
	// with a blend strength at every vertex (17 x 17 per quarter). Read it straight from the loaded land data: no engine
	// calls, so nothing here can wait on the cell loader.
	struct LoadedLand
	{
		float x, y;
		const RE::TESObjectLAND::LoadedLandData* data;
	};
	std::array<LoadedLand, 64> lands{};
	size_t landCount = 0;
	for (std::uint32_t gx = 0; gx < length; ++gx)
		for (std::uint32_t gy = 0; gy < length; ++gy) {
			auto* cell = grid->GetCell(gx, gy);
			if (!cell || !cell->IsAttached() || !cell->IsExteriorCell())
				continue;
			auto* land = cell->GetRuntimeData().cellLand;
			const auto* coords = cell->GetCoordinates();
			if (land && land->loadedData && coords && landCount < lands.size())
				lands[landCount++] = { coords->worldX, coords->worldY, land->loadedData };
		}
	auto snowyTexture = [this](const RE::TESLandTexture* a_texture) {
		if (!a_texture)
			return false;
		if (const auto it = snowTextureCache.find(a_texture); it != snowTextureCache.end())
			return it->second;
		bool snowy = false;
		if (const auto* material = a_texture->materialType) {
			switch (material->materialID) {
			case RE::MATERIAL_ID::kSnow:
			case RE::MATERIAL_ID::kSnowStairs:
			case RE::MATERIAL_ID::kIce:
			case RE::MATERIAL_ID::kIceForm:
				snowy = true;
				break;
			default:
				break;
			}
		}
		// some landscape mods leave snow terrain on a dirt or stone material: trust the texture name then
		if (!snowy && a_texture->textureSet) {
			const char* name = a_texture->textureSet->textures[0].textureName.c_str();
			if (name) {
				std::string lower(name);
				std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				snowy = lower.find("snow") != std::string::npos || lower.find("glacier") != std::string::npos;
			}
		}
		snowTextureCache.emplace(a_texture, snowy);
		return snowy;
	};
	// how much of the ground at this point is snow (0..1); -1 when its land is not loaded
	auto snowAt = [&](const RE::NiPoint3& a_point) -> float {
		for (size_t i = 0; i < landCount; ++i) {
			const auto& l = lands[i];
			const float lx = a_point.x - l.x, ly = a_point.y - l.y;
			if (lx < 0.0f || ly < 0.0f || lx >= 4096.0f || ly >= 4096.0f)
				continue;
			const int qx = lx >= 2048.0f ? 1 : 0, qy = ly >= 2048.0f ? 1 : 0;
			const int quad = qx + 2 * qy;
			const int vx = std::clamp(static_cast<int>(std::lround((lx - qx * 2048.0f) / 128.0f)), 0, 16);
			const int vy = std::clamp(static_cast<int>(std::lround((ly - qy * 2048.0f) / 128.0f)), 0, 16);
			const int vertex = vy * 17 + vx;
			float snow = snowyTexture(l.data->defQuadTextures[quad]) ? 1.0f : 0.0f;
			for (int layer = 0; layer < 6; ++layer) {
				const auto* texture = l.data->quadTextures[quad][layer];
				if (!texture)
					continue;
				const float opacity = std::clamp(static_cast<float>(static_cast<std::uint8_t>(l.data->percents[quad][vertex][layer])) / 100.0f, 0.0f, 1.0f);
				if (opacity <= 0.0f)
					continue;
				snow = snow * (1.0f - opacity) + (snowyTexture(texture) ? opacity : 0.0f);
			}
			return Clamp01(snow);
		}
		return -1.0f;
	};

	static int traced = 0;
	const bool trace = traced < 3;
	if (trace)
		logger::info("[ScreenWeather] ground snow check {}: {} loaded cells", traced + 1, landCount);
	const RE::NiPoint3 centre = a_player->GetPosition();
	constexpr float kRing = 420.0f;  // about 6 m
	float hits = 0.0f, weight = 0.0f;
	for (int i = -1; i < 8; ++i) {
		RE::NiPoint3 point = centre;
		if (i >= 0) {
			const float a = static_cast<float>(i) * 0.785398f;
			point.x += std::cos(a) * kRing;
			point.y += std::sin(a) * kRing;
		}
		const float snow = snowAt(point);
		if (snow < 0.0f)
			continue;
		const float w = i < 0 ? 2.0f : 1.0f;  // underfoot counts double
		weight += w;
		hits += snow * w;
	}
	if (trace) {
		logger::info("[ScreenWeather] ground snow check {} done: {:.1f} of {:.0f}", traced + 1, hits, weight);
		++traced;
	}
	return weight > 0.0f ? Clamp01(hits / weight) : -1.0f;
}

void ScreenWeather::QueryGame()
{
	GameSample result;
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* sky = RE::Sky::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	if (player && cell) {
		result.exterior = !cell->IsInteriorCell() && sky && sky->mode.get() == RE::Sky::Mode::kFull;

		if (auto* water = RE::TESWaterSystem::GetSingleton())
			result.underwater = water->playerUnderwater;

		if (auto* camera = RE::Main::WorldRootCamera()) {
			// NiCamera looks down its local +X axis.
			const float forwardZ = camera->world.rotate.entry[2][0];
			result.lookUp = std::isfinite(forwardZ) ? std::clamp(forwardZ, -1.0f, 1.0f) : 0.0f;
		}

		float weatherFrost = 0.0f;
		float snowGround = 0.0f;
		if (!result.exterior) {
			groundSnow = groundSnowRaw = 0.0f;
			groundSnowSampledAt = -1.0;
		}
		if (result.exterior && sky) {
			result.snowWeather = WeatherFlagAmount(sky, RE::TESWeather::WeatherDataFlag::kSnow);
			result.rainWeather = WeatherFlagAmount(sky, RE::TESWeather::WeatherDataFlag::kRainy);
			result.weatherNow = sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
			result.weatherLast = sky->lastWeather ? sky->lastWeather->GetFormID() : 0;
			result.weatherPct = Clamp01(sky->currentWeatherPct);
			// nicktheduke: clear weathers let the low sun flood the lens; cloudy ones only partly; rain and snow not at all.
			const float pleasant = WeatherFlagAmount(sky, RE::TESWeather::WeatherDataFlag::kPleasant);
			const float cloudy = WeatherFlagAmount(sky, RE::TESWeather::WeatherDataFlag::kCloudy);
			result.clearSky = Clamp01(std::max(pleasant, cloudy * Clamp01(settings.SunCloudyAmount))) * (1.0f - result.rainWeather) * (1.0f - result.snowWeather);
			const auto* weather = sky->currentWeather;
			result.blizzard = weather && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow) &&
			                  weather->data.windSpeed >= std::clamp(settings.BlizzardWindSpeed, 0, 255);
			weatherFrost = std::max(result.snowWeather * settings.WeatherSnowFrost, result.blizzard ? settings.BlizzardFrost : 0.0f);
			// nicktheduke: snow lying on the ground, sampled once a second from the terrain material and eased over ~3 s
			// (it used to come from Bottled's Snow Cover).
			const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
			if (groundSnowSampledAt < 0.0 || now - groundSnowSampledAt >= 1.0 || now < groundSnowSampledAt) {
				const float step = groundSnowSampledAt < 0.0 ? 1.0f : static_cast<float>(std::clamp(now - groundSnowSampledAt, 0.0, 3.0)) / 3.0f;
				const float sampled = SampleGroundSnow(player);   // -1: not safe to read the ground right now
				if (sampled >= 0.0f) {
					groundSnowRaw = sampled;
					groundSnow += (groundSnowRaw - groundSnow) * std::min(step, 1.0f);
				}
				groundSnowSampledAt = now;
				static bool loggedSnow = false;
				if (!loggedSnow && groundSnowRaw >= 0.5f) {
					loggedSnow = true;
					logger::info("[ScreenWeather] snow on the ground found around you ({:.0f}% of the ground checked)", groundSnowRaw * 100.0f);
				}
			}
			snowGround = groundSnow;
			weatherFrost = std::max(weatherFrost, snowGround * settings.SnowGroundFrost);
		}

		float survivalFrost = 0.0f;
		float stage2Norm = 0.15f, stage4Norm = 0.55f;
		const bool survivalOn = settings.UseSurvival && survivalEnabled && coldNeedValue && GlobalValue(survivalEnabled, 0.0f) > 0.0f;
		if (survivalOn) {
			const float maximum = std::max(GlobalValue(coldNeedMax, 1000.0f), 1.0f);
			const float stage2 = GlobalValue(coldStage2, maximum * 0.15f) / maximum;
			const float stage3 = GlobalValue(coldStage3, maximum * 0.28f) / maximum;
			const float stage5 = GlobalValue(coldStage5, maximum * 0.78f) / maximum;
			const float stage1 = GlobalValue(coldStage1, maximum * 0.05f) / maximum;
			stage2Norm = stage2;
			stage4Norm = GlobalValue(coldStage4, maximum * 0.55f) / maximum;
			result.bodyCold = Clamp01(GlobalValue(coldNeedValue, 0.0f) / maximum);
			survivalFrost = Smooth(stage2, stage5, result.bodyCold);
			if (smiAmbient) {
				result.ambientCold = Clamp01(GlobalValue(smiAmbient, 0.0f) / maximum);
				survivalFrost = std::max(survivalFrost, Smooth(stage3, stage5, result.ambientCold) * settings.SurvivalAmbientWeight);
				// nicktheduke: warm areas (by SMI's temperature) keep snowfall frost light.
				if (settings.Temp.WarmAreasBlockFrost)
					weatherFrost *= 0.35f + 0.65f * Smooth(stage1 * 0.5f, stage2, result.ambientCold);
			}
		}

		// Warm clothing keeps snowfall and blizzards off the lens; real body cold (survival) still frosts it.
		result.warmth = PlayerWarmth(player);
		if (std::isfinite(result.warmth) && result.warmth > 0.0f)
			weatherFrost *= 1.0f - Clamp01(settings.WarmthProtection) * Clamp01(result.warmth / kFullWarmth);

		// nicktheduke atmosphere layers, from the weather lists, the live fog and nearby heat.
		if (result.exterior && sky) {
			auto listAmount = [sky](const std::unordered_map<RE::FormID, float>& a_list) {
				if (a_list.empty())
					return 0.0f;
				auto amountOf = [&a_list](const RE::TESWeather* a_weather) {
					if (!a_weather)
						return 0.0f;
					const auto it = a_list.find(a_weather->GetFormID());
					return it != a_list.end() ? it->second : 0.0f;
				};
				const float pct = Clamp01(sky->currentWeatherPct);
				return amountOf(sky->currentWeather) * pct + amountOf(sky->lastWeather) * (1.0f - pct);
			};
			result.ashTarget = listAmount(ashWeathers);
			result.emberTarget = listAmount(emberWeathers);
			result.dustTarget = listAmount(dustWeathers);
			// nicktheduke 2026-11-03: the engine draws ash storms with snow precipitation, so their snow flag means ash, not
			// snow. A weather listed as ash does not count as snowfall (no snowfall frost, no blizzard), so the ash shows.
			if (result.ashTarget > 0.01f) {
				const float keep = 1.0f - Clamp01(result.ashTarget);
				result.snowWeather *= keep;
				weatherFrost *= keep;
				if (result.ashTarget > 0.5f)
					result.blizzard = false;
			}
			// Dry gales lift dust anywhere (Skyrim has little desert, so wind is the main source).
			if (const auto* weather = sky->currentWeather; weather && result.rainWeather < 0.01f && result.snowWeather < 0.01f) {
				const int threshold = std::clamp(settings.DustWindSpeed, 0, 255);
				if (threshold < 255)
					result.dustTarget = std::max(result.dustTarget, 0.6f * Smooth(static_cast<float>(threshold), 255.0f, static_cast<float>(weather->data.windSpeed)));
			}
			// Fog: distance where the weather's fog reaches half density (same model as Bottled's height fog).
			const float fogNear = std::isfinite(sky->fogNear) ? std::max(sky->fogNear, 0.0f) : 0.0f;
			const float fogFar = std::isfinite(sky->fogFar) ? sky->fogFar : 0.0f;
			const float fogPower = std::isfinite(sky->fogPower) ? std::max(sky->fogPower, 0.01f) : 1.0f;
			const float fogMax = std::isfinite(sky->fogClamp) ? Clamp01(sky->fogClamp) : 0.0f;
			if (fogFar > fogNear + 1.0f && fogMax > 0.05f) {
				const float half = (fogFar - fogNear) * std::pow(fogMax * 0.5f, 1.0f / fogPower);
				result.fogTarget = 1.0f - Smooth(2500.0f, 22000.0f, half);
			}
		}
		const bool wantHeat = settings.HeatHazeEnabled || settings.Temp.HeatShimmerEnabled || settings.Temp.ThawEnabled;
		result.heatRaw = wantHeat ? ScanHeat(result) : 0.0f;
		// nicktheduke: the same fire reads differently by place and weather. Indoors the air is already warm, so a hearth bends
		// the view less; outside, rain and snow cool the plume and a cold night thins it a little.
		float heatPlace = 1.0f;
		if (!result.exterior)
			heatPlace = 0.55f;
		else
			heatPlace = Clamp01(1.0f - 0.5f * Clamp01(result.rainWeather) - 0.4f * Clamp01(result.snowWeather)) * (0.8f + 0.2f * DayFactor());
		for (uint32_t i = 0; i < result.shimmerCount; ++i)
			result.shimmerStrength[i] *= result.exterior ? heatPlace : 0.7f;
		result.heatTarget = settings.HeatHazeEnabled ? result.heatRaw * heatPlace : 0.0f;
		result.nearestHeat = lastNearestHeat;
		result.groundSnow = groundSnow;

		// nicktheduke: heat thaws the frost; the melt shows as drips (see Reset).
		if (settings.Temp.ThawEnabled && result.heatRaw > 0.0f) {
			const float keep = 1.0f - Clamp01(result.heatRaw * 0.9f * std::clamp(settings.Temp.ThawStrength, 0.0f, 2.0f));
			weatherFrost *= keep;
			survivalFrost *= keep;
		}

		// nicktheduke: how cold the air is for visible breath. Exterior: area temperature, snow in the sky or on the ground,
		// and body cold. Interior: only when Survival Mode Improved says it is still cold (ice caves).
		if (result.exterior) {
			float cold = std::max(result.snowWeather * 0.9f, snowGround * 0.7f);
			if (result.blizzard)
				cold = 1.0f;
			if (result.ambientCold >= 0.0f)
				cold = std::max(cold, Smooth(stage2Norm * 0.6f, stage4Norm, result.ambientCold));
			if (result.bodyCold >= 0.0f)
				cold = std::max(cold, Smooth(stage2Norm, stage4Norm, result.bodyCold));
			result.breathCold = Clamp01(cold * (1.0f - Clamp01(result.heatRaw)));
		} else if (result.ambientCold >= 0.0f && result.ambientCold >= stage2Norm) {
			result.coldInterior = true;
			result.breathCold = Smooth(stage2Norm * 0.6f, stage4Norm, result.ambientCold);
		}
		if (auto* camera = RE::PlayerCamera::GetSingleton())
			result.firstPerson = camera->IsInFirstPerson();
		result.exertion = LensShared::ComputeExertion(player, bladeAndBlunt);
		UpdateColdBreathNG(settings.Temp.ColdBreathEnabled && settings.Temp.HideColdBreathNGFirstPerson && result.firstPerson);

		// nicktheduke lens priority, one weather layer at a time:
		//   1. frost (any snow in the sky, blizzards, survival cold) beats everything
		//   2. water (rain, swimming) beats heat, ash/dust and fog, so rain next to a fire stays droplets
		//   3. heat haze, 4. ash / dust, 5. fog (planned layers follow the same order)
		// Sun on the lens is not a layer: it only lights what is already on the glass, on clear days.
		// Any snow in the sky (including a rain-to-snow transition) keeps
		// it frost; plain rain keeps it droplets and holds the frost back until the rain stops.
		const bool snowing = result.snowWeather > 0.01f;
		result.rainTarget = result.exterior && !result.underwater && !snowing ? Clamp01(result.rainWeather) : 0.0f;
		result.frostTarget = result.underwater || result.rainTarget > 0.01f ? 0.0f : Clamp01(std::max(survivalFrost, weatherFrost));

		// Below frost and water: heat, then ash / dust (the stronger of the two), then fog.
		const bool frostOrWater = result.frostTarget > 0.01f || result.rainTarget > 0.01f || result.underwater;
		if (frostOrWater)
			result.heatTarget = 0.0f;
		const bool heatWins = result.heatTarget > 0.05f;
		if (frostOrWater || heatWins) {
			result.ashTarget = result.dustTarget = 0.0f;
		} else if (result.ashTarget >= result.dustTarget) {
			result.dustTarget = 0.0f;
		} else {
			result.ashTarget = 0.0f;
		}
		if (frostOrWater || heatWins || result.ashTarget > 0.01f || result.dustTarget > 0.01f)
			result.fogTarget = 0.0f;
		if (result.underwater)
			result.emberTarget = 0.0f;
	}

	{
		std::scoped_lock lock(sampleMutex);
		sample = result;
	}
	queryPending.store(false, std::memory_order_release);
}

void ScreenWeather::Reset()
{
	const float frameDelta = RE::GetSecondsSinceLastFrame();
	const float delta = std::isfinite(frameDelta) ? std::clamp(frameDelta, 0.0f, kMaximumFrameStep) : 0.0f;
	auto* state = globals::state;
	const bool blocked = state->IsMainOrLoadingMenuOpen() || state->isMapMenuOpen || state->isStatsMenuOpen;
	if (!settings.Enabled || blocked) {
		if (state->isLoadingMenuOpen) {
			frost = rain = sheet = 0.0f;
			heat = ash = dust = fogLens = embers = 0.0f;
			thaw = entryFog = lingerCold = 0.0f;
			wasUnderwater = false;
		}
		auto& shared = LensShared::Get();
		shared.weatherCoverage.store(0.0f, std::memory_order_relaxed);
		shared.weatherFrost.store(0.0f, std::memory_order_relaxed);
		shared.coldBreathVisible.store(0.0f, std::memory_order_relaxed);
		// Give Cold Breath NG its first-person puff back while we are off.
		if (!settings.Enabled && coldBreathNGHidden && !queryPending.exchange(true)) {
			if (auto* tasks = SKSE::GetTaskInterface())
				tasks->AddTask([this] { UpdateColdBreathNG(false); queryPending.store(false, std::memory_order_release); });
			else
				queryPending.store(false, std::memory_order_release);
		}
		return;
	}

	elapsed += delta;
	queryElapsed += delta;
	if (queryElapsed >= kQueryInterval) {
		if (auto* tasks = SKSE::GetTaskInterface(); tasks && !queryPending.exchange(true)) {
			queryElapsed = 0.0f;
			tasks->AddTask([this] { QueryGame(); });
		}
	}

	{
		std::scoped_lock lock(sampleMutex);
		frameSample = sample;
	}

	const float frostTarget = settings.FrostEnabled ? frameSample.frostTarget : 0.0f;
	const float frostSeconds = frostTarget > frost ? settings.FrostGrowSeconds : settings.FrostMeltSeconds;
	const float frostStep = frostSeconds > 0.0f ? delta / frostSeconds : 1.0f;
	frost += std::clamp(frostTarget - frost, -frostStep, frostStep);

	const float rainTarget = settings.WaterEnabled ? frameSample.rainTarget : 0.0f;
	const float rainStep = delta / kRainFadeSeconds;
	rain += std::clamp(rainTarget - rain, -rainStep, rainStep);

	if (settings.WaterEnabled && wasUnderwater && !frameSample.underwater)
		sheet = Clamp01(settings.SurfaceSheetAmount);
	else if (frameSample.underwater)
		sheet = 0.0f;
	else
		sheet = std::max(0.0f, sheet - (settings.SheetDrySeconds > 0.0f ? delta / settings.SheetDrySeconds : 1.0f));
	wasUnderwater = frameSample.underwater;

	// nicktheduke: sun on the lens. Strength eases in and out so turning toward the sun or a weather change never pops.
	float sunTarget = 0.0f;
	if (settings.SunLensEnabled && frameSample.exterior && !frameSample.underwater && frameSample.clearSky > 0.01f) {
		auto* sky = RE::Sky::GetSingleton();
		if (sky && sky->sun && sky->sun->root && sky->root) {
			const auto direction = WeatherCompat::CelestialDirection(sky, WeatherCompat::Caster::Sun);
			const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
			sunHeight = length > 0.0f ? direction.z / length : 0.0f;
			// Strongest with the sun low (morning, late afternoon, golden hour), gone once it is high or set.
			const float low = Smooth(-0.03f, 0.06f, sunHeight) * (1.0f - Smooth(0.15f, std::max(settings.SunMaxHeight, 0.2f), sunHeight));
			sunTarget = Clamp01(WeatherCompat::SunVisibility(sky)) * low * frameSample.clearSky;
		}
	}
	sunLight += (sunTarget - sunLight) * std::min(1.0f, delta * 1.5f);

	// nicktheduke: the moon on the lens. Only a full or nearly full moon, at night, on a clear sky, and only while it is up.
	float moonTarget = 0.0f;
	if (settings.MoonLensEnabled && frameSample.exterior && !frameSample.underwater && frameSample.clearSky > 0.01f) {
		auto* sky = RE::Sky::GetSingleton();
		const float night = 1.0f - DayFactor();
		if (sky && sky->root && night > 0.01f) {
			const std::tuple<RE::Moon*, WeatherCompat::Caster, float> moons[2] = { { sky->masser, WeatherCompat::Caster::Masser, 1.0f }, { sky->secunda, WeatherCompat::Caster::Secunda, 0.6f } };
			for (const auto& [moon, caster, size] : moons) {
				if (!moon || !moon->root || !moon->moonMesh || moon->root->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					continue;
				const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().shaderProperty.get());
				if (!prop)
					continue;
				float phase = 1.0f;
				if (auto* texture = prop->GetBaseTexture()) {
					const auto p = WeatherCompat::PhaseFromTexture(texture->name.c_str());
					phase = p == RE::Moon::Phases::Phase::kFull ? 1.0f : ((p == RE::Moon::Phases::Phase::kWaningGibbous || p == RE::Moon::Phases::Phase::kWaxingGibbous) ? 0.4f : 0.0f);
				}
				const auto direction = WeatherCompat::CelestialDirection(sky, caster);
				const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
				const float height = length > 0.0f ? direction.z / length : 0.0f;
				const float value = Clamp01(prop->kBlendColor.alpha) * phase * Smooth(0.0f, 0.12f, height) * size * frameSample.clearSky * night;
				if (value > moonTarget) {
					moonTarget = value;
					moonCaster = static_cast<uint8_t>(caster);
					moonRadius = 0.03f * size;
				}
			}
		}
	}
	moonLight += (moonTarget - moonLight) * std::min(1.0f, delta * 1.0f);

	auto approach = [delta](float& a_value, float a_target, float a_seconds) {
		const float step = a_seconds > 0.0f ? delta / a_seconds : 1.0f;
		a_value += std::clamp(a_target - a_value, -step, step);
	};
	approach(heat, settings.HeatHazeEnabled ? frameSample.heatTarget : 0.0f, 1.5f);
	approach(ash, settings.AshEnabled ? frameSample.ashTarget : 0.0f, 6.0f);
	approach(dust, settings.DustEnabled ? frameSample.dustTarget : 0.0f, 4.0f);
	approach(fogLens, settings.FogLensEnabled ? frameSample.fogTarget : 0.0f, 6.0f);
	approach(embers, settings.EmbersEnabled ? frameSample.emberTarget : 0.0f, 3.0f);
	// a status line every 30 s outdoors (what the lens is reacting to), for checking a setup from the log alone
	statusLogTimer += delta;
	if (statusLogTimer >= 30.0f && frameSample.exterior) {
		statusLogTimer = 0.0f;
		logger::info("[ScreenWeather] status: frost {:.2f} rain {:.2f} | weather {:08X} <- {:08X} ({:.0f}%), snow {:.2f} rain {:.2f} | ground snow {:.2f} | sun {:.2f} heat {:.2f} ash {:.2f} dust {:.2f} fog {:.2f} embers {:.2f}",
			frost, rain, frameSample.weatherNow, frameSample.weatherLast, frameSample.weatherPct * 100.0f, frameSample.snowWeather, frameSample.rainWeather,
			frameSample.groundSnow, sunLight, heat, ash, dust, fogLens, embers);
	}

	// nicktheduke: frost melting near heat runs off as drips.
	const bool melting = settings.Temp.ThawEnabled && frameSample.heatRaw > 0.05f && frost > 0.03f && frameSample.frostTarget < frost - 0.01f;
	approach(thaw, melting ? Clamp01(frost * 2.0f) : 0.0f, 1.0f);

	// nicktheduke: coming in from the cold. A few breaths still show as you warm up, and a warm room fogs a cold lens.
	if (frameSample.exterior) {
		lastExteriorCold = breathCold;
	} else if (wasExterior && !frameSample.coldInterior && lastExteriorCold > 0.2f) {
		if (settings.Temp.EntryFogEnabled && !frameSample.underwater)
			entryFog = Clamp01(lastExteriorCold * std::clamp(settings.Temp.EntryFogStrength, 0.0f, 2.0f));
		lingerCold = lastExteriorCold;
		lastExteriorCold = 0.0f;
	}
	wasExterior = frameSample.exterior;
	entryFog = std::max(0.0f, entryFog - delta / std::clamp(settings.Temp.EntryFogSeconds, 2.0f, 60.0f));
	lingerCold = std::max(0.0f, lingerCold - delta / 12.0f);

	// nicktheduke: cold breath on the shared breathing clock (one rhythm for Screen Weather and Screen Action).
	approach(breathCold, frameSample.breathCold, 3.0f);
	LensShared::AdvanceBreath(static_cast<uint64_t>(state->frameCountAtomic.load(std::memory_order_relaxed)) + 1u, delta, frameSample.exertion);
	{
		auto& shared = LensShared::Get();
		// nicktheduke: in the cold, a visible puff on an exhale, but never more often than every 6 seconds (every breath at
		// rest, or every few breaths when winded), so it keeps coming while you stay in the cold without nagging
		sinceColdPuff = std::min(sinceColdPuff + delta, 99.0f);
		coldPuffAge = std::min(coldPuffAge + delta, 99.0f);
		if (shared.exhaleAge < lastExhaleAge && sinceColdPuff >= std::clamp(settings.Temp.ColdBreathInterval, 2.0f, 15.0f)) {
			coldPuffAge = 0.0f;
			sinceColdPuff = 0.0f;
			// a different shape (or the same shape leaning the other way) every puff, never the same twice in a row
			breathShape = (breathShape + 1u + static_cast<uint32_t>(shared.exhaleCount % 4u)) % 6u;
			exhaleStrength = shared.exhaleStrength;
		}
		lastExhaleAge = shared.exhaleAge;
		exhaleAge = coldPuffAge;
		const bool showBreath = settings.Temp.ColdBreathEnabled && frameSample.firstPerson && !frameSample.underwater;
		// once the air is properly cold the breath shows in full; it only thins out at the edge of cold
		const float visible = showBreath ? Smooth(0.08f, 0.35f, std::max(breathCold, lingerCold)) * std::clamp(settings.Temp.ColdBreathStrength, 0.0f, 2.0f) : 0.0f;
		shared.coldBreathVisible.store(Clamp01(visible), std::memory_order_relaxed);
		shared.weatherFrost.store(frost, std::memory_order_relaxed);
		const float coverage = std::max({ frost * Clamp01(settings.FrostCoverage) * 0.6f, rain * 0.15f + sheet * 0.5f, ash * 0.3f, dust * 0.2f, fogLens * 0.15f, entryFog * 0.5f });
		shared.weatherCoverage.store(Clamp01(coverage), std::memory_order_relaxed);
	}
}

void ScreenWeather::RenderPostProcess(RE::RENDER_TARGET a_output)
{
	const float frostAmount = std::max(frost, previewFrost);
	const float lookUpBias = settings.RainLookUpBias * std::max(frameSample.lookUp, 0.0f);
	const float rainAmount = std::max(rain * settings.RainDropAmount * (1.0f - settings.RainLookUpBias * 0.5f + lookUpBias), previewRain);

	// nicktheduke: where the sun sits on screen. Off-screen positions are kept so light still spills in from the edge.
	float sunStrength = settings.SunLensEnabled ? std::max(sunLight, previewSun) * std::clamp(settings.SunLensStrength, 0.0f, 2.0f) : 0.0f;
	float sunU = 0.8f, sunV = 0.2f;  // preview position when the real sun is not usable
	if (sunStrength > 0.001f) {
		auto* sky = RE::Sky::GetSingleton();
		bool placed = false;
		if (sky && sky->sun && sky->sun->root && sky->root) {
			const auto direction = WeatherCompat::CelestialDirection(sky, WeatherCompat::Caster::Sun);
			float u, v;
			if (WeatherCompat::DirectionToScreen(direction, u, v)) {
				sunU = u;
				sunV = v;
				placed = true;
			}
		}
		if (!placed && previewSun <= 0.001f)
			sunStrength = 0.0f;  // sun behind the camera
	}
	const float dirtAmount = settings.LensDirtEnabled ? std::clamp(settings.LensDirtAmount, 0.0f, 2.0f) : 0.0f;
	const float glintAmount = settings.SunGlintsEnabled ? std::clamp(settings.SunGlintStrength, 0.0f, 3.0f) : 0.0f;
	const bool sunWorks = sunStrength > 0.001f && (dirtAmount > 0.001f || glintAmount > 0.001f);

	// nicktheduke: where the moon sits on screen (only drawn when it is in view or just off its edge).
	float moonStrength = settings.MoonLensEnabled ? std::max(moonLight, previewMoon) * std::clamp(settings.MoonLensStrength, 0.0f, 2.0f) : 0.0f;
	float moonU = 0.25f, moonV = 0.22f;  // preview position
	if (moonStrength > 0.001f) {
		auto* sky = RE::Sky::GetSingleton();
		bool placed = false;
		if (sky && sky->root && moonLight > 0.001f) {
			const auto direction = WeatherCompat::CelestialDirection(sky, static_cast<WeatherCompat::Caster>(moonCaster));
			float u, v;
			if (WeatherCompat::DirectionToScreen(direction, u, v)) {
				moonU = u;
				moonV = v;
				placed = moonU > -0.3f && moonU < 1.3f && moonV > -0.3f && moonV < 1.3f;
			}
		}
		if (!placed && previewMoon <= 0.001f)
			moonStrength = 0.0f;
	}
	const bool moonWorks = moonStrength > 0.001f;
	const float heatAmount = std::max(heat, previewHeat) * std::clamp(settings.HeatHazeStrength, 0.0f, 3.0f);
	const float ashAmount = std::max(ash, previewAsh) * std::clamp(settings.AshStrength, 0.0f, 3.0f);
	const float dustAmount = std::max(dust, previewDust) * std::clamp(settings.DustStrength, 0.0f, 3.0f);
	const float fogAmount = std::max(fogLens, previewFog);
	const float emberAmount = std::max(embers, previewEmbers) * std::clamp(settings.EmberStrength, 0.0f, 3.0f);
	const bool atmosWorks = heatAmount > 0.001f || ashAmount > 0.001f || dustAmount > 0.001f || fogAmount > 0.001f || emberAmount > 0.001f;

	// nicktheduke: temperature layers.
	const float thawAmount = settings.Temp.ThawEnabled ? thaw : 0.0f;
	const float breathAmount = previewBreath > 0.001f ? previewBreath : LensShared::Get().coldBreathVisible.load(std::memory_order_relaxed);
	// nicktheduke: the preview runs on a real-time clock (game time stops while the menu is open)
	const double wallClock = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	const float breathAge = previewBreath > 0.001f ? static_cast<float>(std::fmod(wallClock, 3.0)) : exhaleAge;
	const float entryAmount = std::max(entryFog, previewEntryFog);

	// nicktheduke: heat shimmer sources projected to the screen (world-space shimmer, not a lens effect).
	float4 shimmer[8]{};
	float shimmerDepth[8]{};
	uint32_t shimmerCount = 0;
	float depthSign = 1.0f;
	ID3D11ShaderResourceView* depthView = nullptr;
	if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton())
		depthView = reinterpret_cast<ID3D11ShaderResourceView*>(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV);
	bool depthUsable = depthView != nullptr;
	if (settings.Temp.HeatShimmerEnabled && !frameSample.underwater && frameSample.shimmerCount > 0) {
		RE::NiPoint3 eyeAdjust{};
		WeatherCompat::CameraPosition(eyeAdjust);
		const float aspectRatio = static_cast<float>(inputDescription.Width) / std::max(1.0f, static_cast<float>(inputDescription.Height));
		// nicktheduke: screen position from the main camera; depth from the renderer's own matrices so it matches the
		// depth buffer. Without a trustworthy depth the occlusion test is skipped (the shimmer still shows).
		auto project = [&](float a_x, float a_y, float a_z, float& o_u, float& o_v, float& o_depth) {
			const RE::NiPoint3 world{ a_x, a_y, a_z };
			if (!Camera::WorldToScreen(world, o_u, o_v))
				return false;
			if (!depthUsable || !WeatherCompat::WorldDepth(world, o_depth)) {
				depthUsable = false;
				o_depth = 0.0f;
			}
			return true;
		};
		bool signSet = false;
		for (uint32_t i = 0; i < frameSample.shimmerCount && shimmerCount < 8; ++i) {
			const auto& src = frameSample.shimmerSources[i];
			float u, v, d;
			if (!project(src.x, src.y, src.z, u, v, d) || u < -0.4f || u > 1.4f || v < -0.4f || v > 1.6f)
				continue;
			float u2, v2, d2, u3, v3, d3;
			float radius = 0.0f;
			if (project(src.x, src.y, src.z + src.w, u2, v2, d2))
				radius = std::max(radius, std::abs(v2 - v));
			if (project(src.x + src.w, src.y, src.z, u3, v3, d3))
				radius = std::max(radius, std::abs(u3 - u) * aspectRatio);
			radius = std::clamp(radius, 0.01f, 0.8f);
			if (!signSet) {
				// Which way device depth grows with distance (Reverse-Z flips it).
				float uf, vf, df;
				const float fx = src.x + (src.x - eyeAdjust.x) * 0.25f, fy = src.y + (src.y - eyeAdjust.y) * 0.25f, fz = src.z + (src.z - eyeAdjust.z) * 0.25f;
				if (project(fx, fy, fz, uf, vf, df) && std::abs(df - d) > 1e-7f) {
					depthSign = df > d ? 1.0f : -1.0f;
					signSet = true;
				}
			}
			shimmer[shimmerCount] = float4(u, v, radius, frameSample.shimmerStrength[i] * std::clamp(settings.Temp.HeatShimmerStrength, 0.0f, 3.0f));
			shimmerDepth[shimmerCount] = d;
			++shimmerCount;
		}
	}
	const bool thermalWorks = thawAmount > 0.001f || breathAmount > 0.001f || entryAmount > 0.001f || shimmerCount > 0;

	if (!loaded || !settings.Enabled || !parameters || !sampler || !blendState || !rasterizerState || !depthStencilState ||
		(frostAmount <= 0.001f && rainAmount <= 0.001f && sheet <= 0.001f && !sunWorks && !moonWorks && !atmosWorks && !thermalWorks) ||
		static_cast<unsigned int>(a_output) >= RE::RENDER_TARGETS::kTOTAL || !PrepareShaders())
		return;

	auto* state = globals::state;
	if (state->IsMainOrLoadingMenuOpen() || state->isMapMenuOpen || state->isStatsMenuOpen)
		return;

	auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
	if (!renderer)
		return;
	auto& target = renderer->GetRuntimeData().renderTargets[a_output];
	auto* texture = reinterpret_cast<ID3D11Texture2D*>(target.texture);
	auto* view = reinterpret_cast<ID3D11RenderTargetView*>(target.RTV);
	if (!texture || !view || !PrepareInput(texture, view))
		return;

	const bool floatInput = inputFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || inputFormat == DXGI_FORMAT_R32G32B32A32_FLOAT;
	// nicktheduke: lens glare takes the weather's actual sunlight colour (warm at golden hour, cool moonlight at night),
	// half way to white so it never turns orange.
	float sunR = 1.0f, sunG = 0.95f, sunB = 0.88f;
	if (auto* sky = RE::Sky::GetSingleton()) {
		const auto& c = sky->skyColor[static_cast<uint32_t>(RE::TESWeather::ColorTypes::kSunlight)];
		const float peak = std::max({ c.red, c.green, c.blue });
		if (std::isfinite(peak) && peak > 0.02f) {
			sunR = 0.5f + 0.5f * Clamp01(c.red / peak);
			sunG = 0.5f + 0.5f * Clamp01(c.green / peak);
			sunB = 0.5f + 0.5f * Clamp01(c.blue / peak);
		}
	}
	RenderData data{
		{ 1.0f / static_cast<float>(inputDescription.Width), 1.0f / static_cast<float>(inputDescription.Height), elapsed, floatInput ? 1.0f : 0.0f },
		{ Clamp01(frostAmount), Clamp01(settings.FrostCoverage), std::clamp(settings.FrostScale, 0.25f, 4.0f), Clamp01(settings.FrostOpacity) },
		{ std::clamp(settings.FrostRefraction, 0.0f, 4.0f), std::clamp(settings.FrostBlur, 0.0f, 4.0f), std::clamp(settings.FrostGlow, 0.0f, 3.0f),
			std::lerp(std::clamp(settings.FrostNightIntensity, 0.0f, 3.0f), std::clamp(settings.FrostDayIntensity, 0.0f, 3.0f), DayFactor()) },
		{ settings.FrostTint[0], settings.FrostTint[1], settings.FrostTint[2], std::clamp(settings.FrostEdgeSoftness, 0.05f, 1.0f) },
		{ std::clamp(rainAmount, 0.0f, 2.0f), Clamp01(sheet), std::clamp(settings.RainDropSize, 0.25f, 3.0f), std::clamp(settings.WaterRefraction, 0.0f, 4.0f) },
		{ std::clamp(settings.DropLifetimeSeconds, 0.5f, 20.0f), frostMaskView ? 1.0f : 0.0f, dropAtlasView ? 1.0f : 0.0f, lensDirtView ? 1.0f : 0.0f },
		globals::features::hdrDisplay.GetSharedDataHDR(),
		{ std::clamp(settings.FrostBreathRate, 0.0f, 1.0f), std::clamp(settings.FrostBreathDepth, 0.0f, 1.0f), std::clamp(settings.FrostContrast, 0.0f, 2.0f), std::clamp(settings.Temp.ColdBreathHeight, -0.1f, 0.4f) },
		{ sunU, sunV, sunWorks ? sunStrength : 0.0f, dirtAmount },
		{ glintAmount, sunR, sunG, sunB },
		{ heatAmount, ashAmount, dustAmount, fogAmount },
		{ std::clamp(settings.FogHaloStrength, 0.0f, 3.0f), std::clamp(settings.FogCondensation, 0.0f, 2.0f), emberAmount,
			static_cast<float>(previewBreath > 0.001f ? static_cast<uint32_t>(wallClock / 3.0) % 6u : breathShape) },
		{ Clamp01(thawAmount * std::clamp(settings.Temp.ThawStrength, 0.0f, 2.0f)), Clamp01(breathAmount), std::min(breathAge, 30.0f), Clamp01(entryAmount) },
		{ previewBreath > 0.001f ? 1.0f : exhaleStrength, static_cast<float>(shimmerCount), depthSign, depthView && depthUsable ? 1.0f : 0.0f },
		{},
		{},
		{ moonU, moonV, moonWorks ? moonStrength : 0.0f, moonRadius },
		{ Clamp01(settings.FrostShape), 0.0f, 0.0f, 0.0f }
	};
	for (uint32_t i = 0; i < 8; ++i)
		data.Shimmer[i] = shimmer[i];
	data.ShimmerDepth[0] = float4(shimmerDepth[0], shimmerDepth[1], shimmerDepth[2], shimmerDepth[3]);
	data.ShimmerDepth[1] = float4(shimmerDepth[4], shimmerDepth[5], shimmerDepth[6], shimmerDepth[7]);
	parameters->Update(data);

	auto* context = globals::d3d::context;
	globals::profiler->BeginPass("ScreenWeather::Lens");
	{
		PostProcessState backup(context);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		ID3D11ShaderResourceView* noResources[5]{};
		context->PSSetShaderResources(0, 5, noResources);
		context->CopyResource(inputTexture.get(), texture);

		const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(inputDescription.Width), static_cast<float>(inputDescription.Height), 0.0f, 1.0f };
		context->RSSetViewports(1, &viewport);
		context->RSSetState(rasterizerState.get());
		context->OMSetRenderTargets(1, &view, nullptr);
		context->OMSetBlendState(blendState.get(), nullptr, D3D11_DEFAULT_SAMPLE_MASK);
		context->OMSetDepthStencilState(depthStencilState.get(), 0);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->VSSetShader(vertexShader.get(), nullptr, 0);
		context->PSSetShader(pixelShader.get(), nullptr, 0);

		auto* buffer = parameters->CB();
		ID3D11SamplerState* passSamplers[2]{ sampler.get(), wrapSampler ? wrapSampler.get() : sampler.get() };
		ID3D11ShaderResourceView* passResources[5]{ inputView.get(), frostMaskView.get(), dropAtlasView.get(), lensDirtView.get(), depthView };
		context->PSSetConstantBuffers(0, 1, &buffer);
		context->PSSetSamplers(0, 2, passSamplers);
		context->PSSetShaderResources(0, 5, passResources);
		context->Draw(3, 0);
	}
	globals::profiler->EndPass();
}

void ScreenWeather::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);

	if (ImGui::TreeNodeEx(T(TKEY("frost"), "Frost"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("frost_enabled"), "Lens Frost"), &settings.FrostEnabled);
		ImGui::SliderFloat(T(TKEY("frost_opacity"), "Opacity"), &settings.FrostOpacity, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("frost_coverage"), "Coverage"), &settings.FrostCoverage, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("frost_coverage_tooltip"), "How far the ice vignette closes in from the screen edges when fully frozen."));
		ImGui::SliderFloat(T(TKEY("frost_softness"), "Edge Softness"), &settings.FrostEdgeSoftness, 0.05f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("frost_scale"), "Crystal Size"), &settings.FrostScale, 0.25f, 4.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("frost_day"), "Crystals By Day"), &settings.FrostDayIntensity, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("frost_night"), "Crystals At Night"), &settings.FrostNightIntensity, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("frost_glow"), "Glow"), &settings.FrostGlow, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("frost_glow_tooltip"), "Milky haze from the bright parts of the scene, added under the crystals."));
		ImGui::SliderFloat(T(TKEY("frost_breath_rate"), "Breathing Speed (Hz)"), &settings.FrostBreathRate, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("frost_breath_depth"), "Breathing Depth"), &settings.FrostBreathDepth, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("frost_contrast"), "Ice Contrast"), &settings.FrostContrast, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("frost_shape"), "Frost Shape"), &settings.FrostShape, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("frost_shape_tooltip"), "0 keeps light frost hugging the frame like a rectangle, 1 rounds it toward an oval. Heavier frost always rounds off so it never closes into a box."));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("frost_contrast_tooltip"), "Shades the crystals and tints them cold so the ice still shows against white snow and sky.\n0 = only adds light, as before."));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("frost_breath_tooltip"), "How far the frost edge drifts in and out. Small values creep; large values pulse."));
		ImGui::SliderFloat(T(TKEY("frost_refraction"), "Refraction"), &settings.FrostRefraction, 0.0f, 4.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("frost_blur"), "Blur"), &settings.FrostBlur, 0.0f, 4.0f, "%.2f");
		ImGui::ColorEdit3(T(TKEY("frost_tint"), "Tint"), settings.FrostTint.data());
		ImGui::SliderFloat(T(TKEY("frost_grow"), "Freeze Time (s)"), &settings.FrostGrowSeconds, 0.0f, 120.0f, "%.0f");
		ImGui::SliderFloat(T(TKEY("frost_melt"), "Thaw Time (s)"), &settings.FrostMeltSeconds, 0.0f, 120.0f, "%.0f");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("cold_sources"), "What Makes Frost"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("use_survival"), "Survival Mode Cold"), &settings.UseSurvival);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("use_survival_tooltip"), "Frost follows your Survival Mode cold level (stage 2 to stage 5).\nWith Survival Mode Improved, cold areas and weather also frost the lens before you get cold."));
		ImGui::SliderFloat(T(TKEY("survival_ambient"), "Cold Area Strength"), &settings.SurvivalAmbientWeight, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("weather_snow"), "Snowfall Strength"), &settings.WeatherSnowFrost, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("weather_blizzard"), "Blizzard Strength"), &settings.BlizzardFrost, 0.0f, 1.0f, "%.2f");
		ImGui::SliderInt(T(TKEY("blizzard_wind"), "Blizzard Wind Speed"), &settings.BlizzardWindSpeed, 0, 255);
		ImGui::SliderFloat(T(TKEY("snow_ground"), "Snow On Ground Strength"), &settings.SnowGroundFrost, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("snow_ground_tooltip"), "Light frost (and visible breath) while you stand on snow or ice, even on a clear day.\nRead from the ground around you, so it works with or without Bottled Shaders' Snow Cover."));
		ImGui::SliderFloat(T(TKEY("warmth_protection"), "Warm Clothing Protection"), &settings.WarmthProtection, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("warmth_protection_tooltip"), "How much your clothing's warmth rating holds back frost from snowfall and blizzards.\nSurvival Mode body cold is not reduced: it already accounts for warmth."));

		GameSample shown;
		{
			std::scoped_lock lock(sampleMutex);
			shown = sample;
		}
		ImGui::Text("%s %.2f", T(TKEY("readout_frost"), "Frost now:"), frost);
		if (shown.bodyCold >= 0.0f)
			ImGui::Text("%s %.2f", T(TKEY("readout_body"), "Body cold:"), shown.bodyCold);
		else
			ImGui::Text("%s", T(TKEY("readout_no_survival"), "Survival Mode cold: off or not installed"));
		if (shown.ambientCold >= 0.0f)
			ImGui::Text("%s %.2f", T(TKEY("readout_ambient"), "Area cold (SMI):"), shown.ambientCold);
		ImGui::Text("%s %.2f / %.2f%s", T(TKEY("readout_weather"), "Snow / rain weather:"), shown.snowWeather, shown.rainWeather, shown.blizzard ? " (blizzard)" : "");
		ImGui::Text("%s %.2f", T(TKEY("readout_ground_snow"), "Snow on the ground around you:"), shown.groundSnow);
		if (shown.warmth >= 0.0f)
			ImGui::Text("%s %.0f", T(TKEY("readout_warmth"), "Clothing warmth:"), shown.warmth);
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("water"), "Water"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("water_enabled"), "Lens Water"), &settings.WaterEnabled);
		ImGui::SliderFloat(T(TKEY("rain_amount"), "Rain Drops"), &settings.RainDropAmount, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("rain_size"), "Drop Size"), &settings.RainDropSize, 0.25f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("rain_look_up"), "More When Looking Up"), &settings.RainLookUpBias, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("drop_lifetime"), "Drop Lifetime (s)"), &settings.DropLifetimeSeconds, 0.5f, 20.0f, "%.1f");
		ImGui::SliderFloat(T(TKEY("water_refraction"), "Refraction"), &settings.WaterRefraction, 0.0f, 4.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("surface_sheet"), "Water After Swimming"), &settings.SurfaceSheetAmount, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("sheet_dry"), "Run-off Time (s)"), &settings.SheetDrySeconds, 0.5f, 20.0f, "%.1f");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("sun_lens"), "Sun On The Lens"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("sun_lens_enabled"), "Sun On The Lens"), &settings.SunLensEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("sun_lens_tooltip"), "A low sun on clear days lights up what is on the lens. Works alongside your ENB or Bottled lens flare; it adds no flare of its own."));
		ImGui::BeginDisabled(!settings.SunLensEnabled);
		ImGui::SliderFloat(T(TKEY("sun_lens_strength"), "Strength"), &settings.SunLensStrength, 0.0f, 2.0f, "%.2f");
		ImGui::Checkbox(T(TKEY("sun_glints_enabled"), "Glints On Frost And Water"), &settings.SunGlintsEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("sun_glints_tooltip"), "Frost crystals sparkle and drops light up along the edge facing the sun."));
		ImGui::SliderFloat(T(TKEY("sun_glint_strength"), "Glint Strength"), &settings.SunGlintStrength, 0.0f, 3.0f, "%.2f");
		ImGui::Checkbox(T(TKEY("lens_dirt_enabled"), "Lens Dirt In Sun Glare"), &settings.LensDirtEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("lens_dirt_tooltip"), "Faint dust specks and smudges that only show when the sun shines into the lens."));
		ImGui::SliderFloat(T(TKEY("lens_dirt_amount"), "Lens Dirt Amount"), &settings.LensDirtAmount, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("sun_max_height"), "Sun Height Cutoff"), &settings.SunMaxHeight, 0.2f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("sun_max_height_tooltip"), "Sun elevation where the effect has faded out (0.2 = only near sunrise and sunset, 1.0 = all day)."));
		ImGui::SliderFloat(T(TKEY("sun_cloudy"), "On Cloudy Days"), &settings.SunCloudyAmount, 0.0f, 1.0f, "%.2f");
		ImGui::Text("%s %.2f  (%s %.2f)", T(TKEY("readout_sun"), "Sun on lens now:"), sunLight, T(TKEY("readout_sun_height"), "sun height"), sunHeight);
		ImGui::EndDisabled();
		ImGui::Checkbox(T(TKEY("moon_lens_enabled"), "Full Moon On The Lens"), &settings.MoonLensEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("moon_lens_tooltip"), "A full or nearly full moon on a clear night: a faint silver halo, a thin ring, lens dirt catching its light and a small ghost.\nMuch gentler than the sun. Nothing for crescent or new moons."));
		ImGui::BeginDisabled(!settings.MoonLensEnabled);
		ImGui::SliderFloat(T(TKEY("moon_lens_strength"), "Moon Strength"), &settings.MoonLensStrength, 0.0f, 2.0f, "%.2f");
		ImGui::Text("%s %.2f", T(TKEY("readout_moon"), "Moon on lens now:"), moonLight);
		ImGui::EndDisabled();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("atmosphere"), "Heat, Ash, Dust And Fog"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("atmosphere_tooltip"), "One lens effect at a time: frost beats water, water beats heat haze, heat beats ash and dust, and fog shows only when nothing else does."));
		ImGui::Checkbox(T(TKEY("heat_enabled"), "Heat Haze"), &settings.HeatHazeEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("heat_tooltip"), "Shimmer when you stand near campfires, braziers, forges and lava; lighter near fumaroles and hot springs."));
		ImGui::SliderFloat(T(TKEY("heat_strength"), "Heat Haze Strength"), &settings.HeatHazeStrength, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("heat_radius"), "Heat Range"), &settings.HeatRadiusScale, 0.25f, 4.0f, "%.2f");
		ImGui::Checkbox(T(TKEY("ash_enabled"), "Ash"), &settings.AshEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("ash_tooltip"), "Grey ash settling at the lens edges in ash weathers. Weathers are listed in SKSE\\Plugins\\SloppyCameraFX\\Weather\\LensWeathers.ini."));
		ImGui::SliderFloat(T(TKEY("ash_strength"), "Ash Strength"), &settings.AshStrength, 0.0f, 3.0f, "%.2f");
		ImGui::Checkbox(T(TKEY("embers_enabled"), "Embers"), &settings.EmbersEnabled);
		ImGui::SliderFloat(T(TKEY("ember_strength"), "Ember Strength"), &settings.EmberStrength, 0.0f, 3.0f, "%.2f");
		ImGui::Checkbox(T(TKEY("dust_enabled"), "Dust"), &settings.DustEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("dust_tooltip"), "Dust storms, Helgen and Alduin smoke from the weather list, plus dry gales above the wind speed below."));
		ImGui::SliderFloat(T(TKEY("dust_strength"), "Dust Strength"), &settings.DustStrength, 0.0f, 3.0f, "%.2f");
		ImGui::SliderInt(T(TKEY("dust_wind"), "Dust Wind Speed"), &settings.DustWindSpeed, 0, 255);
		ImGui::Checkbox(T(TKEY("fog_enabled"), "Fog On The Lens"), &settings.FogLensEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("fog_tooltip"), "In dense fog, lights bloom into soft halos and fine condensation beads at the lens edges."));
		ImGui::SliderFloat(T(TKEY("fog_halo"), "Light Halos"), &settings.FogHaloStrength, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("fog_condensation"), "Condensation"), &settings.FogCondensation, 0.0f, 2.0f, "%.2f");
		GameSample shownAtmos;
		{
			std::scoped_lock lock(sampleMutex);
			shownAtmos = sample;
		}
		ImGui::Checkbox(T(TKEY("shimmer_enabled"), "Heat Shimmer Above Lava And Fires"), &settings.Temp.HeatShimmerEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shimmer_tooltip"), "The air just above lava, forges and big fires bends and rises, seen from a distance too.\nThis is in the world, not on the lens, so it shows alongside rain or frost. Replaces Hot Lava - Heat Distortion."));
		ImGui::BeginDisabled(!settings.Temp.HeatShimmerEnabled);
		ImGui::SliderFloat(T(TKEY("shimmer_strength"), "Shimmer Strength"), &settings.Temp.HeatShimmerStrength, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("shimmer_range"), "Shimmer Range"), &settings.Temp.HeatShimmerRange, 0.25f, 3.0f, "%.2f");
		ImGui::EndDisabled();
		ImGui::Text("%s %.2f / %.2f / %.2f / %.2f", T(TKEY("readout_atmos"), "Heat / ash / dust / fog now:"), heat, ash, dust, fogLens);
		if (shownAtmos.nearestHeat >= 0.0f)
			ImGui::Text("%s %.0f", T(TKEY("readout_heat"), "Nearest heat source (units):"), shownAtmos.nearestHeat);
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("temperature"), "Temperature And Breath"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("thaw_enabled"), "Frost Thaws Near Heat"), &settings.Temp.ThawEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("thaw_tooltip"), "Standing by a fire, forge or lava melts the frost, which runs down the lens as drips."));
		ImGui::SliderFloat(T(TKEY("thaw_strength"), "Thaw Strength"), &settings.Temp.ThawStrength, 0.0f, 2.0f, "%.2f");
		ImGui::Checkbox(T(TKEY("warm_areas"), "Warm Areas Hold Back Frost"), &settings.Temp.WarmAreasBlockFrost);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("warm_areas_tooltip"), "With Survival Mode Improved: in areas it reads as warm, snowfall frosts the lens only lightly."));
		ImGui::Checkbox(T(TKEY("cold_breath"), "Cold Breath (First Person)"), &settings.Temp.ColdBreathEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("cold_breath_tooltip"), "Your breath shows as vapor rising from below the screen whenever the air is cold.\nCold decides if it shows; exertion (stamina, sprinting, fighting) sets the rhythm and size.\nShares one breathing rhythm with Screen Action's stamina breaths."));
		ImGui::BeginDisabled(!settings.Temp.ColdBreathEnabled);
		ImGui::SliderFloat(T(TKEY("cold_breath_strength"), "Breath Strength"), &settings.Temp.ColdBreathStrength, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("cold_breath_height"), "Breath Height"), &settings.Temp.ColdBreathHeight, -0.1f, 0.4f, "%.2f");
		ImGui::SliderFloat(T(TKEY("cold_breath_interval"), "Seconds Between Breaths"), &settings.Temp.ColdBreathInterval, 2.0f, 15.0f, "%.1f s");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("cold_breath_interval_tooltip"), "Shortest time between two visible puffs. A puff still waits for an exhale, so when you are calm and breathing slowly it can be longer."));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("cold_breath_height_tooltip"), "Moves the vapor up or down the screen. 0 is the original low position."));
		ImGui::BeginDisabled(coldBreathNGArt.empty());
		ImGui::Checkbox(T(TKEY("hide_cbng"), "Hide Cold Breath NG In First Person"), &settings.Temp.HideColdBreathNGFirstPerson);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("hide_cbng_tooltip"), "Cold Breath NG's puff hangs off your character's head, which floats at the top of the screen in first person\n(especially with Improved Camera). This hides it in first person only; third person and NPCs keep it."));
		ImGui::EndDisabled();
		ImGui::Checkbox(T(TKEY("entry_fog"), "Lens Fogs Coming In From The Cold"), &settings.Temp.EntryFogEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("entry_fog_tooltip"), "A cold lens fogs up in a warm room and clears from the centre out. Stronger the colder you were. Not in cold interiors."));
		ImGui::BeginDisabled(!settings.Temp.EntryFogEnabled);
		ImGui::SliderFloat(T(TKEY("entry_fog_strength"), "Entry Fog Strength"), &settings.Temp.EntryFogStrength, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("entry_fog_seconds"), "Entry Fog Clears In (s)"), &settings.Temp.EntryFogSeconds, 2.0f, 60.0f, "%.0f");
		ImGui::EndDisabled();
		GameSample shownTemp;
		{
			std::scoped_lock lock(sampleMutex);
			shownTemp = sample;
		}
		ImGui::Text("%s %.2f  %s %.2f  %s %.2f", T(TKEY("readout_breath_cold"), "Air cold:"), breathCold, T(TKEY("readout_exertion"), "exertion:"), shownTemp.exertion,
			T(TKEY("readout_entry"), "entry fog:"), entryFog);
		ImGui::Text("%s %s, %s %s", T(TKEY("readout_cbng"), "Cold Breath NG:"), coldBreathNGArt.empty() ? T(TKEY("not_found"), "not found") : T(TKEY("found"), "found"),
			T(TKEY("readout_bnb"), "Blade and Blunt:"), bladeAndBlunt ? T(TKEY("found"), "found") : T(TKEY("not_found"), "not found"));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("preview"), "Preview"))) {
		ImGui::SliderFloat(T(TKEY("preview_frost"), "Force Frost"), &previewFrost, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("preview_rain"), "Force Rain Drops"), &previewRain, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("preview_sun"), "Force Sun On Lens"), &previewSun, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("preview_moon"), "Force Moon On Lens"), &previewMoon, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("preview_heat"), "Force Heat Haze"), &previewHeat, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("preview_ash"), "Force Ash"), &previewAsh, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("preview_embers"), "Force Embers"), &previewEmbers, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("preview_dust"), "Force Dust"), &previewDust, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("preview_fog"), "Force Fog"), &previewFog, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("preview_breath"), "Force Cold Breath"), &previewBreath, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("preview_entry"), "Force Entry Fog"), &previewEntryFog, 0.0f, 1.0f, "%.2f");
		if (ImGui::Button(T(TKEY("preview_sheet"), "Splash Lens")))
			sheet = 1.0f;
		ImGui::TreePop();
	}
}

void ScreenWeather::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ScreenWeather::LoadSettings(json& o_json)
{
	settings = o_json;
	for (float& c : settings.FrostTint)
		c = Clamp01(c);
}

void ScreenWeather::RestoreDefaultSettings()
{
	settings = {};
}

// ---------------------------------------------------------------------------------------------------------------------
// SLOPPY CAMERA FX module

ScreenWeather globals::features::screenWeather;

namespace
{
	class WeatherModule final : public SloppyCameraFX::Module
	{
	public:
		const char* Name() const override { return "Weather"; }
		const char* LegacyName() const override { return "Screen Weather"; }
		bool IsEnabled() const override { return globals::features::screenWeather.settings.Enabled; }

		void OnDataLoaded() override { globals::features::screenWeather.DataLoaded(); }
		void OnGameLoaded() override { globals::features::screenWeather.GameLoaded(); }
		void SetupResources() override { globals::features::screenWeather.SetupResources(); }
		// Every frame, on or off: when off it clears its share of the lens and gives Cold Breath NG its puff back.
		void Update() override { globals::features::screenWeather.Reset(); }
		void Render(uint32_t a_output) override { globals::features::screenWeather.RenderPostProcess(static_cast<RE::RENDER_TARGET>(a_output)); }
		void DrawMenu() override
		{
			ImGui::Text("SLOPPY CAMERA FX - Weather by nicktheduke");
			ImGui::Separator();
			globals::features::screenWeather.DrawSettings();
			ImGui::Separator();
			if (ImGui::Button("Restore Defaults##weather"))
				globals::features::screenWeather.RestoreDefaultSettings();
		}
		void LoadSettings(nlohmann::json& a_json) override { globals::features::screenWeather.LoadSettings(a_json); }
		void SaveSettings(nlohmann::json& a_json) override { globals::features::screenWeather.SaveSettings(a_json); }
	};

	struct Registrar
	{
		Registrar()
		{
			static WeatherModule module;
			SloppyCameraFX::RegisterModule(&module, 100);  // before Combat (200): the same order the lens effects had in Bottled
		}
	} registrar;
}
