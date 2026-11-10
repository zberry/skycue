# Releasing skycue

The checklist for one release, run with podman (and
`qemu-user-static` for the arm64 packages). `0.2.0` stands for the new
version throughout.

1. **Bump the version.** The one version number is the `project()` line in
   `CMakeLists.txt`:

   ```cmake
   project(skycue VERSION 0.2.0 LANGUAGES CXX)
   ```

   The daemon's `--version`, libskycue's CMake config version, the
   source tarball's name and the rpm `Version:` all read it.
   Update the date, the version and the `v=` lines of
   [installing.md](installing.md) and the status line of `README.md` to
   match.

2. **Add the changelog entries.** One new entry at the top of
   `debian/changelog` (`skycue (0.2.0-1) unstable; urgency=medium`, signed
   `skycue maintainers <skycue@users.noreply.github.com>`) and one at the top
   of `%changelog` in `packaging/rpm/skycue.spec`
   (`* <date> skycue maintainers <skycue@users.noreply.github.com> - 0.2.0-1`),
   saying the same thing. Run `scripts/test-data/fetch-test-captures.sh` first so the
   suite runs the capture tests, then the full suite. The metadata test checks that both agree with the
   `project()` version:

   ```sh
   bash tests/scripts/test_package_metadata.sh
   ```

3. **Commit the release.** Commit the bump, the changelogs and the page
   updates, and push the commit. Do not tag yet: the tag comes in step 7,
   after the packages are checked (the source tarball takes its name from
   the version in the commit).

4. **Make the source tarball.** `dist/skycue-0.2.0.tar.gz`: the release commit's tree
   plus the Leaflet and ECharts tarballs from `sources/`. The package builds
   below use it, it is what a from-source reader downloads.

   ```sh
   packaging/make-source-tarball.sh          # -o DIR for another directory than dist/
   ```

5. **Build and verify the six packages.** Dispatch the release workflow
   (`.github/workflows/release.yml`, `workflow_dispatch`) on the pushed
   release commit (its draft-release job is skipped without a tag) and
   wait for every `packages` leg to pass. While the repository is private
   the workflow builds four targets (there is no arm64 runner): the two
   arm64 packages are built locally with the recipe below and attached to
   the draft by hand (step 7). Once the repository is public the workflow
   builds all six. To do it locally instead: each target
   builds in its own container and its packages are then installed and
   checked in a fresh one
   (offline after the install: version, unit, user and directories, a
   replay served over HTTP, the map script's dry run, `--check-inputs`,
   and a build against `libskycue-dev`/`skycue-devel`). The package build
   may use the network; only the checks after the install run without it.
   Each target's packages go to `dist/<target>/`, and each verification
   ends with `verify: OK <target>`. `verify-package.sh` takes
   `dist/skycue-0.2.0.tar.gz` when `--source` is left out; `--systemd` adds
   the run of the unit under systemd (a failure with exit 226/NAMESPACE
   there is the container's limit under rootless podman, not the unit's).

   ```sh
   for t in debian12-amd64 debian12-arm64 debian13-amd64 debian13-arm64; do
     a=${t#*-}
     packaging/build-package.sh "$t" --source dist/skycue-0.2.0.tar.gz &&
     d=${t%-*}; d=${d#debian}
     packaging/verify-package.sh "$t" "dist/$t/skycue_0.2.0-1~deb${d}u1_${a}.deb" "dist/$t/libskycue-dev_0.2.0-1~deb${d}u1_${a}.deb" || break
   done
   for t in el8 el9; do
     packaging/build-package.sh "$t" --source dist/skycue-0.2.0.tar.gz &&
     packaging/verify-package.sh "$t" "dist/$t/skycue-0.2.0-1.$t.x86_64.rpm" "dist/$t/skycue-devel-0.2.0-1.$t.x86_64.rpm" || break
   done
   ```

   The arm64 builds run under emulation and compile with one job per core
   of the host; `SKYCUE_BUILD_JOBS=N` lowers that if the emulator
   crashes under load. They take much longer; Debian 12
   arm64 stands in for Raspberry Pi OS.

   **Do not tag the release until all six targets print `verify: OK`,
   the two arm64 targets included** (from the workflow or from the local
   builds). The local arm64 builds need qemu-user-static
   with binfmt handlers registered on the build host (check that
   `/proc/sys/fs/binfmt_misc/qemu-aarch64` exists). Without them the arm64
   targets cannot be built or verified, and the Raspberry Pi OS package
   would ship untested.

6. **Install on a real Raspberry Pi once.** Before the tag is pushed
   (use the package from the local arm64 build while the repository is
   private, otherwise the one from the workflow run),
   install `skycue_0.2.0-1~deb12u1_arm64.deb` on a Raspberry Pi running Raspberry Pi
   OS (64-bit) and follow [installing.md](installing.md) to the map: the
   emulated build cannot show what a real Pi does at boot, under its
   memory limits or with its own readsb.

7. **Tag and publish.** Tag the release commit and push the tag:

   ```sh
   git tag -a v0.2.0 -m "skycue 0.2.0"
   git push origin v0.2.0
   ```

   The tag run rebuilds the packages and creates a draft release holding
   the tarball, the packages and `SHA256SUMS`. Edit the notes in the draft,
   then publish it. The workflow refuses to touch a release that is
   already published. The manual path, for the arm64 packages while the
   repository is private and for a release built entirely by hand: attach
   to the draft:

   - `dist/skycue-0.2.0.tar.gz` (the source tarball)
   - from `dist/debian12-amd64/`, `dist/debian12-arm64/`,
     `dist/debian13-amd64/` and `dist/debian13-arm64/`:
     `skycue_0.2.0-1~debNNu1_<arch>.deb` and `libskycue-dev_0.2.0-1~debNNu1_<arch>.deb`
     (NN is 12 or 13)
   - from `dist/el8/` and `dist/el9/`: `skycue-0.2.0-1.el<N>.x86_64.rpm` and
     `skycue-devel-0.2.0-1.el<N>.x86_64.rpm`

   When the two arm64 packages are attached by hand during the private
   period, append their lines to `SHA256SUMS` and upload it again:

   ```sh
   sha256sum skycue_*arm64.deb libskycue-dev_*arm64.deb >> SHA256SUMS
   gh release upload v0.2.0 SHA256SUMS --clobber
   ```

   The Debian 12 and Debian 13 debs of one architecture are not
   interchangeable (each depends on its own release's Boost libraries); the
   `~deb12u1` / `~deb13u1` revision that `build-package.sh` gives them keeps
   their file names apart. The committed changelogs keep the plain `-1` revision;
   `build-package.sh` adds the `~debNNu1` suffix to the package name only.
   Debian `-dbgsym` packages that land in `dist/<target>/` are not attached.
