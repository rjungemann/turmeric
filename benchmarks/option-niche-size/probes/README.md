# `(Option cstr)` eligibility probes

The four probes behind item 5 of
[sr3-option-niche-plan.md](../../../docs/upcoming/sr3-option-niche-plan.md).
They answer, by measurement rather than by reading the predicate: can the
largest pointer-payload Option population in the tree reach the niche?

Run each both ways -- `./build/tur run <probe>` and
`./build/tur --enable=option-niche run <probe>`.

| probe | question | answer |
|---|---|---|
| `probe-cstr-newtype.tur` | does a `defopaque` over `:ptr<void> :non-null` take the niche? | **yes** -- typedef emitted by default, absent under the flag, identical correct output both ways |
| `probe-roundtrip.tur` | what does a consumer pay to get a usable `cstr` back? | one `::` ascription, the same ceremony `String` consumers already pay |
| `probe-literal-zero.tur` | is the declaration enforced statically on a USER newtype? | **yes** -- `TUR-E0303` at elaboration |
| `probe-computed-zero.tur` | and dynamically? | **yes** -- legal `some` by default, ctor abort under the niche |

All four are shaped after `env/get` (stdlib/env.tur), which already tests its
raw pointer against 0 and maps null to `(none)` -- so the invariant the niche
needs is established there in source, by construction.

These are probes, not fixtures: they answer a design question and are not
wired into `tests/run.sh`. Promoting one to a fixture is the first step of
actually doing the migration item 5 describes, and should happen with that
decision rather than ahead of it.
