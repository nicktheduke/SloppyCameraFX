// SLOPPY CAMERA FX by nicktheduke. Core helpers: constant buffers, shader compiling with a disk cache, projection.
#include "Compat.h"

#include <d3dcompiler.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

// ---------------------------------------------------------------------------------------------------------------------
// Constant buffer

ConstantBuffer::ConstantBuffer(const ConstantBufferDescription& a_desc, const char* a_name) :
	size(a_desc.size)
{
	auto* device = globals::d3d::device;
	if (!device || size == 0)
		return;
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = size;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device->CreateBuffer(&desc, nullptr, buffer.put())))
		logger::error("[core] could not create constant buffer {}", a_name ? a_name : "");
	else
		Util::SetResourceName(buffer.get(), a_name);
}

void ConstantBuffer::Update(const void* a_data, size_t a_size)
{
	auto* context = globals::d3d::context;
	if (!buffer || !context || !a_data)
		return;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return;
	std::memcpy(mapped.pData, a_data, std::min<size_t>(a_size, size));
	context->Unmap(buffer.get(), 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shader compiling (D3DCompile + a small disk cache keyed by source text, target, entry point and defines)

namespace
{
	constexpr auto kCacheFolder = L"Data\\SKSE\\Plugins\\SloppyCameraFX\\Cache";
	constexpr uint64_t kCacheVersion = 1;

	uint64_t Fnv1a(const void* a_data, size_t a_size, uint64_t a_hash = 1469598103934665603ull)
	{
		const auto* bytes = static_cast<const uint8_t*>(a_data);
		for (size_t i = 0; i < a_size; ++i) {
			a_hash ^= bytes[i];
			a_hash *= 1099511628211ull;
		}
		return a_hash;
	}

	ID3D11DeviceChild* CreateStage(const char* a_target, const void* a_code, size_t a_size)
	{
		auto* device = globals::d3d::device;
		if (!device)
			return nullptr;
		if (!_stricmp(a_target, "ps_5_0")) {
			ID3D11PixelShader* shader = nullptr;
			return SUCCEEDED(device->CreatePixelShader(a_code, a_size, nullptr, &shader)) ? shader : nullptr;
		}
		if (!_stricmp(a_target, "vs_5_0")) {
			ID3D11VertexShader* shader = nullptr;
			return SUCCEEDED(device->CreateVertexShader(a_code, a_size, nullptr, &shader)) ? shader : nullptr;
		}
		if (!_stricmp(a_target, "cs_5_0")) {
			ID3D11ComputeShader* shader = nullptr;
			return SUCCEEDED(device->CreateComputeShader(a_code, a_size, nullptr, &shader)) ? shader : nullptr;
		}
		return nullptr;
	}

	std::mutex cacheMutex;
}

namespace Util
{
	ID3D11DeviceChild* CompileShader(const wchar_t* a_path, const std::vector<std::pair<const char*, const char*>>& a_defines, const char* a_target, const char* a_entry)
	{
		const std::filesystem::path path(a_path);
		const std::string pathText = path.string();
		std::string source;
		{
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				logger::error("[core] shader not found: {}", pathText);
				return nullptr;
			}
			std::ostringstream text;
			text << file.rdbuf();
			source = text.str();
		}

		std::vector<D3D_SHADER_MACRO> macros;
		for (const auto& [name, value] : a_defines)
			if (name && *name)
				macros.push_back({ name, value ? value : "" });
		macros.push_back({ "WINPC", "" });
		macros.push_back({ "DX11", "" });
		macros.push_back({ nullptr, nullptr });

		uint64_t key = Fnv1a(&kCacheVersion, sizeof(kCacheVersion));
		key = Fnv1a(source.data(), source.size(), key);
		key = Fnv1a(a_target, std::strlen(a_target), key);
		key = Fnv1a(a_entry, std::strlen(a_entry), key);
		for (const auto& macro : macros) {
			if (macro.Name)
				key = Fnv1a(macro.Name, std::strlen(macro.Name), key);
			if (macro.Definition)
				key = Fnv1a(macro.Definition, std::strlen(macro.Definition), key);
		}
		const std::filesystem::path cachePath = std::filesystem::path(kCacheFolder) / std::format("{}_{:016X}.cso", path.stem().string(), key);

		{
			std::scoped_lock lock(cacheMutex);
			std::ifstream cached(cachePath, std::ios::binary);
			if (cached) {
				std::vector<char> code((std::istreambuf_iterator<char>(cached)), std::istreambuf_iterator<char>());
				if (!code.empty())
					if (auto* shader = CreateStage(a_target, code.data(), code.size())) {
						logger::info("[core] {} {} loaded from cache", path.filename().string(), a_target);
						return shader;
					}
			}
		}

		const auto start = std::chrono::steady_clock::now();
		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;
		const HRESULT result = D3DCompile(source.data(), source.size(), pathText.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, a_entry, a_target,
			D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
		if (errors) {
			std::string message(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
			if (FAILED(result))
				logger::error("[core] {} {} failed to compile:\n{}", path.filename().string(), a_target, message);
			errors->Release();
		}
		if (FAILED(result) || !code) {
			if (code)
				code->Release();
			return nullptr;
		}
		auto* shader = CreateStage(a_target, code->GetBufferPointer(), code->GetBufferSize());
		if (shader) {
			std::scoped_lock lock(cacheMutex);
			std::error_code error;
			std::filesystem::create_directories(kCacheFolder, error);
			std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
			if (out)
				out.write(static_cast<const char*>(code->GetBufferPointer()), static_cast<std::streamsize>(code->GetBufferSize()));
		}
		code->Release();
		logger::info("[core] {} {} compiled in {:.1f} s", path.filename().string(), a_target,
			std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count());
		return shader;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Projection

namespace Camera
{
	bool WorldToScreen(const RE::NiPoint3& a_world, float& o_u, float& o_v)
	{
		o_u = o_v = 0.5f;
		auto* camera = RE::Main::WorldRootCamera();
		if (!camera)
			return false;
		float x = 0.5f, y = 0.5f, z = 0.0f;
		if (camera->WorldPtToScreenPt3(a_world, x, y, z, 1e-5f) && z >= 0.0f && std::isfinite(x) && std::isfinite(y)) {
			// In front: the engine returns 0..1 with y up.
			o_u = x;
			o_v = 1.0f - y;
			return true;
		}
		// Behind the camera the engine gives up. Do the same maths by hand (rows of worldToCam, like the engine), which
		// mirrors the point; un-mirror it and push it well off screen so it keeps the side it is on.
		const auto& m = camera->GetRuntimeData().worldToCam;
		const float cx = m[0][0] * a_world.x + m[0][1] * a_world.y + m[0][2] * a_world.z + m[0][3];
		const float cy = m[1][0] * a_world.x + m[1][1] * a_world.y + m[1][2] * a_world.z + m[1][3];
		const float cw = m[3][0] * a_world.x + m[3][1] * a_world.y + m[3][2] * a_world.z + m[3][3];
		if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cw) || std::abs(cw) < 1e-4f)
			return false;
		const float nx = cx / cw, ny = cy / cw;
		if (cw > 0.0f) {
			// In front but inside the near plane (very close hits): no mirroring, the projection is right as is.
			o_u = nx * 0.5f + 0.5f;
			o_v = 0.5f - ny * 0.5f;
			return true;
		}
		o_u = (-nx * 10.0f) * 0.5f + 0.5f;
		o_v = 0.5f - (-ny * 10.0f) * 0.5f;
		return false;
	}
}
