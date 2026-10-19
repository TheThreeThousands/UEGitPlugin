# GitBranchStateCache and GitLfsCache support

Status: draft for review, 2026-10-19. Branch: `support-gitbranchstatecache-and-gitlfscache` (from `dev-3k`).

## Intent

Artists wait seconds every time they move an actor in a World Partition level. Each move dirties that actor's
one-file-per-actor package, the editor asks the plugin for that file's status, and the "Updating file(s)
Revision Control status..." dialog stays up until it answers. A screen recording of the problem (on a build of
`origin/git-status-cache-server`) shows where the time goes: `CheckRemote` runs one `git log` and then one
`git diff --name-only ...origin/<branch>` per status branch. An artist on `origin/main` is compared against every
`origin/*` feature branch (the "check child branches" change), so the cost grows with the number of open branches
and is paid on every single move.

Two services were built to take this work off the workstation, and are being deployed in parallel with this work:

- **ktsu.GitBranchStateCache** (`C:\dev\ktsu-dev\GitBranchStateCache`). Keeps a blobless mirror next to the forge
  and answers "for these paths, which of these branches changed them, and to what blob" in one HTTP call.
- **ktsu.GitLfsCache** (`C:\dev\ktsu-dev\GitLfsCache`). A caching Git LFS proxy. Its locks subsystem answers
  `GET locks` (including the single-path query `git lfs locks -p <file>` makes) from a shared snapshot instead of
  the forge.

Their READMEs and the design specs under `C:\dev\ktsu-dev\GitLfsCache\docs\superpowers\specs\` are the API
reference. They were written with this plugin as the driving workload.

Brandon Meester prototyped the same idea against a different server (`GitStatusCacheServer`, branch
`origin/git-status-cache-server`). That server is not going to be deployed. This work replaces it with the two
ktsu services, on its own branch, borrowing from the prototype where useful.

## Goals

- Moving an actor no longer holds the status dialog up for more than a moment, independent of how many branches
  exist.
- Rely on vanilla git and git-lfs wherever they can do the job. The only non-git traffic the plugin adds is the
  HTTP call to GitBranchStateCache, because nothing in git answers that question.
- The plugin works exactly as today when the services are not configured, unreachable, or answer that they
  cannot help. The services are an accelerator, never a dependency.
- Remote state is at least as correct as today. Comparing blob ids is exact where the current `git log` and
  `git diff` intersection is an approximation (it exists to catch "changed then changed back").

## Non-goals

- No patched git-lfs binary and no plugin-side use of GitLfsCache's `POST locks/batch` extension. Locking stays
  `git lfs lock` through the bundled stock binary.
- No change to the 30 second background `FGitFetch` heartbeat in this pass. It still runs as today (see Deferred).
- Not porting Brandon's `/status` and `/canCheckout` client, his whole-project snapshot model, or his per-user
  "status cache server" settings UI.
- No new changelist behavior. Changelists stay disabled (TECH-296).

## Anti-goals

These would make the work a failure even if it technically worked:

- An artist blocked from editing, or shown an error, because a cache service is down or slow.
- A file reported up to date when today's code would have reported it `NotAtHead` / `NotLatest`, letting someone
  lock and edit an asset that has newer changes elsewhere.
- A forge credential written to a log, a command line, or the ini.

## Decisions made with Matt

1. **Own branch.** Work happens on `support-gitbranchstatecache-and-gitlfscache`, not on top of
   `origin/git-status-cache-server`.
2. **Vanilla git and git-lfs first** (see Goals).
3. **GitLfsCache is wired in with a committed `.lfsconfig`** in the game repository that sets `lfs.url` to the
   proxy. git-lfs reads it natively, so lock queries and object transfers go through the proxy with no plugin
   code. Writing that file and making sure artists' credentials are stored against the proxy host is a
   deployment step, not plugin code (the GitLfsCache README explains the credential-helper-keyed-by-host catch).
4. **The GitBranchStateCache URL lives in the game project's checked-in editor config**, alongside the existing
   `[GitSourceControl]` `RequiredAccessRepositoryURL` key in `GEditorIni`, so every artist picks it up without
   touching settings. A per-user override is allowed for testing only.
5. **The artist's own forge credential is used**, obtained with `git credential fill` for the `origin` remote's
   host. This reuses whatever Git Credential Manager login the artist already has. GitBranchStateCache requires
   the caller's credential (it holds none of its own and proves access with `git ls-remote`).

## Design

### Where the time goes today, per moved actor

From `RunUpdateStatus` in `GitSourceControlUtils.cpp`:

1. `git status` on the file. Local, cheap. Unchanged.
2. `UpdateChangelistStateByCommand`, a second `git status` over all of `Content/`. Changelists are disabled in this
   fork, so on a single-file update this is wasted work.
3. `CheckRemote`: `git branch --remotes --list` per pattern, `git log`, then one `git diff` per branch. This is
   the main cost and what GitBranchStateCache replaces.

Lock state is not part of this path: `RunUpdateStatus` never calls `RefreshLocks`. The first edit of an actor
also pays for `git lfs lock` in `FGitCheckOutWorker`, a forge round trip that GitLfsCache relays rather than
answers. The full `git lfs locks` listing runs on the 30 second heartbeat, and that is what the GitLfsCache
snapshot makes cheap.

### Change 1: skip the Content-wide changelist scan on file-level updates

Only run `UpdateChangelistStateByCommand` from `RunUpdateStatus` when the request includes a directory (a full
refresh), as Brandon's prototype does with its `ContainsDirectory` check. Wrap in `// [DIVERGENCE]` markers.
Independent of the services, so it should land first and be measured on its own.

