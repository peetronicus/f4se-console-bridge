#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <memory>
#include "protocol.h"

using UInt8 = unsigned char;
using UInt32 = unsigned long;
using UInt64 = unsigned long long;
#include "f4se/PluginAPI.h"
#include "f4se/GameThreads.h"
#include "f4se_common/f4se_version.h"

namespace fs = std::filesystem;
namespace {
// Runtime-specific facts, not game bytes. See docs/runtime-support.md.
constexpr UInt32 runtime = RUNTIME_VERSION_1_11_240;
constexpr uintptr_t execute_rva = 0x01036250;
constexpr uintptr_t print_rva = 0x0103c250;
constexpr size_t displaced_size = 14;
constexpr char exe_digest[] = "fdcef37ac1230af6d0b0050eb2142b139ef3a867b37b9211fb6edfcc646072f8";
using Execute = void (*)(const char*);
using Print = void (*)(void*, const char*);
Execute execute_command = nullptr;
Print original_print = nullptr;
F4SETaskInterface* tasks = nullptr;
fs::path session_path;
std::atomic<bool> ready{false}, occupied{false}, disabled{false};
std::mutex output_mutex;
std::string console_text;
bool output_truncated = false;
constexpr size_t output_limit = 65536;

void atomic_write(const fs::path& path, const std::string& text) {
    const auto temp = fs::path(path.wstring() + L".tmp");
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("atomic response publication failed");
}

std::string sha256(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0); return {};
    }
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 65536> bytes{};
    bool ok = stream.is_open();
    while (ok && stream) {
        stream.read(bytes.data(), bytes.size());
        if (stream.gcount()) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(stream.gcount()), 0) >= 0;
    }
    ok = ok && stream.eof();
    std::array<unsigned char, 32> digest{};
    ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
    return result;
}

void print_hook(void* object, const char* text) {
    // Observe the normal console stream without suppressing or changing it.
    if (occupied.load() && text) {
        try {
            std::lock_guard lock(output_mutex);
            const size_t available = output_limit - console_text.size();
            const size_t length = strnlen_s(text, output_limit + 1);
            console_text.append(text, (std::min)(length, available));
            output_truncated |= length > available;
        } catch (...) { disabled.store(true); }
    }
    original_print(object, text);
}

void absolute_jump(unsigned char* target, const void* destination) {
    // x64 RIP-indirect jump; the overwritten instructions live in the trampoline.
    target[0] = 0xff; target[1] = 0x25;
    std::memset(target + 2, 0, 4);
    std::memcpy(target + 6, &destination, 8);
}

bool install_print_observer(uintptr_t base) {
    auto* target = reinterpret_cast<unsigned char*>(base + print_rva);
    auto* relay = static_cast<unsigned char*>(VirtualAlloc(nullptr, displaced_size + 14,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!relay) return false;
    std::memcpy(relay, target, displaced_size);
    absolute_jump(relay + displaced_size, target + displaced_size);
    DWORD old = 0;
    if (!VirtualProtect(relay, displaced_size + 14, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(relay, 0, MEM_RELEASE); return false;
    }
    original_print = reinterpret_cast<Print>(relay);
    if (!VirtualProtect(target, displaced_size, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(relay, 0, MEM_RELEASE); original_print = nullptr; return false;
    }
    absolute_jump(target, reinterpret_cast<void*>(&print_hook));
    DWORD ignored = 0;
    VirtualProtect(target, displaced_size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), relay, displaced_size + 14);
    FlushInstructionCache(GetCurrentProcess(), target, displaced_size);
    return true;
}

bool original_entry(const fs::path& path, uintptr_t base, uintptr_t rva, size_t count) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = section[i];
        if (rva < s.VirtualAddress || rva + count > s.VirtualAddress + s.SizeOfRawData) continue;
        if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE)) return false;
        std::ifstream stream(path, std::ios::binary);
        stream.seekg(static_cast<std::streamoff>(s.PointerToRawData + rva - s.VirtualAddress));
        std::vector<char> original(count);
        stream.read(original.data(), static_cast<std::streamsize>(count));
        return stream.gcount() == static_cast<std::streamsize>(count) &&
            std::memcmp(original.data(), reinterpret_cast<const void*>(base + rva), count) == 0;
    }
    return false;
}

struct CommandTask : ITaskDelegate {
    std::string command;
    std::shared_ptr<std::atomic<bool>> dispatched;
    CommandTask(std::string value, std::shared_ptr<std::atomic<bool>> result) : command(std::move(value)), dispatched(std::move(result)) {}
    void Run() override {
        if (!disabled.load()) execute_command(command.c_str());
        dispatched->store(true);
    }
};

