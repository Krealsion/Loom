// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_RUNS_VOCABULARY_HPP
#define ZEN_RUNS_VOCABULARY_HPP

// A RUN MANAGER'S VOCABULARY: the catalog of editable tools, named runs, and what a run's worker
// reports. Loom ships one run manager (`loom-runs`, a loadable weave a session host boots like any
// other artifact, docs/guides/sessions.md) and publishes its vocabulary here, so a client written
// in any language speaks one contract and another manager can replace this one by answering it.
//
// THREE PARTIES, THREE KINDS OF SPEECH, NONE OF IT AUTHORITY BY BEING KNOWN:
//
//   a CLIENT       lists and describes tools, starts named runs, reads them back, asks for a
//                  cancellation, releases a finished run. It learns everything from the manager's
//                  answers; it holds no private state the next client would need.
//   the MANAGER    owns run state: which tool, which revision of it actually ran, the inputs, the
//                  run's own directory, the worker process, what the worker reported, and the
//                  artifacts it verified on disk. It answers; it never acts as a client's agent.
//   a WORKER       one process per run, admitted by the session door as its own session under the
//                  rules its manager was itself allowed to pass on. It reports to the manager and
//                  asks whatever it was granted; it is the actor of its own asks.
//
// A RUN HANDLE IS (lifetime, name). The name is the client's choice, unique within one host
// lifetime; the lifetime is the session door's (`zen/session/vocabulary.hpp`). A Start whose
// answer was lost is recovered by asking for that name again -- never by starting another -- and
// a handle from an ended lifetime is refused by name, never applied to new work.
//
// WHAT A STATE MEANS. `starting` (the process is being started or has not connected yet),
// `running` (its session is admitted), and then exactly one final state: `passed` / `failed` (the
// tool's own verdict), `error` (the tool raised), `cancelled` (a cancellation was requested and
// the worker ended), `crashed` (the process ended without a verdict), `interrupted` (the host is
// ending). A client that stops waiting has stopped waiting; the run's state is the manager's.

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace loom::runs {

inline constexpr const char* kRunsRole = "loom.runs";

#define LOOM_RUNS_SHAPE(Type, Name)                                                               \
    using ZenSelf = Type;                                                                         \
    static constexpr const char* zen_name = Name;                                                 \
    static constexpr std::uint32_t zen_version = 1

// ---- the catalog --------------------------------------------------------------------------------

/// LIST THE TOOLS THE CATALOG NAMES, optionally only those matching every word of `query`.
/// Answered `Tools`. Reading the catalog runs no tool: it reads package manifests.
struct ListTools {
    std::string query;
    LOOM_RUNS_SHAPE(ListTools, "loom.runs.ListTools");
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(query)); }
};

struct Tool {
    std::string id;        ///< "<package>/<tool>", what Start names
    std::string package;
    std::string name;
    std::string summary;   ///< the package author's one line
    bool approved = false; ///< may a run of it start now (the operator's decision)
    std::string approval;  ///< "any revision" / "revision <digest>" / "not approved: <why>"
    std::string revision;  ///< the package's current content digest
    LOOM_RUNS_SHAPE(Tool, "loom.runs.Tool");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(id), ZEN_FIELD(package), ZEN_FIELD(name),
                               ZEN_FIELD(summary), ZEN_FIELD(approved), ZEN_FIELD(approval),
                               ZEN_FIELD(revision));
    }
};

struct Tools {
    std::string catalog;               ///< the catalog file the manager read
    std::vector<std::string> problems; ///< packages it could not read, and why
    std::vector<Tool> rows;
    LOOM_RUNS_SHAPE(Tools, "loom.runs.Tools");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(catalog), ZEN_FIELD(problems), ZEN_FIELD(rows));
    }
};

/// DESCRIBE ONE TOOL: purpose, inputs, outputs, requirements, the authority it asks for, where it
/// comes from, an example and what to do when it refuses. Answered `ToolDescription`. A human help
/// view and a machine are both drawn from this one answer.
struct DescribeTool {
    std::string tool; ///< "<package>/<tool>"
    LOOM_RUNS_SHAPE(DescribeTool, "loom.runs.DescribeTool");
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(tool)); }
};

