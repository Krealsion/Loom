// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/terminal/session.hpp>

#include <zen/switchboard/switchboard.hpp> // lifecycle_policy_schema() only

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace loom {

// ---- the small vocabularies this file owns ---------------------------------

const char* name_of(TranscriptKind kind) noexcept {
    switch (kind) {
    case TranscriptKind::LocalCommand:
        return "command";
    case TranscriptKind::LocalRefusal:
        return "refused-here";
    case TranscriptKind::LocalNotice:
        return "note";
    case TranscriptKind::Submitted:
        return "submitted";
    case TranscriptKind::Received:
        return "received";
    case TranscriptKind::AnswerReceived:
        return "answered";
    }
    return "?";
}

const char* name_of(Addressing mode) noexcept {
    switch (mode) {
    case Addressing::Weave:
        return "weave";
    case Addressing::Role:
        return "role";
    case Addressing::Publish:
        return "publish";
    }
    return "?";
}

const char* name_of(TerminalOutcome outcome) noexcept {
    switch (outcome) {
    case TerminalOutcome::Submitted:
        return "submitted";
    case TerminalOutcome::NotAttached:
        return "not-attached";
    case TerminalOutcome::UnknownShape:
        return "unknown-shape";
    case TerminalOutcome::BadArguments:
        return "bad-arguments";
    case TerminalOutcome::NeedsInput:
        return "needs-input";
    case TerminalOutcome::BadAddress:
        return "bad-address";
    case TerminalOutcome::TooManyAsks:
        return "too-many-asks";
    case TerminalOutcome::NoSuchAsk:
        return "no-such-ask";
    }
    return "?";
}

// ---- the transcript --------------------------------------------------------

std::vector<TranscriptEntry> Transcript::tail(std::size_t n) const {
    std::vector<TranscriptEntry> all = entries_.snapshot();
    if (n >= all.size()) {
        return all;
    }
    return std::vector<TranscriptEntry>(all.end() - static_cast<std::ptrdiff_t>(n), all.end());
}

std::optional<ReceivedMessage> Transcript::received(std::uint64_t id) const {
    // `id` is a STABLE IDENTITY over every message this participant has ever received, not a
    // position in the retained window. Outside the retained range this refuses, exactly as it does
    // for an id that never arrived — a reference that resolved to r7 must never quietly become a
    // different message.
    const std::uint64_t base = received_.evicted(); // ids base+1 .. base+size are retained
    if (id <= base || id > base + received_.size()) {
        return std::nullopt;
    }
    return received_.at(static_cast<std::size_t>(id - base - 1));
}

std::string safe_terminal_text(std::string_view raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte == '\\') {
            out += "\\\\"; // doubled, so a \xNN below is never ambiguous with authored text
        } else if (byte < 0x20 || byte == 0x7f) {
            out += "\\x"; // the bytes that can steer a terminal, and only those
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0f]);
        } else {
            out.push_back(c); // printable ASCII and every UTF-8 byte, untouched
        }
    }
    return out;
}

// ---- the participant's compose view ----------------------------------------

/// The two lookups the ladder needs, answered from THIS participant's own
/// knowledge: the vocabulary its host supplied, and the messages it has itself
/// received. Neither reaches the bus, so composing is something a terminal can do
/// with no standing in the world at all.
class TerminalSession::Source final : public ComposeSource {
public:
    explicit Source(const TerminalSession& session) : session_(session) {}

    std::shared_ptr<const loom::Schema> resolve_schema(std::string_view name,
                                                       std::uint32_t version) const override {
        return session_.vocabulary_.find(name, version);
    }
    std::optional<loom::Cell> resolve_ref(const Ref& ref, std::string* error) const override {
        return session_.resolve_ref(ref, error);
    }

private:
    const TerminalSession& session_;
};

// ---- construction ----------------------------------------------------------

namespace {

std::shared_ptr<const loom::Schema> terminal_state_schema() {
    static const auto s = loom::SchemaBuilder("zen.TerminalState", 1)
                              .field("received", loom::Kind::Int)
                              .field("submitted", loom::Kind::Int)
                              .build();
    return s;
}

} // namespace

