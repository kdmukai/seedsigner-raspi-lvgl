# Building the .so against an alternate seedsigner-lvgl-screens tree

The normal build compiles the screens sources from the pinned submodule at
`sources/seedsigner-lvgl-screens`. During development you often want to build the Pi
extension against a *different* screens tree instead — a sibling clone that carries
uncommitted work, or a commit that is ahead of the submodule pin — without touching the
submodule. `run_build.sh` supports this through two environment overrides, but there are
constraints that aren't obvious from `setup.py` alone.

## The two independent overrides

`setup.py` reads `SEEDSIGNER_LVGL_SCREENS_DIR` (screens sources root) and, separately,
`LVGL_ROOT` (the LVGL checkout). `run_build.sh` **always** passes *both* into the
container as `-e` vars, each defaulting to a path under the submodule:

- `SEEDSIGNER_LVGL_SCREENS_DIR` → `.../sources/seedsigner-lvgl-screens`
- `LVGL_ROOT` → `.../sources/seedsigner-lvgl-screens/third_party/lvgl`

Because they default independently, overriding only `SEEDSIGNER_LVGL_SCREENS_DIR` leaves
`LVGL_ROOT` pointing at the **submodule's** LVGL. That is fine *only when both trees pin
the same LVGL commit* (the common case — LVGL rarely moves). If the alternate tree pins a
different LVGL, override `LVGL_ROOT` too, or you compile screens headers against the wrong
LVGL.

## The mount constraint (the easy trap)

The build runs in Docker with `WS_ROOT` mounted at `/workspace`, and `WS_ROOT` defaults to
the **parent** of this repo (`$repo/..` — the shared workspace dir holding all four repos).
The override path must resolve to a **container** path under `/workspace`, so the alternate
tree must live inside that mounted parent dir. A sibling clone at
`$WS_ROOT/seedsigner-lvgl-screens` becomes `/workspace/seedsigner-lvgl-screens` in the
container:

```
SEEDSIGNER_LVGL_SCREENS_DIR=/workspace/seedsigner-lvgl-screens ./run_build.sh
```

Pass the **container** path (`/workspace/...`), not the host path. A tree outside `WS_ROOT`
isn't mounted and the build can't see it.

## Why it's safe

setuptools writes object files and the final `.so` into this repo's build dir (and
`src/`), never into the screens source tree. So pointing the build at a live working tree
does **not** clobber that tree's files — it only reads `components/seedsigner/*.cpp`,
`components/nlohmann_json`, and the LVGL sources. Untracked docs/notes in the alternate
tree are ignored (the build globs specific source dirs).

## When to reach for it vs. bumping the submodule

- **Override** for a quick device test of uncommitted or ahead-of-pin screens work — no
  submodule mutation, fully reversible (just drop the env var).
- **Bump the submodule pin** (checkout the target commit under the submodule, `git add`
  it, commit) when the screens state is settled and should become a committed build input
  that CI reproduces. CI builds from the submodule with no override, so a pin that a clean
  checkout can't resolve (e.g. a SHA only on a fork the `.gitmodules` URL doesn't point at)
  will fail there even though a local override "worked."
