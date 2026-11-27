# Security

Report a security flaw privately, never in an issue, a pull request or a discussion: those are
public, and a flaw told there is known before it is fixed. Use GitHub's private vulnerability
reporting, **Report a vulnerability** on this repository's
[Security tab](https://github.com/Krealsion/Loom/security), which opens an advisory only the
maintainers and you can read. The fix is made there, and the advisory is published once it has
landed.

Say what the flaw lets someone do, how to reproduce it, and the commit you found it at. A test that
shows it is the most useful report there is.

What Loom contains, and what it deliberately does not, is written down. A weave loaded into the
host's process shares its memory, so code that reaches past its grant there is the trust level
[capabilities](docs/reference/capabilities.md) and
[dynamic weaves](docs/guides/dynamic-weaves.md#what-loading-it-in-process-means) state, not a flaw;
the operating-system sandbox is built against abuse, not escape; and the bridge carries no
transport security ([bridge](docs/reference/bridge.md)). A participant that can do more than those
pages say it can is a flaw, and so is anything that breaks a law in
[`docs/laws/`](docs/laws/README.md).