TerminalSession::TerminalSession(std::string label, TerminalVocabulary vocabulary,
                                 std::shared_ptr<ObservationOrder> order)
    : label_(std::move(label)), vocabulary_(std::move(vocabulary)),
      order_(order ? std::move(order) : std::make_shared<ObservationOrder>()) {}

TerminalSession::~TerminalSession() = default;

void TerminalSession::attach(std::unique_ptr<ParticipantChannel> channel) {
    if (channel == nullptr) {
        throw std::invalid_argument("loom::TerminalSession::attach: no channel");
    }
    if (channel_ != nullptr) {
        // ONE IDENTITY, FOR LIFE. Re-attaching would mean this participant's
        // transcript already holds messages authored as somebody else, which is
        // exactly the merge every other line of this design refuses.
        throw std::invalid_argument(
            "loom::TerminalSession::attach: this participant already speaks as weave " +
            std::to_string(self_.value) + "; a terminal participant has one identity for life");
    }
    if (!channel->self().valid()) {
        throw std::invalid_argument(
            "loom::TerminalSession::attach: the channel names no weave — bind it after the "
            "participant is registered, so it carries the identity the bus assigned");
    }
    self_ = channel->self();
    channel_ = std::move(channel);
}

// ---- type knowledge --------------------------------------------------------

Composition TerminalSession::compose(std::string_view name, std::uint32_t version,
                                     const std::vector<Arg>& args) const {
    const Source source(*this);
    return compose_message(source, name, version, args);
}

std::optional<loom::Cell> TerminalSession::resolve_ref(const Ref& ref, std::string* error) const {
    const auto fail = [&](const std::string& m) -> std::optional<loom::Cell> {
        if (error != nullptr) {
            *error = m;
        }
        return std::nullopt;
    };
    // The label is this participant's own format ("rN"); parse it back to an id.
    if (ref.label.size() < 2 || ref.label[0] != 'r') {
        return fail("bad reference label '" + ref.label + "' (expected rN)");
    }
    std::uint64_t n = 0;
    try {
        std::size_t pos = 0;
        n = std::stoull(ref.label.substr(1), &pos);
        if (pos + 1 != ref.label.size()) {
            return fail("bad reference label '" + ref.label + "'");
        }
    } catch (...) {
        return fail("bad reference label '" + ref.label + "'");
    }
    const std::optional<ReceivedMessage> entry = transcript_.received(n);
    if (!entry) {
        // Two different absences, and a reader needs them apart: an id that never arrived is a
        // typo, an id that was EVICTED is the bounded window telling the truth about its own
        // horizon.
        if (n != 0 && n <= transcript_.received_evicted()) {
            return fail("received message " + ref.label + " was evicted (this participant retains "
                        "the most recent " + std::to_string(kReceivedCapacity) + "; " +
                        std::to_string(transcript_.received_evicted()) + " older ones were " +
                        "discarded)");
        }
        return fail("no such received message: " + ref.label);
    }
    const loom::Cell* c = entry->value.get(ref.field);
    if (c == nullptr) {
        return fail("received message " + ref.label + " (" + entry->value.schema().name() +
                    ") has no field '" + ref.field + "'");
    }
    const loom::Kind k = c->kind();
    if (k == loom::Kind::Message || k == loom::Kind::List) {
        return fail("reference to non-scalar field '" + ref.field + "' is not supported");
    }
    return *c; // copy the scalar Cell out of the immutable received Value
}

// ---- acting ----------------------------------------------------------------

std::uint64_t TerminalSession::record_local(TranscriptKind kind, std::string text) {
    TranscriptEntry e;
    e.seq = order_->next();
    e.kind = kind;
    e.lens = self_;
    e.text = std::move(text);
    const std::uint64_t seq = e.seq;
    transcript_.record(std::move(e));
    return seq;
}

std::uint64_t TerminalSession::record_command(std::string text) {
    return record_local(TranscriptKind::LocalCommand, std::move(text));
}

std::uint64_t TerminalSession::record_notice(std::string text) {
    return record_local(TranscriptKind::LocalNotice, std::move(text));
}

