# GitBranchStateCache and GitLfsCache Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the per-branch `git diff` loop from blocking artists on every actor move by asking GitBranchStateCache instead, with a full fallback to today's local git path.

**Architecture:** A new `GitBranchStateCache` client (pure parsing and comparison functions plus one HTTP call) is consulted from `CheckRemote` for file-level status checks. Its answer is turned into the same per-branch "changed files" lists the existing loop consumes, so the lockable-file filter, `bPendingRestart` and `NotAtHead`/`NotLatest` assignment stay in one place. Lock traffic stays on stock git-lfs and is pointed at GitLfsCache by a committed `.lfsconfig` (deployment, no plugin code). Supporting changes are a changelist-scan skip, timing logs, and a gated non-blocking lock ported from Brandon's prototype.

**Tech Stack:** Unreal Engine C++ (UE4 through 5.8, verified locally on 5.8.3), `HTTP` and `Json` engine modules, Unreal automation tests (`WITH_DEV_AUTOMATION_TESTS`), git and git-lfs command line.

**Spec:** `docs/superpowers/specs/2026-10-19-branch-state-and-lfs-cache-design.md`

## Global Constraints

- Branch: `support-gitbranchstatecache-and-gitlfscache`. Commit after every task. No `Co-Authored-By` lines.
- Changes to existing upstream code are wrapped in `// [DIVERGENCE]` / `// [END DIVERGENCE]`.
- Unreal conventions: tabs, `F`/`E`/`I` prefixes, `In`/`Out` parameter prefixes, `b` booleans, `TEXT()` literals, `FString`/`TArray`/`TMap`, `LOCTEXT` for user-facing text. New files carry no upstream copyright header.
- Worker code: `FGitSourceControlModule::GetThreadSafe()`, never `Get()`.
- Config key: `[GitSourceControl]` `BranchStateCacheUrl` in `GEditorIni`. Per-user override `BranchStateCacheUrlOverride` in the source control settings ini (`FGitSourceControlSettings`), no UI.
- Service request timeout 2.0 seconds. Backoff 30.0 seconds, started only by a refused or failed connection, never by a timeout or an HTTP error status.
- Fall back to the local path when: not configured, `origin` not HTTPS, no credential, connection failure, timeout, non-2xx (including `409`), `partial: true`, `truncated: true`, any branch carrying a non-null `error`, unparsable body, or the request includes a directory.
- The credential is never logged, never placed on a command line, never written to an ini. URLs are logged with userinfo removed.
- `git credential fill` must never show UI or prompt: run with `GIT_TERMINAL_PROMPT=0` and `GCM_INTERACTIVE=never` in its environment.
- `HTTP` and `Json` added to `GitSourceControl.Build.cs` private dependencies.
- Verification build: `C:\dev\3k\UnrealEngine\Engine\Build\BatchFiles\Build.bat GitPluginHostEditor Win64 Development "-Project=C:\dev\3k\UEGitPluginHost\GitPluginHost.uproject" -WaitMutex -NoHotReload`, plus the same with `-DisableUnity` before the final commit of each task. The host project's `Plugins/GitSourceControl` is a junction to this repo.
- Test run: `C:\dev\3k\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "C:\dev\3k\UEGitPluginHost\GitPluginHost.uproject" -ExecCmds="Automation RunTests GitSourceControl.<Group>; Quit" -unattended -nullrhi -nosplash -nop4 -log`. Pass means the log has `Test Completed. Result={Success}` for each test and no `Result={Fail}`.

## Review Focus

