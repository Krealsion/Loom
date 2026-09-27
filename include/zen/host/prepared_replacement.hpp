// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_PREPARED_REPLACEMENT_HPP
#define ZEN_HOST_PREPARED_REPLACEMENT_HPP

// Host wiring: one handle for a prepared replacement.
// PR-02; docs/reference/prepared-replacement.md#the-authoring-handle
//
// A handle that drives one prepared replacement: it resolves the role's holder into the
// incumbent, pairs a candidate it loads with `begin` (unloading it once if begin refuses),
// carries the TxnId, mints the lifecycle authority and spells the standard `zen.Activated`.
// Every operation delegates to one Switchboard or Kernel primitive: no second state machine,
// cached truth, pump, retry, thread or timeout. Every decision stays the author's: when to
// step, what to ask, Ready or Refused, whether and when to commit or abort, and when to pump.
//
// A host header: its constructor needs the `Switchboard&` and `commit()` mints the lifecycle
// authority, neither of which a weave holds. The handle is the transaction's proxy, never its
// owner: dropping it mid-flight does nothing. Move-only, since commit, abort and take_outcome
// happen once.

#include <zen/host/lifecycle_wiring.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/weave/lifecycle.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace loom {

class PreparedReplacement {
public:
    /// The dynamic start: resolve the incumbent from the role, load the candidate sealed through
    /// the Kernel, and begin, or leave the world as it was.
    struct Start {
        WeaveId operator_id{};
        WeaveId coordinator{};
        std::string role;
        std::string candidate_name;
        std::string candidate_path;
        std::uint32_t budget = 0;
    };

    /// The existing-candidate start: the caller holds a sealed candidate already, native or
    /// loaded its own way. The facade begins around it and cleans nothing up.
    struct StartExisting {
        WeaveId operator_id{};
        WeaveId coordinator{};
        std::string role;
        WeaveId candidate{};
        std::uint32_t budget = 0;
    };

    /// Where a start stopped. `None` means it did not stop.
    enum class StartStage : std::uint8_t {
        None = 0,
        AlreadyStarted,   ///< this handle is already bound to a transaction
        NoKernel,         ///< dynamic start on a handle constructed without a Kernel
        NoRoleHolder,     ///< nobody holds the role; checked BEFORE anything loads
        CandidateLoad,    ///< the artifact refused to load (see `error`)
        BeginTransaction, ///< the substrate refused (see `begin_reason`)
    };

    /// A start's result, keeping the underlying words: `begin_reason` is the substrate's refusal
    /// at BeginTransaction, `error` the loader's at CandidateLoad. `cleanup_failed` reports that a
    /// candidate the facade loaded could not be removed after a begin refusal, beside, never in
    /// place of, the original failure.
    struct StartResult {
        bool ok = false;
        StartStage stage = StartStage::None;
        TxnReason begin_reason = TxnReason::None;
        bool cleanup_failed = false;
        std::string error;

        explicit operator bool() const noexcept { return ok; }
    };

    /// A handle that can only wrap an existing sealed candidate; no Kernel is held.
    explicit PreparedReplacement(Switchboard& bus) noexcept : bus_(&bus) {}
    /// A handle that can also load a dynamic candidate through `kernel`.
    PreparedReplacement(Switchboard& bus, Kernel& kernel) noexcept
        : bus_(&bus), kernel_(&kernel) {}

    PreparedReplacement(const PreparedReplacement&) = delete;
    PreparedReplacement& operator=(const PreparedReplacement&) = delete;