struct ToolInput {
    std::string name;
    std::string type;          ///< text / int / bool / number
    bool required = false;
    std::string default_json;  ///< the default as a JSON literal, empty when there is none
    std::string help;
    LOOM_RUNS_SHAPE(ToolInput, "loom.runs.ToolInput");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(type), ZEN_FIELD(required),
                               ZEN_FIELD(default_json), ZEN_FIELD(help));
    }
};

struct ToolOutput {
    std::string name; ///< the artifact's file name inside the run's output directory
    std::string help;
    LOOM_RUNS_SHAPE(ToolOutput, "loom.runs.ToolOutput");
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(help)); }
};

struct ToolDescription {
    std::string id;
    std::string package;
    std::string name;
    std::string summary;
    std::string description;             ///< what it is for, in the author's words
    std::string version;                 ///< the package author's version label
    std::string revision;                ///< the package's content digest, as it stands now
    std::string package_dir;             ///< where the package lives
    std::string source;                  ///< the script a run would execute (a snapshot of it)
    bool approved = false;
    std::string approval;
    std::vector<ToolInput> inputs;
    std::vector<ToolOutput> outputs;
    std::vector<std::string> requires_;  ///< what must hold for it to work (a link, a vocabulary)
    std::vector<std::string> asks;       ///< bus rules it asks its worker to be granted
    std::vector<std::string> vocabulary; ///< shapes it speaks, "<Name> v<N>"; see their schemas
    std::string example;                 ///< one invocation a person can copy
    std::string recovery;                ///< what its refusals mean and what to do about them
    LOOM_RUNS_SHAPE(ToolDescription, "loom.runs.ToolDescription");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(id), ZEN_FIELD(package), ZEN_FIELD(name),
                               ZEN_FIELD(summary), ZEN_FIELD(description), ZEN_FIELD(version),
                               ZEN_FIELD(revision), ZEN_FIELD(package_dir), ZEN_FIELD(source),
                               ZEN_FIELD(approved), ZEN_FIELD(approval), ZEN_FIELD(inputs),
                               ZEN_FIELD(outputs),
                               // `requires` is a C++ keyword; the wire keeps the author's word.
                               ::loom::field_entry("requires", &ToolDescription::requires_),
                               ZEN_FIELD(asks), ZEN_FIELD(vocabulary), ZEN_FIELD(example),
                               ZEN_FIELD(recovery));
    }
};

// ---- runs ---------------------------------------------------------------------------------------

/// START A NAMED RUN of a tool, with its inputs as one JSON object. Answered `Run` (state
/// `starting`) or `zen.Refused`. A name already used in this lifetime for the SAME tool and inputs
/// is answered with that run as it stands -- a retried Start is not a second run -- and for
/// anything else is refused.
struct Start {
    std::string tool;
    std::string name;
    std::string inputs; ///< a JSON object; "{}" or empty for none
    LOOM_RUNS_SHAPE(Start, "loom.runs.Start");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(tool), ZEN_FIELD(name), ZEN_FIELD(inputs));
    }
};

/// Every run this manager holds in this lifetime. Answered `RunList`.
struct List {
    LOOM_RUNS_SHAPE(List, "loom.runs.List");
    static auto zen_fields() { return std::make_tuple(); }
};

/// One run by its handle. Answered `Run`, or `zen.Refused` for an unknown name or another
/// lifetime's handle.
struct Get {
    std::string lifetime;
    std::string name;
    LOOM_RUNS_SHAPE(Get, "loom.runs.Get");
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(lifetime), ZEN_FIELD(name)); }
};

/// ASK FOR A CANCELLATION. A request, not an outcome: the worker is told (it may clean up), and the
/// run is `cancelled` once its process has ended. `force` ends the process without waiting for the
/// worker's cleanup. Answered `Run` as it stands after the request.
struct Cancel {
    std::string lifetime;
    std::string name;
    std::string reason;
    bool force = false;
    LOOM_RUNS_SHAPE(Cancel, "loom.runs.Cancel");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(lifetime), ZEN_FIELD(name), ZEN_FIELD(reason),
                               ZEN_FIELD(force));
    }
};