1. An artist who has not pulled (local `origin/<branch>` ahead of `HEAD`). Expected: files changed upstream still show `NotAtHead`. Pinned by `GitSourceControl.BranchStateCache.NewerFiles.RemoteAheadOfHead` (Task 3) and the manual check in Task 6.
2. A path changed on a branch and then changed back. Expected: up to date. Pinned by `GitSourceControl.BranchStateCache.NewerFiles.RevertedChangeIsUpToDate` (Task 3).
3. A file deleted on a branch but present locally. Expected: reported as newer on that branch (status `D`, null blob differs from the local blob). Pinned by `GitSourceControl.BranchStateCache.NewerFiles.DeleteOnBranch` (Task 3).
4. The service stops responding mid-session. Expected: the next move costs at most 2 seconds and falls back. Moves after a refused connection skip the service for 30 seconds. Pinned by `GitSourceControl.BranchStateCache.Backoff.*` (Task 3) and the manual check in Task 6.
5. Paths with spaces or non-ASCII characters (artists name assets freely). Expected: they round-trip through JSON and `ls-tree -z` unchanged. Pinned by `GitSourceControl.BranchStateCache.Parse.UnicodeAndSpacesInPaths` and `GitSourceControl.BranchStateCache.LsTree.ParsesNulSeparated` (Task 3).

---

## File Structure

| File | Responsibility |
|---|---|
| `Source/GitSourceControl/Private/GitSourceControlLog.{h,cpp}` (create) | `LogGitSourceControl` category, `FGitScopedLogTimer`, `Truncate`, `GetThreadLabel`. Ported from the prototype. |
| `Source/GitSourceControl/Private/GitBranchStateCache.{h,cpp}` (create) | Pure functions: request body, response parsing, `ls-tree` parsing, remote URL parsing, credential output parsing, newer-file computation, backoff. No I/O. |
| `Source/GitSourceControl/Private/GitBranchStateCacheClient.{h,cpp}` (create) | I/O: config lookup, `git credential fill`, base commit, local blob lookup, HTTP request, and the one entry point `CheckRemote` calls. |
| `Source/GitSourceControl/Private/GitBackgroundLock.{h,cpp}` (create) | Ported from the prototype: run `git lfs lock` off the calling path, revert-or-keep popup on failure. |
| `Source/GitSourceControl/Private/Tests/GitBranchStateCacheTests.cpp` (create) | Automation tests for `GitBranchStateCache.h`. |
| `Source/GitSourceControl/Private/GitSourceControlUtils.cpp` (modify) | `RunCommandInternalRaw` timing line, `RunUpdateStatus` changelist skip, `CheckRemote` service path. |
| `Source/GitSourceControl/Private/GitSourceControlOperations.cpp` (modify) | `FGitCheckOutWorker::Execute` gated background lock. |
| `Source/GitSourceControl/Private/GitSourceControlProvider.cpp` (modify) | HTTP pumping in `ExecuteSynchronousCommand` if needed (Task 4). |
| `Source/GitSourceControl/Private/GitSourceControlModule.cpp` (modify) | Plugin location log line. |
| `Source/GitSourceControl/Public/GitSourceControlSettings.h`, `Private/GitSourceControlSettings.cpp` (modify) | `BranchStateCacheUrlOverride` load/save/get. |
| `Source/GitSourceControl/GitSourceControl.Build.cs` (modify) | `HTTP`, `Json`. |
| `GitSourceControl.uplugin` (modify) | Version bump (last task). |

---

### Task 1: Timing logs and plugin location log

**Files:**
- Create: `Source/GitSourceControl/Private/GitSourceControlLog.h`, `Source/GitSourceControl/Private/GitSourceControlLog.cpp`
- Modify: `Source/GitSourceControl/Private/GitSourceControlUtils.cpp` (`RunCommandInternalRaw`, around the `FPlatformProcess::ExecProcess` call near line 265)
- Modify: `Source/GitSourceControl/Private/GitSourceControlModule.cpp` (`StartupModule`)

**Interfaces:**
- Produces: `DECLARE_LOG_CATEGORY_EXTERN(LogGitSourceControl, Log, All)`; `class FGitScopedLogTimer { explicit FGitScopedLogTimer(const FString& InName); ~FGitScopedLogTimer(); void SetOutcome(const FString& InOutcome); double GetElapsedMilliseconds() const; }`; `namespace GitSourceControlLog { FString Truncate(const FString& Text, int32 MaxLength = 300); const TCHAR* GetThreadLabel(); }`

