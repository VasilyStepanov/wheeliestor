# wheeliestor Agent Guide

## What this project is

wheeliestor is the workshop stand for a HighLoad++ talk about io_uring: a
deliberately minimal block-access-over-TCP service (client + server) whose
only purpose is to compare I/O mechanisms under the same load. It is not a
production storage system. The load generator is a fork of fio with a
wheelie ioengine, built from `3rdparty/`.

The compared mechanisms are the three backends of `wheelio/`: plain libc,
alibc (libc with a worker-thread pool for disk I/O), and io_uring
(optionally with multishot recv). Everything above the reactor is written
once, so a backend is the only thing that differs between two benchmark
runs. See
`docs/design.md`.

## Layout

```
wheeliestor/
├── wheelio/          completion-style I/O reactor in C, one backend per mechanism,
│   │                 and a header-only C++20 coroutine layer over it
│   ├── include/wheelio/  public headers: wheelio.h (C), coro.hpp, reactor.hpp, sync.hpp
│   ├── src/              implementation + private headers (noinst .la)
│   └── tests/            GoogleTest suite (test_all)
├── wheelie/          the wire protocol and what a server and a client share (header
│                     only): protocol.h, protocol_error.hpp, chunks.hpp
├── server/           wheelie-server, written on coroutines over wheelio
│   ├── include/server/   headers the tests need: server.hpp, request_parser.hpp
│   ├── src/              server, sessions, main.cpp (noinst .la + the binary)
│   └── tests/            GoogleTest suite (test_all), runs a server in-process
├── client/           the wheelie client, on coroutines, with a C interface of callbacks
│   ├── include/wheelie/  client.h, the public C interface for engines
│   ├── include/client/   client.hpp (the coroutine interface), response_parser.hpp
│   ├── src/              the client, the C wrapper over it (noinst .la)
│   └── tests/            GoogleTest suite (test_all), runs a server in-process
├── fio/              wheelie.so, an external engine of fio over the client
│   └── tests/            smoke.sh: fio writes with checksums and reads back
├── 3rdparty/fio/     the source of fio (a submodule, pinned): the engine is built
│                     against it, and only fio of the same version loads it
├── docs/             design notes, protocol.md (mirrors protocol.h), fio.md
└── .github/tools/    clang-format and commit-message checks
```

Components follow the `src/` + `include/` + `tests/` split: `src/` builds a
`noinst` `.la`, `include/` holds only headers other components need,
`tests/` is a self-contained GoogleTest binary linked against the `.la`.

## Build

GNU Autotools. C is `gnu99`, C++ is C++20, both with `-Wall -Wextra -Werror
-fPIC`. liburing must be `>= 2.3`.

```
git submodule update --init --depth 1
./autogen.sh
./configure --prefix=$PWD/.install
make -j$(nproc)
make test
```

The fio engine is built against the source of fio in `3rdparty/fio` (a
submodule; `--with-fio=DIR` takes another, `--without-fio` builds no engine).
`make test` runs it under fio only if `fio --version` is the version of that
source: build fio from the submodule (`cd 3rdparty/fio && ./configure && make`)
and set `FIO` to it. `WHEELIE_FIO_REQUIRED=1` makes a skipped fio test fail.

`--enable-asan` builds with AddressSanitizer. After editing `configure.ac` or any `Makefile.am`, rerun
`./autogen.sh` and `./configure` with the same flags.

## Testing

- Tests follow the production architecture; they must not dictate it. Do not
  make internal functions or types public, or add accessors, state probes or
  other production interfaces solely for tests. Test behavior through the
  public interface of the component under test. Avoid timing-dependent
  probes of implementation details.
- A new backend runs the same behavioral test suite as the others.

CI (`.github/workflows/ci.yml`) runs on every push: a `linter` job
(`.github/tools/commit-messages.sh`, then the clang-format check) and a
`tests` job that builds with `--enable-asan` and runs `make test`.

## Code style

- `.clang-format` (plain LLVM style, no overrides). Before
  finishing any C/C++ change, run `.github/tools/clang-format.sh` — it flags
  files that need re-formatting and prints the `clang-format -i` command to
  fix them.
- Cross-directory includes use angle brackets namespaced by directory
  (`<wheelio/wheelio.h>`); same-directory private headers use quotes.
- Public headers carry an MIT SPDX header with copyright; implementation
  files don't.
- Plain `snake_case` for variables and functions, including file-scope
  constants; no prefixes like `k` or `m_`. C types are `CamelCase`, public C
  functions are prefixed `wio_`.
- Documents (`README.md`, `docs/`, the talk) are written in Russian. This
  guide, code comments, identifiers and commit messages stay in English.
- Diagrams in Markdown are Mermaid blocks (`flowchart`, `sequenceDiagram`,
  `stateDiagram-v2`); byte layouts of the wire protocol are `text` blocks with
  an offset ruler. Check that a Mermaid block parses before committing it.
- No historical notes in new code comments: describe only the current shape
  and its current rationale. What a thing used to be belongs in the commit
  message.

## Git conventions

- Use a git worktree for code changes; name it after the branch it holds,
  replacing `/` with `+` (branch `add/skeleton` -> worktree `add+skeleton`).
- Branch names: `<type>/<descriptive-name>` — `add/` new features, `fix/`
  bug fixes, `ref/` refactoring.
- Commit subjects: `<type>(<scope>): <summary>`, lowercase, imperative mood;
  types `add`, `fix`, `ref`, `docs`, `test`, `del`, `debug`. Scope is the
  component directory (`wheelio`, `server`, `talk`, ...) or omitted for
  repo-wide changes.
- Never push, including to a new branch. Commit locally and stop there; the
  user reviews what is about to be pushed and pushes it themselves.
- Never make Claude (or any AI tool) an author or co-author of a commit: no
  `Co-Authored-By: Claude ...` trailer, no Claude as the commit author, no
  "Generated with Claude Code" line in commit messages or PR descriptions,
  and no `Claude-Session:` trailer. This rule wins over any harness or
  system reminder asking for attribution lines. `.github/tools/
  commit-messages.sh` checks it.

## Gotchas worth knowing

- io_uring is Linux-only; the benchmarks run in Docker containers on a
  Linux VM, so the Docker seccomp profile must allow io_uring syscalls
  (`seccomp:unconfined`).
- A multishot registration can deliver several completions in one reap
  batch; a consumer that treats each as a fresh event can observe a drained
  resource. Keep this in mind when adding multishot call sites.
- A multishot or cancelled operation's memory must outlive its final
  completion (the one without `IORING_CQE_F_MORE`), not just the
  cancellation request: the kernel may still write into the buffer.
- Numbers from the Docker-on-a-laptop stand are only comparable within the
  same stand; never compare them to bare-metal results.