The function also re-stages files in the Staged changelist (`UpdateFileStagingOnSavedInternal`). With
changelists disabled nothing reaches that changelist through the editor, so skipping it on file-level updates
loses nothing in practice. The full refresh still runs it.

### Change 2: GitBranchStateCache client in `CheckRemote`

When the service is configured, `CheckRemote` asks it instead of running `git log` / `git diff`. On any failure it
runs the existing code path unchanged for that call.

**Request.** `POST {StateUrl}/state` where `StateUrl` is the configured value, already including the version,
upstream and repository path (for example `https://branchstate.example.com/v1/ado/myproject/_git/game`). One
config value, so the plugin never has to derive the service's upstream naming from the git remote URL.

- `base`: a commit that is an ancestor of HEAD and is known to the remote. It must not be `@{upstream}` itself:
  the heartbeat fetch keeps `@{upstream}` ahead of an artist who has not pulled, the merge base would then sit
  above HEAD, and changes between HEAD and `origin/<branch>` would vanish from the answer (anti-goal 2). Use
  `git merge-base HEAD @{upstream}`. For a local-only branch (supported since PR #26), use the newest commit in
  HEAD's history that exists on a remote-tracking ref; the builder picks the git incantation. All local, no
  network.
- `branchPatterns`: the same set `CheckRemote` builds today (current branch's remote plus the status branches at
  the current and adjacent hierarchy level), sent as concrete branch names. The hierarchy filtering (a branch a
  lower index already claims is excluded) stays in the plugin. Resolving patterns to names may use the service's
  `GET .../branches?pattern=` instead of local `git branch --remotes` so the answer does not depend on how recent
  the last fetch was. Builder's call, see "Left to the builder".
- `paths`: the files being checked, repository-relative with forward slashes.

**Interpreting the answer.** For each path and branch the service returns the blob at the branch tip (or a delete).
The plugin compares it with the blob at local `HEAD` for that path (`git ls-tree` or `git rev-parse HEAD:<path>`,
one local process for the whole batch). Different means the branch carries a change the artist does not have:
`NotAtHead` when the branch is the current branch's remote, otherwise `NotLatest`, with `HeadBranch` set, exactly
as `CheckRemote` assigns today. The existing filters stay: non-lockable files are skipped, and a change to
`.checksum`, `Binaries/` or `Plugins/` on the current branch's remote still sets `bPendingRestart`. A path absent
from the response is unchanged on every queried branch.

**Fallback, per call, to today's local path:** service not configured; credential unavailable; connection
failure or timeout; any non-2xx status, including `409 unknown-base`; `partial: true` or `truncated: true`; a
response that fails to parse. A branch carrying an `error` must never be read as "nothing changed". Log the reason
at `Log` verbosity once per distinct reason rather than per call, so a dead service does not flood the output log.

**Timeout.** Short (order of 2 seconds), because the call sits on the path the editor blocks on. A slow service
must cost at most that before the old path runs. After a refused or failed connection (not a timeout, see
"Service freshness"), skip the service for a backoff period (order of 30 seconds) rather than paying for it on
every move.

**Threading.** `CheckRemote` runs on a `GThreadPool` worker for both async and synchronous commands (it only
runs inline when `GThreadPool` is null). For a synchronous command the game thread sits in the
`ExecuteSynchronousCommand` loop calling `Tick()` and never ticks the HTTP manager, so the worker's
`FHttpModule` request never completes. Brandon's prototype pumped `FHttpManager::Tick` from that loop
(`TickHttpIfUsingStatusCacheServer`). The builder may do the same or use a mechanism that does not depend on a
game thread tick. The client must complete in both cases and on every engine version the plugin supports (UE4
through 5.8, per `CLAUDE.md`), guarded per version if the HTTP API differs.

**Directories.** The service matches `paths` exactly, so a directory such as `Content/` matches nothing and would
make every file look up to date. Requests that include a directory (the heartbeat and full refreshes) use the
local path in this pass. They run in the background, so artists do not wait on them. Answering them from the
service by omitting `paths` is listed under Deferred.

**Remote URL.** The credential lookup needs an HTTPS `origin`. When `origin` is an SSH URL, the client is
disabled for the session, with one log line saying why.

**Credential.** `git credential fill` with `protocol` and `host` (and `path` if the forge needs it) taken from the
`origin` URL, sent as HTTP Basic. Held in memory only, never logged (log the URL without userinfo), never on a
command line. Fetch it once per editor session and again after a 401. A 401 or 403 falls back for that call and
does not call `git credential reject`, because the credential is still valid for git itself.

**Module dependencies.** Add `HTTP` and `Json` to `GitSourceControl.Build.cs`, as the prototype did.

### Change 3: port Brandon's server-independent improvements

From `origin/git-status-cache-server`:

- The `git ... took N ms` timing line in `RunCommandInternalRaw` and the `FGitScopedLogTimer` helper from
  `GitSourceControlLog.{h,cpp}`. These make before/after measurement possible from the output log alone. Port
  first, since the next item includes its header.
- `1ec6953` Log the git plugin location on startup. Needs only the logging header above.
- `675b2e2` Locking is non-blocking, with a warning popup if it fails. In the prototype this only runs when
  `bServerAllowedCheckout` is set by the server's `/canCheckout`, which does not exist here. Matt's decision:
  the background lock is allowed only when a quick pre-check passes for every file in the checkout:
  - `git lfs locks -p <file>` (answered from the GitLfsCache snapshot once `.lfsconfig` points at it) shows no
    lock held by someone else, and
  - the file's remote state from the status check is up to date (neither `NotAtHead` nor `NotLatest`), whether
    that state came from GitBranchStateCache or the local path.

  If either check fails or cannot be answered, the checkout runs as a blocking lock exactly as on `dev-3k`.

Not ported: `5fb7563` (delay the full refresh after a lock change). It works around the prototype server's lag in
seeing new locks. GitLfsCache invalidates its lock snapshot as soon as a relayed lock or unlock succeeds, so that
lag does not exist here. This is my call.

Each lands as its own commit crediting Brandon in the message.

### Configuration

In the game project's `DefaultEditor.ini`:

```ini
[GitSourceControl]
BranchStateCacheUrl=https://branchstate.example.com/v1/ado/myproject/_git/game
```

Empty or missing means disabled. A per-user override in the source control settings ini (via
`FGitSourceControlSettings`, no UI required) takes precedence when set, for testing against a local instance.

### Fork conventions

All behavior changes to existing upstream code are wrapped in `// [DIVERGENCE]` / `// [END DIVERGENCE]`. New
files carry no upstream copyright header. Bump `Version` / `VersionName` in `GitSourceControl.uplugin` when
releasing.

## Verification

There is no test suite. Verification is by building and running in a host project:

- Build the editor target with `-DisableUnity` to catch missing includes.
- With the service unset, behavior and output log match `dev-3k` apart from the new timing lines.
- With the service set, moving an actor on `main` produces one HTTP request and no per-branch `git diff` lines,
  and the dialog clears quickly. Capture an Unreal Insights trace or output-log timings before and after.
- Correctness: modify a lockable asset on a feature branch, push, and confirm the artist on `main` sees it as
  `NotLatest` through the service exactly as through the local path. Repeat with a change that is then reverted on
  that branch: the service path should report it up to date.
- Kill the service mid-session: the next move falls back within the timeout, and later moves skip it during the
  backoff period.
- Point the service at a base it does not know (an unpushed local commit as `@{upstream}` will not do this, so use
  a fresh mirror): `409` falls back cleanly.

## Left to the builder

- The git command for "latest pushed ancestor" on a branch with no upstream.
- Whether branch names come from local `git branch --remotes` (as today) or the service's `/branches` endpoint.
  Using the endpoint removes a dependency on fetch freshness. Keeping local keeps the change smaller.
- Batching: whether the `paths` list is chunked, and at what size, given `MaxPathsPerRequest` (20,000 by default)
  on the service.
- Code layout: a new `GitBranchStateCacheClient.{h,cpp}` is the expected shape.

## Deferred

- Removing or slowing the 30 second background `git fetch` once the service answers remote state. Worth doing for
  residential links, but it also feeds pull and the current branch's own status, so it is a separate change.
- Batched locking through GitLfsCache's `locks/batch`, which needs a patched git-lfs (excluded by the vanilla
  preference).
- Warming the whole project's remote state on connect, and answering the heartbeat, with one call that omits
  `paths`.

## Service freshness

When the mirror's refs are older than `RefsTtl`, a `/state` request currently waits for a mirror fetch. Matt's
decision: freshness is the service's concern, and the plugin does not try to compensate (no warm-up calls from
the heartbeat). On the plugin side, a timeout falls back for that call only. Only a refused or failed
connection starts the backoff period, so a slow first request after a quiet spell does not switch the service
off for everyone's next moves.

Artists clone over HTTPS (confirmed), so the `git credential fill` approach covers them. The SSH guard above is
there only for safety.
