// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The Windows kernel's loader: how a weave is opened, and what a refusal says. The explanation is
// worked out only after Windows has refused the load, from the files that load looked at; nothing
// is read before a load, and nothing after one that succeeds.

#include "windows_loader.hpp"

#if defined(_WIN32)

#include <windows.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <set>
#include <string>
#include <vector>

namespace loom::detail {

namespace {

// The weave's own folder for the libraries it needs, then the program's folder, the system folder
// and any folder the host added. Never the current folder, never PATH.
constexpr DWORD kSearch = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;

// How far an explanation follows a library's own libraries, and how many it reads in all.
constexpr std::size_t kMaxDepth = 8;
constexpr std::size_t kMaxImages = 64;
constexpr std::size_t kMaxNamed = 4;

/// `narrow` in UTF-16, read in the program's code page; false when it is not text there.
bool widen(const std::string& narrow, std::wstring& wide) {
    if (narrow.empty() || narrow.size() > static_cast<std::size_t>(INT_MAX) ||
        narrow.find('\0') != std::string::npos) {
        return false;
    }
    const int bytes = static_cast<int>(narrow.size());
    const int n =
        ::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, narrow.data(), bytes, nullptr, 0);
    if (n <= 0) {
        return false;
    }
    wide.assign(static_cast<std::size_t>(n), L'\0');
    return ::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, narrow.data(), bytes, wide.data(),
                                 n) == n;
}

/// UTF-8, for the words: an unpaired surrogate becomes U+FFFD, so the words are always text.
std::string utf8(const std::wstring& wide) {
    if (wide.empty() || wide.size() > static_cast<std::size_t>(INT_MAX)) {
        return {};
    }
    const int chars = static_cast<int>(wide.size());
    const int n =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), chars, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), chars, out.data(), n, nullptr, nullptr);
    return out;
}

/// A name read from a file, kept to printable ASCII so the words stay one line of text.
std::string printable(const std::string& s) {
    std::string out;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u >= 0x20 && u < 0x7f ? c : '?');
    }
    return out;
}

std::wstring lowered(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
    }
    return s;
}

std::wstring full_path(const std::wstring& path) {
    DWORD size = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    while (size != 0) {
        std::wstring out(size, L'\0');
        const DWORD n = ::GetFullPathNameW(path.c_str(), size, out.data(), nullptr);
        if (n == 0) {
            return {};
        }
        if (n < size) {
            out.resize(n);
            return out;
        }
        size = n; // the current folder changed between the two calls
    }
    return {};
}

std::wstring folder_of(const std::wstring& path) {
    const std::size_t at = path.find_last_of(L"\\/");
    return at == std::wstring::npos ? std::wstring{} : path.substr(0, at);
}

bool is_file(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring program_folder() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            return folder_of(buf);
        }
        if (buf.size() >= 32768) {
            return {};
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring system_folder() {
    wchar_t buf[MAX_PATH + 1] = {};
    const UINT n = ::GetSystemDirectoryW(buf, MAX_PATH + 1);
    return (n == 0 || n > MAX_PATH) ? std::wstring{} : std::wstring(buf, n);
}

/// Windows' own words for `code`, on one line and without the final full stop.
std::string system_words(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring w;
    if (n != 0 && text != nullptr) {
        w.assign(text, n);
    }
    if (text != nullptr) {
        ::LocalFree(text);
    }
    for (wchar_t& c : w) {
        if (c == L'\r' || c == L'\n' || c == L'\t') {
            c = L' ';
        }
    }
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) {
        w.pop_back();
    }
    return utf8(w);
}

/// The libraries an image imports when it is loaded, from its import directory. Delay-loaded
/// imports wait for their first call and are not among them. The file is mapped as an image in
/// which no code runs and nothing is imported; false when it cannot be mapped so.
bool imports_of(const std::wstring& path, std::vector<std::string>& names) {
    const HMODULE mapped = ::LoadLibraryExW(
        path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (mapped == nullptr) {
        return false;
    }
    // A data mapping's handle carries its kind in its low bits; the image begins at the rest.
    const auto* base = reinterpret_cast<const unsigned char*>(
        reinterpret_cast<std::uintptr_t>(mapped) & ~static_cast<std::uintptr_t>(3));
    bool read = false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE &&
            nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR_MAGIC) {
            read = true;
            const DWORD size = nt->OptionalHeader.SizeOfImage;
            const IMAGE_DATA_DIRECTORY& dir =
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            constexpr auto step = static_cast<DWORD>(sizeof(IMAGE_IMPORT_DESCRIPTOR));
            for (DWORD at = dir.VirtualAddress;
                 dir.VirtualAddress != 0 && size >= step && at <= size - step && names.size() < 512;
                 at += step) {
                const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + at);
                if (d->Name == 0 || d->Name >= size) {
                    break;
                }
                const char* s = reinterpret_cast<const char*>(base + d->Name);
                const std::size_t room = size - d->Name;
                names.emplace_back(s, ::strnlen(s, room < 260 ? room : 260));
            }
        }
    }
    ::FreeLibrary(mapped);
    return read;
}

