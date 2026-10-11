# The Weaver — reference

A delegate of a **human being's** authority decisions. Laws:
[GATE-05](../laws/admission-laws.md#gate-05--baseline-authority-is-admission-time-delegated-authority-is-live-effective-authority-decides),
[MSG-02](../laws/messaging-laws.md), [ANS-01..07](../laws/answer-authority-laws.md).

```text
USER          decides         a person, at an operator seat
WEAVER        delegates       an ordinary weave holding one GrantAuthority
SESSION       acts            an ordinary weave with its own baseline grant
SWITCHBOARD   enforces        the Kernel, which has the last word
```

Or, in one sentence: **the Kernel enforces, the Weaver decides, the session
acts.** [Live delegation](capabilities.md#live-delegation) is the mechanism by
which a host may appoint an administrator for one live subject's speech; the
Weaver is a policy actor that holds one.

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
| `zen.AuthorityPrompt v2` | Weaver → operator | `prompt`, `requester`, `shape`, `version`, `to_role`, `until`, `requester_says` |
| `zen.ApproveAuthority v2` | operator → Weaver | `prompt` |
| `zen.RefuseAuthority v2` | operator → Weaver | `prompt` |
| `zen.RevokeAuthority v1` | operator → Weaver | *nothing* |
| `zen.DescribeAuthority v1` | operator **or** session → Weaver | *nothing* |
| `zen.AuthorityGranted v1` | Weaver answers the ask | `basis` |
| `zen.AuthorityDescription v1` | Weaver answers a describe | `subject`, `base`, `delegated` |

Refusals are `zen.Refused` and bare successes are `zen.Ack`
([standard shapes](../../include/zen/weave/standard_shapes.hpp)) — not a private
dialect. The decisions of version 1, `zen.ApproveAuthority v1` and
`zen.RefuseAuthority v1`, carry nothing; a Weaver still accepts them, only to
refuse them in words ([below](#a-decision-names-the-prompt-it-answers)).

### Four rules that live in the field lists

Each of these would otherwise be a runtime check somebody could delete.

- **No requester field.** `zen.RequestAuthority` carries no identity, so there
  is nothing to forge. The Weaver reads `mail.sender()` — the bus stamp — and
  compares it with `GrantAuthority::subject()`.
- **No subject field, anywhere.** The operator's four decision shapes name no
  subject either. Cross-subject administration is not refused; it is a sentence
  with nowhere to put the other subject.
- **The decision is the shape, never a value.** Approve and refuse are two
  shapes rather than one carrying a bool, and each carries only the name of the
  prompt it answers. A field can be defaulted or mis-parsed into meaning yes; a
  mistyped shape name is a gate refusal, and a defaulted `prompt` names none.
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
                                               defer the answer, name the prompt N
Weaver    AuthorityPrompt{prompt N} ----------> operator
operator  ApproveAuthority{prompt N} ---------> Weaver
                                               sender == operator seat?
                                               N names the pending prompt?
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

## A decision names the prompt it answers

One pending request does not make a decision that names nothing safe. The
request pending when a decision arrives is not always the one its operator was
shown: a key repeats, a press is doubled, a send arrives late, or the session
asks again the moment it hears `granted`. A contentless second approve would
then approve the session's next ask, which nobody saw.

So every `zen.AuthorityPrompt` carries `prompt`, a number its Weaver gives it
and never gives another prompt, starting at 1; and `zen.ApproveAuthority` and
`zen.RefuseAuthority` carry the `prompt` they answer. The Weaver acts on a
decision only when it names the prompt pending now. A decision naming another
prompt (one already decided, or one never put), naming none (`prompt` 0, or a
version 1 decision, which has no field), or arriving with nothing pending
installs nothing, leaves the pending prompt as it was, and is answered to the
seat with a `zen.Refused` saying why; the session hears nothing. A Weaver that
has given every name an `int64` holds refuses further requests rather than
reuse one.

The name says *which* request, never *who*: a decision is still the seat's only
by bus stamp, and a stranger naming the right prompt is refused as a stranger.
Names count per Weaver, so two Weavers can each have a prompt 1; a seat serving
more than one Weaver tells them apart by the prompt's bus-stamped sender.
`zen.RevokeAuthority` names nothing on purpose: it acts on what is installed,
never on a request.

## Policy, stated

| question | the Weaver's answer |
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
| decision naming another prompt, or none | refused to the seat in words; nothing is installed, and the pending prompt stands as it was |
| session dies while pending | nothing installed, pending cleared, operator told; WeaveIds are never reused, so nothing can inherit it |
| Weaver dies after granting | **installed authority stands.** A grant is not a lease |

## No shadow state

The Weaver stores the pending human question and its prompt's name, the deferred
answer right, the last name it gave a prompt, the operator seat and the
capability — and nothing about authority. Every time it
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
terminal console, a graphical pane, and anything else that ever displays an
`AuthorityPrompt` inherit it. A sanitizer in one renderer protects one renderer.
The field is named `requester_says` so the attribution is at the point of
reading, not in documentation the operator does not have open.

ASCII-only is a real limitation: a non-ASCII purpose arrives escaped rather
than translated.

## A prompt is a send, and a sender cannot observe send fate

This inherits the standing seam
([sender cannot observe send fate](known-seams.md#sender-cannot-observe-send-fate))
and it has two visible consequences here, neither of which the Weaver papers over:

- **The session learns it was denied only if it asked to be told.** A session that
  accepts `zen.DispatchRefused` hears, by exact attempt, that an action was refused
  before any handler ran
  ([sender-visible dispatch refusal](messaging.md#sender-visible-dispatch-refusal)),
  and may then ask; a [terminal session](terminal.md) does exactly that. One that
  does not accept it is told nothing, and the refusal shows only on the host's tap.
  Either way the Weaver is not what connects the two: the session asks on its own.
- **The Weaver cannot know its prompt arrived.** If the operator seat is
  misconfigured, unreachable, or dies, the request simply stays pending and the
  session simply keeps waiting. Nothing is lost or corrupted — the deferred
  answer is bounded, held by one slot, and reclaimed when either party dies — but
  nobody is told. An operator surface that must guarantee delivery of a prompt
  needs a mechanism Loom does not have.

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

The Weaver governs **Loom message authority**: `LiveAuthority` delegation and
revocation for one governed session's speech. It does not govern process memory
safety, hostile in-process native code (a `dlopen`ed weave shares this address
space — see [dynamic weaves](../guides/dynamic-weaves.md)), kernel escape,
anything outside Loom, network authentication, or OS login identity.

There is **no persistence**: restarting anything forgets every approval. There is
**no allow-once** — Loom has no consumable grant, and the prompt says so in
words. There is **no time expiry**. There is no remote authentication: the
operator seat is a WeaveId a host chose, not a person.

The operator seat needs no special powers. A `ConsoleEngine` holding
`allow_any()`, host-wired discovery and the tap can be one, and so can the
[terminal session](terminal.md)'s seat: an ordinary participant with four rules
and none of those three powers, which drives this whole workflow. The **human**
half of the seam remains: a WeaveId is not a person.

## Running it

`zen-weaver-demo` ([`src/weaver/weaver_demo.cpp`](../../src/weaver/weaver_demo.cpp))
boots an operator console, a Weaver, one governed session and one service. Its
REPL is deliberately shape-agnostic — there is no `approve` or `grant` command;
the operator composes `zen.ApproveAuthority` the way it would compose any
registered shape, through the ordinary gated send path, and names the prompt it
was shown by a reference to that prompt's own field: `send <weaver>
zen.ApproveAuthority 2 prompt=$m1.prompt`.

PROVEN BY — [`include/zen/weaver/`](../../include/zen/weaver/),
`tests/test_weaver.cpp` (suite `weaver`), the `grant` suite, and the entry
`terminal_repl`, where a decision is typed against the prompt shown.
