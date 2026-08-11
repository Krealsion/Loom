# The Weaver — reference

The first delegate of a **human being's** authority decisions. Laws:
[GATE-05](../laws/admission-laws.md#gate-05--baseline-authority-is-admission-time-delegated-authority-is-live-effective-authority-decides),
[MSG-02](../laws/messaging-laws.md), [ANS-01..07](../laws/answer-authority-laws.md).

```text
USER          decides         a person, at an operator seat
WEAVER        delegates       an ordinary weave holding one GrantAuthority
SESSION       acts            an ordinary weave with its own baseline grant
SWITCHBOARD   enforces        the Kernel, which has the last word
```

Or, in one sentence: **the Kernel enforces, the Weaver decides, the session
acts.** [GRANT-0](capabilities.md#live-delegation-grant-0) built the mechanism by
which a host may appoint an administrator for one live subject's speech; the
Weaver is the first policy actor to hold one.

## The four parties, and what each is not

| | is | is **not** |
|---|---|---|
| Kernel | the only thing that enforces authority, at every delivery | a policy engine — it has no opinion about who *should* be allowed |
| `GrantAuthority` | the administration mechanism: one subject, one ceiling, one board | a way to send anything; possessing one confers no speech |
| Weaver | one ordinary weave that turns a human's decisions into real delegation | a host, a broker, an auditor, or a store of what it granted |
| operator seat | the exact weave whose word this Weaver treats as the user's | proof of a person — see [what this does not govern](#what-this-does-not-govern) |
| governed session | the one subject the capability names | anything special; it holds no capability and no privileged path |

## The two powers, granted separately

A host bootstrapping a Weaver hands it **two** things, and the separation is
load-bearing:

```text
an ordinary Grant     what this Weaver may SAY
a GrantAuthority      what this Weaver may DELEGATE
```

Conflating them would make every send rule a Weaver happens to own into a
delegable right, and would mean a Weaver could not answer a policy question
without gaining the power to grant what the answer is about. A Weaver permitted
to say `zen.AuthorityGranted` has gained no authority to grant anything.

The reference wiring gives a Weaver a deliberately asymmetric grant: it may say
**"no" to anyone who reaches it** (a refusal is speech, and silence would be
indistinguishable from a lost message) and **"yes" to exactly two weaves** — the
operator seat and the governed session.

## The vocabulary

[`zen/weaver/vocabulary.hpp`](../../include/zen/weaver/vocabulary.hpp) — ordinary
registered shapes, discoverable and composable through the ordinary schema
registry. A governed session includes the vocabulary and **not** the Weaver: it
depends on the request language, never on the policy.

| shape | direction | carries |
|---|---|---|
| `zen.RequestAuthority v1` | session → Weaver (an ask) | `shape`, `version`, `to_role`, `purpose` |
| `zen.AuthorityPrompt v1` | Weaver → operator | `requester`, `shape`, `version`, `to_role`, `until`, `requester_says` |
| `zen.ApproveAuthority v1` | operator → Weaver | *nothing* |
| `zen.RefuseAuthority v1` | operator → Weaver | *nothing* |
| `zen.RevokeAuthority v1` | operator → Weaver | *nothing* |
| `zen.DescribeAuthority v1` | operator **or** session → Weaver | *nothing* |
| `zen.AuthorityGranted v1` | Weaver answers the ask | `basis` |
| `zen.AuthorityDescription v1` | Weaver answers a describe | `subject`, `base`, `delegated` |

Refusals are `zen.Refused` and bare successes are `zen.Ack`
([standard shapes](../../include/zen/weave/standard_shapes.hpp)) — not a private
dialect.

### Four rules that live in the field lists

Each of these would otherwise be a runtime check somebody could delete.

- **No requester field.** `zen.RequestAuthority` carries no identity, so there
  is nothing to forge. The Weaver reads `mail.sender()` — the bus stamp — and
  compares it with `GrantAuthority::subject()`.
- **No subject field, anywhere.** The operator's four decision shapes name no
  subject either. Cross-subject administration is not refused; it is a sentence
  with nowhere to put the other subject.
- **The decision is the shape, never a value.** Approve and refuse are two
  contentless shapes rather than one carrying a bool. A field can be defaulted or
  mis-parsed into meaning yes; a mistyped shape name is a gate refusal.
- **The request language is narrower than `LiveAuthority`.** A request names one
  shape, one version and one office. There is no way to spell "any shape", "any
  target", an exact WeaveId, or an observe rule — regardless of how wide a
  ceiling the host handed the Weaver.

## The flow

```text
session   RequestAuthority ------------------> Weaver
                                               sender == governed subject?
                                               well-formed?
                                               already effective?  -> AuthorityGranted{already-permitted}
                                               defer the answer
Weaver    AuthorityPrompt --------------------> operator
operator  ApproveAuthority -------------------> Weaver
                                               sender == operator seat?
                                               read AuthorityView
                                               delegate(view.delegated + rule)
Weaver    AuthorityGranted{delegated} --------> session   (answers_ask() == true)
Weaver    zen.Ack ----------------------------> operator
session   <the original action, retried by the session itself>
```

The answer to a request is **Loom's own answer**, taken across the operator's
turn with [`defer_answer`](messaging.md#answers) — not a fresh message of the
right shape, and not a correlation the Weaver invented.

**Approval performs nothing.** The session decides, in its own code, whether and
when to retry; no layer replays the message that was refused. This is the
architectural discriminator between an administrator and a broker, and it is
visible at the target: the service sees the *session* as `mail.sender()`.

## Policy, stated

| question | WEAVER-1's answer |
|---|---|
| who may request | only `GrantAuthority::subject()`, by bus stamp |
| who may decide | only the host-configured operator seat, by bus stamp |
| who may inspect | the operator seat, and the governed session about itself |
| how many pending | **zero or one**; a second request is refused visibly and the first is untouched |
| already-effective request | answered `already-permitted` at once — the human is not woken, and no duplicate rule grows |
| approval | additive: `view.delegated` **+** the one approved rule, installed as one replacement |
| refusal | nothing changes; the session hears `zen.Refused` as the authenticated answer |
| beyond the ceiling | the Kernel refuses; nothing changes; both sides are told, and the refusal does not say whether the office exists |
| revoke | the **whole** delegated overlay at once; the admission baseline is untouched |
| decision with nothing pending | refused; it is never banked for a later request |
| session dies while pending | nothing installed, pending cleared, operator told; WeaveIds are never reused, so nothing can inherit it |
| Weaver dies after granting | **installed authority stands.** A grant is not a lease |

## No shadow state

The Weaver stores the pending human question, the deferred answer right, the
operator seat and the capability — and nothing about authority. Every time it
needs to know what a subject may do it calls `mail.describe_authority(...)`,
which reads the values `deliver_one` reads through the predicates `deliver_one`
applies. `zen.AuthorityDescription` is rendered from that snapshot at the moment
of the ask, so a description cannot drift from enforcement: there is nothing
kept between asks to drift.

It carries **two lists, not three**. Effective authority is base ∪ delegated *by
definition*; shipping a materialized third list would recreate exactly the second
store this design exists to avoid ([`grant.hpp`](../../include/zen/switchboard/grant.hpp)
makes the same choice one level down).

## Untrusted text at a decision surface

`purpose` is prose the requester wrote about itself. It is not evidence, never
reaches the authority decision, and is **escaped and bounded by the Weaver**
before an operator sees it (`safe_operator_text`): every byte outside printable
ASCII becomes a visible `\xNN`, a literal backslash is doubled, and truncation is
stated rather than performed silently.

The sanitizer lives at the Weaver rather than in a renderer, on purpose: a
terminal console, a future Workshop pane, and anything else that ever displays an
`AuthorityPrompt` inherit it. A sanitizer in one renderer protects one renderer.
The field is named `requester_says` so the attribution is at the point of
reading, not in documentation the operator does not have open.

ASCII-only is a real V1 limitation: a non-ASCII purpose arrives escaped rather
than translated.

## A prompt is a send, and a sender cannot observe send fate

This inherits the standing seam
([sender cannot observe send fate](known-seams.md#sender-cannot-observe-send-fate))
and it has two visible consequences here, neither of which WEAVER-1 papers over:

- **The session does not learn it was denied.** The human flow "try → see the
  refusal → ask" cannot be automated by the session today. WEAVER-1 proves the
  two halves separately: an unauthorized action *is* denied (visible on the tap),
  and a session *can* ask. It never claims the session connected them.
- **The Weaver cannot know its prompt arrived.** If the operator seat is
  misconfigured, unreachable, or dies, the request simply stays pending and the
  session simply keeps waiting. Nothing is lost or corrupted — the deferred
  answer is bounded, held by one slot, and reclaimed when either party dies — but
  nobody is told. An operator surface that must guarantee delivery of a prompt
  needs a mechanism that does not exist yet.

## The role is an address, never a power

The reference wiring binds the Weaver to the role `loom.weaver`. That is
**routing only** — a role confers no authority whatever, and a weave holding this
one with an inert capability governs nobody (pinned in `tests/test_weaver.cpp`).

It earns its place by solving a real bootstrap ordering problem: a Weaver cannot
be constructed until a capability naming its session exists, and that needs the
session's id — so a session whose baseline named its Weaver *by id* could not be
admitted first. Naming the role instead lets the session and the operator be
admitted before the Weaver exists.

## What this does not govern

WEAVER-1 governs **Loom message authority**: `LiveAuthority` delegation and
revocation for one governed session's speech. It does not govern process memory
safety, hostile in-process native code (a `dlopen`ed weave shares this address
space — see [dynamic weaves](../guides/dynamic-weaves.md)), kernel escape,
anything outside Loom, network authentication, or OS login identity.

There is **no persistence**: restarting anything forgets every approval. There is
**no allow-once** — Loom has no consumable grant, and the prompt says so in
words. There is **no time expiry**. There is no remote authentication: the
operator seat is a WeaveId a host chose, not a person.

WEAVER-1's *bootstrap* operator — a `ConsoleEngine` holding `allow_any()`,
host-wired discovery and the tap — is no longer the only option.
[TERM-0](terminal.md) drives this whole workflow from an operator seat that is an
ordinary participant with four rules and none of those three powers, which
measured that none of them was necessary to be the user. The **human** half of
the seam is untouched: a WeaveId is still not a person.

## Running it

`zen-weaver-demo` ([`src/weaver/weaver_demo.cpp`](../../src/weaver/weaver_demo.cpp))
boots an operator console, a Weaver, one governed session and one service. Its
REPL is deliberately shape-agnostic — there is no `approve` or `grant` command;
the operator composes `zen.ApproveAuthority` the way it would compose any
registered shape, through the ordinary gated send path.

PROVEN BY — [`include/zen/weaver/`](../../include/zen/weaver/),
`tests/test_weaver.cpp` (suite `weaver`), and the unchanged `grant` suite.