struct Places {
    std::wstring weave;   // the folder of the weave the load was asked for
    std::wstring program; // the folder of the program loading it
    std::wstring system;  // the system folder
};

enum class Found { Nowhere, Present, Beside, Program };

/// Where the load finds `name`: already loaded or a system API set (Present), beside the weave,
/// in the program's folder, in the system folder (Present), or nowhere it looks.
Found find(const std::string& name, const Places& at, std::wstring& where) {
    std::wstring w;
    if (!widen(name, w) || w.find_first_of(L"\\/") != std::wstring::npos) {
        return Found::Nowhere;
    }
    const std::wstring low = lowered(w);
    if (low.rfind(L"api-ms-", 0) == 0 || low.rfind(L"ext-ms-", 0) == 0 ||
        ::GetModuleHandleW(w.c_str()) != nullptr) {
        return Found::Present;
    }
    if (!at.weave.empty() && is_file(at.weave + L"\\" + w)) {
        where = at.weave + L"\\" + w;
        return Found::Beside;
    }
    if (!at.program.empty() && is_file(at.program + L"\\" + w)) {
        where = at.program + L"\\" + w;
        return Found::Program;
    }
    if (!at.system.empty() && is_file(at.system + L"\\" + w)) {
        return Found::Present;
    }
    return Found::Nowhere;
}

struct Missing {
    std::vector<std::string> through; // the libraries, each found, that lead to the missing ones
    std::vector<std::string> names;   // what the last of them (or the weave) needs and nothing has
};

/// The first library `image` needs, or one of its found libraries needs, that the load found
/// nowhere; false when every one was found, or the files could not be read.
bool missing_under(const std::wstring& image, const Places& at, std::vector<std::string>& through,
                   std::set<std::wstring>& seen, Missing& out) {
    if (through.size() >= kMaxDepth || seen.size() >= kMaxImages) {
        return false;
    }
    std::vector<std::string> names;
    if (!imports_of(image, names)) {
        return false;
    }
    std::vector<std::string> nowhere;
    std::vector<std::pair<std::string, std::wstring>> found_here;
    for (const std::string& name : names) {
        std::wstring where;
        const Found f = find(name, at, where);
        if (f == Found::Nowhere) {
            nowhere.push_back(name);
        } else if (f == Found::Beside || f == Found::Program) {
            found_here.emplace_back(name, where);
        }
    }
    if (!nowhere.empty()) {
        out.through = through;
        out.names = nowhere;
        return true;
    }
    for (const auto& [name, where] : found_here) {
        if (!seen.insert(lowered(where)).second) {
            continue;
        }
        through.push_back(name);
        if (missing_under(where, at, through, seen, out)) {
            return true;
        }
        through.pop_back();
    }
    return false;
}

std::string listed(const std::vector<std::string>& names) {
    std::string out;
    const std::size_t shown = names.size() < kMaxNamed ? names.size() : kMaxNamed;
    for (std::size_t i = 0; i < shown; ++i) {
        if (i > 0) {
            out += (i + 1 == shown && shown == names.size()) ? " and " : ", ";
        }
        out += printable(names[i]);
    }
    if (shown < names.size()) {
        out += " and " + std::to_string(names.size() - shown) + " more";
    }
    return out;
}

const char* machine_name(WORD machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_AMD64: return "x64";
    case IMAGE_FILE_MACHINE_I386: return "32-bit x86";
    case IMAGE_FILE_MACHINE_ARM64: return "ARM64";
    case IMAGE_FILE_MACHINE_ARMNT: return "32-bit ARM";
    default: return nullptr;
    }
}