    PreparedReplacement(PreparedReplacement&& other) noexcept
        : bus_(other.bus_), kernel_(other.kernel_), id_(other.id_),
          operator_id_(other.operator_id_), candidate_(other.candidate_),
          incumbent_(other.incumbent_), role_(std::move(other.role_)),
          candidate_name_(std::move(other.candidate_name_)) {
        other.unbind();
    }
    PreparedReplacement& operator=(PreparedReplacement&& other) noexcept {
        if (this != &other) {
            bus_ = other.bus_;
            kernel_ = other.kernel_;
            id_ = other.id_;
            operator_id_ = other.operator_id_;
            candidate_ = other.candidate_;
            incumbent_ = other.incumbent_;
            role_ = std::move(other.role_);
            candidate_name_ = std::move(other.candidate_name_);
            other.unbind();
        }
        return *this;
    }

    /// Does nothing: the transaction is the Switchboard's, and a scope ending is not a lifecycle
    /// decision. The kernel suite's destruction case pins it.
    ~PreparedReplacement() = default;

    // ---- starting ----------------------------------------------------------

    /// Resolve the incumbent from the role once, load the candidate sealed, and begin. The
    /// transaction binds the incumbent's exact life, and the role is never re-followed, so later
    /// drift is a transaction failure. A refusal at any stage leaves the world as it was: a
    /// candidate this call loaded is unloaded once if `begin` refuses (instance destroyed,
    /// library closed, name reusable), and the refusal's reason is kept.
    StartResult start(Start s) {
        if (id_.valid()) {
            return {false, StartStage::AlreadyStarted, TxnReason::None, false, {}};
        }
        if (kernel_ == nullptr) {
            return {false, StartStage::NoKernel, TxnReason::None, false,
                    "dynamic start needs the Kernel-taking constructor"};
        }
        // Resolved before anything loads, so an unheld role costs nothing.
        const WeaveId incumbent = bus_->role_holder(s.role);
        if (!incumbent.valid()) {
            return {false, StartStage::NoRoleHolder, TxnReason::None, false, {}};
        }
        const LoadResult loaded =
            kernel_->load_candidate(s.candidate_name, s.candidate_path, s.coordinator);
        if (!loaded.ok) {
            return {false, StartStage::CandidateLoad, TxnReason::None, false, loaded.error};
        }
        const TxnResult begun = bus_->begin_prepared_replacement(
            s.operator_id, s.coordinator, incumbent, loaded.id, s.role, s.budget);
        if (!begun.ok) {
            StartResult r{false, StartStage::BeginTransaction, begun.why, false, {}};
            // The one cleanup the facade owns, for a candidate it loaded; the begin refusal
            // stays the reported failure.
            if (!kernel_->unload(s.candidate_name)) {
                r.cleanup_failed = true;
                r.error = "facade-created candidate could not be removed";
            }
            return r;
        }
        bind(begun.id, s.operator_id, loaded.id, incumbent, std::move(s.role),
             std::move(s.candidate_name));
        return {true, StartStage::None, TxnReason::None, false, {}};
    }

    /// Begin around a candidate the caller brought, with no Kernel. On a begin refusal the
    /// candidate is left as it was; once the transaction exists, an abort discards the sealed
    /// candidate whoever loaded it.
    StartResult start_existing(StartExisting s) {
        if (id_.valid()) {
            return {false, StartStage::AlreadyStarted, TxnReason::None, false, {}};
        }
        const WeaveId incumbent = bus_->role_holder(s.role);
        if (!incumbent.valid()) {
            return {false, StartStage::NoRoleHolder, TxnReason::None, false, {}};
        }
        const TxnResult begun = bus_->begin_prepared_replacement(
            s.operator_id, s.coordinator, incumbent, s.candidate, s.role, s.budget);
        if (!begun.ok) {
            return {false, StartStage::BeginTransaction, begun.why, false, {}};
        }
        bind(begun.id, s.operator_id, s.candidate, incumbent, std::move(s.role),
             std::string{});
        return {true, StartStage::None, TxnReason::None, false, {}};
    }

    // ---- the transaction, without carrying its id --------------------------

    /// Spend one unit of the preparation budget: an explicit step, not a clock.
    TxnResult tick() { return bus_->tick_preparation(id_); }

