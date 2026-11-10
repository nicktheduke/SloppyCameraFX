// SPDX-License-Identifier: GPL-3.0-or-later
// SLOPPY CAMERA FX - Combat by nicktheduke: combat and action effects on the camera lens. See Combat.h.
#include "Combat.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>

#include "LensShared.h"
#include "PrecisionAPI.h"

#define I18N_KEY_PREFIX "feature.screen_action."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenAction::Settings,
	Enabled,
	PhotosensitiveSafe,
	MasterStrength,
	BloodEnabled,
	BloodStrength,
	BloodWithOtherScreenBlood,
	VenomEnabled,
	VenomStrength,
	WebEnabled,
	WebStrength,
	FrostHitsEnabled,
	FrostHitStrength,
	FireHitsEnabled,
	FireHitStrength,
	ShockEnabled,
	ShockStrength,
	DragonfireEnabled,
	DragonfireStrength,
	RimeEnabled,
	RimeStrength,
	PoisonEnabled,
	PoisonStrength,
	StompDustEnabled,
	StompStrength,
	SteamEnabled,
	SteamStrength,
	GritEnabled,
	GritStrength,
	AshPuffEnabled,
	AshPuffStrength,
	ShoutRippleEnabled,
	ShoutStrength,
	ImpactFlashEnabled,
	FlashStrength,
	SlashEnabled,
	SlashStrength,
	StaminaBreathEnabled,
	StaminaBreathStrength,
	HitShakeEnabled,
	HitShakeStrength,
	HitShakeFirstPerson,
	HitShakeThirdPerson,
	ShakeNormal,
	ShakePower,
	ShakeBash,
	ShakeArrow,
	ShakeBlocked,
	ShakeBig,
	ShakeStagger,
	IntegrationPrecision,
	IntegrationFPCameraSettle,
	IntegrationValhalla,
	IntegrationStaggerRemover,
	IntegrationDismember,
	DismemberStrength,
	DismemberKick);

namespace
{
	constexpr auto kShaderPath = L"Data\\SKSE\\Plugins\\SloppyCameraFX\\Shaders\\Combat.hlsl";
	constexpr uint32_t kMaxSplats = 12;
	constexpr float kQueryInterval = 0.15f;
	constexpr float kMaximumFrameStep = 0.1f;

	struct alignas(16) RenderData
	{
		float4 BufferDim;
		float4 HDRData;
		float4 Global;
		float4 LayersA;
		float4 LayersB;
		float4 RimeDir;
		float4 Shout;
		float4 Flash;
		float4 Slash;
		float4 Breath;
		float4 Shake;  // nicktheduke: camera shake: offset xy (screen heights), roll (rad), zoom
		float4 SplatA[kMaxSplats];
		float4 SplatB[kMaxSplats];
		float4 SplatC[kMaxSplats];
	};
	static_assert(sizeof(RenderData) == 752);

	float Clamp01(float a_value) { return std::isfinite(a_value) ? std::clamp(a_value, 0.0f, 1.0f) : 0.0f; }

