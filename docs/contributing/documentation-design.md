# The documentation's design

The design every page and every comment in Loom and in
[Zengine](https://github.com/Krealsion/Zengine) is written to, and what a change to the text is
judged against. It is the guide for all new writing. Text written before it is brought to its
shape a folder at a time, and a folder brought to it is held to it by the checks.

## What it is for

Each answer lives in one place, where its reader would look first, in words a person understands
on the first reading. The two repositories share one shape. Loom's laws and decisions already
stand among its public pages, and its comments point at those pages. Zengine's pages for people
stand beside a layer written for agents (`AGENTS.md` files and its
[agents folder](https://github.com/Krealsion/Zengine/tree/main/agents)), which this design folds
into its public pages.

Smaller is encouraged, and never forced. Text that says each fact once, in words a reader takes in
more easily, is a win even where its bytes hold or grow. A change that can make a page both
clearer and smaller does.

## Principles

1. The code is the truth, and it documents itself first: the names of its functions, types and
   variables say what it does. Comments are the next layer, and pages the one after: each says
   what the layer before it cannot.
2. Plain language: clarity first, cleverness second, sounding smart never. Every comment and every
   page is written in complete sentences, with at least a noun and a verb. "Not persisted either"
   fails, because the reader must guess what is not persisted; "The undo history is not persisted
   either: it lasts one session" passes.
3. A reader understands a comment from its line and the code around it. When they cannot, the
   comment says more. Clear words cost a few bytes and save misunderstandings.
4. A comment earns its place by saying why, what the code must not become, or what cannot be seen
   from where it sits. A comment that restates the code goes.
5. High signal, low noise. Simplicity is the ultimate sophistication: a well-chosen sentence
   carries more than its words, and wise text tells the reader what a fact means for what they are
   about to do.
6. One answer has one owner, placed where its reader would look first. Every other place links to
   it.
7. Contributors are one reader, human or agent. What an agent needs to work here is in the
   contributor's guide, where a person would want it too. What only one harness needs stays out of
   the repositories, and what any harness needs is [the recommended setup](recommended-setup.md).
8. A page earns its place by giving tangible value: as a leaf the reader needs, or as a frame that
   orders what they learn next. Nothing joins the pages that gives neither.
9. Learning is meant to be a pleasure, because interest is what makes a person learn and keep
   learning. Each answer opens onto the question a reader will want next, so their interest leads
   them on to the power they came for. The pages teach without getting in the way: never required,
   there when wanted and silent when not.
10. The required path is short: the contributor's guide and the code being changed. Everything
    else is reference, read when a question needs it.

## Where two principles meet

- **3 and 6.** A rule about one declaration is owned by the comment on that declaration, and no
  page repeats it. A rule that spans several files is owned by a numbered law on a public page,
  and a comment names it in a sentence that says the rule.
- **2 and 5.** A label that only names a unit, an argument, a closing brace or a numbered step of
  a test case may stay a noun phrase: `///< records held right now, in all windows`. A comment
  that explains, warns or forbids is written in sentences.
- **3 and the bytes.** Clear comments are often longer. A comment is judged by what its reader
  understands, and its growth is counted openly, by kind, never hidden in a total.
- **6 and a page's own reader.** What a page's reader needs stays on that page. A maker reads a
  weave's manual inside Zengine's Workshop and cannot open a header, so the manual keeps the facts
  a maker needs, as labels where labels will do, and links the contributor's detail.
- **6 and two readers.** A maker and a contributor look for one fact in different places. A
  second copy stands only where a check holds it equal to its owner; better still, the page draws
  the fact from its owner.
- **7 and 10.** Joining the agent's material to the contributor's guide makes the guide long. Only
  what serves any task is required reading, and the rest is reference.

## The shape

- **Law.** A rule that spans files is a numbered law on a public page in its folder, and a comment
  names it inside a sentence: "...and the drain has no turn or time cap (MSG-09)". A rule spans
  files when other files rely on it, not only when its proof does: a rule about one class that
  guides and tests cite by number stays a law, unless every citation changes in the same change. A
  rule about one declaration that nothing else relies on lives in that declaration's comment. A
  list of the files that prove a law, which a check reads and a person does not, is a manifest
  under `tests/`, not a page. A comment that is only a law id and a path goes.
- **A folder's front page.** One page, its `README.md`: what the folder is, what its parts are, and
  a last section, "Working on it", on its laws, its suites and the host's side of it. A folder has
  no `AGENTS.md` of its own.
- **The start file.** Each repository's root `AGENTS.md` is a short start file that an agent's
  harness loads unasked. It holds no rule of its own: it sends its reader to `CONTRIBUTING.md`,
  then to the front page of the folder the task touches, and asks it to read both again after its
  session is summarized or compressed.
- **The contributor's guide.** `CONTRIBUTING.md` holds what any task needs first, about 8 KB, and
  reference after it.
- **The shared half.** What the two repositories' guides share is owned by
  [Loom's guide](../../CONTRIBUTING.md), and Zengine's links it by address.
- **The recommended setup** is [a page among Loom's contributor pages](recommended-setup.md). It
  names kinds of machine (cores, memory, disk), never one person's, and says how a harness
  reaches the start file.
- **Frozen history** stays in the tree, in an area of its own whose `README.md` says why it exists:
  it is in no total, and on no required path.
- **No dates and no authors.** A page or a comment says what holds, not when it was decided or by
  whom; Git's history keeps when it changed. A record of a time, such as an audit under
  `docs/evidence/`, keeps the dates and names it was written with, and frozen history stays as it
  was written.
- **What the design binds.** Every page and comment; what the program says (a notice, a refusal, a
  label, a `--help`); test names. Commits, issues and pull requests are bound lightly, to plain
  language: they hold the story of a change, and no page or comment tells it.

## The checks

The checks fall into two kinds.

- **Checks that catch a mistake hold**: a link that resolves, a name that names something, a
  copied value equal to its owner, a change proven comment-only, a manual's sections held to the
  messages its weave accepts, a private word or id.
- **Checks that only cap size are paused**, so the text can be reshaped freely: the page budgets,
  the start file's bound, the limit on a comment block's lines, and a manual's section bounds. A
  cap comes back, if at all, once the rewritten text has shown what each kind of page needs.

The census counts a class's or a file's lines of code only, so a comment never pushes one past its
bound. A check of the old shape holds a folder not yet brought to this one, and holds a folder
brought to it to this shape. New checks are wanted for a bare pointer, shouted words, a page no
page links to, and a path that should be a link. Plain words, a comment that stands alone and a
complete sentence stay with review.

## Measures

A change to the text is measured by kind, never by one total: the bytes of each kind of page, of
the agent's material and of comments; the places a fact is said; the bytes a reader must read
before a task can start. A change whose text grows says what its reader gets for it.

## The comment rule, taught

One real passage, before and after its rewrite: `TimerWeave::fire_due`, in Zengine's
[timer weave's header](https://github.com/Krealsion/Zengine/blob/main/timer/timer_weave.hpp). A
real passage shows each comment guarding something a change could break, where a made-up one
would have to fake it.

```cpp
    /// Fires every counting timer due by the time the wait last sent, in table order.
    void fire_due(loom::Mail& mail) {
        ...
        // An indexed loop, on purpose: firing adds to the bus's queue but never to `entries_`, and
        // no ask can arrive in the middle of this loop, because dispatch runs on one thread.
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            ...
            if (e.repeat) {
                // A timer that is late by less than its delay keeps its rhythm: its next beat is
                // one delay after the beat it was due. One later than that starts again a delay
                // from now. Either way it never fires twice to catch up, so a host that stalled
                // gets one beat when it resumes, not a flood of stale ones.
                *e.next_due = add_clamped(*e.next_due, e.delay_ms);
                if (*e.next_due <= now) {
                    e.next_due = add_clamped(now, e.delay_ms);
                }
```

- **The summary** says what the name cannot: whose time "due" is measured against, and the order.
  It was "Fire every anchored timer due by the time last handed": the same facts in two words of
  the file's own. "Fires the due timers" would restate the name.
- **The warning** says what the code must not become. A tidy reader turns an index loop into a
  range-for; the comment says why the loop is safe as it stands and what a change must prove
  first. On a page it would be where the person changing the loop never looks.
- **The rule for a late timer** is the comment most worth having: the two branches look like a
  bug, and only the comment says they are a rule. It was right and hard to read ("Hold the lattice
  when merely late; never burst to catch up"). A rewrite that is easy to read can still be wrong:
  one that dropped "merely" would drop the second branch with it, and a reader cannot catch that,
  because a reader can say what a comment tells them, not whether the code agrees.

So: a comment says why, what must not be, or what cannot be seen from its line; it is a complete
sentence in plain words; a clearer one is often longer; and **a comment rewritten is a claim about
the code, checked against the code, branch by branch, before its words are polished.** A reader's
understanding shows the words are clear, never that they are true.

A comment that exists only because a name is poor is a sign the name should change. Where the
rename is beyond the change at hand, the comment is made clear and the name is left for a change of
its own.

## Readers, where they start, and what they ask

| Reader | Starts at | Asks |
|---|---|---|
| A maker using Zengine's Workshop | the package's read-me before the first start; then Workshop's manual, inside Workshop | What do I need to start it? How do I make this? What did that just do? |
| A weaver running Loom's host | [the documentation's front page](../README.md) | How do I start it? What may run in it? How do I keep it running? |
| An agent using Workshop or a host through a session | the tool package's guide, and the manual a role answers | What can I do here? What did that just do? |
| A stranger using Loom or Zengine as a library | the repository's `README.md` | What is it? Can I trust it? How do I build my first weave? |
| A contributor, human or agent | `CONTRIBUTING.md`, which `AGENTS.md` points to | Where does this live? Why is it this way? What must I not break? How do I prove my change? What will it be judged against? |
| A reviewer | this design | Does this change hold to the design? |

## Kinds of text, and what each holds

- **Names and code** say what happens.
- **Comments** say why, what the code must not become, and what cannot be seen from here.
- **Tests** are code and comments too. A case's name says what it proves, in a sentence, one claim
  a name, and its comments say why the case has its shape, so that no one simplifies it into a
  case that passes for the wrong reason. A comment on a single check says what the check rules
  out, or nothing.
- **What the program says** is text a maker reads, written in the code and held to principle 2.
- **A folder's front page** says what the folder is, what its parts are, and how to work on it.
- **Guides, references and manuals** serve makers and strangers, held to these principles.
- **Examples and tutorials** are code and steps a reader copies. A check compiles or replays them.
- **The glossary** says each of the project's own words once: a weave, a weaver, a role. A page
  explains such a word where a reader first meets it, or links the glossary. Loom's
  [terminology](../terminology.md) owns every word the two repositories share; Zengine's glossary
  holds only its own, and links Loom's. A word Loom's glossary holds is the proper name and is
  kept. A thing is called by its proper name: a word the code uses in a sense of its own gets a
  proper name the glossary holds, or gives way to plain words for what happens, longer or not.
  - "Says", of a weave, becomes "sends a message".
  - "Hand", for one source of presses and keys at a window, is the **input actor**, after the
    code's own `InputActor`. Its other senses take plain words: the host's wait sends the time,
    and an area is given to an owner.
  - "Room" gets no proper name. Each use says what it means, laws included: the area a window
    gives a pane to draw in, the bounds a layout is solved in, the part of Workshop no pane covers,
    or plain space.
- **Laws** stand on public pages in each folder, numbered, each a rule that spans files.
- **The contributor's guide** holds how to work on the code: `CONTRIBUTING.md`.
- **The recommended setup** says how we recommend a contributor work, human or agent: a small start
  file an agent reads first, and the safety net it gives after a compression; a separate clone to
  work in, and where builds go; which lanes need what hardware, and how to run them on a smaller
  machine.
- **The architecture** says once each principle of the code that spans the whole tree.
- **Decisions** say why a choice was made. A reader reaches one from the place that needs it, by a
  link, and no one is required to read them.
- **Issues, commits and pull requests** hold what a change was asked to do and the story of what it
  did.
- **Mechanical truth** is the manifests and the checks that hold them. A list a check reads, such
  as the files that prove a law, is a manifest and not a page.
- **Frozen history** is kept as it was written, in its own area.
