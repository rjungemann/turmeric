# Archive Cleanup Summary

Cleanup of docs/archive completed successfully. Below is a summary of changes.

## Overview

- **21 active planning documents** remain in `docs/archive/` (current phase work)
- **6 new user guides** created in `docs/guides/` (extracted from archive)
- **5 completed/superseded documents** moved to `docs/archive/history/` 
- **5 obsolete documents** deleted

## New User Guides Created

| Guide | Purpose | Extracted From |
|-------|---------|-----------------|
| [effects-system-guide.md](guides/effects-system-guide.md) | Algebraic effects, dependency injection | effects-plan.md |
| [async-await-guide.md](guides/async-await-guide.md) | Async/await with fibers | async-await-plan.md |
| [threading-guide.md](guides/threading-guide.md) | OS threads, Arc, Mutex, Atomic | thread-safety-and-primitives-plan.md |
| [logic-programming-guide.md](guides/logic-programming-guide.md) | Backtracking, logic programming | backtracking-cloneable-continuations-plan.md |
| [checkpointing-guide.md](guides/checkpointing-guide.md) | Serializable continuations, persistent workflows | serializable-continuations-plan.md |
| [stm-tutorial.md](guides/stm-tutorial.md) | Software transactional memory | stm-plan-2.md |

**Bonus:** [error-handling-rationale.md](design/error-handling-rationale.md) created in docs/design/

## Files Moved to History

Completed phases and superseded decisions, kept for reference:

- `cmake-migration-plan.md` — CMake migration completed (Phase 16)
- `deferred-tasks-phase7-phase11.md` — Phases 7–11 complete
- `hamt-feasibility.md` — Feasibility analysis (decision: include in v2)
- `deferred-prerequisite-decisions.md` — Design decisions now resolved
- `higher-kinded-types-plan.md` — HKT architecture (mostly shipped in Phase H3)

See [docs/archive/history/](archive/history/README.md) for context.

## Files Deleted

Fully superseded or obsolete:

- `cellular-automata-implementation-status.md` — Working PoC shipped; examples/cellular-automata.tur is source of truth
- `cellular-automata-prerequisites.md` — Prerequisites satisfied (Phase H3 typeclasses shipped)
- `cellular-automata-comonad-tutorial-plan.md` — Plan complete; see guides/cellular-automata-comonad-tutorial.md
- `turmeric-plan.archive.md` — Superseded by current turmeric-plan.md
- `stm-plan.md` — Kept stm-plan-2.md (more detailed); deleted original

## Active Planning Documents Retained

21 planning documents for phases in flight or under design:

**Current/Next Phases:**
- deferred-tasks-T19-T21.md (Phase 19–21 tracking)
- thread-safety-and-primitives-plan.md
- fiber-asm-ctx-plan.md

**Planned Features:**
- async-await-plan.md
- effects-plan.md
- stm-plan-2.md
- hamt-plan.md

**Design Explorations:**
- gadts-plan.md
- numeric-types-plan.md
- module-system-plan.md (+ alternative)
- signal-processing-arrows-plan.md
- unsafe-operations-plan.md
- And 6 others (see [docs/archive/README.md](archive/README.md))

## Documentation Improvements

### New READMEs

- [docs/archive/README.md](archive/README.md) — Explains active planning documents and cross-references to guides
- [docs/archive/history/README.md](archive/history/README.md) — Context for historical documents
- [docs/guides/README.md](guides/README.md) — Organized index of all user guides

### Cross-References

All new guides include:
- Clear links back to related archive planning docs
- Links to related guides and design documents
- "See Also" sections for navigation

## Directory Structure After Cleanup

```
docs/
├── guides/
│   ├── README.md (NEW)
│   ├── effects-system-guide.md (NEW)
│   ├── async-await-guide.md (NEW)
│   ├── threading-guide.md (NEW)
│   ├── logic-programming-guide.md (NEW)
│   ├── checkpointing-guide.md (NEW)
│   ├── stm-tutorial.md (NEW)
│   ├── c-integration-guide.md
│   ├── cellular-automata-comonad-tutorial.md
│   ├── custom-effects-tutorial.md
│   ├── error-handling-guide.md
│   ├── hkt-guide.md
│   ├── minikanren-tutorial.md
│   ├── module-system-guide.md
│   ├── snake-game-tutorial.md
│   └── test-runner-contract.md
├── archive/
│   ├── README.md (NEW)
│   ├── 21 active planning documents
│   └── history/
│       ├── README.md (NEW)
│       └── 5 historical documents
├── design/
│   ├── error-handling-rationale.md (NEW)
│   └── [other design docs...]
└── [other docs...]
```

## Next Steps

- Guides are ready for use as reference documentation
- Archive README helps navigate planning documents
- Developers should reference guides for user-facing features, archive for design discussions
- Historical documents are now clearly separated, reducing noise in active planning

---

**Completed:** May 13, 2026