	float Smooth(float a_edge0, float a_edge1, float a_x)
	{
		if (a_edge1 <= a_edge0)
			return a_x >= a_edge1 ? 1.0f : 0.0f;
		const float t = std::clamp((a_x - a_edge0) / (a_edge1 - a_edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	bool ContainsInsensitive(std::string_view a_text, std::string_view a_word)
	{
		if (a_text.size() < a_word.size())
			return false;
		auto it = std::search(a_text.begin(), a_text.end(), a_word.begin(), a_word.end(),
			[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
		return it != a_text.end();
	}

	bool RaceHas(const RE::Actor* a_actor, const char* a_keyword)
	{
		auto* race = a_actor ? a_actor->GetRace() : nullptr;
		return race && race->HasKeywordString(a_keyword);
	}

	std::string_view RaceEditorID(const RE::Actor* a_actor)
	{
		auto* race = a_actor ? a_actor->GetRace() : nullptr;
		const char* id = race ? race->GetFormEditorID() : nullptr;
		return id ? std::string_view(id) : std::string_view();
	}

	class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
	{
	public:
		static HitSink* GetSingleton()
		{
			static HitSink singleton;
			return &singleton;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
		{
			if (a_event)
				globals::features::screenAction.OnHit(a_event);
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	class SpellCastSink final : public RE::BSTEventSink<RE::TESSpellCastEvent>
	{
	public:
		static SpellCastSink* GetSingleton()
		{
			static SpellCastSink singleton;
			return &singleton;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
		{
			if (a_event)
				globals::features::screenAction.OnSpellCast(a_event);
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	template <class E, void (ScreenAction::*Handler)(const E*)>
	class ForwardSink final : public RE::BSTEventSink<E>
	{
	public:
		static ForwardSink* GetSingleton()
		{
			static ForwardSink singleton;
			return &singleton;
		}
		RE::BSEventNotifyControl ProcessEvent(const E* a_event, RE::BSTEventSource<E>*) override
		{
			if (a_event)
				(globals::features::screenAction.*Handler)(a_event);
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	using MagicEffectApplySink = ForwardSink<RE::TESMagicEffectApplyEvent, &ScreenAction::OnMagicEffectApply>;
	using EquipSink = ForwardSink<RE::TESEquipEvent, &ScreenAction::OnEquip>;
	using ObjectLoadedSink = ForwardSink<RE::TESObjectLoadedEvent, &ScreenAction::OnObjectLoaded>;

	class AnimationSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		static AnimationSink* GetSingleton()
		{
			static AnimationSink singleton;
			return &singleton;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
		{
			if (a_event)
				globals::features::screenAction.OnPlayerAnimation(a_event);
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// nicktheduke: hit shake profiles. Mirrors the approved preview model (_claude_work/hitshake/shake.py) exactly.
	// amp: screen heights, rot: radians, freq: Hz, tau: decay (s), life (s), shape 0 = damped wobble, 1 = single bump
	// peaking at t0, zoom: zoom kick at the bump's peak, noise: share of organic (value noise) motion.
	struct ShakeProfile
	{
		float amp, rot, freq, tau, life;
		int shape;
		float t0, zoom, noise;
	};
	constexpr std::array<ShakeProfile, 8> kShakeProfiles{ {
		{ 0.0055f, 0.0035f, 9.0f, 0.050f, 0.25f, 0, 0.0f, 0.000f, 0.35f },   // normal: quick jolt
		{ 0.0100f, 0.0080f, 5.5f, 0.170f, 0.70f, 0, 0.0f, 0.000f, 0.40f },   // power: jolt and rebounds
		{ 0.0040f, 0.0020f, 0.0f, 0.000f, 0.30f, 1, 0.045f, 0.015f, 0.15f },  // bash: thud
		{ 0.0035f, 0.0020f, 15.0f, 0.025f, 0.12f, 0, 0.0f, 0.000f, 0.20f },  // arrow: snap
		{ 0.0022f, 0.0012f, 7.0f, 0.070f, 0.30f, 0, 0.0f, 0.000f, 0.30f },   // blocked: muffled
		{ 0.0150f, 0.0100f, 3.8f, 0.220f, 0.90f, 0, 0.0f, 0.000f, 0.60f },   // big creature: heavy reverb
		{ 0.0100f, 0.0180f, 0.0f, 0.000f, 0.30f, 1, 0.070f, 0.006f, 0.20f },  // stagger: quick lurch
		{ 0.0140f, 0.0260f, 0.0f, 0.000f, 0.30f, 1, 0.060f, 0.008f, 0.20f },  // knockdown: quick, harder lurch
	} };
	constexpr float kShakeMaxOffset = 0.020f;  // soft cap at strength 1 (screen heights)
	constexpr float kShakeMaxRoll = 0.026f;    // soft cap at strength 1 (about 1.5 degrees)
	constexpr size_t kMaxKicks = 4;

	float ShakeHash(int64_t a_i, uint32_t a_salt)
	{
		uint32_t x = static_cast<uint32_t>(a_i) * 0x9E3779B1u + a_salt * 0x85EBCA77u;
		x ^= x >> 15;
		x *= 0x2C1B3C6Du;
		x ^= x >> 12;
		x *= 0x297A2D39u;
		x ^= x >> 15;
		return static_cast<float>(x & 0xFFFFFFu) / 16777216.0f;
	}

	float ShakeNoise(float a_x, uint32_t a_salt)  // smooth value noise, -1..1
	{
		const float fl = std::floor(a_x);
		const float f = a_x - fl;
		const float u = f * f * (3.0f - 2.0f * f);
		const int64_t i = static_cast<int64_t>(fl);
		const float a = ShakeHash(i, a_salt), b = ShakeHash(i + 1, a_salt);
		return (a + (b - a) * u) * 2.0f - 1.0f;
	}

	// Reads "key = value" from an ini section (case-insensitive), the way the game's virtual file system sees it.
	std::optional<std::string> IniValue(const std::filesystem::path& a_path, std::string_view a_section, std::string_view a_key)
	{
		std::ifstream file(a_path);
		if (!file)
			return std::nullopt;
		auto lower = [](std::string a_text) {
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		};
		auto trim = [](std::string a_text) {
			const auto first = a_text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return first == std::string::npos ? std::string() : a_text.substr(first, last - first + 1);
		};
		const std::string section = lower(std::string(a_section)), key = lower(std::string(a_key));
		std::string line, current;
		while (std::getline(file, line)) {
			line = trim(line);
			if (line.empty() || line[0] == ';' || line[0] == '#')
				continue;
			if (line.front() == '[') {
				current = lower(trim(line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1)));
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos || current != section)
				continue;
			if (lower(trim(line.substr(0, eq))) == key) {
				std::string value = trim(line.substr(eq + 1));
				if (const auto comment = value.find(';'); comment != std::string::npos)
					value = trim(value.substr(0, comment));
				return value;
			}
		}
		return std::nullopt;
	}

	// MCM Helper: the user's saved value first, then the mod's default.
	std::optional<std::string> McmValue(std::string_view a_mod, std::string_view a_section, std::string_view a_key)
	{
		if (auto value = IniValue(std::filesystem::path("Data/MCM/Settings") / (std::string(a_mod) + ".ini"), a_section, a_key))
			return value;
		return IniValue(std::filesystem::path("Data/MCM/Config") / std::string(a_mod) / "settings.ini", a_section, a_key);
	}

	bool IniBool(const std::optional<std::string>& a_value, bool a_fallback)
	{
		if (!a_value || a_value->empty())
			return a_fallback;
		const char c = static_cast<char>(std::tolower(static_cast<unsigned char>((*a_value)[0])));
		if (c == 't' || c == '1')
			return true;
		if (c == 'f' || c == '0')
			return false;
		try {
			return std::stof(*a_value) != 0.0f;
		} catch (...) {
			return a_fallback;
		}
	}

	float IniFloat(const std::optional<std::string>& a_value, float a_fallback)
	{
		if (!a_value || a_value->empty())
			return a_fallback;
		try {
			const float value = std::stof(*a_value);
			return std::isfinite(value) ? value : a_fallback;
		} catch (...) {
			return a_fallback;
		}
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
			context->PSGetSamplers(0, 1, &samplerState);
			context->PSGetShaderResources(0, 1, &resource);
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
			context->PSSetSamplers(0, 1, &samplerState);
			context->PSSetShaderResources(0, 1, &resource);
			for (IUnknown* object : std::initializer_list<IUnknown*>{ renderTarget, depthStencil, blendState, depthState, rasterizerState, inputLayout,
					 vertexShader, pixelShader, constantBuffer, samplerState, resource }) {
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
		ID3D11SamplerState* samplerState = nullptr;
		ID3D11ShaderResourceView* resource = nullptr;
	};

	// Cooldown keys (main thread).
	enum CooldownKey : uint32_t
	{
		kCoolBlood = 1,
		kCoolVenom,
		kCoolWeb,
		kCoolIce,
		kCoolFire,
		kCoolShock,
		kCoolStomp,
		kCoolSteam,
		kCoolShout,
		kCoolFlash
	};
}

// ---------------------------------------------------------------------------------------------------------------------
// Setup

void ScreenAction::SetupResources()
{
	auto* device = globals::d3d::device;
	parameters = std::make_unique<ConstantBuffer>(ConstantBufferDesc<RenderData>(), "ScreenAction::Parameters");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	if (SUCCEEDED(device->CreateSamplerState(&samplerDesc, sampler.put())))
		Util::SetResourceName(sampler.get(), "ScreenAction::Sampler");

	D3D11_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (SUCCEEDED(device->CreateBlendState(&blendDesc, blendState.put())))
		Util::SetResourceName(blendState.get(), "ScreenAction::BlendState");

	D3D11_RASTERIZER_DESC rasterizerDesc{};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.DepthClipEnable = true;
	if (SUCCEEDED(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put())))
		Util::SetResourceName(rasterizerState.get(), "ScreenAction::Rasterizer");

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	if (SUCCEEDED(device->CreateDepthStencilState(&depthDesc, depthStencilState.put())))
		Util::SetResourceName(depthStencilState.get(), "ScreenAction::DepthState");

	if (!sampler || !blendState || !rasterizerState || !depthStencilState)
		logger::error("[Combat] Failed to create post-process states");
	splats.reserve(kMaxSplats);
	PrepareShaders();
}

bool ScreenAction::PrepareShaders()
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
			Util::SetResourceName(newVertex.get(), "ScreenAction::FullscreenVS");
			Util::SetResourceName(newPixel.get(), "ScreenAction::LensPS");
			vertexShader = std::move(newVertex);
			pixelShader = std::move(newPixel);
		} else {
			logger::error("[Combat] Shader compilation failed; the effect stays off");
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

void ScreenAction::ClearShaderCache()
{
	reloadShaders = true;
}

bool ScreenAction::PrepareInput(ID3D11Texture2D* a_output, ID3D11RenderTargetView* a_view)
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
	if (FAILED(device->CreateTexture2D(&copyDescription, nullptr, texture.put())))
		return false;
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = viewDescription.Format;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	winrt::com_ptr<ID3D11ShaderResourceView> view;
	if (FAILED(device->CreateShaderResourceView(texture.get(), &srv, view.put())))
		return false;
	Util::SetResourceName(texture.get(), "ScreenAction::SceneCopy");
	Util::SetResourceName(view.get(), "ScreenAction::SceneCopy SRV");
	inputDescription = description;
	inputFormat = viewDescription.Format;
	inputTexture = std::move(texture);
	inputView = std::move(view);
	return true;
}

void ScreenAction::PostPostLoad()
{
	ConnectPrecision();
}

void ScreenAction::ConnectPrecision()
{
	if (precisionConnected)
		return;
	// Precision reports every hit with its exact position and the weapon's swing velocity: used for the impact
	// flash and slash streak. Without Precision the plain hit event is used instead (flash only, no swing).
	if (auto* api = reinterpret_cast<PRECISION_API::IVPrecision1*>(PRECISION_API::RequestPluginAPI(PRECISION_API::InterfaceVersion::V1))) {
		const auto result = api->AddPostHitCallback(SKSE::GetPluginHandle(), [](const PRECISION_API::PrecisionHitData& a_data, const RE::HitData&) {
			globals::features::screenAction.OnPrecisionHit(a_data.attacker, a_data.target, a_data.hitPos, a_data.hitPointVelocity);
		});
		precisionConnected = result == PRECISION_API::APIResult::OK || result == PRECISION_API::APIResult::AlreadyRegistered;
	}
	logger::info("[Combat] Precision {}", precisionConnected ? "connected" : "not found (using plain hit events)");
}

double ScreenAction::Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ScreenAction::ReadIntegrations()
{
	// nicktheduke: what the other mods are doing, so the hit shake can step aside or sync. No plugin of our own: DLLs are found
	// by module, plugins by name, settings read from their own ini and MCM files through the game's file system.
	fpcsLoaded = GetModuleHandleW(L"FPCameraSettle.dll") != nullptr;
	fpcsHitReaction = false;
	if (fpcsLoaded) {
		const std::filesystem::path ini("Data/SKSE/Plugins/FPCameraSettle.ini");
		const bool settleOn = IniBool(IniValue(ini, "General", "bEnabled"), true);
		const bool hitDrawn = IniBool(IniValue(ini, "TakingHit_Drawn", "bEnabled"), true);
		const bool hitSheathed = IniBool(IniValue(ini, "TakingHit_Sheathed", "bEnabled"), true);
		const bool punch = IniBool(IniValue(ini, "FOVPunch", "bHitEnabled"), true);
		fpcsHitReaction = settleOn && (hitDrawn || hitSheathed || punch);
	}

	valhallaLoaded = false;
	if (auto* handler = RE::TESDataHandler::GetSingleton())
		valhallaLoaded = handler->LookupLoadedModByName("ValhallaCombat.esp") || handler->LookupLoadedLightModByName("ValhallaCombat.esp");
	valhallaTimedBlockShake = false;
	valhallaTimedBlockWindow = 0.0f;
	if (valhallaLoaded) {
		const bool timed = IniBool(McmValue("ValhallaCombat", "TimedBlocking", "bTimedBlockToggle"), true);
		valhallaTimedBlockShake = timed && IniBool(McmValue("ValhallaCombat", "TimedBlocking", "bTimedBlockScreenShake"), true);
		valhallaTimedBlockWindow = std::clamp(IniFloat(McmValue("ValhallaCombat", "TimedBlocking", "fTimedBlockWindow"), 0.3f), 0.0f, 1.5f);
	}

	// 3rd Person Camera Stagger Remover replaces the stagger camera animation with a loose file.
	std::error_code error;
	staggerRemover = std::filesystem::exists("Data/meshes/actors/character/animations/staggercamera.hkx", error);

	precisionHitstopOnTarget = false;
	precisionHitstop = 0.0f;
	if (precisionConnected) {
		const bool hitstop = IniBool(McmValue("Precision", "Hitstop", "bEnableHitstop"), true);
		precisionHitstopOnTarget = hitstop && IniBool(McmValue("Precision", "Hitstop", "bApplyHitstopToTarget"), true);
		precisionHitstop = std::clamp(IniFloat(McmValue("Precision", "Hitstop", "fHitstopDurationNPC"), 0.07f), 0.0f, 0.3f);
		precisionHitstopPowerMult = std::clamp(IniFloat(McmValue("Precision", "Hitstop", "fHitstopDurationPowerAttackMultiplier"), 1.3f), 1.0f, 3.0f);
	}
	logger::info("[Combat] Hit shake integrations: FP Camera Settle {} (hit reaction {}), Valhalla {} (timed block shake {}, window {:.2f}s), stagger camera remover {}, Precision hitstop on target {} ({:.3f}s)",
		fpcsLoaded ? "found" : "not found", fpcsHitReaction ? "on" : "off", valhallaLoaded ? "found" : "not found", valhallaTimedBlockShake ? "on" : "off",
		valhallaTimedBlockWindow, staggerRemover ? "found" : "not found", precisionHitstopOnTarget ? "yes" : "no", precisionHitstop);
}

void ScreenAction::ConnectPlayerAnimation()
{
	// The player's animation graphs are rebuilt on load and race change; registering again is harmless (sinks are a set).
	if (auto* player = RE::PlayerCharacter::GetSingleton()) {
		player->RemoveAnimationGraphEventSink(AnimationSink::GetSingleton());
		player->AddAnimationGraphEventSink(AnimationSink::GetSingleton());
	}
}

bool ScreenAction::ShakeAllowed(bool a_firstPerson) const
{
	if (!settings.Enabled || !settings.HitShakeEnabled || settings.HitShakeStrength <= 0.0f)
		return false;
	if (!a_firstPerson)
		return settings.HitShakeThirdPerson;
	if (!settings.HitShakeFirstPerson)
		return false;
	// FP Camera Settle has its own first person hit jolt: Auto steps aside while it is on, On always steps aside.
	switch (settings.IntegrationFPCameraSettle) {
	case 0:
		return !(fpcsLoaded && fpcsHitReaction);
	case 1:
		return !fpcsLoaded;
	default:
		return true;
	}
}

void ScreenAction::OnPlayerAnimation(const RE::BSAnimationGraphEvent* a_event)
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player || a_event->holder != player)
		return;
	const std::string_view tag = a_event->tag.c_str() ? a_event->tag.c_str() : "";
	if (tag == "blockStartOut" || tag == "blockStart") {
		if (!wasBlocking)
			blockRaisedAt = Now();
		wasBlocking = true;
		return;
	}
	if (tag == "blockStop") {
		wasBlocking = false;
		return;
	}
	if (tag != "staggerStart")
		return;
	// Stagger: whatever caused it (vanilla, a poise mod), sized by the game's own stagger magnitude. Both graphs
	// (first and third person) send it, so only once per stagger.
	const double now = Now();
	if (now - lastStaggerAt < 0.25)
		return;
	lastStaggerAt = now;
	auto* camera = RE::PlayerCamera::GetSingleton();
	const bool firstPerson = camera && camera->IsInFirstPerson();
	if (firstPerson || !ShakeAllowed(false))
		return;  // first person: the stagger animation already moves the view
	float magnitude = 0.5f;
	player->GetGraphVariableFloat("staggerMagnitude", magnitude);
	if (!std::isfinite(magnitude))
		magnitude = 0.5f;
	Pending kick;
	kick.isShake = true;
	kick.variant = static_cast<uint32_t>(ShakeType::kStagger);
	kick.strength = 0.6f + 0.8f * std::clamp(magnitude, 0.0f, 1.0f);
	float direction = 0.0f;
	player->GetGraphVariableFloat("staggerDirection", direction);
	// staggerDirection: 0 = from the front, 0.25 right, 0.5 behind, 0.75 left (fraction of a turn)
	if (std::isfinite(direction)) {
		const float angle = direction * 6.2831853f;
		kick.hasSwing = true;
		kick.swing = { -std::sin(angle), 0.0f, 0.0f };  // marker: screen push x in swing.x
	}
	Queue(kick);
}

void ScreenAction::DataLoaded()
{
	ConnectPrecision();  // Precision's author suggests asking again later if the early request failed
	if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
		events->AddEventSink<RE::TESHitEvent>(HitSink::GetSingleton());
		events->AddEventSink<RE::TESSpellCastEvent>(SpellCastSink::GetSingleton());
	}
	if (auto* handler = RE::TESDataHandler::GetSingleton()) {
		auto isLoaded = [handler](std::string_view a_name) { return handler->LookupLoadedModByName(a_name) || handler->LookupLoadedLightModByName(a_name); };
		otherScreenBlood = false;
		for (const char* name : { "ScreenBloodEnhanced.esp", "EnhancedBloodTextures.esp", "Screen Blood.esp" }) {
			if (isLoaded(name)) {
				otherScreenBlood = true;
				otherScreenBloodName = name;
				break;
			}
		}
		sanguineSymphony = isLoaded("Sanguine Symphony.esp");
		screenBloodEnhanced = isLoaded("ScreenBloodEnhanced.esp");
		if (screenBloodEnhanced) {
			auto find = [](const char* a_id) { return RE::TESForm::LookupByEditorID<RE::TESGlobal>(a_id); };
			sbe.count = find("ScreenBloodEnhancedGlobal_SplatterCount");
			sbe.distance = find("ScreenBloodEnhancedGlobal_Distance");
			sbe.heading = find("ScreenBloodEnhancedGlobal_Heading");
			sbe.unarmed = find("ScreenBloodEnhancedGlobal_Unarmed");
			sbe.blunt = find("ScreenBloodEnhancedGlobal_Blunt");
			sbe.blade = find("ScreenBloodEnhancedGlobal_Blade");
			sbe.projectile = find("ScreenBloodEnhancedGlobal_Projectile");
			sbe.power = find("ScreenBloodEnhancedGlobal_PowerAttack");
			sbe.wipe = find("ScreenBloodEnhancedGlobal_AnimTrigger");
			logger::info("[Combat] Screen Blood Enhanced settings {}, wipe {}", sbe.count ? "found" : "not found", sbe.wipe ? "found" : "not found");
		}
		bladeAndBlunt = isLoaded("BladeAndBlunt.esp");
	}
	ReadIntegrations();
	logger::info("[Combat] Other screen blood: {}, Sanguine Symphony: {}, Blade and Blunt: {}",
		otherScreenBlood ? otherScreenBloodName : "none", sanguineSymphony ? "yes" : "no", bladeAndBlunt ? "yes" : "no");

	// nicktheduke: Seb263's Dismembering Framework and Next-Gen Decapitations, by their own form IDs.
	if (auto* handler = RE::TESDataHandler::GetSingleton()) {
		dfLoaded = handler->LookupLoadedModByName("Dismembering Framework.esm") || handler->LookupLoadedLightModByName("Dismembering Framework.esm");
		ngdLoaded = handler->LookupLoadedModByName("Next-Gen Decapitations.esp") || handler->LookupLoadedLightModByName("Next-Gen Decapitations.esp");
		if (dfLoaded) {
			dfSpray = handler->LookupForm<RE::SpellItem>(0x80C, "Dismembering Framework.esm");
			dfSprayTrace = handler->LookupForm<RE::SpellItem>(0x812, "Dismembering Framework.esm");
			dfSprayEffect = handler->LookupForm<RE::EffectSetting>(0x80D, "Dismembering Framework.esm");
			dfSprayTraceEffect = handler->LookupForm<RE::EffectSetting>(0x811, "Dismembering Framework.esm");
			dfLimb = handler->LookupForm(0x801, "Dismembering Framework.esm");
		}
		if (ngdLoaded) {
			ngdNeck = handler->LookupForm(0x806, "Next-Gen Decapitations.esp");
			ngdNeckAfflicted = handler->LookupForm(0x814, "Next-Gen Decapitations.esp");
		}
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton(); events && (dfLoaded || ngdLoaded)) {
			events->AddEventSink<RE::TESMagicEffectApplyEvent>(MagicEffectApplySink::GetSingleton());
			events->AddEventSink<RE::TESEquipEvent>(EquipSink::GetSingleton());
			events->AddEventSink<RE::TESObjectLoadedEvent>(ObjectLoadedSink::GetSingleton());
		}
	}
	logger::info("[Combat] Dismembering Framework {} (spray spell {}, spray effect {}, limb {}), Next-Gen Decapitations {} (neck {})",
		dfLoaded ? "found" : "not found", dfSpray ? "ok" : "-", dfSprayEffect ? "ok" : "-", dfLimb ? "ok" : "-", ngdLoaded ? "found" : "not found", ngdNeck ? "ok" : "-");
}

void ScreenAction::GameLoaded()
{
	std::scoped_lock lock(pendingMutex);
	pending.clear();
	splats.clear();
	dragonfire = rime = poison = shock = steam = stomp = staminaShimmer = 0.0f;
	shoutAge = flashAge = 99.0f;
	kicks.clear();
	shakeX = shakeY = shakeRot = 0.0f;
	shakeZoom = 1.0f;
	lastHealth = -1.0f;
	wasBlocking = wasRagdoll = false;
	if (auto* tasks = SKSE::GetTaskInterface())
		tasks->AddTask([this] { ConnectPlayerAnimation(); });
}

// ---------------------------------------------------------------------------------------------------------------------
// Game side (main thread)

bool ScreenAction::BloodAllowed() const
{
	return settings.BloodEnabled && (!otherScreenBlood || settings.BloodWithOtherScreenBlood != 0);
}

bool ScreenAction::ReplacingOtherBlood() const
{
	return settings.Enabled && settings.BloodEnabled && otherScreenBlood && settings.BloodWithOtherScreenBlood == 1;
}

int ScreenAction::SBESplatterCount(RE::Actor* a_victim, RE::TESObjectWEAP* a_weapon, bool a_power) const
{
	// Screen Blood Enhanced's own rule (its OnWeaponHit script), read from its live settings, so ours shows exactly
	// when and as much as theirs would have.
	auto value = [](const RE::TESGlobal* a_global, float a_fallback) { return a_global && std::isfinite(a_global->value) ? a_global->value : a_fallback; };
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* race = a_victim ? a_victim->GetRace() : nullptr;
	if (!player || !a_weapon || !race || !race->HasKeywordString("SBE_ApplyScreenBlood"))
		return 0;
	float splatter = value(sbe.count, 10.0f);
	const auto type = a_weapon->GetWeaponType();
	if (type == RE::WEAPON_TYPE::kHandToHandMelee)
		splatter *= value(sbe.unarmed, 0.6f);
	else if (type == RE::WEAPON_TYPE::kOneHandMace || a_weapon->HasKeywordString("WeapTypeWarhammer"))
		splatter *= value(sbe.blunt, 0.8f);
	else if (type == RE::WEAPON_TYPE::kBow || type == RE::WEAPON_TYPE::kStaff || type == RE::WEAPON_TYPE::kCrossbow)
		splatter *= value(sbe.projectile, 0.8f);
	else
		splatter *= value(sbe.blade, 1.0f);
	if (a_power)
		splatter *= value(sbe.power, 2.0f);

	const float maxDistance = value(sbe.distance, 4.0f) * 69.99104f;
	const float maxHeading = value(sbe.heading, 70.0f);
	if (maxDistance <= 0.0f || maxHeading <= 0.0f)
		return 0;
	const auto me = player->GetPosition();
	const auto them = a_victim->GetPosition();
	const float closeness = std::max(0.0f, maxDistance - me.GetDistance(them)) / maxDistance;
	// Heading angle to the target in degrees (Skyrim yaw: 0 = north, clockwise).
	const float toTarget = std::atan2(them.x - me.x, them.y - me.y);
	float diff = (toTarget - player->data.angle.z) * 57.29578f;
	while (diff > 180.0f)
		diff -= 360.0f;
	while (diff < -180.0f)
		diff += 360.0f;
	const float facing = std::max(0.0f, maxHeading - std::abs(diff)) / maxHeading;
	if (closeness <= 0.0f || facing <= 0.0f)
		return 0;
	splatter *= closeness * closeness * closeness;                       // their ease-in cubic
	splatter *= 1.0f - (1.0f - facing) * (1.0f - facing) * (1.0f - facing);  // their ease-out cubic
	return static_cast<int>(std::floor(splatter + 0.5f));
}

void ScreenAction::UpdateOtherSplatter()
{
	// Hide the other mod's splatter (the game's own screen blood, which Screen Blood Enhanced and EBT drive through these
	// settings) while ours replaces it; put it back the moment we stop. They can re-apply their values at any load, so
	// this runs on every scan and remembers whatever they last set.
	static constexpr std::array<const char*, 5> kOpacity{ "fBloodSplatterMinOpacity", "fBloodSplatterMaxOpacity", "fBloodSplatterMinOpacity2",
		"fBloodSplatterMaxOpacity2", "fBloodSplatterFlareMult" };
	auto* collection = RE::GameSettingCollection::GetSingleton();
	if (!collection || !otherScreenBlood)
		return;
	if (auto* duration = collection->GetSetting("fBloodSplatterDuration"))
		otherBloodDuration = duration->GetFloat();
	if (auto* countBase = collection->GetSetting("fBloodSplatterCountBase"))
		otherReceiveBlood = countBase->GetFloat() > 0.0f;
	const bool hide = ReplacingOtherBlood();
	for (const char* name : kOpacity) {
		auto* setting = collection->GetSetting(name);
		if (!setting)
			continue;
		const float current = setting->GetFloat();
		if (hide) {
			if (current != 0.0f) {
				savedSplatter[name] = current;
				setting->data.f = 0.0f;
			}
		} else if (vanillaSplatterHidden) {
			if (const auto it = savedSplatter.find(name); it != savedSplatter.end() && current == 0.0f)
				setting->data.f = it->second;
		}
	}
	vanillaSplatterHidden = hide;

	// Their wipe: when the wipe animation starts, the lens is cleared 0.6 s in (matching their script).
	if (sbe.wipe) {
		const float trigger = sbe.wipe->value;
		if (trigger > 0.0f && lastWipeTrigger <= 0.0f && hide) {
			Pending wipe;
			wipe.isWipe = true;
			Queue(wipe);
		}
		lastWipeTrigger = trigger;
	}
}

uint32_t ScreenAction::ClassifyBlood(RE::Actor* a_actor) const
{
	// Uses the race's blood material first (the same thing Sanguine Symphony keys its blood groups on, so the lens
	// agrees with the world with or without it), then race keywords and names for the finer cases.
	auto* race = a_actor ? a_actor->GetRace() : nullptr;
	if (!race)
		return 0;
	const auto id = RaceEditorID(a_actor);
	if (ContainsInsensitive(id, "AtronachFlame"))
		return 7;
	if (ContainsInsensitive(id, "AtronachFrost"))
		return 8;
	if (ContainsInsensitive(id, "AtronachStorm"))
		return 9;
	if (ContainsInsensitive(id, "Ghost") || ContainsInsensitive(id, "Wisp") || a_actor->IsGhost())
		return 10;
	// nicktheduke: conjured spirits and familiars have no blood either. Same ectoplasm as ghosts, so they read consistently.
	// Atronachs keep their own (checked above); reanimated corpses are commanded, not summoned, and still bleed.
	for (const char* spirit : { "Familiar", "Spectral", "Spirit", "Wraith", "Phantom", "Shade", "Spectre", "Conjured" })
		if (ContainsInsensitive(id, spirit))
			return 10;
	if (a_actor->IsSummoned() && !race->HasKeywordString("ActorTypeDaedra") && !race->HasKeywordString("ActorTypeNPC"))
		return 10;
	if (race->HasKeywordString("ActorTypeDwarven"))
		return 3;
	if (const auto* material = race->bloodImpactMaterial) {
		const auto materialID = material->GetFormID();
		if (materialID == 0x0009644C || materialID == 0x0009150B)
			return 3;  // dwemer oil
		if (materialID == 0x0010D5CC)
			return 2;  // insect ichor
	}
	if (ContainsInsensitive(id, "Skeleton"))
		return 5;
	if (ContainsInsensitive(id, "Draugr") || ContainsInsensitive(id, "Lich") || ContainsInsensitive(id, "Mummy"))
		return 4;
	if (ContainsInsensitive(id, "Spriggan"))
		return 6;
	for (const char* insect : { "Chaurus", "Spider", "Hopper", "Kwama", "Scrib", "Netch", "Lurker" })
		if (ContainsInsensitive(id, insect))
			return 2;
	if (race->HasKeywordString("Vampire") || ContainsInsensitive(id, "Vampire") || a_actor->HasKeywordString("Vampire"))
		return 1;
	return 0;
}

void ScreenAction::Queue(const Pending& a_pending)
{
	std::scoped_lock lock(pendingMutex);
	if (pending.size() < 32)
		pending.push_back(a_pending);
}

void ScreenAction::OnPrecisionHit(RE::Actor* a_attacker, RE::TESObjectREFR* a_target, const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity)
{
	if (!settings.Enabled)
		return;
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;
	if (a_target == player && a_attacker && a_attacker != player) {
		// nicktheduke: a blade landing on you: remember where and which way it moved, for the hit shake that follows.
		std::scoped_lock lock(swingMutex);
		incomingSwing.time = Now();
		incomingSwing.position = a_position;
		incomingSwing.velocity = a_velocity;
		return;
	}
	if (a_attacker != player || !a_target || a_target == player || !(settings.ImpactFlashEnabled || settings.SlashEnabled))
		return;
	Pending hit;
	hit.isFlash = true;
	hit.hasWorld = true;
	hit.world = a_position;
	hit.hasSwing = std::isfinite(a_velocity.x) && a_velocity.Length() > 1.0f;
	hit.swing = a_velocity;
	hit.strength = 1.0f;
	Queue(hit);
}

bool ScreenAction::DismemberActive() const
{
	return settings.Enabled && settings.IntegrationDismember != 2 && (dfLoaded || ngdLoaded) && settings.DismemberStrength > 0.0f;
}

void ScreenAction::OnMagicEffectApply(const RE::TESMagicEffectApplyEvent* a_event)
{
	if (!dfSprayEffect || !DismemberActive())
		return;
	if (a_event->magicEffect == dfSprayEffect->GetFormID() || (dfSprayTraceEffect && a_event->magicEffect == dfSprayTraceEffect->GetFormID()))
		OnGore(a_event->caster ? a_event->caster.get() : a_event->target.get(), false, 1);
}

void ScreenAction::OnEquip(const RE::TESEquipEvent* a_event)
{
	if (!ngdNeck || !a_event->equipped || !DismemberActive())
		return;
	if (a_event->baseObject == ngdNeck->GetFormID() || (ngdNeckAfflicted && a_event->baseObject == ngdNeckAfflicted->GetFormID()))
		OnGore(a_event->actor.get(), true, 3);
}

void ScreenAction::OnObjectLoaded(const RE::TESObjectLoadedEvent* a_event)
{
	if (!dfLimb || !a_event->loaded || !DismemberActive())
		return;
	auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->formID);
	auto* base = ref ? ref->GetBaseObject() : nullptr;
	if (base && base == dfLimb)
		OnGore(ref, false, 2);
}

void ScreenAction::OnGore(RE::TESObjectREFR* a_source, bool a_decapitation, uint32_t a_signal)
{
	// nicktheduke: a limb or head just came off. Close in front of you: arterial spray on the lens, coloured by creature.
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!a_source || !player || a_source == player || !BloodAllowed())
		return;
	if (a_signal < 4 && goreSignals[a_signal]++ < 5)
		logger::info("[Combat] gore signal {} ({}) from {:08X}", a_signal, a_signal == 0 ? "spray spell" : a_signal == 1 ? "spray effect" : a_signal == 2 ? "limb" : "decapitation", a_source->GetFormID());
	auto* victim = a_source->As<RE::Actor>();
	const RE::NiPoint3 at = a_source->GetPosition();
	// One spray per dismemberment, however many of its signals arrive (spray spell, effect, limb, NGD armour).
	const double now = Now();
	const RE::FormID key = victim ? victim->GetFormID() : 0;
	for (auto it = goreSeen.begin(); it != goreSeen.end();)
		it = now - it->second > 2.0 ? goreSeen.erase(it) : std::next(it);
	if (key) {
		if (auto it = goreSeen.find(key); it != goreSeen.end() && now - it->second < 0.6 && !a_decapitation)
			return;
		goreSeen[key] = now;
	} else {
		static double lastAnonymous = -100.0;
		if (now - lastAnonymous < 0.6)
			return;
		lastAnonymous = now;
	}
	const RE::NiPoint3 me = player->GetPosition();
	const float distance = me.GetDistance(at);
	const float reach = a_decapitation ? 450.0f : 380.0f;
	if (distance > reach)
		return;
	// In front of you (within about 100 degrees of where you face); the side it lands on comes from projection.
	float diff = (std::atan2(at.x - me.x, at.y - me.y) - player->data.angle.z) * 57.29578f;
	while (diff > 180.0f)
		diff -= 360.0f;
	while (diff < -180.0f)
		diff += 360.0f;
	if (std::abs(diff) > 100.0f)
		return;
	const float close = 1.0f - Smooth(120.0f, reach, distance);
	Pending spray;
	spray.type = SplatType::kBlood;
	spray.variant = victim ? ClassifyBlood(victim) : 0;
	spray.count = a_decapitation ? 4 : 3;
	spray.strength = std::clamp((0.85f + 0.45f * close) * (a_decapitation ? 1.25f : 1.0f) * std::clamp(settings.DismemberStrength, 0.0f, 2.0f), 0.0f, 2.0f);
	spray.hasWorld = true;
	spray.world = at;
	spray.world.z += victim ? victim->GetHeight() * (a_decapitation ? 0.85f : 0.6f) : 30.0f;
	spray.life = (ReplacingOtherBlood() && otherBloodDuration > 0.5f) ? otherBloodDuration : 10.0f;
	if (spray.strength > 0.05f)
		Queue(spray);
	auto* camera = RE::PlayerCamera::GetSingleton();
	if (settings.DismemberKick && distance < 300.0f && ShakeAllowed(camera && camera->IsInFirstPerson())) {
		Pending kick;
		kick.isShake = true;
		kick.variant = static_cast<uint32_t>(ShakeType::kNormal);
		kick.strength = (a_decapitation ? 0.7f : 0.5f) * std::max(close, 0.3f);
		kick.hasWorld = true;
		kick.world = spray.world;
		Queue(kick);
	}
}

void ScreenAction::OnSpellCast(const RE::TESSpellCastEvent* a_event)
{
	if (dfSpray && (a_event->spell == dfSpray->GetFormID() || (dfSprayTrace && a_event->spell == dfSprayTrace->GetFormID()))) {
		if (DismemberActive())
			OnGore(a_event->object.get(), false, 0);
		return;
	}
	if (!settings.Enabled || !settings.ShoutRippleEnabled)
		return;
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player || a_event->object.get() != player)
		return;
	auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_event->spell);
	if (!spell || spell->GetSpellType() != RE::MagicSystem::SpellType::kVoicePower)
		return;
	Pending shout;
	shout.isShout = true;
	shout.strength = 1.0f;
	Queue(shout);
}

void ScreenAction::OnHit(const RE::TESHitEvent* a_event)
{
	if (!settings.Enabled)
		return;
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* target = a_event->target.get();
	auto* cause = a_event->cause.get();
	if (!player || !target || !cause)
		return;
	auto ready = [this](uint32_t a_key, float a_seconds) {
		auto& left = cooldowns[a_key];
		if (left > 0.0f)
			return false;
		left = a_seconds;
		return true;
	};
	const bool blocked = a_event->flags.any(RE::TESHitEvent::Flag::kHitBlocked);
	const bool power = a_event->flags.any(RE::TESHitEvent::Flag::kPowerAttack);
	auto* source = a_event->source ? RE::TESForm::LookupByID(a_event->source) : nullptr;

	// ---- The player hits something ----
	if (cause == player && target != player) {
		auto* victim = target->As<RE::Actor>();
		if (!victim || blocked)
			return;
		auto* weapon = source ? source->As<RE::TESObjectWEAP>() : nullptr;
		const bool ranged = a_event->projectile != 0;
		const float distance = player->GetPosition().GetDistance(target->GetPosition());
		RE::NiPoint3 torso = target->GetPosition();
		torso.z += victim->GetHeight() * 0.6f;
		if (BloodAllowed() && screenBloodEnhanced && settings.BloodWithOtherScreenBlood == 1 && sbe.count) {
			// Screen Blood Enhanced decides when and how much; we draw it in the creature's colour.
			const int amount = SBESplatterCount(victim, weapon, power);
			if (amount > 0 && ready(kCoolBlood, 0.1f)) {
				Pending blood;
				blood.type = SplatType::kBlood;
				blood.variant = ClassifyBlood(victim);
				blood.count = static_cast<uint32_t>(std::clamp(1 + amount / 6, 1, 4));
				blood.strength = std::clamp(0.5f + static_cast<float>(amount) / 20.0f, 0.5f, 1.3f);
				blood.hasWorld = true;
				blood.world = torso;
				blood.life = otherBloodDuration;
				Queue(blood);
			}
		} else if (BloodAllowed() && distance < (ranged ? 280.0f : 420.0f) && ready(kCoolBlood, 0.12f)) {
			Pending blood;
			blood.type = SplatType::kBlood;
			blood.variant = ClassifyBlood(victim);
			blood.count = ranged ? 1 : (power ? 3 : 1 + (distance < 200.0f ? 1 : 0));
			blood.strength = (ranged ? 0.6f : (power ? 1.2f : 0.9f)) * (1.0f - 0.5f * Smooth(150.0f, 420.0f, distance));
			blood.hasWorld = true;
			blood.world = torso;
			Queue(blood);
		}
		// Without Precision: a flash on the target for weapon hits (no swing direction available).
		if (!precisionConnected && settings.ImpactFlashEnabled && weapon && !ranged) {
			Pending flash;
			flash.isFlash = true;
			flash.hasWorld = true;
			flash.world = torso;
			flash.strength = power ? 1.0f : 0.75f;
			Queue(flash);
		}
		return;
	}

	// ---- Something hits the player ----
	if (target != player || cause == player)
		return;
	auto* attacker = cause->As<RE::Actor>();
	const RE::NiPoint3 from = cause->GetPosition();

	// ---- nicktheduke: hit shake ----
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		const bool firstPerson = camera && camera->IsInFirstPerson();
		auto* hitSpell = source ? source->As<RE::MagicItem>() : nullptr;
		auto* hitWeapon = source ? source->As<RE::TESObjectWEAP>() : nullptr;
		const bool bash = a_event->flags.any(RE::TESHitEvent::Flag::kBashAttack);
		const bool arrow = a_event->projectile != 0 && (!hitWeapon || hitWeapon->GetWeaponType() == RE::WEAPON_TYPE::kBow || hitWeapon->GetWeaponType() == RE::WEAPON_TYPE::kCrossbow) && !hitSpell;
		if (!hitSpell && ShakeAllowed(firstPerson)) {
			const auto raceID = attacker ? RaceEditorID(attacker) : std::string_view();
			const bool big = attacker && (RaceHas(attacker, "ActorTypeDragon") || ContainsInsensitive(raceID, "Giant") || ContainsInsensitive(raceID, "Mammoth") ||
			                              ContainsInsensitive(raceID, "Centurion") || ContainsInsensitive(raceID, "Troll") || ContainsInsensitive(raceID, "Lurker") ||
			                              ContainsInsensitive(raceID, "Gargoyle") || ContainsInsensitive(raceID, "Werewolf") || ContainsInsensitive(raceID, "Werebear"));
			// How hard it landed: the share of your health it took (0.75x for a scratch, up to 1.5x).
			float damageScale = 1.0f;
			if (auto* values = player->AsActorValueOwner()) {
				const float maximum = values->GetPermanentActorValue(RE::ActorValue::kHealth);
				const float current = values->GetActorValue(RE::ActorValue::kHealth);
				if (std::isfinite(maximum) && maximum > 1.0f && std::isfinite(current)) {
					if (lastHealth >= 0.0f)
						damageScale = std::clamp(0.75f + 2.5f * std::max(0.0f, lastHealth - current) / maximum, 0.75f, 1.5f);
					lastHealth = current;
				}
			}
			const double now = Now();
			// Valhalla Combat shakes the screen on its own timed blocks: leave those to it.
			bool timedBlock = false;
			if (blocked && valhallaLoaded && settings.IntegrationValhalla != 2 && (settings.IntegrationValhalla == 1 || valhallaTimedBlockShake))
				timedBlock = now - blockRaisedAt <= static_cast<double>(std::max(valhallaTimedBlockWindow, 0.05f));
			if (!timedBlock) {
				Pending kick;
				kick.isShake = true;
				ShakeType type = ShakeType::kNormal;
				float strength = damageScale;
				if (blocked) {
					type = ShakeType::kBlocked;
					strength = (power ? 1.6f : 1.0f) * (big ? 2.0f : 1.0f);
				} else if (arrow) {
					type = ShakeType::kArrow;
				} else if (bash) {
					type = ShakeType::kBash;
				} else if (big) {
					type = ShakeType::kBig;
					strength *= power ? 1.2f : 0.85f;
				} else if (power) {
					type = ShakeType::kPower;
				}
				kick.variant = static_cast<uint32_t>(type);
				kick.strength = strength;
				kick.hasWorld = true;
				kick.world = from;
				if (attacker)
					kick.world.z += attacker->GetHeight() * 0.6f;
				// Precision: the blade's own movement as it landed, and its hitstop on you before the jolt.
				if (precisionConnected && settings.IntegrationPrecision != 2 && !arrow) {
					std::scoped_lock lock(swingMutex);
					if (now - incomingSwing.time < 0.25 && std::isfinite(incomingSwing.velocity.x) && incomingSwing.velocity.Length() > 1.0f) {
						kick.hasSwing = true;
						kick.world = incomingSwing.position;
						kick.swing = incomingSwing.velocity;
					}
					if (precisionHitstopOnTarget && !blocked)
						kick.delay = std::min(precisionHitstop * (power ? precisionHitstopPowerMult : 1.0f), 0.15f);
				}
				Queue(kick);
			}
		}
	}

	// Replacing the other mod's look: they also show blood when you take a hit, so we do it for them.
	if (ReplacingOtherBlood() && otherReceiveBlood && !blocked && source && source->As<RE::TESObjectWEAP>() && ready(kCoolFlash, 0.35f)) {
		Pending own;
		own.type = SplatType::kBlood;
		own.variant = ClassifyBlood(player);
		own.count = power ? 2 : 1;
		own.strength = power ? 1.0f : 0.75f;
		own.life = otherBloodDuration;
		Queue(own);
	}
	auto* spell = source ? source->As<RE::MagicItem>() : nullptr;
	RE::BGSProjectile* projectileBase = nullptr;
	if (a_event->projectile)
		projectileBase = RE::TESForm::LookupByID<RE::BGSProjectile>(a_event->projectile);
	std::string_view projectileModel;
	if (projectileBase) {
		if (const char* model = projectileBase->GetModel())
			projectileModel = model;
	}
	const bool dragon = attacker && RaceHas(attacker, "ActorTypeDragon");
	const bool dwarven = attacker && RaceHas(attacker, "ActorTypeDwarven");

	if (spell) {
		if (spell->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower) {
			if (settings.ShoutRippleEnabled && ready(kCoolShout, 1.0f)) {
				Pending shout;
				shout.isShout = true;
				shout.strength = 0.8f;
				shout.hasWorld = true;
				shout.world = from;
				Queue(shout);
			}
			return;
		}
		const std::string_view spellName = spell->GetName() ? spell->GetName() : "";
		bool fire = false, frost = false, shocking = false, poisonous = spell->IsPoison(), web = ContainsInsensitive(spellName, "web") || ContainsInsensitive(projectileModel, "web");
		for (auto* effect : spell->effects) {
			auto* base = effect ? effect->baseEffect : nullptr;
			if (!base)
				continue;
			switch (base->data.resistVariable) {
			case RE::ActorValue::kResistFire:
				fire = true;
				break;
			case RE::ActorValue::kResistFrost:
				frost = true;
				break;
			case RE::ActorValue::kResistShock:
				shocking = true;
				break;
			case RE::ActorValue::kPoisonResist:
				poisonous = true;
				break;
			default:
				break;
			}
			if (const char* effectName = base->GetName(); effectName && ContainsInsensitive(effectName, "web"))
				web = true;
		}
		// Dragon breath is handled by the active effect scan (it lasts as long as you stand in it).
		if (dragon && (fire || frost))
			return;
		const bool spit = projectileBase && (ContainsInsensitive(projectileModel, "spit") || ContainsInsensitive(spellName, "spit"));
		Pending hit;
		hit.hasWorld = true;
		hit.world = from;
		if (web) {
			if (settings.WebEnabled && ready(kCoolWeb, 1.2f)) {
				hit.type = SplatType::kWeb;
				hit.strength = 1.0f;
				Queue(hit);
			}
		} else if (poisonous && projectileBase) {
			if (settings.VenomEnabled && ready(kCoolVenom, 0.6f)) {
				hit.type = SplatType::kVenom;
				hit.strength = spit ? 1.0f : 0.8f;
				Queue(hit);
			}
		} else if (fire && dwarven) {
			if (settings.SteamEnabled && ready(kCoolSteam, 0.5f)) {
				hit.type = SplatType::kSteam;
				hit.strength = 1.0f;
				Queue(hit);
			}
		} else if (frost) {
			if (settings.FrostHitsEnabled && ready(kCoolIce, 0.6f)) {
				hit.type = SplatType::kIce;
				hit.strength = 1.0f;
				Queue(hit);
			}
		} else if (fire) {
			if (settings.FireHitsEnabled && ready(kCoolFire, 0.5f)) {
				hit.type = SplatType::kFire;
				hit.strength = 1.0f;
				Queue(hit);
			}
		} else if (shocking) {
			if (settings.ShockEnabled && ready(kCoolShock, 0.4f)) {
				Pending pulse;
				pulse.type = SplatType::kFire;  // marker, handled as a shock pulse below
				pulse.variant = 99;
				Queue(pulse);
			}
		}
		return;
	}

	// Melee or creature attack: big creatures kick up dust on a heavy hit.
	if (attacker && settings.StompDustEnabled && (power || blocked)) {
		const auto id = RaceEditorID(attacker);
		const bool big = dragon || ContainsInsensitive(id, "Giant") || ContainsInsensitive(id, "Mammoth") || ContainsInsensitive(id, "Centurion") ||
		                 ContainsInsensitive(id, "Troll") || ContainsInsensitive(id, "Lurker") || ContainsInsensitive(id, "Gargoyle");
		if (big && ready(kCoolStomp, 1.0f)) {
			Pending dust;
			dust.type = SplatType::kGrit;
			dust.variant = 98;  // marker: stomp dust layer
			dust.strength = power ? 1.0f : 0.6f;
			Queue(dust);
		}
	}
}

void ScreenAction::QueryGame()
{
	UpdateOtherSplatter();
	GameSample result;
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (player && player->Is3DLoaded()) {
		if (auto* camera = RE::PlayerCamera::GetSingleton())
			result.firstPerson = camera->IsInFirstPerson();
		if (auto* water = RE::TESWaterSystem::GetSingleton())
			result.underwater = water->playerUnderwater;
		result.exertion = LensShared::ComputeExertion(player, bladeAndBlunt);
		if (auto* values = player->AsActorValueOwner()) {
			const float maximum = values->GetPermanentActorValue(RE::ActorValue::kStamina);
			const float current = values->GetActorValue(RE::ActorValue::kStamina);
			if (std::isfinite(maximum) && maximum > 1.0f && std::isfinite(current))
				result.staminaPct = Clamp01(current / maximum);
			// nicktheduke: health before the next hit (hit shake strength), block raise time (Valhalla timed blocks)
			const float health = values->GetActorValue(RE::ActorValue::kHealth);
			if (std::isfinite(health))
				lastHealth = health;
		}
		{
			const bool blocking = player->IsBlocking();
			if (blocking && !wasBlocking)
				blockRaisedAt = Now() - kQueryInterval * 0.5;
			wasBlocking = blocking;
			// Knockdown: the moment you go into ragdoll (third person only; first person has its own get-up view).
			const bool ragdoll = player->IsInRagdollState();
			if (ragdoll && !wasRagdoll && !result.firstPerson && ShakeAllowed(false)) {
				Pending kick;
				kick.isShake = true;
				kick.variant = static_cast<uint32_t>(ShakeType::kKnockdown);
				kick.strength = 1.0f;
				Queue(kick);
			}
			wasRagdoll = ragdoll;
		}

		// Lasting effects on the player: dragon breath, poison, shock, burning.
		const RE::NiPoint3 eye = player->GetPosition();
		float bestRime = 0.0f;
		if (auto* magicTarget = player->AsMagicTarget()) {
			if (auto* list = magicTarget->GetActiveEffectList()) {
				for (auto* active : *list) {
					if (!active || active->flags.any(RE::ActiveEffect::Flag::kInactive) || !active->effect || !active->effect->baseEffect)
						continue;
					auto* base = active->effect->baseEffect;
					if (!base->IsDetrimental() && !base->IsHostile())
						continue;
					const auto caster = active->GetCasterActor();
					const bool dragon = caster && RaceHas(caster.get(), "ActorTypeDragon");
					const bool dwarven = caster && RaceHas(caster.get(), "ActorTypeDwarven");
					switch (base->data.resistVariable) {
					case RE::ActorValue::kResistFire:
						if (dragon)
							result.dragonfire = 1.0f;
						else if (dwarven)
							result.steam = std::max(result.steam, 0.8f);
						else
							result.dragonfire = std::max(result.dragonfire, 0.3f);  // burning: a lighter version
						break;
					case RE::ActorValue::kResistFrost:
						{
							const float amount = dragon ? 1.0f : 0.35f;
							if (amount > bestRime && caster) {
								bestRime = amount;
								result.rimeSource = caster->GetPosition();
								result.rimeHasSource = true;
							}
							break;
						}
					case RE::ActorValue::kResistShock:
						result.shock = std::max(result.shock, 0.6f);
						break;
					case RE::ActorValue::kPoisonResist:
						result.poison = 1.0f;
						break;
					default:
						if (active->spell && active->spell->IsPoison())
							result.poison = 1.0f;
						break;
					}
				}
			}
		}

		// Nearby: frost atronachs and ice wraiths, fresh explosions, ash piles from disintegrated enemies.
		if (auto* tes = RE::TES::GetSingleton()) {
			tes->ForEachReferenceInRange(player, 900.0f, [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref == player || a_ref->IsDisabled() || a_ref->IsDeleted())
					return RE::BSContainer::ForEachResult::kContinue;
				const float distance = eye.GetDistance(a_ref->GetPosition());
				if (auto* actor = a_ref->As<RE::Actor>()) {
					if (!actor->IsDead() && distance < 450.0f && settings.RimeEnabled) {
						const auto id = RaceEditorID(actor);
						if (ContainsInsensitive(id, "AtronachFrost") || ContainsInsensitive(id, "IceWraith")) {
							const float amount = 0.3f * (1.0f - Smooth(200.0f, 450.0f, distance));
							if (amount > bestRime) {
								bestRime = amount;
								result.rimeSource = a_ref->GetPosition();
								result.rimeHasSource = true;
							}
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* base = a_ref->GetBaseObject();
				if (!base)
					return RE::BSContainer::ForEachResult::kContinue;
				const auto refID = a_ref->GetFormID();
				if (seenRefs.contains(refID))
					return RE::BSContainer::ForEachResult::kContinue;
				if (base->GetFormType() == RE::FormType::Explosion) {
					seenRefs[refID] = 10.0f;
					// nicktheduke 2026-11-02: only real blasts throw grit. Blood spray, hit sparks and other impact-only effects
					// are explosions too (no force, no damage, tiny radius) and used to put dark specks in every melee
					// splatter. A blast right on top of you is handled by the blast's own effects, not lens grit.
					bool realBlast = false;
					if (auto* explosion = base->As<RE::BGSExplosion>()) {
						const auto& ed = explosion->data;
						const char* ename = base->GetName();
						const bool cosmetic = ename && (ContainsInsensitive(ename, "blood") || ContainsInsensitive(ename, "spray") ||
							ContainsInsensitive(ename, "impact") || ContainsInsensitive(ename, "spark") || ContainsInsensitive(ename, "hit"));
						realBlast = !cosmetic && ed.radius >= 64.0f && (ed.force > 0.0f || ed.damage > 0.0f);
					}
					if (settings.GritEnabled && realBlast && distance > 180.0f && distance < 900.0f) {
						Pending grit;
						grit.type = SplatType::kGrit;
						grit.strength = 1.0f - Smooth(250.0f, 900.0f, distance);
						grit.hasWorld = true;
						grit.world = a_ref->GetPosition();
						grit.count = distance < 400.0f ? 2 : 1;
						Queue(grit);
						static int gritLogged = 0;
						if (gritLogged < 3) {
							++gritLogged;
							logger::info("[Combat] explosion grit from {:08X} at {:.0f} units", base->GetFormID(), distance);
						}
						const char* name = base->GetName();
						if (settings.FireHitsEnabled && name && (ContainsInsensitive(name, "fire") || ContainsInsensitive(name, "flame"))) {
							Pending embers = grit;
							embers.type = SplatType::kFire;
							embers.count = 1;
							Queue(embers);
						}
					}
				} else if (base->GetFormType() == RE::FormType::Activator && settings.AshPuffEnabled && distance < 600.0f) {
					auto* model = base->As<RE::TESModel>();
					const char* path = model ? model->GetModel() : nullptr;
					if (path && ContainsInsensitive(path, "ashpile")) {
						seenRefs[refID] = 30.0f;
						Pending ash;
						ash.type = SplatType::kAsh;
						ash.strength = 1.0f - Smooth(200.0f, 600.0f, distance);
						ash.hasWorld = true;
						ash.world = a_ref->GetPosition();
						Queue(ash);
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		result.rime = bestRime;
	}
	{
		std::scoped_lock lock(sampleMutex);
		sample = result;
	}
	for (auto it = seenRefs.begin(); it != seenRefs.end();) {
		it->second -= kQueryInterval;
		it = it->second <= 0.0f ? seenRefs.erase(it) : std::next(it);
	}
	for (auto& [key, left] : cooldowns)
		left = std::max(0.0f, left - kQueryInterval);
	queryPending.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------------------------------
// Render side

float ScreenAction::Random()
{
	randomState ^= randomState << 13;
	randomState ^= randomState >> 17;
	randomState ^= randomState << 5;
	return static_cast<float>(randomState & 0x00FFFFFFu) / 16777216.0f;
}

bool ScreenAction::Project(const RE::NiPoint3& a_world, float& o_u, float& o_v) const
{
	// Main camera projection (behind the camera: mirrored back and pushed off screen, keeping its side).
	return Camera::WorldToScreen(a_world, o_u, o_v);
}

void ScreenAction::AddSplat(const Splat& a_splat)
{
	if (splats.size() >= kMaxSplats) {
		// Make room: the oldest (relative to its life) goes first.
		auto oldest = std::max_element(splats.begin(), splats.end(), [](const Splat& a, const Splat& b) { return a.age / a.life < b.age / b.life; });
		*oldest = a_splat;
		return;
	}
	splats.push_back(a_splat);
}

void ScreenAction::SpawnFromPending(const Pending& a_pending)
{
	const float master = std::clamp(settings.MasterStrength, 0.0f, 2.0f);
	const float aspect = inputDescription.Height > 0 ? static_cast<float>(inputDescription.Width) / static_cast<float>(inputDescription.Height) : 16.0f / 9.0f;

	if (a_pending.isWipe) {
		wipeIn = 0.6f;
		return;
	}

	if (a_pending.isShake) {
		// nicktheduke: which way the camera is knocked, on screen. Precision's blade: along its swing. Otherwise away from
		// whoever hit you. Stagger: from the game's stagger direction.
		const auto type = static_cast<ShakeType>(std::min<uint32_t>(a_pending.variant, static_cast<uint32_t>(ShakeType::kKnockdown)));
		float dx = 0.0f, dy = 0.0f;
		if (type == ShakeType::kStagger || type == ShakeType::kKnockdown) {
			dx = a_pending.hasSwing ? a_pending.swing.x : (Random() - 0.5f);
			dy = 0.35f;
		} else if (a_pending.hasSwing && a_pending.hasWorld) {
			RE::NiPoint3 ahead = a_pending.world + a_pending.swing * (8.0f / std::max(a_pending.swing.Length(), 1.0f));
			float u, v, u2, v2;
			Project(a_pending.world, u, v);
			Project(ahead, u2, v2);
			dx = (u2 - u) * aspect;
			dy = v2 - v;
		}
		if (!(std::isfinite(dx) && std::isfinite(dy)) || dx * dx + dy * dy < 1e-8f) {
			dx = Random() - 0.5f;
			dy = 0.3f;
			if (a_pending.hasWorld) {
				float u, v;
				Project(a_pending.world, u, v);  // off screen keeps its side
				if (std::isfinite(u) && std::isfinite(v)) {
					const float ax = (u - 0.5f) * aspect, ay = v - 0.5f;
					if (std::abs(ax) > 0.08f)
						dx = -ax;  // knocked away from them
					dy = 0.25f - 0.3f * std::clamp(ay, -1.0f, 1.0f);
				}
			}
		}
		AddKick(type, a_pending.strength, dx, dy, a_pending.delay);
		return;
	}

	if (a_pending.isShout) {
		shoutAge = 0.0f;
		shoutStrength = Clamp01(a_pending.strength * std::clamp(settings.ShoutStrength, 0.0f, 2.0f) * master);
		shoutU = 0.5f;
		shoutV = 0.5f;
		return;
	}

	if (a_pending.isFlash) {
		// Photosensitive safe: at most about three flashes a second, and gentler.
		const float minimumGap = settings.PhotosensitiveSafe ? 0.34f : 0.1f;
		if (sinceFlash < minimumGap)
			return;
		float u = 0.5f, v = 0.5f;
		if (a_pending.hasWorld && !Project(a_pending.world, u, v))
			return;
		if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
			return;
		sinceFlash = 0.0f;
		flashAge = 0.0f;
		flashU = u;
		flashV = v;
		const float peak = settings.PhotosensitiveSafe ? 0.6f : 1.0f;
		flashStrength = settings.ImpactFlashEnabled ? Clamp01(a_pending.strength * std::clamp(settings.FlashStrength, 0.0f, 2.0f) * master) * peak : 0.0f;
		slashStrength = 0.0f;
		if (settings.SlashEnabled && a_pending.hasSwing) {
			// Swing direction on screen: project a point a little along the swing velocity.
			RE::NiPoint3 ahead = a_pending.world + a_pending.swing * (8.0f / std::max(a_pending.swing.Length(), 1.0f));
			float u2, v2;
			if (Project(ahead, u2, v2)) {
				float dx = (u2 - u) * aspect, dy = v2 - v;
				const float length = std::sqrt(dx * dx + dy * dy);
				if (length > 1e-5f) {
					slashDirX = dx / length;
					slashDirY = dy / length;
					slashStrength = Clamp01(std::clamp(settings.SlashStrength, 0.0f, 2.0f) * master) * (settings.PhotosensitiveSafe ? 0.7f : 1.0f);
				}
			}
		}
		if (!settings.ImpactFlashEnabled && slashStrength > 0.0f)
			flashStrength = 0.0001f;  // keep the slash's anchor alive without a flash
		return;
	}

	if (a_pending.type == SplatType::kFire && a_pending.variant == 99) {
		shockPulse = 1.0f;
		// nicktheduke: each hit starts exactly one discharge in the shader, at full strength from the first frame
		shockAge = 0.0f;
		shockHits = std::fmod(shockHits + 1.0f, 4096.0f);
		if (settings.ShockEnabled)
			shock = std::max(shock, Clamp01(std::clamp(settings.ShockStrength, 0.0f, 2.0f) * std::clamp(settings.MasterStrength, 0.0f, 2.0f)));
		return;
	}
	if (a_pending.type == SplatType::kGrit && a_pending.variant == 98) {
		stomp = std::max(stomp, Clamp01(a_pending.strength * std::clamp(settings.StompStrength, 0.0f, 2.0f) * master));
		return;
	}

	// Where on screen: toward the side the thing came from, outside the clear centre.
	float dirX = Random() * 2.0f - 1.0f, dirY = Random() * 2.0f - 1.0f;
	if (a_pending.hasWorld) {
		float u, v;
		Project(a_pending.world, u, v);
		if (std::isfinite(u) && std::isfinite(v)) {
			dirX = (u - 0.5f) * aspect;
			dirY = v - 0.5f;
		}
	}
	const float length = std::sqrt(dirX * dirX + dirY * dirY);
	if (length < 1e-4f) {
		dirX = Random() * 2.0f - 1.0f;
		dirY = Random() * 2.0f - 1.0f;
	}
	const float norm = 1.0f / std::max(std::sqrt(dirX * dirX + dirY * dirY), 1e-4f);
	dirX *= norm;
	dirY *= norm;

	float strengthSetting = 1.0f, life = 5.0f, size = 0.12f;
	switch (a_pending.type) {
	case SplatType::kBlood:
		strengthSetting = settings.BloodStrength;
		life = a_pending.variant == 4 || a_pending.variant == 5 ? 4.0f : (a_pending.variant == 7 || a_pending.variant == 9 ? 1.5f : 8.0f);
		size = 0.07f + 0.06f * a_pending.strength;
		break;
	case SplatType::kVenom:
		strengthSetting = settings.VenomStrength;
		life = 8.0f;
		size = 0.14f;
		break;
	case SplatType::kWeb:
		strengthSetting = settings.WebStrength;
		life = 9.0f;
		size = 0.26f;
		break;
	case SplatType::kIce:
		strengthSetting = settings.FrostHitStrength;
		life = 3.5f;  // nicktheduke 2026-11g: long enough for the frost to grow along the cracks before it thaws
		size = 0.18f;
		break;
	case SplatType::kFire:
		strengthSetting = settings.FireHitStrength;
		life = 1.3f;
		size = 0.2f;
		break;
	case SplatType::kGrit:
		strengthSetting = settings.GritStrength;
		life = 3.0f;
		size = 0.16f;
		break;
	case SplatType::kAsh:
		strengthSetting = settings.AshPuffStrength;
		life = 3.5f;
		size = 0.22f;
		break;
	case SplatType::kSteam:
		strengthSetting = settings.SteamStrength;
		life = 4.0f;
		size = 0.3f;
		break;
	}
	const uint32_t count = std::clamp<uint32_t>(a_pending.count, 1, 4);
	for (uint32_t i = 0; i < count; ++i) {
		// Spread around the incoming side; radius keeps the splat outside the safe zone (in screen heights).
		const float spread = (Random() - 0.5f) * (count > 1 ? 1.4f : 0.7f);
		const float cs = std::cos(spread), sn = std::sin(spread);
		const float rx = dirX * cs - dirY * sn, ry = dirX * sn + dirY * cs;
		const float radius = LensShared::kSafeZone + 0.1f + size * 0.5f + Random() * 0.18f;
		Splat splat;
		splat.type = a_pending.type;
		splat.variant = a_pending.variant;
		splat.u = 0.5f + rx * radius / aspect;
		splat.v = 0.5f + ry * radius;
		splat.size = size * (0.75f + 0.5f * Random()) * (i == 0 ? 1.0f : 0.7f);
		splat.life = (a_pending.life > 0.5f ? std::clamp(a_pending.life, 4.0f, 20.0f) : life) * (0.85f + 0.3f * Random());
		splat.seed = Random() * 100.0f;
		splat.angle = (Random() - 0.5f) * 0.8f;
		splat.alpha = Clamp01(a_pending.strength * std::clamp(strengthSetting, 0.0f, 2.0f) * master);
		if (splat.alpha > 0.01f)
			AddSplat(splat);
	}
}

void ScreenAction::AddKick(ShakeType a_type, float a_strength, float a_dirX, float a_dirY, float a_delay)
{
	const float length = std::sqrt(a_dirX * a_dirX + a_dirY * a_dirY);
	if (!std::isfinite(length) || length < 1e-6f)
		return;
	float strength = std::clamp(a_strength, 0.0f, 2.0f);
	switch (a_type) {
	case ShakeType::kNormal: strength *= settings.ShakeNormal; break;
	case ShakeType::kPower: strength *= settings.ShakePower; break;
	case ShakeType::kBash: strength *= settings.ShakeBash; break;
	case ShakeType::kArrow: strength *= settings.ShakeArrow; break;
	case ShakeType::kBlocked: strength *= settings.ShakeBlocked; break;
	case ShakeType::kBig: strength *= settings.ShakeBig; break;
	default:
		strength *= settings.ShakeStagger;
		// 3rd Person Camera Stagger Remover: its users asked for a calmer stagger camera
		if (settings.IntegrationStaggerRemover != 2 && (staggerRemover || settings.IntegrationStaggerRemover == 1))
			strength *= 0.5f;
		break;
	}
	if (strength <= 0.001f)
		return;
	// A new hit of the same kind while one is still going refreshes it instead of stacking (no seizure flurries).
	const auto& profile = kShakeProfiles[static_cast<size_t>(a_type)];
	for (auto& kick : kicks) {
		if (kick.type == a_type && kick.age < profile.life * 0.6f) {
			kick.strength = std::min(std::max(kick.strength, strength) + 0.3f * std::min(kick.strength, strength), 2.0f);
			kick.age = std::min(kick.age, 0.0f) - std::max(a_delay, 0.0f);
			kick.dirX = a_dirX / length;
			kick.dirY = a_dirY / length;
			return;
		}
	}
	Kick kick;
	kick.type = a_type;
	kick.age = -std::clamp(a_delay, 0.0f, 0.3f);
	kick.strength = strength;
	kick.dirX = a_dirX / length;
	kick.dirY = a_dirY / length;
	kick.seed = Random() * 100.0f;
	if (kicks.size() >= kMaxKicks) {
		auto oldest = std::max_element(kicks.begin(), kicks.end(), [](const Kick& a, const Kick& b) {
			return a.age / kShakeProfiles[static_cast<size_t>(a.type)].life < b.age / kShakeProfiles[static_cast<size_t>(b.type)].life;
		});
		*oldest = kick;
	} else {
		kicks.push_back(kick);
	}
}

void ScreenAction::UpdateShake(float a_delta)
{
	// nicktheduke: mirrors combine() in the approved preview model. Damped wobble or single bump per kick, organic value noise,
	// then a soft cap on the total so stacked hits never exceed a small, readable jolt, and a zoom that hides the edges.
	float tx = 0.0f, ty = 0.0f, roll = 0.0f, zoomKick = 0.0f;
	for (auto& kick : kicks) {
		kick.age += a_delta;
		const auto& p = kShakeProfiles[static_cast<size_t>(kick.type)];
		const float t = kick.age;
		if (t < 0.0f || t >= p.life)
			continue;
		const float fade = 1.0f - std::max(0.0f, (t - p.life * 0.75f) / (p.life * 0.25f));
		const float px = -kick.dirY, py = kick.dirX;
		const float sign = kick.dirX >= 0.0f ? 1.0f : -1.0f;
		float along, side, rot, zoom = 0.0f;
		if (p.shape == 0) {
			const float env = std::exp(-t / p.tau);
			const float w = 6.2831853f * p.freq;
			const float mainWave = std::sin(w * t) * env;
			const float nf = p.freq * 1.7f;
			const float n1 = ShakeNoise(kick.seed + t * nf, 11) * env;
			const float n2 = ShakeNoise(kick.seed + t * nf, 23) * env;
			const float n3 = ShakeNoise(kick.seed + t * nf * 0.8f, 37) * env;
			along = mainWave + p.noise * 0.5f * n1;
			side = p.noise * 0.7f * n2;
			rot = p.rot * (0.6f * std::sin(w * 0.8f * t + 1.3f) * env + 0.6f * n3) * sign;
		} else {
			const float b = (t / p.t0) * std::exp(1.0f - t / p.t0);
			const float n2 = ShakeNoise(kick.seed + t * 6.0f, 23);
			along = b;
			side = p.noise * 0.5f * n2 * b;
			rot = p.rot * b * sign;
			zoom = p.zoom * b;
		}
		const float scale = kick.strength * fade;
		tx += (kick.dirX * along + px * side) * p.amp * scale;
		ty += (kick.dirY * along + py * side) * p.amp * scale;
		roll += rot * scale;
		zoomKick += zoom * scale;
	}
	std::erase_if(kicks, [](const Kick& a_kick) { return a_kick.age >= kShakeProfiles[static_cast<size_t>(a_kick.type)].life; });

	const float master = std::clamp(settings.HitShakeStrength, 0.0f, 2.0f) * std::clamp(settings.MasterStrength, 0.0f, 2.0f);
	tx *= master;
	ty *= master;
	roll *= master;
	zoomKick *= master;
	const float capOffset = kShakeMaxOffset * std::max(master, 0.01f);
	const float length = std::sqrt(tx * tx + ty * ty);
	if (length > 1e-9f) {
		const float scale = capOffset * std::tanh(length / capOffset) / length;
		tx *= scale;
		ty *= scale;
	}
	const float capRoll = kShakeMaxRoll * std::max(master, 0.01f);
	roll = capRoll * std::tanh(roll / capRoll);
	if (!(std::isfinite(tx) && std::isfinite(ty) && std::isfinite(roll) && std::isfinite(zoomKick))) {
		tx = ty = roll = zoomKick = 0.0f;
		kicks.clear();
	}
	shakeX = tx;
	shakeY = ty;
	shakeRot = roll;
	shakeZoom = 1.0f + 2.2f * std::sqrt(tx * tx + ty * ty) + 2.1f * std::abs(roll) + std::max(zoomKick, 0.0f);
}

void ScreenAction::Reset()
{
	const float frameDelta = RE::GetSecondsSinceLastFrame();
	const float delta = std::isfinite(frameDelta) ? std::clamp(frameDelta, 0.0f, kMaximumFrameStep) : 0.0f;
	auto* state = globals::state;
	const bool blocked = state->IsMainOrLoadingMenuOpen() || state->isMapMenuOpen || state->isStatsMenuOpen;
	auto& shared = LensShared::Get();
	if (!settings.Enabled || blocked) {
		kicks.clear();
		shakeX = shakeY = shakeRot = 0.0f;
		shakeZoom = 1.0f;
		shakeClockStarted = false;
		if (state->isLoadingMenuOpen) {
			splats.clear();
			dragonfire = rime = poison = shock = steam = stomp = staminaShimmer = 0.0f;
		}
		shared.actionCoverage.store(0.0f, std::memory_order_relaxed);
		// Turned off while replacing another mod's screen blood: give it its splatter back.
		if (!settings.Enabled && vanillaSplatterHidden && !queryPending.exchange(true)) {
			if (auto* tasks = SKSE::GetTaskInterface())
				tasks->AddTask([this] { UpdateOtherSplatter(); queryPending.store(false, std::memory_order_release); });
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

	// New events become splats and pulses.
	std::deque<Pending> incoming;
	{
		std::scoped_lock lock(pendingMutex);
		incoming.swap(pending);
	}
	for (const auto& item : incoming)
		SpawnFromPending(item);

	// nicktheduke: the shake runs on real time, so menu previews play while the game is paused.
	{
		const auto clockNow = std::chrono::steady_clock::now();
		const float shakeDelta = shakeClockStarted ? std::clamp(std::chrono::duration<float>(clockNow - shakeClock).count(), 0.0f, kMaximumFrameStep) : 0.0f;
		shakeClock = clockNow;
		shakeClockStarted = true;
		UpdateShake(shakeDelta);
	}

	// Screen Blood Enhanced's wipe: everything on the glass smears off in a quarter second.
	if (wipeIn >= 0.0f) {
		wipeIn -= delta;
		if (wipeIn < 0.0f) {
			for (auto& splat : splats) {
				if (splat.age / splat.life < 0.8f) {
					splat.life = 0.3f / 0.2f;
					splat.age = 0.8f * splat.life;
				}
			}
		}
	}

	// Age the splats.
	for (auto& splat : splats)
		splat.age += delta;
	std::erase_if(splats, [](const Splat& a_splat) { return a_splat.age >= a_splat.life; });

	auto approach = [delta](float& a_value, float a_target, float a_seconds) {
		const float step = a_seconds > 0.0f ? delta / a_seconds : 1.0f;
		a_value += std::clamp(a_target - a_value, -step, step);
	};
	const float master = std::clamp(settings.MasterStrength, 0.0f, 2.0f);
	approach(dragonfire, settings.DragonfireEnabled ? frameSample.dragonfire * std::clamp(settings.DragonfireStrength, 0.0f, 2.0f) * master : 0.0f, frameSample.dragonfire > dragonfire ? 0.5f : 2.0f);
	// Rime grows slowly while you stand in the breath and lingers after.
	approach(rime, settings.RimeEnabled ? frameSample.rime * std::clamp(settings.RimeStrength, 0.0f, 2.0f) * master : 0.0f, frameSample.rime > rime ? 2.5f : 8.0f);
	approach(poison, settings.PoisonEnabled ? frameSample.poison * std::clamp(settings.PoisonStrength, 0.0f, 2.0f) * master : 0.0f, frameSample.poison > poison ? 3.0f : 4.0f);
	approach(steam, settings.SteamEnabled ? frameSample.steam * std::clamp(settings.SteamStrength, 0.0f, 2.0f) * master : 0.0f, 1.5f);
	shockPulse = std::max(0.0f, shockPulse - delta / 1.2f);
	shockAge = std::min(shockAge + delta, 99.0f);
	approach(shockPersist, settings.ShockEnabled ? frameSample.shock * std::clamp(settings.ShockStrength, 0.0f, 2.0f) * master : 0.0f, 0.6f);
	approach(shock, settings.ShockEnabled ? std::max(frameSample.shock, shockPulse) * std::clamp(settings.ShockStrength, 0.0f, 2.0f) * master : 0.0f, 0.6f);
	stomp = std::max(0.0f, stomp - delta / 1.4f);  // nicktheduke: a quick puff, not a lingering cloud

	// Poison ticks: a slow, gentle pulse about once a second.
	if (poison > 0.01f) {
		poisonTick += delta;
		if (poisonTick > 1.1f)
			poisonTick = 0.0f;
	}

	if (frameSample.rimeHasSource) {
		float u, v;
		Project(frameSample.rimeSource, u, v);
		const float aspect = inputDescription.Height > 0 ? static_cast<float>(inputDescription.Width) / static_cast<float>(inputDescription.Height) : 16.0f / 9.0f;
		float dx = (u - 0.5f) * aspect, dy = v - 0.5f;
		const float length = std::sqrt(dx * dx + dy * dy);
		if (std::isfinite(length) && length > 1e-4f) {
			rimeDirX += (dx / length - rimeDirX) * std::min(1.0f, delta * 3.0f);
			rimeDirY += (dy / length - rimeDirY) * std::min(1.0f, delta * 3.0f);
		}
	}

	shoutAge += delta;
	flashAge += delta;
	sinceFlash += delta;

	// Stamina breaths on the shared breathing clock. Ducks under Screen Weather's cold vapor so the two never stack.
	LensShared::AdvanceBreath(static_cast<uint64_t>(state->frameCountAtomic.load(std::memory_order_relaxed)) + 1u, delta, frameSample.exertion);
	// nicktheduke: stamina breaths only when you are actually out of breath: below 35% stamina (vanilla's winded point;
	// Blade and Blunt's exhaustion also runs off stamina), building in fully by 15%
	const float winded = 1.0f - Smooth(0.15f, 0.35f, frameSample.staminaPct);
	const float vapor = shared.coldBreathVisible.load(std::memory_order_relaxed);
	const float shimmerTarget = settings.StaminaBreathEnabled && frameSample.firstPerson && !frameSample.underwater ? winded * (1.0f - 0.85f * Clamp01(vapor)) * std::clamp(settings.StaminaBreathStrength, 0.0f, 2.0f) : 0.0f;
	approach(staminaShimmer, shimmerTarget, 1.0f);
}

void ScreenAction::RenderPostProcess(RE::RENDER_TARGET a_output)
{
	const float poisonAmount = std::max(poison, previewPoison);
	const float dragonAmount = std::max(dragonfire, previewDragonfire);
	const float rimeAmount = std::max(rime, previewRime);
	const float shockAmount = std::max(shock, previewShock);
	const bool shoutLive = shoutStrength > 0.001f && shoutAge < 1.0f;
	const bool flashLive = (flashStrength > 0.0f || slashStrength > 0.0f) && flashAge < 0.4f;
	const bool shaking = shakeZoom > 1.00001f || std::abs(shakeX) > 1e-6f || std::abs(shakeY) > 1e-6f || std::abs(shakeRot) > 1e-6f;
	const bool anything = shaking || !splats.empty() || poisonAmount > 0.001f || dragonAmount > 0.001f || rimeAmount > 0.001f || shockAmount > 0.001f ||
	                      steam > 0.001f || stomp > 0.001f || shoutLive || flashLive || staminaShimmer > 0.001f || previewStaminaBreath;

	auto& shared = LensShared::Get();
	if (!loaded || !settings.Enabled || !anything || !parameters || !sampler || !blendState || !rasterizerState || !depthStencilState ||
		static_cast<unsigned int>(a_output) >= RE::RENDER_TARGETS::kTOTAL || !PrepareShaders()) {
		shared.actionCoverage.store(0.0f, std::memory_order_relaxed);
		return;
	}
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

	// Lens budget: what Screen Weather already covers plus ours stays under the shared cap; ours makes room.
	float coverage = 0.0f;
	for (const auto& splat : splats)
		coverage += splat.size * splat.size * 2.0f * splat.alpha;
	coverage += poisonAmount * 0.25f + dragonAmount * 0.1f + rimeAmount * 0.25f + steam * 0.3f + stomp * 0.2f;
	const float weatherCoverage = shared.weatherCoverage.load(std::memory_order_relaxed);
	const float room = std::max(LensShared::kLensBudget - weatherCoverage * 0.6f, 0.12f);
	const float budget = coverage > room ? std::max(room / coverage, 0.35f) : 1.0f;
	shared.actionCoverage.store(Clamp01(coverage * budget), std::memory_order_relaxed);

	const bool floatInput = inputFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || inputFormat == DXGI_FORMAT_R32G32B32A32_FLOAT;
	const float poisonPulse = std::exp(-poisonTick * 2.5f) * (poison > 0.01f ? 1.0f : 0.0f);
	RenderData data{};
	data.BufferDim = { 1.0f / static_cast<float>(inputDescription.Width), 1.0f / static_cast<float>(inputDescription.Height), elapsed, floatInput ? 1.0f : 0.0f };
	data.HDRData = globals::features::hdrDisplay.GetSharedDataHDR();
	data.Global = { budget, LensShared::kSafeZone, settings.PhotosensitiveSafe ? 1.0f : 0.0f, static_cast<float>(std::min<size_t>(splats.size(), kMaxSplats)) };
	data.LayersA = { Clamp01(dragonAmount), Clamp01(rimeAmount), Clamp01(poisonAmount), Clamp01(shockAmount) };
	data.LayersB = { Clamp01(stomp), Clamp01(steam), poisonPulse, shared.coldBreathVisible.load(std::memory_order_relaxed) };
	// nicktheduke: .w = seconds since the last shock hit; the shader plays one discharge from it ("Shock Hit" in the preview
	// fires a real hit; the "Force Shock" slider previews the lasting state)
	const float shockHitAge = shockAge;
	data.RimeDir = { rimeDirX, rimeDirY, shared.weatherFrost.load(std::memory_order_relaxed), shockHitAge };
	data.Shout = { shoutAge, shoutLive ? shoutStrength : 0.0f, shoutU, shoutV };
	data.Flash = { flashU, flashV, flashAge, flashLive ? flashStrength : 0.0f };
	data.Slash = { slashDirX, slashDirY, flashAge, flashLive ? slashStrength : 0.0f };
	{
		const float exhaleAge = shared.exhaleAge;
		const float shockSeed = shockHits;   // nicktheduke: per-hit bolt layout
		const float persistShown = std::max(shockPersist, previewShock);
		// nicktheduke: "Preview Stamina Breaths" cycles an exhale every 2.2 s at full strength
		// real-time clock: game time stops while the menu is open, which would freeze the preview between breaths
		const double wallClock = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		const float breathAgeShown = previewStaminaBreath ? static_cast<float>(std::fmod(wallClock, 2.2)) : std::min(exhaleAge, 30.0f);
		const float breathStrengthShown = previewStaminaBreath ? 1.0f : shared.exhaleStrength;
		const float shimmerShown = previewStaminaBreath ? Clamp01(std::clamp(settings.StaminaBreathStrength, 0.0f, 2.0f)) : Clamp01(staminaShimmer);  // preview follows the strength slider
		// .w: whole part = shock hit counter (bolt layout), fraction = lasting shock strength (edge crackle)
		data.Breath = { breathAgeShown, breathStrengthShown, shimmerShown, std::fmod(shockSeed, 4096.0f) + std::min(Clamp01(persistShown), 0.99f) };
	}
	data.Shake = { shakeX, shakeY, shakeRot, shaking ? shakeZoom : 1.0f };
	for (size_t i = 0; i < splats.size() && i < kMaxSplats; ++i) {
		const auto& splat = splats[i];
		data.SplatA[i] = { splat.u, splat.v, splat.size, Clamp01(splat.age / std::max(splat.life, 0.01f)) };
		data.SplatB[i] = { static_cast<float>(splat.type), static_cast<float>(splat.variant), splat.seed, splat.angle };
		data.SplatC[i] = { splat.alpha, 0.0f, 0.0f, 0.0f };
	}
	parameters->Update(data);

	auto* context = globals::d3d::context;
	globals::profiler->BeginPass("ScreenAction::Lens");
	{
		PostProcessState backup(context);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		ID3D11ShaderResourceView* noResource = nullptr;
		context->PSSetShaderResources(0, 1, &noResource);
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
		ID3D11SamplerState* passSampler = sampler.get();
		ID3D11ShaderResourceView* passResource = inputView.get();
		context->PSSetConstantBuffers(0, 1, &buffer);
		context->PSSetSamplers(0, 1, &passSampler);
		context->PSSetShaderResources(0, 1, &passResource);
		context->Draw(3, 0);
	}
	globals::profiler->EndPass();
}

// ---------------------------------------------------------------------------------------------------------------------
// Settings

void ScreenAction::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	ImGui::Checkbox(T(TKEY("photosafe"), "Photosensitive Safe Mode"), &settings.PhotosensitiveSafe);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("photosafe_tooltip"), "At most about three gentle flashes a second, softer slash streaks, and no strobing anywhere.\nRecommended on."));
	ImGui::SliderFloat(T(TKEY("master"), "Overall Strength"), &settings.MasterStrength, 0.0f, 2.0f, "%.2f");
	ImGui::Text("%s", T(TKEY("budget_note"), "The centre of the screen always stays clear. With the Weather module on, both share one lens budget."));

	auto toggle = [](const char* a_label, bool& a_enabled, float& a_strength, const char* a_strengthLabel, const char* a_tooltip) {
		ImGui::Checkbox(a_label, &a_enabled);
		if (a_tooltip)
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", a_tooltip);
		ImGui::BeginDisabled(!a_enabled);
		ImGui::SliderFloat(a_strengthLabel, &a_strength, 0.0f, 2.0f, "%.2f");
		ImGui::EndDisabled();
	};

	if (ImGui::TreeNodeEx(T(TKEY("hits"), "Hits And Fluids"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
		toggle(T(TKEY("blood"), "Blood"), settings.BloodEnabled, settings.BloodStrength, T(TKEY("blood_strength"), "Blood Strength"),
			T(TKEY("blood_tooltip"), "Hits close in front of you spatter the lens, by creature: red blood (darker for vampires), insect ichor,\ndwemer oil, dry draugr dust, bone dust, spriggan sap, atronach embers, ice or sparks, ghost mist.\nPower attacks splatter more; ranged hits only when close."));
		if (otherScreenBlood) {
			ImGui::TextColored(ImGui::ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s %s", T(TKEY("blood_other"), "Also installed:"), otherScreenBloodName.c_str());
			const char* modes[] = { T(TKEY("blood_mode_theirs"), "Theirs only (ours off)"), T(TKEY("blood_mode_replace"), "Ours replaces their splatter"),
				T(TKEY("blood_mode_both"), "Both") };
			settings.BloodWithOtherScreenBlood = std::clamp(settings.BloodWithOtherScreenBlood, 0, 2);
			ImGui::Combo(T(TKEY("blood_mode"), "With Their Screen Blood"), &settings.BloodWithOtherScreenBlood, modes, 3);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("blood_mode_tooltip"), "Ours replaces theirs: their mod still decides when blood hits and how much (its settings are read live),\nits wipe animation and key still work and clear ours, and you see our creature-coloured blood instead of theirs.\nTheir splatter is hidden only while this is selected and comes back the moment you change it."));
		}
		ImGui::Text("%s %s", T(TKEY("readout_sanguine"), "Sanguine Symphony:"), sanguineSymphony ? T(TKEY("matched"), "found, blood groups matched") : T(TKEY("not_found"), "not found"));
		toggle(T(TKEY("venom"), "Venom Spit"), settings.VenomEnabled, settings.VenomStrength, T(TKEY("venom_strength"), "Venom Strength"),
			T(TKEY("venom_tooltip"), "Spider and chaurus spit: green goo with a few trapped bubbles that drips off."));
		toggle(T(TKEY("web"), "Webs"), settings.WebEnabled, settings.WebStrength, T(TKEY("web_strength"), "Web Strength"),
			T(TKEY("web_tooltip"), "Web spells: pale blue-white strands that cling, sag and slip off."));
		toggle(T(TKEY("frost_hits"), "Frost Hits And Ice Spit"), settings.FrostHitsEnabled, settings.FrostHitStrength, T(TKEY("frost_hit_strength"), "Frost Hit Strength"),
			T(TKEY("frost_hits_tooltip"), "A quick crystal burst from the side the frost came from. Merges with the Weather module's frost instead of stacking."));
		toggle(T(TKEY("fire_hits"), "Fire Hits And Fire Spit"), settings.FireHitsEnabled, settings.FireHitStrength, T(TKEY("fire_hit_strength"), "Fire Hit Strength"),
			T(TKEY("fire_hits_tooltip"), "Embers burst outward from the side the fire came from."));
		toggle(T(TKEY("shock"), "Shock"), settings.ShockEnabled, settings.ShockStrength, T(TKEY("shock_strength"), "Shock Strength"),
			T(TKEY("shock_tooltip"), "A slow colour shimmer and faint crackle at the edges. Never flickers fast."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("lasting"), "Breath, Poison And Atmosphere"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
		toggle(T(TKEY("dragonfire"), "Dragonfire Embers"), settings.DragonfireEnabled, settings.DragonfireStrength, T(TKEY("dragonfire_strength"), "Dragonfire Strength"),
			T(TKEY("dragonfire_tooltip"), "Standing in dragon fire breath: warm edges, heat shimmer and embers. A lighter version while you burn."));
		toggle(T(TKEY("rime"), "Frost Breath Rime"), settings.RimeEnabled, settings.RimeStrength, T(TKEY("rime_strength"), "Rime Strength"),
			T(TKEY("rime_tooltip"), "Dragon frost breath grows feathered rime from the side it comes from, and lingers.\nFrost atronachs and ice wraiths close by add a little."));
		toggle(T(TKEY("poison"), "Poisoned"), settings.PoisonEnabled, settings.PoisonStrength, T(TKEY("poison_strength"), "Poison Strength"),
			T(TKEY("poison_tooltip"), "Purple clouds seep in from the edges and pulse gently with each poison tick."));
		toggle(T(TKEY("steam"), "Dwemer Steam"), settings.SteamEnabled, settings.SteamStrength, T(TKEY("steam_strength"), "Steam Strength"), nullptr);
		toggle(T(TKEY("stomp"), "Stomp Dust"), settings.StompDustEnabled, settings.StompStrength, T(TKEY("stomp_strength"), "Stomp Strength"),
			T(TKEY("stomp_tooltip"), "Heavy hits from giants, mammoths, dragons and other big creatures kick dust up from below. Visual only, no shake."));
		toggle(T(TKEY("grit"), "Explosion Grit"), settings.GritEnabled, settings.GritStrength, T(TKEY("grit_strength"), "Grit Strength"), nullptr);
		toggle(T(TKEY("ash"), "Disintegrate Ash"), settings.AshPuffEnabled, settings.AshPuffStrength, T(TKEY("ash_strength"), "Ash Strength"), nullptr);
		toggle(T(TKEY("stamina_breath"), "Stamina Breaths (First Person)"), settings.StaminaBreathEnabled, settings.StaminaBreathStrength, T(TKEY("stamina_breath_strength"), "Stamina Breath Strength"),
			T(TKEY("stamina_breath_tooltip"), "When you are winded, each exhale is a faint shimmer of warm air rising from below. No fog, no colour.\nShares the Weather module's breathing rhythm and fades under its cold breath vapor."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("impact"), "Impact"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("%s %s", T(TKEY("readout_precision"), "Precision:"), precisionConnected ? T(TKEY("precision_on"), "connected (exact hit point and swing)") : T(TKEY("precision_off"), "not found (flash only)"));
		toggle(T(TKEY("flash"), "Impact Flash"), settings.ImpactFlashEnabled, settings.FlashStrength, T(TKEY("flash_strength"), "Flash Strength"),
			T(TKEY("flash_tooltip"), "A small, very short brightening at the exact point your weapon lands, timed with Precision's hitstop."));
		toggle(T(TKEY("slash"), "Slash Streak"), settings.SlashEnabled, settings.SlashStrength, T(TKEY("slash_strength"), "Slash Strength"),
			T(TKEY("slash_tooltip"), "A thin arc along your swing at the hit point, about 80 ms. Needs Precision."));
		toggle(T(TKEY("shout"), "Shout Ripple"), settings.ShoutRippleEnabled, settings.ShoutStrength, T(TKEY("shout_strength"), "Shout Strength"),
			T(TKEY("shout_tooltip"), "A ripple through the lens when you shout or are hit by a shout. Off by default: the game's own shout effect already plays."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("hitshake"), "Hit Shake"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("hitshake_enabled"), "Shake When You Are Hit"), &settings.HitShakeEnabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("hitshake_tooltip"), "The picture jolts away from the hit, by kind of hit. The HUD never moves.\nFast hits refresh one jolt instead of piling up, and the total is capped small."));
		ImGui::BeginDisabled(!settings.HitShakeEnabled);
		ImGui::SliderFloat(T(TKEY("hitshake_strength"), "Shake Strength"), &settings.HitShakeStrength, 0.0f, 2.0f, "%.2f");
		ImGui::Checkbox(T(TKEY("hitshake_fp"), "First Person"), &settings.HitShakeFirstPerson);
		ImGui::SameLine();
		ImGui::Checkbox(T(TKEY("hitshake_tp"), "Third Person"), &settings.HitShakeThirdPerson);
		if (ImGui::TreeNodeEx(T(TKEY("hitshake_types"), "By Kind Of Hit"))) {
			ImGui::SliderFloat(T(TKEY("shake_normal"), "Normal Hit (quick jolt)"), &settings.ShakeNormal, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("shake_power"), "Power Attack (jolt and rebounds)"), &settings.ShakePower, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("shake_bash"), "Bash (thud)"), &settings.ShakeBash, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("shake_arrow"), "Arrow And Bolt (snap)"), &settings.ShakeArrow, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("shake_blocked"), "Blocked Hit (muffled)"), &settings.ShakeBlocked, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("shake_big"), "Big Creature (heavy reverb)"), &settings.ShakeBig, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("shake_stagger"), "Stagger And Knockdown (quick lurch)"), &settings.ShakeStagger, 0.0f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("shake_stagger_tooltip"), "Third person only: in first person the stagger animation already moves the view.\nWorks with any poise or stagger mod: it follows the game's own stagger, sized by its magnitude."));
			ImGui::TreePop();
		}
		if (ImGui::TreeNodeEx(T(TKEY("hitshake_integrations"), "Integrations"), ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
			const char* modes[] = { T(TKEY("int_auto"), "Auto"), T(TKEY("int_on"), "On"), T(TKEY("int_off"), "Off") };
			auto combo = [&modes](const char* a_label, int& a_value, const char* a_status, const char* a_tooltip) {
				a_value = std::clamp(a_value, 0, 2);
				ImGui::SetNextItemWidth(110.0f);
				ImGui::Combo(a_label, &a_value, modes, 3);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", a_tooltip);
				ImGui::SameLine();
				ImGui::TextDisabled("%s", a_status);
			};
			std::string status;
			status = precisionConnected ? std::format("found, hitstop on you {:.0f} ms", precisionHitstopOnTarget ? precisionHitstop * 1000.0f : 0.0f) : std::string("not found");
			combo("Precision##shake", settings.IntegrationPrecision, status.c_str(),
				T(TKEY("int_precision_tooltip"), "Auto/On: the jolt follows the enemy blade's real movement and starts as Precision's hitstop on you ends.\nOff: direction from where the attacker stands, no delay. Precision's own shake on your hits is left alone."));
			status = fpcsLoaded ? (fpcsHitReaction ? "found, its hit jolt is on: first person shake steps aside" : "found, its hit jolt is off: ours runs") : "not found";
			combo("FP Camera Settle##shake", settings.IntegrationFPCameraSettle, status.c_str(),
				T(TKEY("int_fpcs_tooltip"), "Auto: no first person shake while FP Camera Settle's own hit reaction (TakingHit or FOV punch) is on.\nOn: always leave first person to it. Off: always shake."));
			status = valhallaLoaded ? (valhallaTimedBlockShake ? std::format("found, timed block shake on ({:.2f} s window)", valhallaTimedBlockWindow) : std::string("found, timed block shake off")) : std::string("not found");
			combo("Valhalla Combat##shake", settings.IntegrationValhalla, status.c_str(),
				T(TKEY("int_valhalla_tooltip"), "Auto: blocks inside Valhalla's timed block window are left to its own screen shake.\nOn: always skip them. Off: shake on every block."));
			status = (dfLoaded || ngdLoaded) ? std::format("{}{}{}", dfLoaded ? "Dismembering Framework" : "", dfLoaded && ngdLoaded ? " + " : "", ngdLoaded ? "Next-Gen Decapitations" : "") : std::string("not found");
			combo("Dismemberment (Seb263)##shake", settings.IntegrationDismember, status.c_str(),
				T(TKEY("int_dismember_tooltip"), "Auto/On: when a limb or head comes off close in front of you, an arterial spray hits the lens in the creature's colour.\nFollows your blood settings and Screen Blood Enhanced mode. Off: nothing extra."));
			ImGui::BeginDisabled(settings.IntegrationDismember == 2 || !(dfLoaded || ngdLoaded));
			ImGui::SliderFloat(T(TKEY("dismember_strength"), "Dismemberment Spray"), &settings.DismemberStrength, 0.0f, 2.0f, "%.2f");
			ImGui::Checkbox(T(TKEY("dismember_kick"), "Small Jolt With It"), &settings.DismemberKick);
			ImGui::EndDisabled();
			status = staggerRemover ? "found: third person lurch at half strength" : "not found";
			combo("3rd Person Camera Stagger Remover##shake", settings.IntegrationStaggerRemover, status.c_str(),
				T(TKEY("int_stagger_tooltip"), "Auto/On: a calmer stagger lurch for people who removed the vanilla stagger camera. Off: full lurch."));
			if (ImGui::Button(T(TKEY("int_rescan"), "Re-read Their Settings")))
				ReadIntegrations();
			ImGui::TreePop();
		}
		ImGui::EndDisabled();
		ImGui::Text("%s", T(TKEY("shake_preview_label"), "Preview (hit from the left):"));
		const char* names[] = { "Normal", "Power", "Bash", "Arrow", "Blocked", "Big", "Stagger", "Knockdown" };
		for (uint32_t i = 0; i < static_cast<uint32_t>(ShakeType::kTotal); ++i) {
			if (i > 0 && i != 4)
				ImGui::SameLine();
			if (ImGui::Button(std::format("{}##shakepreview", names[i]).c_str())) {
				Pending kick;
				kick.isShake = true;
				kick.variant = i;
				kick.strength = 1.0f;
				kick.hasSwing = i >= static_cast<uint32_t>(ShakeType::kStagger);
				kick.swing = { 1.0f, 0.0f, 0.0f };
				if (!kick.hasSwing) {
					// a point to your left, so the jolt goes right
					if (auto* player = RE::PlayerCharacter::GetSingleton()) {
						const float yaw = player->data.angle.z;
						kick.hasWorld = true;
						kick.world = player->GetPosition() + RE::NiPoint3(-std::cos(yaw) * 300.0f, std::sin(yaw) * 300.0f, 100.0f);
					}
				}
				Queue(kick);
			}
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("preview"), "Preview"))) {
		auto spawn = [this](SplatType a_type, uint32_t a_variant) {
			Pending item;
			item.type = a_type;
			item.variant = a_variant;
			item.count = a_type == SplatType::kBlood ? 2 : 1;
			Queue(item);
		};
		if (ImGui::Button(T(TKEY("p_blood"), "Blood")))
			spawn(SplatType::kBlood, 0);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_ichor"), "Ichor")))
			spawn(SplatType::kBlood, 2);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_oil"), "Oil")))
			spawn(SplatType::kBlood, 3);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_dust"), "Draugr")))
			spawn(SplatType::kBlood, 4);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_ghost"), "Ghost")))
			spawn(SplatType::kBlood, 10);
		if (ImGui::Button(T(TKEY("p_venom"), "Venom")))
			spawn(SplatType::kVenom, 0);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_web"), "Web")))
			spawn(SplatType::kWeb, 0);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_ice"), "Ice")))
			spawn(SplatType::kIce, 0);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_fire"), "Fire")))
			spawn(SplatType::kFire, 0);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_grit"), "Grit")))
			spawn(SplatType::kGrit, 0);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_ash"), "Ash")))
			spawn(SplatType::kAsh, 0);
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_steam"), "Steam")))
			spawn(SplatType::kSteam, 0);
		if (ImGui::Button(T(TKEY("p_flash"), "Flash And Slash"))) {
			flashAge = 0.0f;
			sinceFlash = 0.0f;
			flashU = 0.62f;
			flashV = 0.45f;
			flashStrength = settings.PhotosensitiveSafe ? 0.6f : 1.0f;
			slashDirX = 0.8f;
			slashDirY = -0.6f;
			slashStrength = settings.PhotosensitiveSafe ? 0.7f : 1.0f;
		}
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_shout"), "Shout"))) {
			Pending item;
			item.isShout = true;
			Queue(item);
		}
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("p_stomp"), "Stomp"))) {
			Pending item;
			item.type = SplatType::kGrit;
			item.variant = 98;
			Queue(item);
		}
		ImGui::SliderFloat(T(TKEY("p_poison"), "Force Poison"), &previewPoison, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("p_dragonfire"), "Force Dragonfire"), &previewDragonfire, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("p_rime"), "Force Rime"), &previewRime, 0.0f, 1.0f, "%.2f");
		if (ImGui::Button(T(TKEY("p_shock_hit"), "Shock Hit"))) {
			// nicktheduke: one discharge, exactly like a real hit
			shockPulse = 1.0f;
			shockAge = 0.0f;
			shockHits = std::fmod(shockHits + 1.0f, 4096.0f);
			shock = std::max(shock, Clamp01(std::clamp(settings.ShockStrength, 0.0f, 2.0f) * std::clamp(settings.MasterStrength, 0.0f, 2.0f)));
		}
		ImGui::SameLine();
		ImGui::Checkbox(T(TKEY("p_stamina_breath"), "Preview Stamina Breaths"), &previewStaminaBreath);
		ImGui::SliderFloat(T(TKEY("p_shock"), "Force Shock (lasting)"), &previewShock, 0.0f, 1.0f, "%.2f");
		ImGui::TreePop();
	}
}

void ScreenAction::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ScreenAction::LoadSettings(json& o_json)
{
	settings = o_json;
}

void ScreenAction::RestoreDefaultSettings()
{
	settings = {};
}

// ---------------------------------------------------------------------------------------------------------------------
// nicktheduke: recorded self test (weather\SelfTest.cpp drives it). Same triggers as the Preview buttons.

void ScreenAction::SelfTestTick(std::string_view a_name, float a_t, float a_prev, bool a_end)
{
	auto at = [&](float a_mark) { return !a_end && a_prev < a_mark && a_t >= a_mark; };
	auto spawn = [this](SplatType a_type, uint32_t a_variant) {
		Pending item;
		item.type = a_type;
		item.variant = a_variant;
		item.count = a_type == SplatType::kBlood ? 2 : 1;
		Queue(item);
	};
	const std::pair<std::string_view, uint32_t> fluids[] = { { "ichor", 2 }, { "oil", 3 }, { "draugr", 4 }, { "ghost", 10 } };
	const std::pair<std::string_view, SplatType> singles[] = { { "venom", SplatType::kVenom }, { "web", SplatType::kWeb },
		{ "ice", SplatType::kIce }, { "fire", SplatType::kFire }, { "grit", SplatType::kGrit }, { "ash_puff", SplatType::kAsh },
		{ "steam", SplatType::kSteam } };
	if (a_name == "blood") {
		for (float m : { 0.0f, 1.5f, 3.0f })
			if (at(m))
				spawn(SplatType::kBlood, 0);
		return;
	}
	for (const auto& [name, variant] : fluids) {
		if (a_name == name) {
			for (float m : { 0.0f, 1.5f })
				if (at(m))
					spawn(SplatType::kBlood, variant);
			return;
		}
	}
	for (const auto& [name, type] : singles) {
		if (a_name == name) {
			if (at(0.0f))
				spawn(type, 0);
			return;
		}
	}
	if (a_name == "flash_slash") {
		for (float m : { 0.0f, 2.5f }) {
			if (at(m)) {
				flashAge = 0.0f;
				sinceFlash = 0.0f;
				flashU = 0.62f;
				flashV = 0.45f;
				flashStrength = settings.PhotosensitiveSafe ? 0.6f : 1.0f;
				slashDirX = 0.8f;
				slashDirY = -0.6f;
				slashStrength = settings.PhotosensitiveSafe ? 0.7f : 1.0f;
			}
		}
	} else if (a_name == "shout") {
		if (at(0.0f)) {
			Pending item;
			item.isShout = true;
			Queue(item);
		}
	} else if (a_name == "stomp") {
		if (at(0.0f)) {
			Pending item;
			item.type = SplatType::kGrit;
			item.variant = 98;
			Queue(item);
		}
	} else if (a_name == "poison") {
		previewPoison = a_end ? 0.0f : 1.0f;
	} else if (a_name == "dragonfire") {
		previewDragonfire = a_end ? 0.0f : 1.0f;
	} else if (a_name == "rime") {
		previewRime = a_end ? 0.0f : 1.0f;
	} else if (a_name == "shock_hit") {
		for (float m : { 0.0f, 3.0f }) {
			if (at(m)) {
				shockPulse = 1.0f;
				shockAge = 0.0f;
				shockHits = std::fmod(shockHits + 1.0f, 4096.0f);
				shock = std::max(shock, Clamp01(std::clamp(settings.ShockStrength, 0.0f, 2.0f) * std::clamp(settings.MasterStrength, 0.0f, 2.0f)));
			}
		}
	} else if (a_name == "shock_lasting") {
		previewShock = a_end ? 0.0f : 0.8f;
	} else if (a_name == "stamina_breath") {
		previewStaminaBreath = !a_end;
	} else if (a_name == "hit_shake") {
		// Normal, Power, Bash, Big, Stagger: one every 1.5 s
		const uint32_t types[] = { 0, 1, 2, 5, 6 };
		for (uint32_t k = 0; k < 5; ++k) {
			if (!at(static_cast<float>(k) * 1.5f))
				continue;
			const uint32_t i = types[k];
			Pending kick;
			kick.isShake = true;
			kick.variant = i;
			kick.strength = 1.0f;
			kick.hasSwing = i >= static_cast<uint32_t>(ShakeType::kStagger);
			kick.swing = { 1.0f, 0.0f, 0.0f };
			if (!kick.hasSwing) {
				if (auto* player = RE::PlayerCharacter::GetSingleton()) {
					const float yaw = player->data.angle.z;
					kick.hasWorld = true;
					kick.world = player->GetPosition() + RE::NiPoint3(-std::cos(yaw) * 300.0f, std::sin(yaw) * 300.0f, 100.0f);
				}
			}
			Queue(kick);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// SLOPPY CAMERA FX module

ScreenAction globals::features::screenAction;

namespace
{
	class CombatModule final : public SloppyCameraFX::Module
	{
	public:
		const char* Name() const override { return "Combat"; }
		const char* LegacyName() const override { return "Screen Action"; }
		bool IsEnabled() const override { return globals::features::screenAction.settings.Enabled; }

		void OnPostPostLoad() override { globals::features::screenAction.PostPostLoad(); }
		void OnDataLoaded() override { globals::features::screenAction.DataLoaded(); }
		void OnGameLoaded() override { globals::features::screenAction.GameLoaded(); }
		void SetupResources() override { globals::features::screenAction.SetupResources(); }
		void Update() override { globals::features::screenAction.Reset(); }
		void Render(uint32_t a_output) override { globals::features::screenAction.RenderPostProcess(static_cast<RE::RENDER_TARGET>(a_output)); }
		void DrawMenu() override
		{
			ImGui::Text("SLOPPY CAMERA FX - Combat by nicktheduke");
			ImGui::Separator();
			globals::features::screenAction.DrawSettings();
			ImGui::Separator();
			if (ImGui::Button("Restore Defaults##combat"))
				globals::features::screenAction.RestoreDefaultSettings();
		}
		void LoadSettings(nlohmann::json& a_json) override { globals::features::screenAction.LoadSettings(a_json); }
		void SaveSettings(nlohmann::json& a_json) override { globals::features::screenAction.SaveSettings(a_json); }
	};

	struct Registrar
	{
		Registrar()
		{
			static CombatModule module;
			SloppyCameraFX::RegisterModule(&module, 200);  // after Weather (100): the same order the lens effects had in Bottled
		}
	} registrar;
}
