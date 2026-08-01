#ifndef ZEN_HOST_PREPARED_REPLACEMENT_HPP
#define ZEN_HOST_PREPARED_REPLACEMENT_HPP

// HOST WIRING — one good handle, attached to the real machine (R2B-4a).
//
// The prepared-replacement substrate is complete and its laws are accepted:
// a sealed candidate, one bounded transaction, one authenticated preparation
// conversation, an admission that IS the activation, and exactly one terminal
// outcome for the exact operator that began it. What it did not have was a
// pleasant way to be driven. A normal host had to sequence eight primitives,
// carry a TxnId into its coordinator's handlers, remember which operator owns
// the outcome, and hand-build the lifecycle authority and the standard
// `zen.Activated` message on every commit.
//
// This class is that sequencing, written once. It is SUGAR IN THE STRICT SENSE:
//
//   every semantic operation visibly delegates to one accepted Switchboard or
//   Kernel primitive — no duplicated validation, no second state machine, no
//   readiness authority, no cached truth, no hidden pump, no hidden retry, no
//   thread, no timeout, and NO lifecycle decision the caller did not make.
//
// What it removes is plumbing: resolving the role's current holder into the
// incumbent id, pairing a freshly loaded candidate with `begin` (and unloading
// it exactly once if begin refuses), carrying the TxnId, minting the host
// authority, spelling the standard activation, and the out-parameter ceremony
// around the terminal outcome.
//
// What it deliberately KEEPS in the author's hands, because they are decisions
// and not mechanics: when to spend a preparation step, what to ask, whether an
// answer is offered as Ready or Refused, whether and when to COMMIT, whether to
// abort, and when the bus is pumped. A candidate becoming Ready commits
// nothing; a handle going out of scope changes nothing.
//
// WHY THIS IS A HOST HEADER. `commit()` mints this Loom's lifecycle authority,
// and the constructor requires the `Switchboard&` itself — the same boundary
// `host_lifecycle_authority` already draws: a weave holds a `Bus&`/`Mail&` and
// can never construct one of these. A dynamic candidate participates fully in
// its preparation; it does not thereby gain the right to manage the host
// Loom's replacement transactions.
//
// THE HANDLE IS THE TRANSACTION'S PROXY, NEVER ITS OWNER. The transaction
// lives in the Switchboard; dropping the handle mid-flight performs no
// topology mutation, no transaction command, and pumps nothing — Zen does not
// make lifecycle decisions because a C++ scope happened to end. The one thing
// the handle does own is the START WINDOW of a candidate it loaded itself:
// if `begin` then refuses, the facade unloads that candidate exactly once
// (the caller never asked to manage an artifact separately). A candidate the
// CALLER brought (`start_existing`) is never destroyed by a failed start —
// and once a transaction exists, cleanup belongs to substrate law either way
// (aborting discards a sealed candidate; the adapter's destructor releases
// the artifact).
//
// Move-only, deliberately: a copied handle would be two objects that appear to
// own one set of one-shot operations (commit, abort, take_outcome).

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
    /// The dynamic start: the facade resolves the incumbent from the role,
    /// loads the candidate SEALED through the Kernel, and begins the
    /// transaction — or leaves the world exactly as it found it.
    struct Start {
        WeaveId operator_id{};
        WeaveId coordinator{};
        std::string role;
        std::string candidate_name;
        std::string candidate_path;
        std::uint32_t budget = 0;
    };

    /// The existing-candidate start: the caller already holds a sealed
    /// candidate (native or loaded by its own arrangement). The facade begins
    /// the transaction around it and owns NO artifact cleanup — the caller
    /// brought the candidate, the caller keeps it if begin refuses.
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

    /// A start result that keeps the underlying words. `begin_reason` is the
    /// substrate's own refusal when the stage is BeginTransaction; `error` is
    /// the loader's own words when the stage is CandidateLoad. `cleanup_failed`
    /// reports the facade-created candidate could not be removed after a begin
    /// refusal — BOTH facts are reported, and the original failure is never
    /// replaced by a cleanup story.
    struct StartResult {
        bool ok = false;
        StartStage stage = StartStage::None;
        TxnReason begin_reason = TxnReason::None;
        bool cleanup_failed = false;
        std::string error;

        explicit operator bool() const noexcept { return ok; }
    };

    /// A handle that can only wrap an EXISTING sealed candidate — nothing to
    /// load, so no Kernel is required or held.
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

    /// DELIBERATELY NOTHING. No abort, no unload, no pump: the transaction is
    /// the Switchboard's, and a scope ending is not a lifecycle decision. The
    /// destruction proof in the kernel suite exists to keep future "helpful
    /// RAII" from changing this.
    ~PreparedReplacement() = default;

    // ---- starting ----------------------------------------------------------

    /// Resolve the incumbent from the role ONCE, load the candidate sealed,
    /// begin the transaction. The incumbent is resolved here and bound by the
    /// transaction as an exact life — the facade never re-follows the role, so
    /// later drift remains a transaction failure, exactly as the substrate says.
    ///
    /// ATOMIC ON FAILURE: a refusal at any stage leaves the world as it was.
    /// In particular, a candidate this call loaded is unloaded exactly once if
    /// `begin` refuses — instance destroyed, library closed, name reusable —
    /// and the begin refusal's own reason is preserved.
    StartResult start(Start s) {
        if (id_.valid()) {
            return {false, StartStage::AlreadyStarted, TxnReason::None, false, {}};
        }
        if (kernel_ == nullptr) {
            return {false, StartStage::NoKernel, TxnReason::None, false,
                    "dynamic start needs the Kernel-taking constructor"};
        }
        // The role is resolved BEFORE anything loads, so "nobody holds the
        // role" costs nothing and leaks nothing.
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
            // The one cleanup the facade owns: it loaded this candidate, and the
            // caller never asked to manage an artifact separately. Exactly once,
            // and the begin refusal above stays the headline.
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

    /// Begin around a candidate the CALLER brought. No Kernel involved, and on
    /// a begin refusal the candidate is left exactly as it was — the facade
    /// destroys nothing it did not create. (Once the transaction exists, the
    /// substrate's own law applies as always: an abort discards the sealed
    /// candidate, whoever loaded it.)
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

    /// Spend exactly one unit of the preparation budget — the deterministic
    /// step, not a clock. `PreparationExhausted` remains visible.
    TxnResult tick() { return bus_->tick_preparation(id_); }

    /// Open this transaction's ONE preparation conversation with `payload` as
    /// the ask. Domain payloads need not carry the transaction id: the handle
    /// knows its transaction, and the BUS — not the payload — proves which
    /// conversation an answer belongs to.
    template <class T>
    TxnResult ask(const T& payload) {
        return ask(Message(to_value(payload)));
    }
    TxnResult ask(Message msg) {
        return bus_->ask_candidate_to_prepare(id_, std::move(msg));
    }

    /// OFFER THE DELIVERY CURRENTLY BEING HANDLED to this transaction's
    /// authenticated readiness gate. Named as what it is: the coordinator does
    /// not assert readiness, it offers what it is holding, and the Switchboard
    /// remains the judge — this is an authenticated answer, from the exact
    /// candidate, to this transaction's exact ask, or it is refused. Called
    /// outside a delivery, from the wrong delivery, from the wrong coordinator,
    /// or on the wrong handle, it refuses exactly as the substrate does.
    TxnResult offer_current_answer(PreparationAnswer answer) {
        return bus_->accept_preparation_answer(id_, answer);
    }

    /// SCHEDULE the admission, with the standard activation. `ok` means the
    /// admission is scheduled — NEVER that the replacement committed. After a
    /// successful return, `state()` is `AdmissionPending` until the admission
    /// envelope actually dispatches (pumping is the caller's, never this
    /// method's); the transaction terminalizes `Committed` or aborts inside
    /// that dispatch, and `take_outcome()` is how the result is collected.
    ///
    /// The sequence stays EXPLICIT: this layer does not own activation-sequence
    /// allocation, and no host sequence owner exists to consume — the operator
    /// supplies the number, exactly as with the raw primitive. Internally this
    /// is the whole of the wiring a caller no longer writes:
    /// `commit_prepared_replacement(id, host_lifecycle_authority(bus),
    /// Message(to_value(Activated{sequence})), sequence)`.
    TxnResult commit(std::int64_t sequence) {
        return bus_->commit_prepared_replacement(
            id_, host_lifecycle_authority(*bus_),
            Message(to_value(Activated{sequence})), sequence);
    }

    /// Abort, as the exact operator this handle was started with. Unloads
    /// nothing itself (the substrate discards the sealed candidate), pumps
    /// nothing, consumes no outcome, retries nothing.
    TxnResult abort() { return bus_->abort_prepared_replacement(id_, operator_id_); }

    /// The transaction's real state, asked of the Switchboard EVERY time.
    /// There is no facade state machine and no cache to drift: this is
    /// `transaction_state(id())`, nothing else. (After the terminal outcome
    /// has been collected the substrate no longer remembers the transaction,
    /// and answers as it does for any unknown id.)
    TxnState state() const { return bus_->transaction_state(id_); }

    /// Collect this transaction's terminal outcome — once, by the exact
    /// operator life that began it, and only THIS transaction's: a handle
    /// never consumes a sibling transaction's result even when the same
    /// operator began both. `TxnReason` arrives exactly as the substrate
    /// recorded it; twenty reasons are not flattened into two.
    std::optional<TxnOutcome> take_outcome() {
        TxnOutcome out{};
        if (bus_->take_outcome(operator_id_, id_, out)) {
            return out;
        }
        return std::nullopt;
    }

    // ---- the useful facts --------------------------------------------------

    bool started() const noexcept { return id_.valid(); }
    /// For diagnostics, logging, tests and unusual integration — ordinary
    /// operations never need it.
    TxnId id() const noexcept { return id_; }
    WeaveId candidate() const noexcept { return candidate_; }
    /// The incumbent AS RESOLVED AT START — the exact participant the
    /// transaction bound, deliberately never re-derived from the role table.
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