- [ ] **Step 1: Port `GitSourceControlLog.{h,cpp}`** from `git show origin/git-status-cache-server:Source/GitSourceControl/Private/GitSourceControlLog.{h,cpp}`. Change the category comment to drop the "GitStatusCacheServer requests" mention.
- [ ] **Step 2: Port the timing line** in `RunCommandInternalRaw` from commit `21177fc` (`git ... took %.1f ms (return code %d)`, logged with the thread label and the truncated command), wrapped in `[DIVERGENCE]` markers.
- [ ] **Step 3: Port `1ec6953`** (plugin location log in `StartupModule`), wrapped in `[DIVERGENCE]` markers.
- [ ] **Step 4: Build.** Run the verification build. Expected: `Result: Succeeded`.
- [ ] **Step 5: Commit** with message `Add git command timing and plugin location logging` and a body line `Ported from origin/git-status-cache-server by Brandon Meester.`

### Task 2: Skip the Content-wide changelist scan on file-level status updates

**Files:**
- Modify: `Source/GitSourceControl/Private/GitSourceControlUtils.cpp` (`RunUpdateStatus`, the `UpdateChangelistStateByCommand()` call near line 1752)

**Interfaces:**
- Produces: `static bool ContainsDirectory(const TArray<FString>& InPaths)` in `GitSourceControlUtils.cpp` (file-local), true when any path is an existing directory. Task 5 reuses it.

- [ ] **Step 1: Add `ContainsDirectory`** (as in the prototype, `Algo::AnyOf` over `FPaths::DirectoryExists`), and call `UpdateChangelistStateByCommand()` only when it returns true. Wrap in `[DIVERGENCE]` with a comment: changelists are disabled (TECH-296), the full refresh still rebuilds them. Time the call with `FGitScopedLogTimer ChangelistTimer(TEXT("UpdateChangelistStateByCommand"))`.
- [ ] **Step 2: Build.** Expected: `Result: Succeeded`.
- [ ] **Step 3: Commit** `Skip the Content-wide changelist scan on file-level status updates`.

### Task 3: Pure branch state functions, test first

**Files:**
- Create: `Source/GitSourceControl/Private/GitBranchStateCache.h`, `Source/GitSourceControl/Private/GitBranchStateCache.cpp`
- Create: `Source/GitSourceControl/Private/Tests/GitBranchStateCacheTests.cpp`
- Modify: `Source/GitSourceControl/GitSourceControl.Build.cs` (add `"HTTP"`, `"Json"` to private dependencies; `Json` is needed here)

**Interfaces (all in `namespace GitBranchStateCache`):**
- `struct FPathChange { FString Branch; FString Blob; /* empty when deleted */ FString Status; /* git raw status letter */ };`
- `struct FStateResponse { TArray<FString> BranchNames; TArray<FString> BranchesWithErrors; TMap<FString, TArray<FPathChange>> Paths; /* repository-relative, forward slashes */ bool bPartial = false; bool bTruncated = false; };`
- `FString BuildStateRequestBody(const FString& InBase, const TArray<FString>& InBranches, const TArray<FString>& InRelativePaths);` Branches go in `branchPatterns` as concrete names.
- `bool ParseStateResponse(const FString& InJson, FStateResponse& OutResponse, FString& OutError);` false on malformed JSON or missing `branches`/`paths`.
- `bool IsUsable(const FStateResponse& InResponse, FString& OutReason);` false when `bPartial`, `bTruncated`, or any branch error.
- `TMap<FString, FString> ParseLsTree(const FString& InNulSeparatedOutput);` input is `git ls-tree -r -z HEAD -- <paths>` output (`<mode> SP <type> SP <oid> TAB <path> NUL`). Returns path to blob id.
- `struct FRemoteParts { FString Protocol; FString Host; FString Path; };`
- `bool ParseHttpsRemote(const FString& InRemoteUrl, FRemoteParts& OutParts);` false for anything not `https://` (SSH `git@host:path`, `ssh://`, `http://`, local paths). Strips userinfo and a trailing `/`.
- `FString StripUserInfo(const FString& InUrl);`
- `bool ParseCredentialFill(const TArray<FString>& InLines, FString& OutUserName, FString& OutPassword);` reads `username=` and `password=` lines. False when `password` is missing.
- `TMap<FString, FString> ComputeNewerFiles(const FStateResponse& InResponse, const TMap<FString, FString>& InHeadBlobs, const FString& InCurrentRemoteBranch);` returns relative path to branch name for every path whose blob on a branch differs from `InHeadBlobs` (a missing local entry counts as an empty blob). When several branches qualify, the current remote branch wins, otherwise the first in `BranchNames` order. This mirrors `CheckRemote`'s `bCurrentBranch || !NewerFiles.Contains` rule.
- `class FBackoff { public: explicit FBackoff(double InBackoffSeconds = 30.0); bool ShouldSkip(double InNowSeconds) const; void RecordConnectionFailure(double InNowSeconds); void RecordSuccess(); };`

