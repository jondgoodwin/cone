> **Closed 2026-08-21 by taking the first option.** `Cone Vault/Design/` and
> `Cone Vault/Todo/` are deleted; the repo is the only copy. Before deleting,
> every vault file was compared against the whole `design/` + `workitems/`
> corpus sentence by sentence. Nothing was lost: most files were identical or
> reworded and expanded in the repo, and the one substantial vault-only block —
> the "Overloading requirements" and implementation plan in the vault's
> `Namedef Refactor.md` — is the draft of work that shipped as PR #70 and is
> recorded in [[overload-refactor|Overload Refactor]]. `Cone Vault/Videos/` has no repo
> counterpart and stays.

`Cone Vault/` and the repository hold two copies of the same notes, and they
have already diverged.

**Not urgent.** Nothing is broken and nothing is blocked. This is recorded so
that whoever picks it up — possibly much later — does not have to work out the
situation from scratch, and so that the divergence is a known state rather than
a surprise.

## What is duplicated

| Vault | Repo | State |
| --- | --- | --- |
| `Cone Vault/Design/` — `IR Nodes.md`, `Names and Namespaces.md`, `Return.md` | `design/` — 30 notes | the three vault files are the originals the repo folder grew from; all three have been substantially rewritten and relocated since |
| `Cone Vault/Todo/` — ~48 items | `workitems/` — 48 items | **25 identical, 21 diverged, 2 repo-only** |

Measured by comparing whitespace-normalised content. Most of the divergence is
recent: the design-documentation work rewrote a dozen items and added
[[bugs|Bugs]].

## Why it is worth eventually resolving

It is the exact failure mode the design notes exist to prevent — two documents
that disagree, with nothing stating which to trust. The concrete risk is
one-directional and small but real: **an edit made in the vault to a file that
has diverged is lost**, because the repo copy is the one that ships, gets
reviewed, and is read by anyone working from a clone.

It is safe to defer because the repo copy is unambiguously the live one today.
Everything that reads these notes — the compiler's source comments, `CLAUDE.md`,
`test/run.py`, `test/tags.toml` and every `cases.toml` — points into `design/`
and `workitems/`. The vault copies are read by a person, not by tooling.

## Options

- **Canonical repo, vault as a view.** Delete the duplicated vault folders, or
  replace each with a one-line pointer. Loses Obsidian's graph and backlinks
  over these notes unless the vault is re-rooted at the repo.
- **Re-root the vault on the repo.** Point Obsidian at `cone/workitems/` and
  `cone/design/` directly, so there is one copy edited through two interfaces.
  Probably the cleanest, and costs a vault reconfiguration. Wiki-links keep
  working: they carry a kebab-cased target piped to a display name, so the
  filename is script-safe and the link still reads as prose.
- **Deliberate split.** Keep both, and write down which is canonical for which
  folder. Cheapest now, and the option that decays worst.

## What would make it urgent

- A vault edit turns out to have been lost.
- Anything starts automating over `design/` — a link checker, a docs build,
  publication to conesite — since it would then be enforcing rules against only
  one of the two copies.
- Another contributor joins, at which point "which copy is real" stops being
  answerable from memory.
