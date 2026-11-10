// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#include "session_files.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace loom::host {

namespace {

#ifdef _WIN32

/// The process user's SID, copied out of its token; empty when the token cannot be read.
std::vector<unsigned char> process_user_sid() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    DWORD size = 0;
    (void)::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> info(size);
    std::vector<unsigned char> sid;
    if (size != 0 && ::GetTokenInformation(token, TokenUser, info.data(), size, &size)) {
        const PSID user = reinterpret_cast<const TOKEN_USER*>(info.data())->User.Sid;
        if (::IsValidSid(user)) {
            const DWORD length = ::GetLengthSid(user);
            sid.resize(length);
            if (!::CopySid(length, sid.data(), user)) {
                sid.clear();
            }
        }
    }
    ::CloseHandle(token);
    return sid;
}

/// Create `tmp`, which must not exist, owned by the process user and readable and writable by
/// that user alone: a protected DACL with one entry, so nothing the directory grants is
/// inherited. The descriptor is the file's from its creation, before any byte is written.
bool write_owner_only(const std::filesystem::path& tmp, const std::string& text,
                      std::string* why) {
    std::vector<unsigned char> sid = process_user_sid();
    if (sid.empty()) {
        *why = "cannot read this process's user to make " + tmp.string() + " owner-only";
        return false;
    }
    const PSID user = static_cast<PSID>(sid.data());
    const DWORD acl_size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) -
                                              sizeof(DWORD) + ::GetLengthSid(user));
    std::vector<unsigned char> acl_bytes(acl_size);
    PACL acl = reinterpret_cast<PACL>(acl_bytes.data());
    SECURITY_DESCRIPTOR sd;
    if (!::InitializeAcl(acl, acl_size, ACL_REVISION) ||
        !::AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, user) ||
        !::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) ||
        !::SetSecurityDescriptorOwner(&sd, user, FALSE) ||
        !::SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE) ||
        !::SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
        *why = "cannot describe an owner-only " + tmp.string();
        return false;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    const HANDLE file = ::CreateFileW(tmp.wstring().c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *why = "cannot create " + tmp.string();
        return false;
    }
    std::size_t off = 0;
    while (off < text.size()) {
        const std::size_t left = text.size() - off;
        const DWORD chunk = left > 0x10000000U ? 0x10000000U : static_cast<DWORD>(left);
        DWORD written = 0;
        if (!::WriteFile(file, text.data() + off, chunk, &written, nullptr) || written == 0) {
            ::CloseHandle(file);
            *why = "cannot write " + tmp.string();
            return false;
        }
        off += written;
    }
    if (!::CloseHandle(file)) {
        *why = "cannot finish " + tmp.string();
        return false;
    }
    return true;
}

#else

/// Create `tmp`, which must not exist, with mode 0600 from its first byte. O_EXCL and
/// O_NOFOLLOW refuse a file or a link someone else put there.
bool write_owner_only(const std::filesystem::path& tmp, const std::string& text,
                      std::string* why) {
    const int fd = ::open(tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        *why = "cannot create " + tmp.string();
        return false;
    }
    std::size_t off = 0;
    while (off < text.size()) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n <= 0) {
            ::close(fd);
            *why = "cannot write " + tmp.string();
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) {
        *why = "cannot finish " + tmp.string();
        return false;
    }
    return true;
}

#endif

bool write_plain(const std::filesystem::path& tmp, const std::string& text, std::string* why) {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        *why = "cannot create " + tmp.string();
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (out.fail()) {
        *why = "cannot write " + tmp.string();
        return false;
    }
    return true;
}

} // namespace

#ifdef _WIN32

bool reads_as_owner_only(const std::filesystem::path& file, std::string* why) {
    std::wstring wfile = std::filesystem::absolute(file).wstring();
    wchar_t volume[MAX_PATH + 1] = {};
    DWORD flags = 0;
    if (::GetVolumePathNameW(wfile.c_str(), volume, MAX_PATH) &&
        ::GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) &&
        (flags & FILE_PERSISTENT_ACLS) == 0) {
        *why = "its file system keeps no permissions (FAT and exFAT keep none)";
        return false;
    }
    std::vector<unsigned char> sid = process_user_sid();
    if (sid.empty()) {
        *why = "this process's user cannot be read";
        return false;
    }
    const PSID user = static_cast<PSID>(sid.data());
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(wfile.c_str(), SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                                nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
        *why = "its security descriptor cannot be read back";
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    std::string fault;
    if (!::GetSecurityDescriptorControl(sd, &control, &revision)) {
        fault = "its security descriptor cannot be read back";
    } else if (owner == nullptr || !::EqualSid(owner, user)) {
        fault = "it reads back owned by another account";
    } else if (dacl == nullptr) {
        fault = "it reads back with no access list, which grants everyone";
    } else if ((control & SE_DACL_PROTECTED) == 0) {
        fault = "it reads back taking permissions from its directory";
    } else if (dacl->AceCount != 1) {
        fault = "it reads back with " + std::to_string(dacl->AceCount) + " access entries, not 1";
    } else {
        void* ace = nullptr;
        if (!::GetAce(dacl, 0, &ace) ||
            static_cast<const ACE_HEADER*>(ace)->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            !::EqualSid(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart, user)) {
            fault = "its one access entry is not this user's";
        }
    }
    ::LocalFree(sd);
    if (!fault.empty()) {
        *why = fault;
        return false;
    }
    return true;
}

namespace {
constexpr const char* kKeyBelongs = "serve from a directory on NTFS, such as one under your profile";
}

#else

bool reads_as_owner_only(const std::filesystem::path& file, std::string* why) {
    struct stat st {};
    if (::lstat(file.c_str(), &st) != 0) {
        *why = "it cannot be read back";
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        *why = "it reads back as something other than a plain file";
        return false;
    }
    if (st.st_uid != ::geteuid()) {
        *why = "it reads back owned by uid " + std::to_string(st.st_uid) + ", not " +
               std::to_string(::geteuid());
        return false;
    }
    const unsigned mode = static_cast<unsigned>(st.st_mode) & 07777U;
    if (mode != 0600U) {
        char octal[8] = {};
        std::snprintf(octal, sizeof octal, "%04o", mode);
        *why = std::string("it reads back as mode ") + octal + ", not 0600";
        return false;
    }
    return true;
}

namespace {
constexpr const char* kKeyBelongs =
    "serve from a directory on a file system that keeps POSIX permissions, such as one under "
    "your home directory";
}

#endif

bool write_session_text(const std::filesystem::path& path, const std::string& text,
                        bool owner_only, std::string* why) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    if (!(owner_only ? write_owner_only(tmp, text, why) : write_plain(tmp, text, why))) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    // One writer: the host holds the directory's session lock for its whole life. A rename on
    // one volume keeps the file's security descriptor.
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        *why = "cannot put " + path.string() + " in place: " + ec.message();
        return false;
    }
    // What was asked is not yet what holds: the key is read back where it now stands, and a
    // key anyone but this user could read is taken away again.
    std::string fault;
    if (owner_only && !reads_as_owner_only(path, &fault)) {
        std::filesystem::remove(path, ec);
        *why = "not serving " + path.parent_path().string() + ": " + path.filename().string() +
               " would not be this user's alone there, because " + fault + "; " + kKeyBelongs;
        return false;
    }
    return true;
}

} // namespace loom::host
