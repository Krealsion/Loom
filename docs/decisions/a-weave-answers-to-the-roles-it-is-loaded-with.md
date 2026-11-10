# A weave answers to the roles it is loaded with

**Context.** A role is a name another party addresses and trusts. Applications built on Loom
met services that needed a second name: a middleman fronting the roles of an ecosystem, and a
host whose own role must sit beside a contract's neutral name. With one role per weave the only
ways to give a service a second name were a second weave relaying to the first, or a rename of
everything that names the first. The table that routing, role authorship and role claims read
already maps each role to its one holder; what held a weave to one role was its own record and
the calls that bind it.

**Decision.** A weave may hold several roles, each still with one holder
([MSG-04](../laws/messaging-laws.md#msg-04--role-addressing-is-destination-not-authorship)).
The roles are bound when the weave is registered or loaded, as a set, judged whole and bound
all or none, at most `kMaxRolesPerWeave` of them. A holder may release one role at any time and
keeps the rest; a release is the holder leaving that role, with the consequences the holder
leaving by removal has for it. `roles_of` says what a weave holds now, in binding order. A
replacement moves one role, so until a replacement can move a set it refuses an incumbent holding
several, in words, before anything moves
([MSG-13](../laws/messaging-laws.md#msg-13--a-weave-answers-to-the-set-of-roles-it-was-bound-to)).

**Alternatives considered.**
- *No: a second name is a second weave that relays to the first* — rejected: a relay per extra
  name, a second place where an answer is decided, and a hop in every conversation, for a
  service that is one state.
- *No: rename the service* — rejected: it breaks every party that used the old name, and it is
  repeated each time a service needs another.
- *Bind and release while running, both* — rejected for binding: who holds a role would become a
  fact a weave changes as it runs, which every role law rules out by construction, and a live
  bind would need an authority, refusals and an answer to binding during a replacement of its
  own. Releasing keeps every law true: it is the holder leaving, as removal already is.
- *The weave declares its roles in its manifest* — rejected: the host's choice would move into
  the artifact, and the admission policy would have to second-guess it.
- *Keep `role_of`, answering the first role bound, or nothing when there are several* —
  rejected: a silent wrong answer or a silent empty one for every holder of two.
- *A replacement moves only the named role* — rejected: the incumbent is sealed for retirement as
  the role moves, and a sealed weave holds no role, so the rest would be left on a retiring
  weave or unheld. The whole set moves, or nothing does.
- *Per-role grants or per-role accept-sets* — not taken: the grant stays the weave's, so a weave
  holding two roles sends with one grant. Whether a second role is right is a judgement the
  application owns; Loom states the mechanism.

**Consequences.** One service answers to the names its parties use, with one state and one
grant. `register_weave`, `Kernel::load`, the isolation host's `mount`, `LoadLibrary` v3,
`zen.LoadWeave` v2, a boot row's `roles` and the console's `start` take a set; the older
`LoadLibrary` v2 and `zen.LoadWeave` v1 are read as a set of one. `ListLibraries` and
`zen.ListLoaded` say `name@r1+r2`. `AdmissionRequest::roles` asks a policy about the set.
`mail.release_role`, `Switchboard::release_role`, `Kernel::release_role` and the control door's
`ReleaseRole` let a holder leave one role; a loaded library has no release verb across the
dynamic ABI and releases through the door. A prepared replacement, an admission, a direct
commit and the Weave Manager's swap refuse an incumbent holding several roles until a
replacement can move a set.

**Current laws supported.**
[MSG-04](../laws/messaging-laws.md#msg-04--role-addressing-is-destination-not-authorship),
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit),
[MSG-13](../laws/messaging-laws.md#msg-13--a-weave-answers-to-the-set-of-roles-it-was-bound-to),
[SENSE-05](../laws/sense-laws.md#sense-05--reading-is-authorized-and-the-repository-is-bounded),
[KERN-03](../laws/kernel-laws.md#kern-03--role-truth-is-derived-never-cached).

**Evidence.** Suite `role_authorship` (a set reached, authoring and claiming as each role; a set
refused whole; release and removal; a release aborting a replacement; a two-role incumbent's
replacement refused), suite `manager` (a library loaded into a set, the door's list and release,
the older loads, a two-role holder's swap refused), suite `host_policy` (a boot row's `roles`).