    /// Open this transaction's one preparation conversation with `payload` as the ask. The
    /// payload need not carry the transaction id: the bus proves which conversation an answer
    /// belongs to.
    template <class T>
    TxnResult ask(const T& payload) {
        return ask(Message(to_value(payload)));
    }
    TxnResult ask(Message msg) {
        return bus_->ask_candidate_to_prepare(id_, std::move(msg));
    }

    /// Offer the delivery being handled to this transaction's readiness gate. The Switchboard
    /// judges: an authenticated answer from the exact candidate to this transaction's ask, or a
    /// refusal, exactly as the substrate refuses outside a delivery, from the wrong one, from the
    /// wrong coordinator or on the wrong handle.
    TxnResult offer_current_answer(PreparationAnswer answer) {
        return bus_->accept_preparation_answer(id_, answer);
    }

    /// Schedule the admission, with the standard activation. `ok` means scheduled, never
    /// committed: `state()` is `AdmissionPending` until the admission envelope dispatches, when
    /// the pumping the caller does ends it `Committed` or aborted; `take_outcome()` collects it.
    /// The caller supplies the activation sequence
    /// (docs/reference/known-seams.md#activation-sequence-ownership). It calls
    /// `commit_prepared_replacement(id, host_lifecycle_authority(bus),
    /// Message(to_value(Activated{sequence})), sequence)`.
    TxnResult commit(std::int64_t sequence) {
        return bus_->commit_prepared_replacement(
            id_, host_lifecycle_authority(*bus_),
            Message(to_value(Activated{sequence})), sequence);
    }

    /// Abort, as the operator this handle started with. Unloads, pumps, consumes and retries
    /// nothing; the substrate discards the sealed candidate.
    TxnResult abort() { return bus_->abort_prepared_replacement(id_, operator_id_); }

    /// The transaction's state, asked of the Switchboard each time (`transaction_state(id())`).
    /// Once the outcome has been collected, the substrate answers as for an unknown id.
    TxnState state() const { return bus_->transaction_state(id_); }

    /// Collect this transaction's terminal outcome: once, by the exact operator life that began
    /// it, and never a sibling transaction's. `TxnReason` arrives as the substrate recorded it.
    std::optional<TxnOutcome> take_outcome() {
        TxnOutcome out{};
        if (bus_->take_outcome(operator_id_, id_, out)) {
            return out;
        }
        return std::nullopt;
    }

    // ---- the useful facts --------------------------------------------------

    bool started() const noexcept { return id_.valid(); }
    /// For diagnostics, tests and unusual integration.
    TxnId id() const noexcept { return id_; }
    WeaveId candidate() const noexcept { return candidate_; }
    /// The incumbent as resolved at start: the participant the transaction bound, never
    /// re-read from the role table.
    WeaveId incumbent() const noexcept { return incumbent_; }
    const std::string& role() const noexcept { return role_; }
    /// Non-empty exactly when this handle loaded the candidate itself.
    const std::string& candidate_name() const noexcept { return candidate_name_; }

private:
    void bind(TxnId id, WeaveId op, WeaveId candidate, WeaveId incumbent, std::string role,
              std::string candidate_name) {
        id_ = id;
        operator_id_ = op;
        candidate_ = candidate;
        incumbent_ = incumbent;
        role_ = std::move(role);
        candidate_name_ = std::move(candidate_name);
    }
    void unbind() noexcept {
        id_ = TxnId{};
        operator_id_ = WeaveId{};
        candidate_ = WeaveId{};
        incumbent_ = WeaveId{};
        role_.clear();
        candidate_name_.clear();
    }

    Switchboard* bus_;
    Kernel* kernel_ = nullptr;
    TxnId id_{};
    WeaveId operator_id_{};
    WeaveId candidate_{};
    WeaveId incumbent_{};
    std::string role_;
    std::string candidate_name_;
};

} // namespace loom

#endif // ZEN_HOST_PREPARED_REPLACEMENT_HPP