- [ ] **Step 1: Write the failing tests** in `GitBranchStateCacheTests.cpp` under `#if WITH_DEV_AUTOMATION_TESTS`, each an `IMPLEMENT_SIMPLE_AUTOMATION_TEST` with flags `EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter` (on 5.5+ the flags type is `EAutomationTestFlags_ApplicationContextMask`; follow what the engine header in use compiles):
  - `GitSourceControl.BranchStateCache.Request.Body`: `BuildStateRequestBody("a1b2", {"origin/main","origin/KO-1"}, {"Content/A.uasset"})` parses back as JSON with `base == "a1b2"`, `branchPatterns == ["origin/main","origin/KO-1"]`, `paths == ["Content/A.uasset"]`.
  - `GitSourceControl.BranchStateCache.Parse.Valid`: the README example response (one branch `origin/main`, path `Content/Chars/Bar.uasset` with blob `9f8e7d`, status `M`) yields `BranchNames == {"origin/main"}`, `Paths["Content/Chars/Bar.uasset"][0].Blob == "9f8e7d"`, `Status == "M"`, and `IsUsable` true.
  - `GitSourceControl.BranchStateCache.Parse.DeleteHasEmptyBlob`: `"blob": null, "status": "D"` gives `Blob.IsEmpty()` and `Status == "D"`.
  - `GitSourceControl.BranchStateCache.Parse.Malformed`: `"{"`, `""` and `{"base":"x"}` all return false with a non-empty error.
  - `GitSourceControl.BranchStateCache.Parse.NotUsable`: `partial: true` → `IsUsable` false; `truncated: true` → false; a branch with `"error": "fetch failed"` → false.
  - `GitSourceControl.BranchStateCache.Parse.UnicodeAndSpacesInPaths`: path `Content/Maps/Café Level/Foo Bar.umap` survives `BuildStateRequestBody` and `ParseStateResponse` byte-for-byte.
  - `GitSourceControl.BranchStateCache.LsTree.ParsesNulSeparated`: `"100644 blob aaa\tContent/A.uasset\0100644 blob bbb\tContent/Dir With Space/B.umap\0"` gives two entries with those exact paths and blobs.
  - `GitSourceControl.BranchStateCache.Remote.Https`: `https://user@dev.azure.com/org/proj/_git/game` gives protocol `https`, host `dev.azure.com`, path `org/proj/_git/game`. `git@github.com:studio/game.git`, `ssh://git@host/x`, `http://host/x` return false.
  - `GitSourceControl.BranchStateCache.Remote.StripUserInfo`: `https://user:secret@host/x` becomes `https://host/x`.
  - `GitSourceControl.BranchStateCache.Credential.Parse`: `{"protocol=https","host=h","username=u","password=p"}` gives `u`/`p`. Without a `password=` line it returns false.
  - `GitSourceControl.BranchStateCache.NewerFiles.DifferentBlob`: response path `Content/A.uasset` on `origin/KO-1` blob `bbb`, head blob `aaa` → `{"Content/A.uasset": "origin/KO-1"}`.
  - `GitSourceControl.BranchStateCache.NewerFiles.RevertedChangeIsUpToDate`: same path, branch blob `aaa`, head blob `aaa` → empty.
  - `GitSourceControl.BranchStateCache.NewerFiles.DeleteOnBranch`: branch status `D` with empty blob, head blob `aaa` → reported on that branch.
  - `GitSourceControl.BranchStateCache.NewerFiles.CurrentBranchWins`: path differs on `origin/KO-1` (listed first) and on current remote `origin/main` → mapped to `origin/main`.
  - `GitSourceControl.BranchStateCache.NewerFiles.RemoteAheadOfHead`: current remote `origin/main` blob `ccc`, head blob `aaa` → `{"path": "origin/main"}` (the caller maps this to `NotAtHead`).
  - `GitSourceControl.BranchStateCache.Backoff.SkipsAfterFailure`: after `RecordConnectionFailure(100.0)`, `ShouldSkip(129.9)` true, `ShouldSkip(130.0)` false.
  - `GitSourceControl.BranchStateCache.Backoff.SuccessClears`: failure at 100.0 then `RecordSuccess()` → `ShouldSkip(101.0)` false.
