# Fuzz findings

`scripts/dev/run-fuzz.sh` copies every libFuzzer artefact (crash, leak, timeout,
out-of-memory input) here as `<target>-<artefact name>`. The artefacts are
ignored by git; only this README and `.gitkeep` are tracked.

This directory is never used as a corpus: the runner reads seeds from
`tests/fuzz/seeds/<target>/` and keeps the generated corpus in the build
tree. Each finding becomes a regression test in the parser's own test file
(named `fuzz_<target>_<what>`) before the fix, and the artefact is then
deleted. See `docs/hardening.md`, "Fuzzing".
