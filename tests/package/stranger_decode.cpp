// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE DECODE ALLOWANCE, READ AS A STRANGER: a consumer that sizes a structure of its own against
// the cells one decode may materialise, from the installed declaration alone, at compile time.
// It then sends a batch of exactly that size and one more through the bytes, so the installed
// value is the one admit() enforces, not merely a number that compiles.

#include <zen/serialize.hpp>
#include <zen/weave/shape.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Batch {
    std::vector<std::int64_t> items;
    ZEN_SHAPE(Batch, 1, ZEN_FIELD(items));
};

// A Batch costs one cell for its one declared field and one per item, so the most items one
// decode can carry is the allowance less that one: a bound the consumer derives, not copies.
constexpr std::uint64_t kMostItems = loom::kMaxDecodedCells - 1;
static_assert(kMostItems > 0 && kMostItems < loom::kMaxDecodedCells,
              "the consumer's batch bound is derived from the installed allowance");

loom::Admission round_trip(std::uint64_t n) {
    Batch b;
    b.items.assign(static_cast<std::size_t>(n), 0);
    return loom::admit(loom::parse(loom::serialize(loom::to_value(b))), loom::schema_of<Batch>());
}

} // namespace

int main() {
    std::printf("package witness: decode allowance %llu cells, batch bound %llu items\n",
                static_cast<unsigned long long>(loom::kMaxDecodedCells),
                static_cast<unsigned long long>(kMostItems));
    int failures = 0;
    const loom::Admission at = round_trip(kMostItems);
    if (!at.ok()) {
        std::fprintf(stderr, "  FAIL: a batch at the bound is refused: %s\n",
                     at.first_error().message().c_str());
        ++failures;
    }
    const loom::Admission over = round_trip(kMostItems + 1);
    if (over.ok() || over.first_error().kind != loom::ErrorKind::MalformedBytes) {
        std::fprintf(stderr, "  FAIL: a batch one item past the bound is not refused as "
                             "MalformedBytes\n");
        ++failures;
    }
    if (failures != 0) {
        std::fprintf(stderr, "package witness: decode allowance FAILED (%d)\n", failures);
        return 1;
    }
    std::puts("package witness: decode allowance PASSED");
    return 0;
}