- [ ] **Step 2: Add the header with the declarations above and empty stub bodies, then build.** Expected: build succeeds.
- [ ] **Step 3: Run the tests.** Run the test command with `<Group>` = `BranchStateCache`. Expected: the tests fail (stubs return defaults).
- [ ] **Step 4: Implement the functions** in `GitBranchStateCache.cpp` using `TJsonReader`/`FJsonSerializer` for JSON. `ParseLsTree` splits on `\0` then on the first `\t`.
- [ ] **Step 5: Run the tests.** Expected: every `GitSourceControl.BranchStateCache.*` test reports `Result={Success}`.
- [ ] **Step 6: Build with `-DisableUnity`.** Expected: `Result: Succeeded`.
- [ ] **Step 7: Commit** `Add GitBranchStateCache request, response and comparison logic with tests`.

### Task 4: Client I/O: config, credential, base, blobs, HTTP

**Files:**
- Create: `Source/GitSourceControl/Private/GitBranchStateCacheClient.h`, `Source/GitSourceControl/Private/GitBranchStateCacheClient.cpp`
- Modify: `Source/GitSourceControl/Public/GitSourceControlSettings.h`, `Source/GitSourceControl/Private/GitSourceControlSettings.cpp`
- Modify (only if Step 4 needs it): `Source/GitSourceControl/Private/GitSourceControlProvider.cpp` (`ExecuteSynchronousCommand` loop near line 806)

**Interfaces:**
- Consumes: everything in `GitBranchStateCache.h` (Task 3); `GitSourceControlUtils::RunCommand`, `GetRemoteUrl`.
- Produces, in `namespace GitBranchStateCacheClient`:
  - `FString GetStateUrl();` returns the per-user override if set, else `GEditorIni` `[GitSourceControl] BranchStateCacheUrl`, else empty. Trailing `/` removed.
  - `bool TryGetChangedFilesByBranch(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InAbsoluteFiles, const TArray<FString>& InBranches, const FString& InCurrentRemoteBranch, TMap<FString, TArray<FString>>& OutChangedRelativeFilesByBranch);` true only when the service answered usably. The output has one entry per branch name that has newer files (from `ComputeNewerFiles`, inverted to branch → files). Every failure returns false after logging the reason once per distinct reason at `Log` on `LogGitSourceControl`.
- `FGitSourceControlSettings` gains `FString GetBranchStateCacheUrlOverride() const;` loaded and saved under the existing settings section with key `BranchStateCacheUrlOverride`.

