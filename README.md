# ci-metrics

Data-only branch. No source, no build, no CI.

`suite-timings-<year>.jsonl` holds one JSON object per CTest suite per CI run,
appended by `tools/ci/publish-timings.sh` on pushes to `main`. See
`docs/upcoming/suite-timing-trends-plan.md` on `main` for the schema and the
reason timings are only comparable within a fixed
(build_type, os, cc, nproc, jit) tuple.

Never merge this branch into `main`.