/// FORGET A FINISHED RUN, freeing its place; `remove` also deletes its directory, which the manager
/// created. An active run is refused. Answered `zen.Ack` or `zen.Refused`.
struct Release {
    std::string lifetime;
    std::string name;
    bool remove = false;
    LOOM_RUNS_SHAPE(Release, "loom.runs.Release");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(lifetime), ZEN_FIELD(name), ZEN_FIELD(remove));
    }
};

/// RUNS RECORDED BY EARLIER LIFETIMES in this session directory, read from their `run.json` files:
/// evidence of what happened, never live state. Answered `RunList` with `live` false on each row.
struct Past {
    LOOM_RUNS_SHAPE(Past, "loom.runs.Past");
    static auto zen_fields() { return std::make_tuple(); }
};

struct Artifact {
    std::string name;
    std::string path;
    std::int64_t bytes = 0;
    std::string sha256;
    std::string state; ///< verified / missing / size-mismatch / digest-mismatch / outside
    LOOM_RUNS_SHAPE(Artifact, "loom.runs.Artifact");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(path), ZEN_FIELD(bytes),
                               ZEN_FIELD(sha256), ZEN_FIELD(state));
    }
};

/// One ask a worker made, in its own words: which of ITS conversations (`correlation`, the number
/// the worker's session put on it), to whom, through which link, and how it came out. Joined with
/// the host's history by that correlation (`loom.history.DeliveriesTo`).
struct Asked {
    std::int64_t correlation = 0;
    std::string office;   ///< the office asked (a far office when `via` names a link)
    std::string via;      ///< the link it crossed, or empty for a local ask
    std::string shape;
    std::int64_t version = 0;
    bool settle = false;
    std::string outcome;  ///< answer / refused / dispatch-refused / unlinked / lost / pending
    std::string detail;
    LOOM_RUNS_SHAPE(Asked, "loom.runs.Asked");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(correlation), ZEN_FIELD(office), ZEN_FIELD(via),
                               ZEN_FIELD(shape), ZEN_FIELD(version), ZEN_FIELD(settle),
                               ZEN_FIELD(outcome), ZEN_FIELD(detail));
    }
};

struct Run {
    std::string lifetime;
    std::string name;
    std::string tool;
    std::string revision;       ///< the digest of the package snapshot this run executes
    std::string snapshot;       ///< where that snapshot is
    std::string inputs;         ///< the JSON object it was started with
    std::string directory;      ///< the run's own directory; outputs live in `<directory>/out`
    std::string state;
    bool live = true;           ///< held by this manager in this lifetime (false: a past record)
    std::string step;           ///< the worker's last word about where it is
    std::vector<std::string> pending; ///< what the worker says it is waiting on, now
    bool cancel_requested = false;
    std::string cancel_reason;
    std::int64_t session = 0;   ///< the worker's session on this bus, once admitted
    std::string established;    ///< the name the door established for it
    std::int64_t pid = 0;
    std::int64_t exit_code = 0;
    std::string process;        ///< not-started / running / exited / killed / unknown
    std::string summary;        ///< the tool's own verdict line
    std::string failure;        ///< why it did not pass: the tool's words, or the manager's
    std::vector<Artifact> artifacts;
    std::vector<Asked> asks;
    std::int64_t asks_dropped = 0;  ///< the oldest asks the bounded record let go, counted
    std::vector<std::string> notes;
    std::int64_t notes_dropped = 0;
    std::int64_t started_ms = 0; ///< wall clock, for a person
    std::int64_t ended_ms = 0;
    std::string log;             ///< the worker's stdout/stderr file
    LOOM_RUNS_SHAPE(Run, "loom.runs.Run");
    static auto zen_fields() {
        return std::make_tuple(
            ZEN_FIELD(lifetime), ZEN_FIELD(name), ZEN_FIELD(tool), ZEN_FIELD(revision),
            ZEN_FIELD(snapshot), ZEN_FIELD(inputs), ZEN_FIELD(directory), ZEN_FIELD(state),
            ZEN_FIELD(live), ZEN_FIELD(step), ZEN_FIELD(pending), ZEN_FIELD(cancel_requested),
            ZEN_FIELD(cancel_reason), ZEN_FIELD(session), ZEN_FIELD(established), ZEN_FIELD(pid),
            ZEN_FIELD(exit_code), ZEN_FIELD(process), ZEN_FIELD(summary), ZEN_FIELD(failure),
            ZEN_FIELD(artifacts), ZEN_FIELD(asks), ZEN_FIELD(asks_dropped), ZEN_FIELD(notes),
            ZEN_FIELD(notes_dropped), ZEN_FIELD(started_ms), ZEN_FIELD(ended_ms),
            ZEN_FIELD(log));
    }
};