- [ ] **Step 1: Settings override.** Add the getter, load and save it alongside the existing keys. Build. Expected: `Result: Succeeded`.
- [ ] **Step 2: Credential lookup.** A file-local function that runs `git credential fill` with stdin `protocol=<p>\nhost=<h>\npath=<path>\n\n` via `FPlatformProcess::CreateProc` with read and write pipes (`ExecProcess` cannot write stdin), with `GIT_TERMINAL_PROMPT=0` and `GCM_INTERACTIVE=never` set for the child, and a 5 second wait limit. The parsed credential is cached in a static guarded by an `FCriticalSection` for the session and cleared on a `401`. Never logged.
- [ ] **Step 3: Base and blobs.** Base: `git merge-base HEAD @{upstream}`; when the branch has no upstream, `git merge-base HEAD <InBranches[0]>`. Local blobs: one `git ls-tree -r -z HEAD -- <relative files>` through `RunCommandInternalRaw` (raw output keeps the NUL separators), parsed with `ParseLsTree`. Requests with more than `GitSourceControlConstants::MaxFilesPerBatch` files are split into batches for `ls-tree` only. The service request carries all paths in one body.
- [ ] **Step 4: HTTP call.** `FHttpModule::Get().CreateRequest()`, `POST`, `Content-Type: application/json`, `Authorization: Basic <base64 user:password>`, timeout 2.0 seconds. The worker blocks on an `FEvent` until completion. Use `SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread)` where `IHttpRequest.h` declares it (present on 5.8.3, so guard by engine version after checking which version introduced it). On older engines, pump `FHttpManager::Tick(0.0f)` from the `ExecuteSynchronousCommand` wait loop while a branch state request is in flight, as the prototype's `TickHttpIfUsingStatusCacheServer` did. A response with no connection (`EHttpRequestStatus::Failed` with connection error) calls `RecordConnectionFailure`. A timeout does not.
- [ ] **Step 5: Wire up `TryGetChangedFilesByBranch`.** Order: `GetStateUrl` empty → false. `FBackoff::ShouldSkip` → false. `ParseHttpsRemote` on `GetRemoteUrl` fails → false and disable for the session. Credential → base → blobs → request → `ParseStateResponse` → `IsUsable` → `ComputeNewerFiles` → invert into `OutChangedRelativeFilesByBranch` → `RecordSuccess` → true. Time the whole call with `FGitScopedLogTimer`.
- [ ] **Step 6: Build, then build with `-DisableUnity`.** Expected: `Result: Succeeded` both times.
- [ ] **Step 7: Commit** `Add GitBranchStateCache client`.

### Task 5: Use the service in CheckRemote

**Files:**
- Modify: `Source/GitSourceControl/Private/GitSourceControlUtils.cpp` (`CheckRemote`, lines 1424 to 1515)

**Interfaces:**
- Consumes: `GitBranchStateCacheClient::TryGetChangedFilesByBranch` (Task 4), `ContainsDirectory` (Task 2).

- [ ] **Step 1: Extract the per-branch filter.** Move the body that walks `Intersection` (lockable check, `bPendingRestart` for `.checksum`/`Binaries/`/`Plugins/`, `NewerFiles.Add`) into a local lambda `ApplyChangedFiles(const FString& Branch, const TArray<FString>& ChangedRelativeFiles)`, and have the existing `git log`/`git diff` loop call it. No behavior change.
- [ ] **Step 2: Build and confirm no behavior change.** Build. Then in the host editor with the service unset, run `git status` on a file through the plugin (console `SourceControl.Status` or the content browser) and compare the `LogGitSourceControl` lines with `dev-3k`: the same `git log` and `git diff` commands appear.
- [ ] **Step 3: Add the service path.** Before the `git log` call: when `!ContainsDirectory(Files)` and `TryGetChangedFilesByBranch(..., BranchesToDiff.Array(), bDiffAgainstRemoteCurrent ? CurrentBranchName : FString(), Changed)` returns true, call `ApplyChangedFiles` for each entry and skip the `git log`/`git diff` loop. Wrap in `[DIVERGENCE]`.
- [ ] **Step 4: Build, then build with `-DisableUnity`.** Expected: `Result: Succeeded`.
- [ ] **Step 5: Commit** `Ask GitBranchStateCache for remote state in CheckRemote`.

### Task 6: End-to-end check against a running service

