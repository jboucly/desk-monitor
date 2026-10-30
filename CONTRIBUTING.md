# Contributing

Thanks for your interest! Issues and pull requests are welcome, in English or French.

## Before you start

- For a bug, open an issue with what you did, what you expected and what happened. Add the serial log (`idf.py monitor`) for a firmware bug, and the OS for a companion bug.
- For a bigger change (new widget, protocol change), open an issue first so we can agree on the approach.
- The architecture, the threading rules and the hardware gotchas are described in [CLAUDE.md](CLAUDE.md). The protocols are in [protocol/README.md](protocol/README.md). Read them before touching `firmware/core/`.

## Conventions

- Code, comments and commit messages are in English. The UI text (screen and editor) is in French.
- Commit messages follow [Conventional Commits](https://www.conventionalcommits.org): `feat(companion): …`, `fix(sim): …`.
- An icon used anywhere must be declared in `protocol/icons.json`, then regenerate the fonts with `node firmware/tools/gen_assets.mjs`.

## Checking a change

The firmware has no unit tests. Before opening a pull request:

```sh
. firmware/tools/idf_env.sh

# UI changes: check them in the simulator first, headless screenshots work too
cmake -S firmware/sim -B firmware/sim/build -G Ninja && cmake --build firmware/sim/build
firmware/sim/build/desk_monitor_sim --no-window --mock-pc --do page=1 --do wait=500 --do shot=/tmp/page.png

# The ESP32 build catches C++ differences the simulator does not
cd firmware && idf.py build && cd ..

# Web editor
cd web && npm run build && cd ..

# Companion
cd companion/src-tauri && cargo test
```

Add screenshots to the pull request for any visual change.

## Releasing (maintainers)

Push a version tag: the [Release workflow](.github/workflows/release.yml) builds the firmware and the macOS companion, then publishes a GitHub release with the files. The version comes from the tag, so there is nothing to bump by hand.

```sh
git tag v1.1.0 && git push origin v1.1.0          # a tag with a suffix (v1.1.0-beta.1) gives a pre-release
```

To check the build without publishing, run the workflow by hand (Actions → Release → Run workflow): the files are attached to the run as artifacts.

## Platform status

The companion app has only been tested on macOS. The Windows code (metrics, shortcuts) has never been compiled, and Windows now playing is not written yet: help is especially welcome there.
