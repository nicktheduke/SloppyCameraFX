// SPDX-License-Identifier: GPL-3.0-or-later
// SLOPPY CAMERA FX - Combat by nicktheduke: combat and action effects on the camera lens (blood, venom, web, frost, fire, shock,
// dragonfire, poison, dust, steam, grit, ash, shout ripple, impact flash and slash, stamina breaths).
// Standalone: works without Screen Weather; with it, the two share one lens budget and one breathing rhythm.
#pragma once

#include "Compat.h"
#include "Module.h"

#include <chrono>
#include <future>

#include <deque>
#include <mutex>
#include <unordered_map>

struct ScreenAction
{
	struct Settings
	{
		bool Enabled = true;
		bool PhotosensitiveSafe = true;  // rate-limits flashes, softens bright changes, no strobing anywhere
		float MasterStrength = 1.0f;

		bool BloodEnabled = true;
		float BloodStrength = 1.0f;
		// With Screen Blood Enhanced (or another screen blood mod) installed:
		// 0 = theirs only, 1 = ours replaces their splatter (they still decide when, their wipe clears ours), 2 = both
		int BloodWithOtherScreenBlood = 1;
		bool VenomEnabled = true;
		float VenomStrength = 1.0f;
		bool WebEnabled = true;
		float WebStrength = 1.0f;
		bool FrostHitsEnabled = true;  // frost spell hits and ice spit
		float FrostHitStrength = 1.0f;
		bool FireHitsEnabled = true;  // fire spell hits and fire spit
		float FireHitStrength = 1.0f;
		bool ShockEnabled = true;
		float ShockStrength = 1.0f;
		bool DragonfireEnabled = true;
		float DragonfireStrength = 1.0f;
		bool RimeEnabled = true;  // dragon frost breath, frost atronachs and ice wraiths close by
		float RimeStrength = 1.0f;
		bool PoisonEnabled = true;
		float PoisonStrength = 1.0f;
		bool StompDustEnabled = true;
		float StompStrength = 1.0f;
		bool SteamEnabled = true;
		float SteamStrength = 1.0f;
		bool GritEnabled = true;
		float GritStrength = 1.0f;
		bool AshPuffEnabled = true;
		float AshPuffStrength = 1.0f;
		bool ShoutRippleEnabled = false;  // off by default: the vanilla shout screen effect already plays
		float ShoutStrength = 1.0f;
		bool ImpactFlashEnabled = true;
		bool ImpactFlashThirdPerson = false;  // off: the flash is a first person effect (on: also as a hit spark in third person)
		float FlashStrength = 1.0f;
		bool SlashEnabled = true;
		float SlashStrength = 1.0f;
		bool StaminaBreathEnabled = true;
		float StaminaBreathStrength = 1.0f;

		// nicktheduke: hit shake. The whole image jolts when you are hit (never the HUD), by kind of hit.
		bool HitShakeEnabled = true;
		float HitShakeStrength = 1.0f;
		bool HitShakeFirstPerson = true;
		bool HitShakeThirdPerson = true;
		float ShakeNormal = 1.0f;
		float ShakePower = 1.0f;
		float ShakeBash = 1.0f;
		float ShakeArrow = 1.0f;
		float ShakeBlocked = 1.0f;
		float ShakeBig = 1.0f;
		float ShakeStagger = 1.0f;  // stagger and knockdown lurch (third person)
		// Integrations: 0 = Auto (adapt to what is installed and how it is set), 1 = On (always), 2 = Off (ignore it).
		int IntegrationPrecision = 0;       // incoming hit direction from the blade, start after hitstop
		int IntegrationFPCameraSettle = 0;  // first person: step aside while its own hit reaction is on
		int IntegrationValhalla = 0;        // skip timed blocks while its timed block shake is on
		int IntegrationStaggerRemover = 0;  // third person lurch at half strength
		// nicktheduke: Seb263's Dismembering Framework and Next-Gen Decapitations: arterial spray on the lens when a limb or
		// head comes off close in front of you. 0 = Auto, 1 = On, 2 = Off.
		int IntegrationDismember = 0;
		float DismemberStrength = 1.0f;
		bool DismemberKick = false;  // a small jolt with it (off by default: the lens spray carries it)
	};

	void SetupResources();
	void PostPostLoad();
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

	bool loaded = true;

	/** @brief Draws the action effects over the tone-mapped image (after the Weather module). */
	void RenderPostProcess(RE::RENDER_TARGET a_output);

	Settings settings;

	enum class SplatType : uint32_t
	{
		kBlood = 0,
		kVenom = 1,
		kWeb = 2,
		kIce = 3,
		kFire = 4,
		kGrit = 5,
		kAsh = 6,
		kSteam = 7
	};