#if defined(_M_ARM64) || defined(__aarch64__)
constexpr WORD kThisMachine = IMAGE_FILE_MACHINE_ARM64;
#elif defined(_M_X64) || defined(__x86_64__)
constexpr WORD kThisMachine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr WORD kThisMachine = IMAGE_FILE_MACHINE_I386;
#endif

/// Why Windows could not take the file as a library of this program: not a PE image at all, or
/// one built for another machine. Empty when the file is a library for this machine.
std::string not_a_library(const std::wstring& full, const std::string& shown) {
    const HANDLE f = ::CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return {};
    }
    unsigned char head[4096] = {};
    DWORD got = 0;
    const BOOL ok = ::ReadFile(f, head, sizeof head, &got, nullptr);
    ::CloseHandle(f);
    if (!ok) {
        return {};
    }
    const std::string none = "'" + shown + "' is not a Windows library";
    if (got < sizeof(IMAGE_DOS_HEADER) || head[0] != 'M' || head[1] != 'Z') {
        return none;
    }
    LONG lfanew = 0;
    std::memcpy(&lfanew, head + offsetof(IMAGE_DOS_HEADER, e_lfanew), sizeof lfanew);
    if (lfanew <= 0 || static_cast<DWORD>(lfanew) > got - 6 ||
        std::memcmp(head + lfanew, "PE\0\0", 4) != 0) {
        return none;
    }
    WORD machine = 0;
    std::memcpy(&machine, head + lfanew + 4, sizeof machine);
    if (machine == kThisMachine) {
        return {};
    }
    const char* theirs = machine_name(machine);
    const std::string other = theirs != nullptr ? theirs : "another machine";
    return "'" + shown + "' is a library for " + other + ", and this program is " +
           machine_name(kThisMachine);
}

std::string refusal(const std::wstring& given, const std::wstring& full, DWORD code) {
    const std::string shown = utf8(given);
    const std::string number = "Windows error " + std::to_string(code);
    const DWORD attributes = ::GetFileAttributesW(full.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD why = ::GetLastError();
        if (why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND) {
            return "no file at '" + shown + "' (" + number + ")";
        }
        return "'" + shown + "' could not be loaded (" + number + ": " + system_words(code) + ")";
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return "'" + shown + "' is a folder, not a library (" + number + ")";
    }
    if (code == ERROR_MOD_NOT_FOUND) {
        const Places at{folder_of(full), program_folder(), system_folder()};
        std::vector<std::string> through;
        std::set<std::wstring> seen{lowered(full)};
        Missing missing;
        if (missing_under(full, at, through, seen, missing)) {
            std::string words = "'" + shown + "' needs ";
            for (const std::string& name : missing.through) {
                words += printable(name) + ", which needs ";
            }
            words += listed(missing.names);
            words += missing.names.size() == 1 ? ", which is not" : ", none of which is";
            return words + " beside the weave, in this program's folder or in the system folder (" +
                   number + ")";
        }
        return "'" + shown + "' or a library it needs could not be found (" + number + ": " +
               system_words(code) + ")";
    }
    if (code == ERROR_BAD_EXE_FORMAT) {
        const std::string why = not_a_library(full, shown);
        if (!why.empty()) {
            return why + " (" + number + ")";
        }
    }
    return "'" + shown + "' could not be loaded (" + number + ": " + system_words(code) + ")";
}

} // namespace

void* windows_open_library(const std::string& path, std::string& error) {
    if (path.empty()) {
        error = "no path was given";
        return nullptr;
    }
    std::wstring given;
    if (!widen(path, given)) {
        error = "'" + printable(path) + "' is not text in this program's code page (" +
                std::to_string(::GetACP()) + "), so it names no file";
        return nullptr;
    }
    const std::wstring full = full_path(given);
    if (full.empty()) {
        const DWORD code = ::GetLastError();
        error = "'" + utf8(given) + "' is not a path Windows can complete (Windows error " +
                std::to_string(code) + ")";
        return nullptr;
    }
    const HMODULE module = ::LoadLibraryExW(full.c_str(), nullptr, kSearch);
    if (module == nullptr) {
        const DWORD code = ::GetLastError();
        try {
            error = refusal(given, full, code);
        } catch (...) {
            error = "'" + utf8(given) + "' could not be loaded (Windows error " +
                    std::to_string(code) + ")";
        }
    }
    return static_cast<void*>(module);
}

} // namespace loom::detail

#endif