bool reparse(const fs::path& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void worker() {
    try {
        atomic_write(session_path / "session.json", "{\"protocol\":1,\"pid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"runtime\":\"1.11.240\",\"status\":\"waiting_for_game\"}\n");
        while (!ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        atomic_write(session_path / "session.json", "{\"protocol\":1,\"pid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"runtime\":\"1.11.240\",\"status\":\"ready\"}\n");
        // One command in flight; response history is owned and cleaned by the client.
        while (!disabled.load()) {
            for (const auto& entry : fs::directory_iterator(session_path)) {
                if (entry.path().extension() != ".request" || reparse(entry.path())) continue;
                const std::string id = entry.path().stem().string();
                if (!bridge::valid_id(id)) continue;
                const auto pending = session_path / (id + ".processing");
                if (!MoveFileExW(entry.path().c_str(), pending.c_str(), 0)) continue;
                std::ifstream stream(pending, std::ios::binary);
                std::array<char, 1025> bytes{};
                stream.read(bytes.data(), bytes.size());
                std::string command(bytes.data(), static_cast<size_t>(stream.gcount()));
                stream.close();
                if (!bridge::valid_command(command)) {
                    atomic_write(session_path / (id + ".response.json"), "{\"id\":" + bridge::quote(id) +
                        ",\"status\":\"rejected\",\"error\":\"expected one nonempty command, maximum 1023 bytes, no control characters\"}\n");
                    fs::remove(pending); continue;
                }
                { std::lock_guard lock(output_mutex); console_text.clear(); output_truncated = false; }
                occupied.store(true);
                // A pending game task owns its state even if dispatch times out.
                auto dispatched = std::make_shared<std::atomic<bool>>(false);
                tasks->AddTask(new CommandTask(command, dispatched));
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (!dispatched->load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const bool sent = dispatched->load();
                if (sent) {
                    // Console execution may itself enqueue work. This is an observation window, not a success assertion.
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                } else disabled.store(true);
                occupied.store(false);
                std::string observed; bool truncated;
                { std::lock_guard lock(output_mutex); observed = console_text; truncated = output_truncated; }
                atomic_write(session_path / (id + ".response.json"), "{\"id\":" + bridge::quote(id) +
                    ",\"status\":" + bridge::quote(sent ? "dispatched" : "dispatch_timeout") +
                    ",\"command\":" + bridge::quote(command) + ",\"console\":" + bridge::quote(observed) +
                    ",\"consoleTruncated\":" + (truncated ? "true" : "false") +
                    ",\"observationMs\":1000,\"commandSuccess\":null}\n");
                fs::remove(pending);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        atomic_write(session_path / "session.json", "{\"protocol\":1,\"status\":\"disabled_after_timeout\"}\n");
    } catch (...) { disabled.store(true); }
}

void message(F4SEMessagingInterface::Message* value) {
    if (value->type == F4SEMessagingInterface::kMessage_GameLoaded) ready.store(true);
}
}

extern "C" {
__declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version = {
    F4SEPluginVersionData::kVersion, 1, "F4SE Console Bridge", "F4SE Console Bridge contributors",
    0, 0, {runtime, 0}, 0, 0, 0, {0}
};

__declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface* f4se) {
    if (!f4se || f4se->isEditor || f4se->runtimeVersion != runtime) return false;
    try {
        wchar_t executable[32768]{};
        if (!GetModuleFileNameW(nullptr, executable, 32768) || sha256(executable) != exe_digest) return false;
        tasks = static_cast<F4SETaskInterface*>(f4se->QueryInterface(kInterface_Task));
        auto* messages = static_cast<F4SEMessagingInterface*>(f4se->QueryInterface(kInterface_Messaging));
        if (!tasks || tasks->interfaceVersion < 2 || !messages) return false;
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return false;
        fs::path root = fs::path(local) / "F4SEConsoleBridge";
        CoTaskMemFree(local);
        fs::create_directories(root);
        if (reparse(root)) return false;
        GUID guid{}; if (FAILED(CoCreateGuid(&guid))) return false;
        const auto* bytes = reinterpret_cast<const unsigned char*>(&guid);
        constexpr char hex[] = "0123456789abcdef";
        std::string token;
        for (size_t i = 0; i < sizeof(guid); ++i) { token += hex[bytes[i] >> 4]; token += hex[bytes[i] & 15]; }
        session_path = root / (std::to_string(GetCurrentProcessId()) + "-" + token);
        if (!fs::create_directory(session_path)) return false;
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!original_entry(executable, base, execute_rva, 32) ||
            !original_entry(executable, base, print_rva, displaced_size)) return false;
        execute_command = reinterpret_cast<Execute>(base + execute_rva);
        if (!messages->RegisterListener(f4se->GetPluginHandle(), "F4SE", message)) return false;
        // From here onward the DLL must remain loaded even if the mailbox cannot start.
        if (!install_print_observer(base)) { disabled.store(true); return true; }
        try { std::thread(worker).detach(); } catch (...) { disabled.store(true); }
        return true;
    } catch (...) { return false; }
}
}
