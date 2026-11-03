// SLOPPY CAMERA FX by nicktheduke.
// The small slice of the Community Shaders framework the lens modules were written against, rebuilt on plain
// D3D11 + CommonLibSSE so the modules run with vanilla, Community Shaders or Bottled Shaders alike.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <DirectXMath.h>

#include "SKSEMenuFramework.h"

using float2 = DirectX::XMFLOAT2;
using float3 = DirectX::XMFLOAT3;
using float4 = DirectX::XMFLOAT4;

// Menu: SKSE Menu Framework's ImGui mirror, so module menus keep their ImGui calls.
namespace ImGui = ImGuiMCP;

// Translation hooks from Community Shaders: the default text is used as is.
#define TKEY(a_key) a_key
inline const char* T(const char*, const char* a_text) { return a_text; }

namespace winrt
{
	/** @brief Minimal COM pointer with the cppwinrt surface the modules use (get, put, attach, bool, move). */
	template <class T>
	class com_ptr
	{
	public:
		com_ptr() = default;
		com_ptr(const com_ptr&) = delete;
		com_ptr& operator=(const com_ptr&) = delete;
		com_ptr(com_ptr&& a_other) noexcept : ptr(std::exchange(a_other.ptr, nullptr)) {}
		com_ptr& operator=(com_ptr&& a_other) noexcept
		{
			if (this != &a_other) {
				reset();
				ptr = std::exchange(a_other.ptr, nullptr);
			}
			return *this;
		}
		~com_ptr() { reset(); }
		T* get() const { return ptr; }
		T** put()
		{
			reset();
			return &ptr;
		}
		void attach(T* a_value)
		{
			reset();
			ptr = a_value;
		}
		void reset()
		{
			if (ptr) {
				ptr->Release();
				ptr = nullptr;
			}
		}
		T* operator->() const { return ptr; }
		explicit operator bool() const { return ptr != nullptr; }

	private:
		T* ptr = nullptr;
	};
}

/** @brief Dynamic constant buffer (CPU write, discard on update). */
struct ConstantBufferDescription
{
	UINT size = 0;
};

template <class T>
ConstantBufferDescription ConstantBufferDesc()
{
	static_assert(sizeof(T) % 16 == 0, "constant buffers are multiples of 16 bytes");
	return { static_cast<UINT>(sizeof(T)) };
}

class ConstantBuffer
{
public:
	ConstantBuffer(const ConstantBufferDescription& a_desc, const char* a_name);
	template <class T>
	void Update(const T& a_data)
	{
		Update(&a_data, sizeof(T));
	}
	void Update(const void* a_data, size_t a_size);
	ID3D11Buffer* CB() const { return buffer.get(); }

private:
	winrt::com_ptr<ID3D11Buffer> buffer;
	UINT size = 0;
};

namespace globals
{
	namespace d3d
	{
		inline ID3D11Device* device = nullptr;
		inline ID3D11DeviceContext* context = nullptr;
	}

	/** @brief Menu and frame state the modules read on the render thread (kept by core from menu events). */
	struct GameState
	{
		std::atomic<bool> mainMenu{ false };
		std::atomic<bool> loadingMenu{ false };
		std::atomic<bool> isMapMenuOpen{ false };
		std::atomic<bool> isStatsMenuOpen{ false };
		std::atomic<bool> isLoadingMenuOpen{ false };
		std::atomic<uint64_t> frameCountAtomic{ 0 };
		bool IsMainOrLoadingMenuOpen() const { return mainMenu.load(std::memory_order_relaxed) || loadingMenu.load(std::memory_order_relaxed); }
	};
	inline GameState gameState;
	inline GameState* state = &gameState;

	struct NullProfiler
	{
		void BeginPass(const char*) {}
		void EndPass() {}
	};
	inline NullProfiler nullProfiler;
	inline NullProfiler* profiler = &nullProfiler;

	namespace features
	{
		/** @brief HDR output info: plain SDR unless a future HDR path fills it (the shaders treat zeros as SDR). */
		struct HDRDisplayShim
		{
			float4 GetSharedDataHDR() const { return { 0.0f, 0.0f, 0.0f, 0.0f }; }
		};
		inline HDRDisplayShim hdrDisplay;
	}
}

namespace Util
{
	inline void SetResourceName(ID3D11DeviceChild* a_resource, const char* a_name)
	{
		if (a_resource && a_name)
			a_resource->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(std::strlen(a_name)), a_name);
	}

	/**
	 * @brief Compiles (or loads from SLOPPY CAMERA FX's own disk cache) one shader stage. Thread safe; the caller
	 * owns the returned reference. Returns nullptr on failure (logged with the compiler's message).
	 */
	ID3D11DeviceChild* CompileShader(const wchar_t* a_path, const std::vector<std::pair<const char*, const char*>>& a_defines, const char* a_target, const char* a_entry);

	/** @brief `if (auto _tt = HoverTooltipWrapper()) ImGui::Text(...)` shows a tooltip for the item just drawn. */
	class HoverTooltipWrapper
	{
	public:
		HoverTooltipWrapper() :
			active(ImGuiMCP::IsItemHovered() && ImGuiMCP::BeginTooltip()) {}
		~HoverTooltipWrapper()
		{
			if (active)
				ImGuiMCP::EndTooltip();
		}
		HoverTooltipWrapper(const HoverTooltipWrapper&) = delete;
		HoverTooltipWrapper& operator=(const HoverTooltipWrapper&) = delete;
		explicit operator bool() const { return active; }

	private:
		bool active;
	};
}

namespace Camera
{
	/**
	 * @brief Projects a world point to screen uv (0..1, v down) with the main camera. Behind the camera the point is
	 * mirrored and pushed off screen so it keeps its side; returns false then.
	 */
	bool WorldToScreen(const RE::NiPoint3& a_world, float& o_u, float& o_v);
}
