// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "process.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <dlfcn.h>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace loom::runs {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string error_text(DWORD code) {
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string out;
    if (n != 0 && msg != nullptr) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, msg, static_cast<int>(n), nullptr, 0,
                                            nullptr, nullptr);
        out.assign(static_cast<std::size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, msg, static_cast<int>(n), out.data(), len, nullptr,
                            nullptr);
        LocalFree(msg);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return out + " (error " + std::to_string(code) + ")";
}

/// One argument, quoted so CommandLineToArgvW (and every C runtime) reads it back unchanged.
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return a;
    }
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

bool same_name(const std::wstring& entry, const std::wstring& name) {
    const std::size_t eq = entry.find(L'=', 1); // "=C:=C:\..." entries start with '='
    if (eq == std::wstring::npos || eq != name.size()) {
        return false;
    }
    return CompareStringOrdinal(entry.c_str(), static_cast<int>(eq), name.c_str(),
                                static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
}

/// The host's environment with `overrides` applied, as the sorted block CreateProcessW wants.
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::wstring> entries;
    wchar_t* block = GetEnvironmentStringsW();
    if (block != nullptr) {
        for (const wchar_t* p = block; *p != L'\0'; p += wcslen(p) + 1) {
            entries.emplace_back(p);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : overrides) {
        const std::wstring wname = widen(name);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const std::wstring& e) { return same_name(e, wname); }),
                      entries.end());
        entries.push_back(wname + L"=" + widen(value));
    }
    std::stable_sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring out;
    for (const std::wstring& e : entries) {
        out += e;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

} // namespace

bool ChildProcess::spawn(const SpawnSpec& spec, std::string* why) {
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE out = CreateFileW(widen(spec.output).c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        *why = "cannot create " + spec.output + ": " + error_text(GetLastError());
        return false;
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        CloseHandle(out);
        *why = "cannot open the null device: " + error_text(GetLastError());
        return false;
    }
    // EXACTLY THESE TWO HANDLES ARE INHERITED, whatever else in this process is inheritable.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    HANDLE handles[2] = {out, nul};
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   sizeof(handles), nullptr, nullptr)) {
        const DWORD e = GetLastError();
        CloseHandle(out);
        CloseHandle(nul);
        *why = "cannot restrict what the worker inherits: " + error_text(e);
        return false;
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = out;
    si.lpAttributeList = attributes;

    std::wstring command = quote(widen(spec.program));
    for (const std::string& a : spec.args) {
        command += L' ';
        command += quote(widen(a));
    }
    std::wstring env = environment_block(spec.env);
    const std::wstring cwd = widen(spec.cwd);

    // A JOB, SO THE WORKER'S WHOLE TREE ENDS TOGETHER -- on `terminate`, and when this process
    // (the host) ends and the job's last handle closes.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        (void)SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                      sizeof(limits));
    }
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED |
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP,
        env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    const DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(out);
    CloseHandle(nul);
    if (!ok) {
        if (job != nullptr) {
            CloseHandle(job);
        }
        *why = "cannot start " + spec.program + ": " + error_text(create_error);
        return false;
    }
    if (job != nullptr && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job); // it still runs; `terminate` then ends only the worker itself
        job = nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    job_ = job;
    pid_ = static_cast<std::int64_t>(pi.dwProcessId);
    return true;
}

bool ChildProcess::ended() {
    if (ended_) {
        return true;
    }
    if (process_ == nullptr) {
        return false;
    }
    if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    exit_code_ = static_cast<int>(code);
    ended_ = true;
    return true;
}

bool ChildProcess::terminate() {
    if (process_ == nullptr || ended()) {
        return false;
    }
    if (job_ != nullptr) {
        return TerminateJobObject(job_, 1) != 0;
    }
    return TerminateProcess(process_, 1) != 0;
}

void ChildProcess::release() noexcept {
    if (process_ != nullptr) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (job_ != nullptr) {
        CloseHandle(job_); // KILL_ON_JOB_CLOSE: a worker does not outlive the record that owns it
        job_ = nullptr;
    }
    pid_ = 0;
}

