# Releasing skycue

The checklist for one release, run on a developer machine with podman (and
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
   saying the same thing. Run `scripts/fetch-test-captures.sh` first so the
   suite runs the capture tests, then the full suite. The metadata test checks that both agree with the
   `project()` version:

   ```sh
   bash tests/scripts/test_package_metadata.sh
   ```

3. **Commit and tag.** Commit the bump, the changelogs and the page
   updates, then tag the commit (the source tarball takes its name from the
   version, and the stack repository pins the tarball of this tag):

   ```sh
   git tag -a v0.2.0 -m "skycue 0.2.0"
   ```

4. **Make the source tarball.** `dist/skycue-0.2.0.tar.gz`: the tagged tree
   plus the Leaflet and ECharts tarballs from `sources/`. The package builds
   below use it, it is what a from-source reader downloads, and it is what
   the stack repository pins.

   ```sh
   scripts/make-source-tarball.sh          # -o DIR for another directory than dist/
   ```

5. **Build and verify the six packages.** Each target builds in its own
   container and its packages are then installed and checked in a fresh one
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
     scripts/build-package.sh "$t" --source dist/skycue-0.2.0.tar.gz &&
     d=${t%-*}; d=${d#debian}
     scripts/verify-package.sh "$t" "dist/$t/skycue_0.2.0-1~deb${d}u1_${a}.deb" "dist/$t/libskycue-dev_0.2.0-1~deb${d}u1_${a}.deb" || break
   done
   for t in el8 el9; do
     scripts/build-package.sh "$t" --source dist/skycue-0.2.0.tar.gz &&
     scripts/verify-package.sh "$t" "dist/$t/skycue-0.2.0-1.$t.x86_64.rpm" "dist/$t/skycue-devel-0.2.0-1.$t.x86_64.rpm" || break
   done
   ```

   The arm64 builds run under emulation and take much longer; Debian 12
   arm64 stands in for Raspberry Pi OS.

   **Do not tag the release until all six targets print `verify: OK`,
   the two arm64 targets included.** Their builds need qemu-user-static
   with binfmt handlers registered on the build host (check that
   `/proc/sys/fs/binfmt_misc/qemu-aarch64` exists). Without them the arm64
   targets cannot be built or verified, and the Raspberry Pi OS package
   would ship untested.

6. **Install on a real Raspberry Pi once.** Before the tag is pushed,
   install `skycue_0.2.0-1~deb12u1_arm64.deb` on a Raspberry Pi running Raspberry Pi
   OS (64-bit) and follow [installing.md](installing.md) to the map: the
   emulated build cannot show what a real Pi does at boot, under its
   memory limits or with its own readsb.

7. **Publish.** Push the commit and the tag, and attach to the release:

   - `dist/skycue-0.2.0.tar.gz` (the source tarball)
   - from `dist/debian12-amd64/`, `dist/debian12-arm64/`,
     `dist/debian13-amd64/` and `dist/debian13-arm64/`:
     `skycue_0.2.0-1~debNNu1_<arch>.deb` and `libskycue-dev_0.2.0-1~debNNu1_<arch>.deb`
     (NN is 12 or 13)
   - from `dist/el8/` and `dist/el9/`: `skycue-0.2.0-1.el<N>.x86_64.rpm` and
     `skycue-devel-0.2.0-1.el<N>.x86_64.rpm`

   The Debian 12 and Debian 13 debs of one architecture are not
   interchangeable (each depends on its own release's Boost libraries); the
   `~deb12u1` / `~deb13u1` revision that `build-package.sh` gives them keeps
   their file names apart. The committed changelogs stay at `0.2.0-1`. Debian `-dbgsym` packages that land in `dist/<target>/`
   are not attached.

8. **Update the stack's pin after tagging.** In the stack repository, point
   its pin at the published tarball:

   ```sh
   scripts/pin-source.sh skycue <asset URL> skycue-0.2.0.tar.gz
   ```

This list is meant to become a GitHub Actions workflow later; the workflow
will call the same scripts, so each step stays runnable by hand.
