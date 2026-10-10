// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The Windows loader (src/kernel/windows_loader.cpp) through Kernel::load: where a weave's own
// libraries are looked for, and what a refusal says when something the weave needs is missing.
// Each case copies the fixtures into folders of its own, so where a library is found is decided by
// the case's arrangement alone; the fixtures' build folders are never loaded from.

#include "switchboard_fixtures.hpp"

#include <zen/kernel/kernel.hpp>

#include <doctest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace loom;

fs::path from_utf8(const std::string& s) {
    std::u8string u;
    for (const char c : s) {
        u.push_back(static_cast<char8_t>(static_cast<unsigned char>(c)));
    }
    return fs::path(u);
}

std::string to_utf8(const fs::path& p) {
    std::string s;
    for (const char8_t c : p.u8string()) {
        s.push_back(static_cast<char>(c));
    }
    return s;
}

/// A fresh, empty folder for one case, under the suite's own work folder.
fs::path fresh(const std::u8string& name) {
    const fs::path dir = from_utf8(ZEN_LOADER_WORK) / fs::path(name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

fs::path copy_into(const fs::path& dir, const char* built) {
    const fs::path from = from_utf8(built);
    const fs::path to = dir / from.filename();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    return to;
}

struct Outcome {
    bool ok = false;
    std::string error;
};

/// Loads the weave at `path` into a kernel of its own and, when it loaded, unloads it again. No
/// library of the case stays loaded: a later case's weave would be given it by name.
Outcome load_and_unload(const std::string& path) {
    Outcome out;
    {
        Switchboard sb;
        Kernel kernel(sb, sbfx::fixture_admission());
        const LoadResult r = kernel.load("loader", path, "loader");
        if (r.ok) {
            CHECK(kernel.unload("loader"));
        }
        out = {r.ok, r.error};
    }
    CHECK(::GetModuleHandleW(from_utf8(ZEN_LOADER_MIDDLE_NAME).c_str()) == nullptr);
    CHECK(::GetModuleHandleW(from_utf8(ZEN_LOADER_LEAF_NAME).c_str()) == nullptr);
    return out;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

/// PATH with `dir` in front of it, for the life of this object.
class PathFirst {
public:
    explicit PathFirst(const fs::path& dir) {
        const DWORD n = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
        had_ = n != 0;
        if (had_) {
            old_.assign(n, L'\0');
            ::GetEnvironmentVariableW(L"PATH", old_.data(), n);
            old_.resize(n - 1);
        }
        const std::wstring now = dir.wstring() + (had_ ? L";" + old_ : std::wstring{});
        ::SetEnvironmentVariableW(L"PATH", now.c_str());
    }
    ~PathFirst() { ::SetEnvironmentVariableW(L"PATH", had_ ? old_.c_str() : nullptr); }
    PathFirst(const PathFirst&) = delete;
    PathFirst& operator=(const PathFirst&) = delete;

private:
    bool had_ = false;
    std::wstring old_;
};

/// The current folder set to `dir`, for the life of this object.
class InFolder {
public:
    explicit InFolder(const fs::path& dir) : old_(fs::current_path()) { fs::current_path(dir); }
    ~InFolder() {
        std::error_code ec;
        fs::current_path(old_, ec);
    }
    InFolder(const InFolder&) = delete;
    InFolder& operator=(const InFolder&) = delete;

private:
    fs::path old_;
};

} // namespace

TEST_SUITE("windows_loader") {
    TEST_CASE("a weave's own libraries beside it are found") {
        const fs::path dir = fresh(u8"beside");
        const fs::path weave = copy_into(dir, ZEN_LOADER_WEAVE);
        copy_into(dir, ZEN_LOADER_MIDDLE);
        copy_into(dir, ZEN_LOADER_LEAF);
        const Outcome r = load_and_unload(to_utf8(weave));
        CHECK_MESSAGE(r.ok, r.error);
    }

    TEST_CASE("a library only on PATH is not looked for") {
        const fs::path weave = copy_into(fresh(u8"path-weave"), ZEN_LOADER_WEAVE);
        const fs::path libs = fresh(u8"path-libs");
        copy_into(libs, ZEN_LOADER_MIDDLE);
        copy_into(libs, ZEN_LOADER_LEAF);
        const PathFirst on_path(libs);
        const Outcome r = load_and_unload(to_utf8(weave));
        CHECK_FALSE(r.ok);
        CHECK_MESSAGE(has(r.error, ZEN_LOADER_MIDDLE_NAME), r.error);
    }

    TEST_CASE("a library only in the current folder is not looked for") {
        const fs::path weave = copy_into(fresh(u8"cwd-weave"), ZEN_LOADER_WEAVE);
        const fs::path libs = fresh(u8"cwd-libs");
        copy_into(libs, ZEN_LOADER_MIDDLE);
        copy_into(libs, ZEN_LOADER_LEAF);
        const InFolder in_libs(libs);
        const Outcome r = load_and_unload(to_utf8(weave));
        CHECK_FALSE(r.ok);
        CHECK_MESSAGE(has(r.error, ZEN_LOADER_MIDDLE_NAME), r.error);
    }

    TEST_CASE("a weave in a folder named in any script loads and unloads") {
        // e with diaeresis (in the Windows-1252 code page) and Cyrillic Zhe (in none of the
        // Western single-byte ones): the narrow path is UTF-8, as this program's code page is.
        const fs::path dir = fresh(u8"Zoë Ж");
        const fs::path weave = copy_into(dir, ZEN_LOADER_WEAVE);
        copy_into(dir, ZEN_LOADER_MIDDLE);
        copy_into(dir, ZEN_LOADER_LEAF);
        const Outcome r = load_and_unload(to_utf8(weave));
        CHECK_MESSAGE(r.ok, r.error);
    }

    TEST_CASE("a refusal names the library the weave needs and nothing has") {
        const fs::path weave = copy_into(fresh(u8"missing"), ZEN_LOADER_WEAVE);
        const Outcome r = load_and_unload(to_utf8(weave));
        CHECK_FALSE(r.ok);
        CHECK_MESSAGE(has(r.error, "open failed: "), r.error);
        CHECK_MESSAGE(has(r.error, to_utf8(weave)), r.error);
        CHECK_MESSAGE(has(r.error, ZEN_LOADER_MIDDLE_NAME), r.error);
        CHECK_MESSAGE(has(r.error, "not beside the weave"), r.error);
        CHECK_FALSE(has(r.error, "admission refused"));
        CHECK_FALSE(has(r.error, "\n"));
    }

    TEST_CASE("a refusal names a library's missing library through the library that needs it") {
        const fs::path dir = fresh(u8"missing-leaf");
        const fs::path weave = copy_into(dir, ZEN_LOADER_WEAVE);
        copy_into(dir, ZEN_LOADER_MIDDLE);
        const Outcome r = load_and_unload(to_utf8(weave));
        CHECK_FALSE(r.ok);
        const std::size_t middle = r.error.find(ZEN_LOADER_MIDDLE_NAME);
        const std::size_t leaf = r.error.find(ZEN_LOADER_LEAF_NAME);
        CHECK_MESSAGE(middle != std::string::npos, r.error);
        CHECK_MESSAGE(leaf != std::string::npos, r.error);
        CHECK_MESSAGE(middle < leaf, r.error);
    }

    TEST_CASE("a refusal of a file that is not a library says so") {
        const fs::path text = fresh(u8"not-a-library") / "text.dll";
        std::ofstream(text) << "not a library\n";
        const Outcome r = load_and_unload(to_utf8(text));
        CHECK_FALSE(r.ok);
        CHECK_MESSAGE(has(r.error, "not a Windows library"), r.error);
    }

    TEST_CASE("a refusal of a path with no file says so") {
        const fs::path nothing = fresh(u8"no-file") / "nothing.dll";
        const Outcome r = load_and_unload(to_utf8(nothing));
        CHECK_FALSE(r.ok);
        CHECK_MESSAGE(has(r.error, "no file at"), r.error);
        CHECK_MESSAGE(has(r.error, to_utf8(nothing)), r.error);
    }
}
