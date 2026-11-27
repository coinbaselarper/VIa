#include "package_splash.h"
#include "title_window.h"

#include "image_decode.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>
#include <string_view>
#include <vector>

namespace rt::package_splash {
namespace {
using Microsoft::WRL::ComPtr;
std::filesystem::path image_path;

// macOS: no XmlLite. The two manifests are flat XML; the first <element ... attribute="value"> is taken.
std::filesystem::path declared_image(const std::filesystem::path& package, const wchar_t* manifest,
                                     const wchar_t* element, const wchar_t* attribute) {
    const auto config = package / manifest;
    std::ifstream in(config, std::ios::binary);
    if (!in) return {};
    std::stringstream text;
    text << in.rdbuf();
    const std::string xml = text.str();
    const std::string tag = rthost::to_utf8(element), attr = rthost::to_utf8(attribute);
    const std::regex pattern("<" + tag + "\\b[^>]*\\b" + attr + "\\s*=\\s*\"([^\"]*)\"");
    std::smatch match;
    if (!std::regex_search(xml, match, pattern) || match[1].str().empty()) return {};
    std::string value = match[1].str();
    std::replace(value.begin(), value.end(), '\\', '/');
    const std::filesystem::path asset(value);
    if (asset.has_root_path() || value.find(':') != std::string::npos) return {};
    for (const auto& part : asset)
        if (part == "..") return {};
    std::error_code ec;
    const auto root = std::filesystem::canonical(package, ec);
    if (ec) return {};
    const auto file = std::filesystem::canonical(root / asset, ec);
    if (ec) return {};
    const auto relative = file.lexically_relative(root);
    if (relative.empty() || *relative.begin() == ".." || !std::filesystem::is_regular_file(file, ec)) return {};
    return file;
}

std::vector<BYTE> load_pixels(UINT width, UINT height) {
    if (image_path.empty() || !width || !height || std::uint64_t(width) * height > (64u << 20)) return {};
    std::vector<BYTE> pixels;
    try {
        if (!rthost::decode_image_bgra_fit(image_path.c_str(), width, height, pixels)) pixels.clear();
    } catch (const std::bad_alloc&) {
        // Optional startup art must leave the first title frame available on low-memory hosts.
    }
    return pixels;
}

}  // namespace

void configure(const std::filesystem::path& package) {
    image_path.clear();
    wchar_t value[4]{};
    if (GetEnvironmentVariableW(L"LEONIDA_PACKAGE_SPLASH", value, 4) == 1 && value[0] == L'0') return;
    wchar_t window_mode[16]{};
    const DWORD length = GetEnvironmentVariableW(L"LEONIDA_PRESENT_WINDOW", window_mode, 16);
    if (length && length < 16 && (std::wstring_view(window_mode) == L"own" || std::wstring_view(window_mode) == L"hidden"))
        return;
    image_path = declared_image(package, L"MicrosoftGame.Config", L"ShellVisuals", L"SplashScreenImage");
    if (image_path.empty()) image_path = declared_image(package, L"appxmanifest.xml", L"SplashScreen", L"Image");
}

bool configured() { return !image_path.empty(); }

// macOS: no GDI to paint the title's window with; the splash reaches the window through create_texture and
// WindowPresenter::present_splash only.
bool paint_window(HWND) { return false; }

ID3D12Resource* create_texture(ID3D12Device* device, UINT width, UINT height) {
    if (!device) return nullptr;
    const auto pixels = load_pixels(width, height);
    if (pixels.empty()) return nullptr;
    const UINT pitch = width * 4;
    const UINT bytes = pitch * height;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 1, 1};
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&texture))) ||
        FAILED(texture->WriteToSubresource(0, nullptr, pixels.data(), pitch, bytes)))
        return nullptr;
    return texture.Detach();
}
}  // namespace rt::package_splash