struct RunList {
    std::string lifetime;
    std::int64_t active = 0;
    std::int64_t capacity = 0; ///< how many runs this manager holds at most
    std::vector<Run> rows;
    LOOM_RUNS_SHAPE(RunList, "loom.runs.RunList");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(lifetime), ZEN_FIELD(active), ZEN_FIELD(capacity),
                               ZEN_FIELD(rows));
    }
};

// ---- what a worker says to its manager ----------------------------------------------------------
//
// Accepted only from the session the door admitted for that run: the bus stamps the sender, and a
// report is matched by that stamp, never by a run name it carries. Each is answered `zen.Ack`.

/// The worker is up: the interpreter it runs under and the revision it loaded.
struct WorkerStarted {
    std::string run;
    std::string python;
    std::string revision;
    LOOM_RUNS_SHAPE(WorkerStarted, "loom.runs.WorkerStarted");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(run), ZEN_FIELD(python), ZEN_FIELD(revision));
    }
};

/// Where the tool is, and what it is waiting on right now.
struct Progress {
    std::string step;
    std::string note;
    std::vector<std::string> pending;
    LOOM_RUNS_SHAPE(Progress, "loom.runs.Progress");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(step), ZEN_FIELD(note), ZEN_FIELD(pending));
    }
};

/// One of the worker's asks, as it stands (see `Asked`).
struct AskReport {
    Asked ask;
    LOOM_RUNS_SHAPE(AskReport, "loom.runs.AskReport");
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(ask)); }
};

/// A file the tool says it produced. The manager checks it on disk itself -- inside the run's own
/// output directory, the size and the SHA-256 -- before listing it as verified.
struct Produced {
    std::string name;
    std::string path;
    std::int64_t bytes = 0;
    std::string sha256;
    LOOM_RUNS_SHAPE(Produced, "loom.runs.Produced");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(path), ZEN_FIELD(bytes),
                               ZEN_FIELD(sha256));
    }
};

/// The tool's verdict: passed / failed / error / cancelled, its summary line, and why not.
struct Finished {
    std::string outcome;
    std::string summary;
    std::string failure;
    LOOM_RUNS_SHAPE(Finished, "loom.runs.Finished");
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(outcome), ZEN_FIELD(summary), ZEN_FIELD(failure));
    }
};

/// THE WORKER'S STANDING QUESTION: "is there anything I must do?" Its manager holds the answer
/// (a deferred answer: the worker's own question is what lets the manager speak to it, so the
/// manager needs no rule naming the worker -- only, like every answerer, a grant for the shape)
/// and gives it once: `cancel` when a cancellation is requested, `finish` when the run is over.
/// Answered `Directive`.
struct Control {
    LOOM_RUNS_SHAPE(Control, "loom.runs.Control");
    static auto zen_fields() { return std::make_tuple(); }
};

struct Directive {
    std::string directive; ///< "cancel" / "finish"
    std::string reason;
    LOOM_RUNS_SHAPE(Directive, "loom.runs.Directive");
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(directive), ZEN_FIELD(reason)); }
};

#undef LOOM_RUNS_SHAPE

/// The shapes a worker must be able to say to its manager, for the rules its manager asks the door
/// to grant (each `<Name> v1 -> role loom.runs`).
inline std::vector<std::string> worker_report_shapes() {
    return {WorkerStarted::zen_name, Progress::zen_name, AskReport::zen_name, Produced::zen_name,
            Finished::zen_name, Control::zen_name};
}

/// The shapes a client says to a run manager.
inline std::vector<std::string> client_request_shapes() {
    return {ListTools::zen_name, DescribeTool::zen_name, Start::zen_name, List::zen_name,
            Get::zen_name,       Cancel::zen_name,       Release::zen_name, Past::zen_name};
}

} // namespace loom::runs

#endif // ZEN_RUNS_VOCABULARY_HPP
