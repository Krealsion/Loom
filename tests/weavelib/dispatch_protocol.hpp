// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
#pragma once
#include <zen/weave.hpp>
namespace dispatch_test {
struct Command {
    std::string mode;
    std::int64_t target = 0;
    std::string role;
    std::string correlation = "0";
    ZEN_SHAPE(Command, 1, ZEN_FIELD(mode), ZEN_FIELD(target), ZEN_FIELD(role),
              ZEN_FIELD(correlation));
};
struct Payload {
    std::int64_t value = 0;
    ZEN_SHAPE(Payload, 1, ZEN_FIELD(value));
};
} // namespace dispatch_test