TerminalResult TerminalSession::send(const Address& to, std::string_view name,
                                     std::uint32_t version, const std::vector<Arg>& args) {
    return author(to, name, version, args, /*as_ask=*/false);
}

TerminalResult TerminalSession::ask(const Address& to, std::string_view name,
                                    std::uint32_t version, const std::vector<Arg>& args) {
    return author(to, name, version, args, /*as_ask=*/true);
}

TerminalResult TerminalSession::author(const Address& to, std::string_view name,
                                       std::uint32_t version, const std::vector<Arg>& args,
                                       bool as_ask) {
    TerminalResult result;
    const auto refuse = [&](TerminalOutcome outcome, std::string why) {
        result.outcome = outcome;
        result.detail = why;
        // A LOCAL refusal, recorded as one. Nothing was authored, so nothing was denied by
        // anybody: the transcript must never let this read as Loom saying no.
        result.entry = record_local(TranscriptKind::LocalRefusal, std::move(why));
        return result;
    };

    if (!attached()) {
        return refuse(TerminalOutcome::NotAttached,
                      "this participant has no identity yet; nothing was authored");
    }
    if (!to.well_formed()) {
        return refuse(TerminalOutcome::BadAddress,
                      to.mode == Addressing::Role
                          ? std::string("a role send needs an office name")
                          : std::string("a directed send needs a weave id"));
    }
    if (as_ask && to.mode == Addressing::Publish) {
        return refuse(TerminalOutcome::BadAddress,
                      "a publication cannot be an ask: it has no one respondent, so there is no "
                      "conversation for Loom to authorize an answer in");
    }
    if (as_ask && pending_.size() >= kMaxOutstandingAsks) {
        // The outstanding conversations are UNTOUCHED. A new ask must never displace one somebody
        // is waiting on, so this refuses before anything is composed or authored.
        return refuse(TerminalOutcome::TooManyAsks,
                      "this participant is already waiting on " + std::to_string(pending_.size()) +
                          " answers (the most it will track at once); cancel one first");
    }

    const Composition composition = compose(name, version, args);
    if (composition.status == Composition::Status::Error) {
        return refuse(composition.schema == nullptr ? TerminalOutcome::UnknownShape
                                                    : TerminalOutcome::BadArguments,
                      composition.error);
    }
    if (composition.status == Composition::Status::NeedsInput) {
        result.outcome = TerminalOutcome::NeedsInput;
        result.detail = "the composer will not guess which value fills which field";
        result.open_fields = composition.open_fields;
        result.unplaced = composition.unplaced;
        result.entry = record_local(TranscriptKind::LocalRefusal, result.detail);
        return result;
    }

    // THE CORRELATION IS THIS PARTICIPANT'S OWN, monotonic and never zero. Loom echoes it back on
    // whatever answer it authorizes, which is what lets several conversations be outstanding at
    // once without this core inventing a request id beside the one Loom already keeps.
    const std::uint64_t correlation = ++correlation_;
    loom::Value payload = assemble(composition);

    TranscriptEntry entry;
    entry.seq = order_->next();
    entry.kind = TranscriptKind::Submitted;
    entry.lens = self_;
    entry.shape = composition.schema->name();
    entry.version = composition.schema->version();
    entry.correlation = correlation;
    entry.addressing = to.mode;
    entry.target = to.target;
    entry.role = to.role;

    switch (to.mode) {
    case Addressing::Weave:
        (void)channel_->send(to.target, std::move(payload), correlation);
        break;
    case Addressing::Role:
        (void)channel_->send_to_role(to.role, std::move(payload), correlation);
        break;
    case Addressing::Publish:
        // The fanout count is the one delivery fact an ordinary sender is given, and it says how
        // many deliveries were QUEUED. Each is still independently gated afterwards.
        entry.recipients = channel_->publish(std::move(payload), correlation);
        break;
    }
    // The Ticket is deliberately dropped. It is a HOST-side journal handle: a participant cannot
    // read an outcome from it, so keeping one would be keeping the shape of an answer this
    // participant will never have.

    ++submitted_count_;
    transcript_.record(entry);
    result.outcome = TerminalOutcome::Submitted;
    result.entry = entry.seq;

    if (as_ask) {
        PendingAsk p;
        p.id = ++next_ask_;
        p.correlation = correlation;
        p.shape = entry.shape;
        p.version = entry.version;
        p.addressing = to.mode;
        p.target = to.target;
        p.role = to.role;
        p.submitted = entry.seq;
        pending_.push_back(std::move(p));
        result.ask = next_ask_;
    }
    return result;
}

