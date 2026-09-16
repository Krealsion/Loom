// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/content_id.hpp>

#include "detail/sha256.hpp"

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace loom {

namespace {

std::atomic<std::uint64_t> g_scans{0};

} // namespace

std::string file_content_id(const std::string& path) {
    g_scans.fetch_add(1, std::memory_order_relaxed);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("file_content_id: cannot open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    // WHOLE-FILE, not a prefix and not a stat. Two builds of one source differ deep
    // inside; an identity that read only a header would answer "same artifact" for
    // code that is not the code a person approved.
    return loom::detail::sha256_hex_prefix(ss.str(), 16);
}

std::string file_content_id_or_empty(const std::string& path) noexcept {
    try {
        return file_content_id(path);
    } catch (...) {
        return {};
    }
}

std::uint64_t file_content_id_scans() noexcept { return g_scans.load(std::memory_order_relaxed); }

// ---- BuildIdentity ------------------------------------------------------------------

struct BuildIdentity::Observation {
    std::string path;

    std::mutex m;
    bool done = false;      ///< under `m`; once true, the two strings below never change again
    std::string content_id;
    std::string failure;
};

BuildIdentity::BuildIdentity(std::string path) : observation_(std::make_shared<Observation>()) {
    observation_->path = std::move(path);
}

BuildIdentity BuildIdentity::known(std::string content_id) {
    BuildIdentity b;
    b.observation_ = std::make_shared<Observation>();
    b.observation_->done = true;
    if (content_id.empty()) {
        b.observation_->failure = "no build identity was supplied";
    } else {
        b.observation_->content_id = std::move(content_id);
    }
    return b;
}

const BuildIdentity::Observation& BuildIdentity::observe() const {
    Observation& o = *observation_;
    std::lock_guard<std::mutex> lock(o.m);
    if (!o.done) {
        // THE ONE READING. Everything after it — the other stage, a second ask, a copy —
        // answers from here.
        try {
            o.content_id = file_content_id(o.path);
        } catch (const std::exception&) {
            o.failure = "cannot read '" + o.path + "', so its build cannot be identified";
        }
        o.done = true;
    }
    return o;
}

namespace {

const std::string& no_identity() {
    static const std::string none;
    return none;
}

const std::string& nothing_named() {
    static const std::string why = "no artifact file was named, so there is no build to identify";
    return why;
}

} // namespace

const std::string& BuildIdentity::content_id() const {
    return observation_ ? observe().content_id : no_identity();
}

bool BuildIdentity::identified() const { return !content_id().empty(); }

const std::string& BuildIdentity::failure() const {
    return observation_ ? observe().failure : nothing_named();
}

} // namespace loom