**Files:** none changed unless a defect is found (fix it in the owning task's files and note it in the commit).

- [ ] **Step 1: Run GitBranchStateCache locally** from `C:\dev\ktsu-dev\GitBranchStateCache` (`dotnet run --project GitBranchStateCache.Tool -- --port 8080 --mirror-root <scratch> --upstream <name>=<forge> --allow <name>=<repo path>`) against a small test repository on the same forge (not the game repository), with a few `.uasset` files marked lockable in `.gitattributes`.
- [ ] **Step 2: Point a host editor at it.** Set `BranchStateCacheUrlOverride` to `http://localhost:8080/v1/<name>/<repo path>`. HTTP to localhost is allowed for the override only. Open the test repository as the host project's repository (or a copy of the host project committed into it).
- [ ] **Step 3: Check these, recording the log lines for each:**
  - Changing a lockable file on another status branch and pushing makes it `NotLatest` with that branch named. The log shows one service request and no per-branch `git diff`.
  - Committing a change on `origin/<current>` from another clone, then fetching without pulling, makes the file `NotAtHead`.
  - Changing then reverting a file on a branch leaves it up to date.
  - Stopping the service: the next status check falls back within about 2 seconds (timing line). Checks during the next 30 seconds do not try the service.
  - With the override cleared and no `BranchStateCacheUrl`, the log matches `dev-3k` apart from timing lines.
- [ ] **Step 4: Commit** any fixes found, one per defect.

### Task 7: Gated non-blocking lock

**Files:**
- Create: `Source/GitSourceControl/Private/GitBackgroundLock.h`, `Source/GitSourceControl/Private/GitBackgroundLock.cpp`
- Modify: `Source/GitSourceControl/Private/GitSourceControlOperations.cpp` (`FGitCheckOutWorker::Execute`, lines 153 to 183)

**Interfaces:**
- Produces: `namespace GitBackgroundLock { void LockFiles(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& RelativeFiles); }`, ported from `675b2e2`.
- Consumes: `GitSourceControlUtils::RefreshLocks`, provider cached states.

- [ ] **Step 1: Port `GitBackgroundLock.{h,cpp}`** from `675b2e2`, updating the header comment to describe the new gate rather than "the status cache server already approved".
- [ ] **Step 2: Add the gate** in `FGitCheckOutWorker::Execute` before the lock call, wrapped in `[DIVERGENCE]`. Lock in the background only when both hold for every file in `LockableRelativeFiles`:
  - `RefreshLocks` into a temporary state map succeeds and shows no file as `ELockState::LockedOther`.
  - The provider's cached state (`GetState(..., EStateCacheUsage::Use)`) has `RemoteState` neither `ERemoteState::NotAtHead` nor `ERemoteState::NotLatest`.

  Otherwise run the existing blocking `RunLFSCommand(TEXT("lock"), ...)` path unchanged. Log which path was taken and why.
- [ ] **Step 3: Build, then build with `-DisableUnity`.** Expected: `Result: Succeeded`.
- [ ] **Step 4: Manual check** on the Task 6 setup:
  - An unlocked, up-to-date file checks out without the blocking dialog and appears locked after the background lock completes.
  - A file locked by a second user takes the blocking path and shows today's failure.
  - A `NotLatest` file takes the blocking path.
  - Forcing the background lock to fail (lock the file from another clone between the gate and the lock) shows the revert-or-keep popup.
- [ ] **Step 5: Commit** `Lock in the background when the file is unlocked and up to date`, body line `GitBackgroundLock ported from origin/git-status-cache-server by Brandon Meester.`

### Task 8: Version bump and docs

**Files:**
- Modify: `GitSourceControl.uplugin` (`Version` +1, `VersionName` patch +1)
- Modify: `CLAUDE.md` (a short "Branch state cache" paragraph under Architecture naming `BranchStateCacheUrl`, the fallback rule, and the `.lfsconfig` deployment step)

- [ ] **Step 1: Bump the version and add the paragraph.**
- [ ] **Step 2: Final build with `-DisableUnity`** and a full run of `GitSourceControl.BranchStateCache`. Expected: build succeeds and all tests pass.
- [ ] **Step 3: Commit** `Bump plugin version for GitBranchStateCache support`.