TerminalResult TerminalSession::request_authority(std::string_view shape, std::int64_t version,
                                                  std::string_view to_role,
                                                  std::string_view purpose,
                                                  std::string_view office) {
    // SUGAR, AND ONLY SUGAR. Four named arguments and an ordinary role-addressed ask; the shape is
    // resolved out of the vocabulary the host supplied, like every other. Nothing here knows what
    // a Weaver is, and nothing here calls one.
    std::vector<Arg> args;
    args.push_back(Arg{std::string("shape"), FieldValue{std::string(shape)}});
    args.push_back(Arg{std::string("version"), FieldValue{version}});
    args.push_back(Arg{std::string("to_role"), FieldValue{std::string(to_role)}});
    args.push_back(Arg{std::string("purpose"), FieldValue{std::string(purpose)}});
    return ask(Address::to_role(std::string(office)), "zen.RequestAuthority", 1, args);
}

TerminalResult TerminalSession::describe_authority(std::string_view office) {
    return ask(Address::to_role(std::string(office)), "zen.DescribeAuthority", 1, {});
}

TerminalResult TerminalSession::cancel_ask(std::uint64_t ask_id) {
    TerminalResult result;
    const auto it = std::find_if(pending_.begin(), pending_.end(),
                                 [&](const PendingAsk& p) { return p.id == ask_id; });
    if (it == pending_.end()) {
        result.outcome = TerminalOutcome::NoSuchAsk;
        result.detail = "ask " + std::to_string(ask_id) + " is not outstanding";
        result.entry = record_local(TranscriptKind::LocalRefusal, result.detail);
        return result;
    }
    const std::string shape = it->shape;
    pending_.erase(it);
    result.outcome = TerminalOutcome::Submitted;
    result.ask = ask_id;
    // SAID EXACTLY, because the tempting shorter sentence would be a lie: Loom has no cancellation
    // vocabulary, so nothing at the far end was told anything, and the answer may still arrive.
    result.entry = record_local(TranscriptKind::LocalNotice,
                                "stopped waiting locally on ask " + std::to_string(ask_id) + " (" +
                                    shape + "). The request was NOT cancelled — nobody was told, "
                                    "and its answer may still arrive.");
    return result;
}

bool TerminalSession::waiting_on(std::uint64_t ask_id) const noexcept {
    for (const PendingAsk& p : pending_) {
        if (p.id == ask_id) {
            return true;
        }
    }
    return false;
}

// ---- the Weave contract ----------------------------------------------------

std::vector<std::shared_ptr<const loom::Schema>> TerminalSession::accepted_schemas() const {
    // EXACTLY THE DOORS the host declared — never the catalog, and never
    // AcceptMode::AnyRegistered. A participant that accepted every registered shape would be
    // accepting shapes chosen by whoever else happens to be running, which is a decision its host
    // never made.
    return vocabulary_.doors();
}