ChildProcess::~ChildProcess() { release(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(other.pid_), exit_code_(other.exit_code_), ended_(other.ended_),
      process_(other.process_), job_(other.job_) {
    other.pid_ = 0;
    other.process_ = nullptr;
    other.job_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        release();
        pid_ = other.pid_;
        exit_code_ = other.exit_code_;
        ended_ = other.ended_;
        process_ = other.process_;
        job_ = other.job_;
        other.pid_ = 0;
        other.process_ = nullptr;
        other.job_ = nullptr;
    }
    return *this;
}

std::string ChildProcess::find_on_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return {};
    }
    std::string all(path);
    std::size_t start = 0;
    while (start <= all.size()) {
        const std::size_t end = all.find(';', start);
        const std::string dir = all.substr(start, end == std::string::npos ? std::string::npos
                                                                           : end - start);
        if (!dir.empty()) {
            for (const char* ext : {"", ".exe"}) {
                const std::filesystem::path candidate = std::filesystem::path(dir) / (name + ext);
                std::error_code ec;
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    return candidate.string();
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return {};
}

#else // POSIX

bool ChildProcess::spawn(const SpawnSpec& spec, std::string* why) {
    // Everything the child needs is built BEFORE fork: after it, only async-signal-safe calls.
    std::vector<std::string> argv_store;
    argv_store.push_back(spec.program);
    for (const std::string& a : spec.args) {
        argv_store.push_back(a);
    }
    std::vector<char*> argv;
    for (std::string& a : argv_store) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_store;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string entry(*e);
        const std::size_t eq = entry.find('=');
        const std::string name = entry.substr(0, eq);
        bool overridden = false;
        for (const auto& o : spec.env) {
            overridden = overridden || o.first == name;
        }
        if (!overridden) {
            env_store.push_back(entry);
        }
    }
    for (const auto& [name, value] : spec.env) {
        env_store.push_back(name + "=" + value);
    }
    std::vector<char*> envp;
    for (std::string& e : env_store) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    std::string program = spec.program;
    if (program.find('/') == std::string::npos) {
        program = find_on_path(program);
        if (program.empty()) {
            *why = "cannot find " + spec.program + " on PATH";
            return false;
        }
    }
    const int out = ::open(spec.output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) {
        *why = "cannot create " + spec.output + ": " + std::strerror(errno);
        return false;
    }
    const int nul = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (nul < 0) {
        ::close(out);
        *why = std::string("cannot open /dev/null: ") + std::strerror(errno);
        return false;
    }
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) {
        max_fd = 65536;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out);
        ::close(nul);
        *why = std::string("cannot fork: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        // THE CHILD: its own process group (so `terminate` reaches everything it starts), the
        // three standard descriptors and nothing else, the run's directory, then the program.
        (void)::setpgid(0, 0);
        (void)::dup2(nul, 0);
        (void)::dup2(out, 1);
        (void)::dup2(out, 2);
        for (long fd = 3; fd < max_fd; ++fd) {
            (void)::close(static_cast<int>(fd));
        }
        if (!spec.cwd.empty() && ::chdir(spec.cwd.c_str()) != 0) {
            ::_exit(126);
        }
        ::execve(program.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    (void)::setpgid(pid, pid); // the same, from this side, so it holds before either runs on
    ::close(out);
    ::close(nul);
    pid_ = static_cast<std::int64_t>(pid);
    return true;
}

bool ChildProcess::ended() {
    if (ended_) {
        return true;
    }
    if (pid_ == 0) {
        return false;
    }
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (r == 0) {
        return false;
    }
    if (r < 0) {
        // Somebody else reaped it, or it is not ours: it is over, and its code is unknown.
        exit_code_ = -1;
        ended_ = true;
        return true;
    }
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status)
                                   : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    ended_ = true;
    return true;
}

bool ChildProcess::terminate() {
    if (pid_ == 0 || ended()) {
        return false;
    }
    return ::kill(-static_cast<pid_t>(pid_), SIGKILL) == 0 ||
           ::kill(static_cast<pid_t>(pid_), SIGKILL) == 0;
}

void ChildProcess::release() noexcept {
    // A worker does not outlive the record that owns it: end it and reap it, so no zombie stays
    // behind in the host's process table.
    if (pid_ != 0 && !ended()) {
        (void)terminate();
        int status = 0;
        (void)::waitpid(static_cast<pid_t>(pid_), &status, 0);
    }
    pid_ = 0;
}

ChildProcess::~ChildProcess() { release(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(other.pid_), exit_code_(other.exit_code_), ended_(other.ended_) {
    other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        release();
        pid_ = other.pid_;
        exit_code_ = other.exit_code_;
        ended_ = other.ended_;
        other.pid_ = 0;
    }
    return *this;
}

std::string ChildProcess::find_on_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return {};
    }
    std::string all(path);
    std::size_t start = 0;
    while (start <= all.size()) {
        const std::size_t end = all.find(':', start);
        const std::string dir = all.substr(start, end == std::string::npos ? std::string::npos
                                                                           : end - start);
        if (!dir.empty()) {
            const std::string candidate = dir + "/" + name;
            if (::access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return {};
}

#endif

std::string image_directory_of(const void* address) {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module)) {
        return {};
    }
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0) {
        return {};
    }
    path.resize(n);
    return std::filesystem::path(path).parent_path().string();
#else
    Dl_info info{};
    if (dladdr(address, &info) == 0 || info.dli_fname == nullptr) {
        return {};
    }
    std::error_code ec;
    return std::filesystem::weakly_canonical(std::filesystem::path(info.dli_fname), ec)
        .parent_path()
        .string();
#endif
}

} // namespace loom::runs