	/** @brief Something that happened in the game, queued on the main thread and turned into lens effects on the render thread. */
	struct Pending
	{
		SplatType type = SplatType::kBlood;
		uint32_t variant = 0;
		uint32_t count = 1;
		float strength = 1.0f;
		bool hasWorld = false;
		RE::NiPoint3 world{};  // where it came from (projected to place the splat on that side of the screen)
		bool isFlash = false;
		bool hasSwing = false;
		RE::NiPoint3 swing{};  // world-space swing velocity (Precision)
		bool isShout = false;
		bool isWipe = false;     // Screen Blood Enhanced wiped the lens
		float life = 0.0f;       // overrides the splat lifetime when set
		bool isShake = false;    // nicktheduke: hit shake kick (type in variant, strength, direction from world/swing)
		float delay = 0.0f;      // nicktheduke: seconds before the shake starts (Precision hitstop)
	};

	enum class ShakeType : uint32_t
	{
		kNormal = 0,
		kPower,
		kBash,
		kArrow,
		kBlocked,
		kBig,
		kStagger,
		kKnockdown,
		kTotal
	};

	void Queue(const Pending& a_pending);
	void OnHit(const RE::TESHitEvent* a_event);       // main thread
	void OnSpellCast(const RE::TESSpellCastEvent* a_event);
	void OnPrecisionHit(RE::Actor* a_attacker, RE::TESObjectREFR* a_target, const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity);
	void OnPlayerAnimation(const RE::BSAnimationGraphEvent* a_event);  // main thread: stagger and block raise
	void OnMagicEffectApply(const RE::TESMagicEffectApplyEvent* a_event);
	void OnEquip(const RE::TESEquipEvent* a_event);
	void OnObjectLoaded(const RE::TESObjectLoadedEvent* a_event);

private:
	struct Splat
	{
		SplatType type = SplatType::kBlood;
		uint32_t variant = 0;
		float u = 0.5f, v = 0.5f;
		float size = 0.1f;
		float age = 0.0f;
		float life = 5.0f;
		float seed = 0.0f;
		float alpha = 1.0f;
		float angle = 0.0f;
	};

	struct GameSample
	{
		bool firstPerson = false;
		bool underwater = false;
		float exertion = 0.0f;
		float staminaPct = 1.0f;  // nicktheduke: current / maximum stamina (stamina breaths only below 35%)
		float dragonfire = 0.0f;
		float rime = 0.0f;
		RE::NiPoint3 rimeSource{};
		bool rimeHasSource = false;
		float poison = 0.0f;
		float shock = 0.0f;
		float steam = 0.0f;
	};

	void QueryGame();  // main thread
	void ConnectPrecision();
	bool PrepareShaders();
	bool PrepareInput(ID3D11Texture2D* a_output, ID3D11RenderTargetView* a_view);
	void SpawnFromPending(const Pending& a_pending);
	void AddSplat(const Splat& a_splat);
	bool Project(const RE::NiPoint3& a_world, float& o_u, float& o_v) const;
	uint32_t ClassifyBlood(RE::Actor* a_actor) const;
	bool BloodAllowed() const;
	float Random();

	std::mutex pendingMutex;
	std::deque<Pending> pending;
	std::unordered_map<uint32_t, float> cooldowns;  // per effect kind, seconds left (main thread)
	std::unordered_map<RE::FormID, float> seenRefs;  // explosions and ash piles already reacted to

	std::mutex sampleMutex;
	GameSample sample;
	GameSample frameSample;
	std::atomic<bool> queryPending{ false };
	float queryElapsed = 0.0f;
	float elapsed = 0.0f;

	std::vector<Splat> splats;
	float dragonfire = 0.0f;
	float rime = 0.0f;
	float rimeDirX = 1.0f, rimeDirY = 0.0f;
	float poison = 0.0f;
	float poisonTick = 0.0f;
	float shock = 0.0f;
	float shockPulse = 0.0f;
	float shockAge = 99.0f;  // nicktheduke: seconds since the last shock hit (one discharge per hit)
	float shockHits = 0.0f;  // nicktheduke: hit counter, seeds a new bolt layout per hit
	float shockPersist = 0.0f;  // nicktheduke: lasting shock effect on the player (cloaks, auras), shown as edge crackle
	float steam = 0.0f;
	float stomp = 0.0f;
	float shoutAge = 99.0f, shoutStrength = 0.0f, shoutU = 0.5f, shoutV = 0.5f;
	float flashAge = 99.0f, flashStrength = 0.0f, flashU = 0.5f, flashV = 0.5f;
	float slashDirX = 1.0f, slashDirY = 0.0f, slashStrength = 0.0f;
	float sinceFlash = 99.0f;
	float staminaShimmer = 0.0f;
	uint32_t randomState = 0x9E3779B9u;