void TerminalSession::handle(const loom::Message& in, loom::Bus& bus) {
    // THIS FUNCTION SENDS NOTHING, and that is a load-bearing property rather than an omission.
    // "Approval grants authority and does not replay intent" is true here because there is no
    // statement in this body that could author a message — not a reply, not a retry, not an
    // acknowledgement. A future edit that added one would have to add the first.
    (void)bus;
    ++received_count_;

    const bool answers = in.provenance.answers_ask();
    const std::uint64_t message_id = transcript_.retain(
        ReceivedMessage{in.sender, std::string(in.provenance.authored_role()), answers,
                        in.correlation, in.payload});

    TranscriptEntry entry;
    entry.seq = order_->next();
    entry.kind = answers ? TranscriptKind::AnswerReceived : TranscriptKind::Received;
    entry.lens = self_;
    entry.shape = in.payload.schema().name();
    entry.version = in.payload.schema().version();
    entry.correlation = in.correlation;
    // TRUSTED, AND NOT PAYLOAD. The sender is the bus stamp; the authored role is an office Loom
    // verified at the authorship moment. A payload field that happens to name somebody is never
    // copied into either.
    entry.sender = in.sender;
    entry.authored_role = std::string(in.provenance.authored_role());
    entry.answers_ask = answers;
    entry.message = message_id;

    if (answers) {
        // WHICH ask, by LOOM'S OWN correlation — the one the answer doors copy out of the request
        // they are answering. This is strictly stronger than the standing consumer obligation for
        // the standard reply shapes (match correlation AND bus-stamped sender): an unsolicited
        // `zen.Ack` from a weave that merely holds the grant for it carries no answer provenance
        // at all, so it never reaches this branch.
        const auto it =
            std::find_if(pending_.begin(), pending_.end(),
                         [&](const PendingAsk& p) { return p.correlation == in.correlation; });
        if (it != pending_.end()) {
            entry.answers = it->id;
            pending_.erase(it);
        }
        // ...and when it matches nothing, it is still recorded as the authenticated answer it is.
        // An answer to an ask this participant stopped waiting on is a true fact about the world,
        // not an error, and pretending otherwise would be the second lie after "I cancelled it".
    }
    transcript_.record(std::move(entry));
}

loom::Value TerminalSession::snapshot() const {
    loom::Value v(terminal_state_schema());
    // Totals OBSERVED, not retained: once the bounded windows saturate, a retained count would sit
    // at the capacity forever and say nothing.
    v.set("received", loom::Cell::integer(received_count_));
    v.set("submitted", loom::Cell::integer(submitted_count_));
    return v;
}

loom::Value TerminalSession::policy() const {
    loom::Value v(loom::lifecycle_policy_schema());
    // A terminal participant does not reload. A revived one would come back with an empty
    // transcript and no outstanding conversations, having silently dropped questions somebody is
    // waiting on — so it fails visibly instead.
    v.set("max_reloads", loom::Cell::integer(0));
    v.set("revive_from_last_good", loom::Cell::boolean(true));
    return v;
}

void TerminalSession::revive(const loom::Value&) {}

// ---- the desk --------------------------------------------------------------

TerminalDesk::TerminalDesk(TerminalSession& acting, TerminalSession& operator_seat)
    : acting_(&acting), operator_(&operator_seat) {
    if (!acting.attached() || !operator_seat.attached()) {
        throw std::invalid_argument(
            "loom::TerminalDesk: both participants must be attached — a desk pairs two live "
            "identities, and an unattached one has none to be different from");
    }
    if (acting.id() == operator_seat.id()) {
        // THE SAME REFUSAL loom::Weaver makes about its own two roles, one layer up and for the
        // same reason. A presentation whose operator seat IS its governed session is a session
        // that approves its own requests: "no weave can widen its own authority" would then be
        // false, defeated not by a bug but by one line of host wiring, silently. It is a host
        // misconfiguration, so it fails loudly, at the boot that wires it.
        throw std::invalid_argument(
            "loom::TerminalDesk: the operator seat must not be the governed participant — one "
            "weave holding both would be able to approve its own authority requests");
    }
    if (acting.order() != operator_seat.order()) {
        // A merged chronology has to MEAN something. Two independent counters interleave into an
        // order this presentation made up, and a made-up order is worse than none.
        throw std::invalid_argument(
            "loom::TerminalDesk: the two participants must share one ObservationOrder, or a "
            "merged chronology would be an interleaving the presentation invented");
    }
}

std::vector<DeskEntry> TerminalDesk::chronology() const {
    std::vector<DeskEntry> out;
    for (TranscriptEntry& e : acting_->transcript().entries()) {
        out.push_back(DeskEntry{acting_->label(), std::move(e)});
    }
    for (TranscriptEntry& e : operator_->transcript().entries()) {
        out.push_back(DeskEntry{operator_->label(), std::move(e)});
    }
    std::stable_sort(out.begin(), out.end(), [](const DeskEntry& a, const DeskEntry& b) {
        return a.entry.seq < b.entry.seq;
    });
    return out;
}

} // namespace loom