	// Integrations, detected by plugin name (no ESP of our own).
	bool otherScreenBlood = false;  // Screen Blood Enhanced or Enhanced Blood Textures
	std::string otherScreenBloodName;
	bool screenBloodEnhanced = false;
	bool vanillaSplatterHidden = false;
	std::unordered_map<std::string, float> savedSplatter;  // their splatter settings, restored when we stop hiding them
	float otherBloodDuration = 0.0f;  // their splatter duration, so ours lasts as long
	bool otherReceiveBlood = false;   // they show blood when you are hit
	float lastWipeTrigger = 0.0f;
	float wipeIn = -1.0f;             // render thread: seconds until the wipe clears the lens
	struct SBEGlobals
	{
		RE::TESGlobal* count = nullptr;
		RE::TESGlobal* distance = nullptr;
		RE::TESGlobal* heading = nullptr;
		RE::TESGlobal* unarmed = nullptr;
		RE::TESGlobal* blunt = nullptr;
		RE::TESGlobal* blade = nullptr;
		RE::TESGlobal* projectile = nullptr;
		RE::TESGlobal* power = nullptr;
		RE::TESGlobal* wipe = nullptr;
	} sbe;
	bool ReplacingOtherBlood() const;
	int SBESplatterCount(RE::Actor* a_victim, RE::TESObjectWEAP* a_weapon, bool a_power) const;
	void UpdateOtherSplatter();  // main thread
	bool sanguineSymphony = false;
	bool bladeAndBlunt = false;
	bool precisionConnected = false;

	// nicktheduke: hit shake.
	struct Kick
	{
		ShakeType type = ShakeType::kNormal;
		float age = 0.0f;  // negative while waiting (hitstop)
		float strength = 1.0f;
		float dirX = 1.0f, dirY = 0.0f;  // camera push direction on screen (screen heights, unit)
		float seed = 0.0f;
	};
	std::vector<Kick> kicks;
	float shakeX = 0.0f, shakeY = 0.0f, shakeRot = 0.0f, shakeZoom = 1.0f;
	std::chrono::steady_clock::time_point shakeClock{};
	bool shakeClockStarted = false;
	void AddKick(ShakeType a_type, float a_strength, float a_dirX, float a_dirY, float a_delay);
	void UpdateShake(float a_delta);
	bool ShakeAllowed(bool a_firstPerson) const;
	void ReadIntegrations();
	void ConnectPlayerAnimation();
	static double Now();
	// What the integrations found (read at data load, refreshed when the menu opens).
	bool fpcsLoaded = false;
	bool fpcsHitReaction = false;  // its TakingHit / FOV punch on hit is on
	bool valhallaLoaded = false;
	bool valhallaTimedBlockShake = false;
	float valhallaTimedBlockWindow = 0.0f;
	bool staggerRemover = false;
	bool precisionHitstopOnTarget = false;
	float precisionHitstop = 0.0f;
	float precisionHitstopPowerMult = 1.0f;
	// Main thread state for incoming hits.
	float lastHealth = -1.0f;
	double blockRaisedAt = -100.0;
	bool wasBlocking = false;
	bool wasRagdoll = false;
	double lastStaggerAt = -100.0;
	struct IncomingSwing
	{
		double time = -100.0;
		RE::NiPoint3 position{};
		RE::NiPoint3 velocity{};
	} incomingSwing;
	std::mutex swingMutex;

	// nicktheduke: Dismembering Framework / Next-Gen Decapitations (found by plugin name and their own form IDs, no plugin of ours)
	bool dfLoaded = false;
	bool ngdLoaded = false;
	RE::SpellItem* dfSpray = nullptr;
	RE::SpellItem* dfSprayTrace = nullptr;
	RE::EffectSetting* dfSprayEffect = nullptr;
	RE::EffectSetting* dfSprayTraceEffect = nullptr;
	RE::TESForm* dfLimb = nullptr;
	RE::TESForm* ngdNeck = nullptr;
	RE::TESForm* ngdNeckAfflicted = nullptr;
	std::unordered_map<RE::FormID, double> goreSeen;  // actor -> last time (one spray per dismemberment)
	uint32_t goreSignals[4]{};                          // spell cast, effect apply, limb loaded, NGD equip (logged)
	bool DismemberActive() const;
	void OnGore(RE::TESObjectREFR* a_source, bool a_decapitation, uint32_t a_signal);

	// Menu-only previews (never saved).
	float previewPoison = 0.0f;
	float previewDragonfire = 0.0f;
	float previewRime = 0.0f;
	float previewShock = 0.0f;
	bool previewStaminaBreath = false;  // nicktheduke: preview exhales without being winded or in first person

	winrt::com_ptr<ID3D11VertexShader> vertexShader;
	winrt::com_ptr<ID3D11PixelShader> pixelShader;
	winrt::com_ptr<ID3D11Texture2D> inputTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> inputView;
	winrt::com_ptr<ID3D11SamplerState> sampler;
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
	extern ScreenAction screenAction;
}
