# Batched Locking and Whole-Tree Branch State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lock and unlock many files in one request through GitLfsCache's `locks/batch`, and answer directory status refreshes from GitBranchStateCache, both falling back to today's behavior.

**Architecture:** The HTTP and credential plumbing in `GitBranchStateCacheClient.cpp` moves to a shared `GitServiceHttp` so a second client can use it. A new `GitLfsLockBatch` (pure) and `GitLfsLockBatchClient` (I/O) answer lock and unlock calls, behind one `GitSourceControlUtils::RunLfsLockOperation` that every lock and unlock site calls and that falls back to `RunLFSCommand`. The branch state client learns to omit `paths` when the request contains a directory.

**Tech Stack:** Unreal Engine C++ (UE4 through 5.8, verified on 5.8.3), `HTTP` and `Json` modules, Unreal automation tests, git and git-lfs.

**Spec:** `docs/superpowers/specs/2026-10-19-branch-state-and-lfs-cache-design.md`, Changes 4 and 5 and decisions 7 and 8.

## Global Constraints

- Branch `support-gitbranchstatecache-and-gitlfscache`, continuing from `5c77214`. Commit after every task. No `Co-Authored-By` lines.
- Changes to existing upstream code are wrapped in `// [DIVERGENCE]` / `// [END DIVERGENCE]`. New files carry no upstream copyright header.
- Unreal conventions as in `CLAUDE.md`. Worker code uses `FGitSourceControlModule::GetThreadSafe()`.
- Credentials: never logged, never on a command line, `credential.interactive=never`, 5 second fill timeout, cleared on 401, the session disabled on a failed fill or a second 401 in a row.
- URLs a credential is sent to: https, or http to localhost only. The LFS endpoint is not a user override, so `IsAllowedStateUrl(Url, false)` applies, extended to allow localhost http for it (git-lfs already sends the same credential there).
- Lock batch timeout: `2.0 + 0.05 * PathCount` seconds, capped at 60. Branch state timeout stays 2.0.
- Backoff: 30 seconds after a refused connection or 3 timeouts in a row, per client. A 404 from `locks/batch` disables the batch route for the session.
- A well-formed 200 from `locks/batch` is final: no stock-command retry, even with failed items.
- Builds: `scratchpad\build-both.ps1 -Tag <tag>` (unity, then `-DisableUnity -Module=GitSourceControl`). Both must report `Result: Succeeded`.
- Tests: `C:\dev\3k\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "C:\dev\3k\UEGitPluginHost\GitPluginHost.uproject" -ExecCmds="Automation RunTests GitSourceControl; Quit" -unattended -nullrhi -nosplash -nop4 -log`. Pass means every `Result={Success}` and no `Result={Fail}`. Kill any stray `CredentialProvider.Microsoft` process afterwards.

## Review Focus

1. A batch where some files are locked by someone else. Expected: only those fail, their messages reach the user, the rest are locked and writable, and the operation reports failure as `git lfs lock` does. Pinned by `GitSourceControl.LfsLockBatch.Parse.MixedResults`.
2. `lfs.url` pointing at the forge, not GitLfsCache. Expected: 404 once, stock commands from then on. Pinned by `GitSourceControl.LfsLockBatch.Parse.RejectsNonBatchBody` and the manual check.
3. A response that drops or reorders items. Expected: fall back is not allowed (upstream may already have acted), so missing paths count as failed. Pinned by `GitSourceControl.LfsLockBatch.Parse.MissingItemsFail`.
4. A batch-locked file that a later sync makes read-only. Expected: writable again after the next status update. Manual check.
5. A directory refresh on a repository with more changed paths than the service returns. Expected: `truncated` falls back to local git. Pinned by the existing `Parse.NotUsable` plus `WholeTree.KeepsRequestedPaths`.

---

## File Structure

| File | Responsibility |
|---|---|
| `Private/GitServiceHttp.{h,cpp}` (new) | `PostJson`, `RunCredentialFill`, `BasicAuthorization`, `IsRequestInFlight`, shared by both clients |
| `Private/GitBranchStateCacheClient.cpp` | Uses `GitServiceHttp`. Whole-tree requests |
| `Private/GitBranchStateCache.{h,cpp}` | `BuildStateRequestBody` gains `bInIncludePaths`, new `IsUnderRequestedPath` |
| `Private/GitLfsLockBatch.{h,cpp}` (new) | Body, response parsing, `git lfs env` endpoint parsing, ref name, timeout |
| `Private/GitLfsLockBatchClient.{h,cpp}` (new) | Endpoint discovery, credential, POST, backoff |
| `Private/GitSourceControlUtils.{h,cpp}` | `RunLfsLockOperation`, writable fix for locked files in `UpdateCachedStates` |
| `Private/GitSourceControlOperations.cpp`, `GitBackgroundLock.cpp` | Lock and unlock sites call `RunLfsLockOperation` |
| `Private/GitSourceControlProvider.cpp` | Sync loop pumps HTTP while `GitServiceHttp::IsRequestInFlight()` |
| `Private/Tests/GitLfsLockBatchTests.cpp` (new), `GitBranchStateCacheTests.cpp` | Tests |

---

### Task 1: Shared HTTP and credential plumbing

Refactor, no behavior change.

**Interfaces:**
- Produces, in `namespace GitServiceHttp`:
  - `struct FPostResult { bool bResponded; bool bConnectionFailure; bool bTimedOut; int32 ResponseCode; FString Body; FString Error; }`
  - `FPostResult PostJson(const FString& InUrl, const FString& InAuthorization, const FString& InBody, float InTimeoutSeconds)`
  - `bool RunCredentialFill(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const GitBranchStateCache::FRemoteParts& InRemote, FString& OutUserName, FString& OutPassword)`
  - `FString BasicAuthorization(const FString& InUserName, const FString& InPassword)`
  - `bool IsRequestInFlight()`
- `GitBranchStateCacheClient::IsRequestInFlight` is removed. The provider calls `GitServiceHttp::IsRequestInFlight`.

- [ ] **Step 1:** Move the code from the anonymous namespace in `GitBranchStateCacheClient.cpp` into `GitServiceHttp.cpp`, with the `GIT_BRANCH_STATE_MODERN_HTTP` guard renamed `GIT_SERVICE_HTTP_MODERN`. The wait-loop deadline uses `InTimeoutSeconds + 1.0`.
- [ ] **Step 2:** Build. Expected: both `Result: Succeeded`.
- [ ] **Step 3:** Run tests. Expected: 24/24 `Result={Success}`.
- [ ] **Step 4:** Commit "Share the HTTP and credential code between service clients".

### Task 2: Whole-tree branch state

**Interfaces:**
- `FString BuildStateRequestBody(const FString& InBase, const TArray<FString>& InBranches, const TArray<FString>& InRelativePaths, bool bInIncludePaths = true)`
- `bool IsUnderRequestedPath(const FString& InRelativePath, const TArray<FString>& InRequestedRelativePaths)`: true when the path equals a requested path or starts with a requested path followed by `/`. Requested directories may arrive with or without a trailing slash.

- [ ] **Step 1: Write the failing tests** in `GitBranchStateCacheTests.cpp`:
  - `GitSourceControl.BranchStateCache.Request.BodyWithoutPaths`: `BuildStateRequestBody(TEXT("abc"), {TEXT("origin/main")}, {TEXT("Content")}, false)` parses to an object with `base` and `branchPatterns` and no `paths` field.
  - `GitSourceControl.BranchStateCache.WholeTree.KeepsRequestedPaths`: with requested `{TEXT("Content"), TEXT("Config/"), TEXT("Game.uproject")}`, true for `Content/A.uasset`, `Config/DefaultEngine.ini`, `Game.uproject`. False for `ContentOld/A.uasset`, `Source/X.cpp`, `Game.uproject.bak`.
- [ ] **Step 2:** Run tests. Expected: compile failure (no 4-argument overload, no `IsUnderRequestedPath`), which is the RED for both.
- [ ] **Step 3:** Implement both in `GitBranchStateCache.cpp`.
- [ ] **Step 4:** Run tests. Expected: 26/26.
- [ ] **Step 5:** In `TryGetChangedFilesByBranch`, when any input is a directory (`FPaths::DirectoryExists`): build the body without paths, read HEAD blobs with `ls-tree -r HEAD --` using the same relative inputs as pathspecs (batched by 50, directories recurse), and drop response paths for which `IsUnderRequestedPath` is false before `ComputeNewerFiles`. In `CheckRemote`, remove the `!ContainsDirectory(Files)` guard around the service call, inside the existing divergence block. Keep `ContainsDirectory` (the changelist scan still uses it).
- [ ] **Step 6:** Build. Expected: both succeed. Run tests: 26/26.
- [ ] **Step 7:** Commit "Answer directory status refreshes from GitBranchStateCache".

### Task 3: Batched lock logic

**Interfaces**, in `namespace GitLfsLockBatch` (`GitLfsLockBatch.h`):
- `enum class EOperation : uint8 { Lock, Unlock };`
- `struct FItemResult { FString Path; bool bOk = false; int32 Status = 0; FString Message; FString LockId; FString Owner; };`
- `FString BuildBody(EOperation InOperation, const TArray<FString>& InRelativePaths, const FString& InRefName)`: `{"operation":"lock"|"unlock","paths":[...],"ref":{"name":...}}`, `ref` left out when `InRefName` is empty, `force` never present.
- `bool ParseResponse(const FString& InJson, const TArray<FString>& InRequestedPaths, TArray<FItemResult>& OutResults, FString& OutError)`: false when the body is not an object with a `results` array. Otherwise one `FItemResult` per requested path, in request order, matched by `path`. A requested path with no result is `bOk = false`, `Message = "no result from GitLfsCache"`.
- `bool ParseLfsEnvEndpoint(const TArray<FString>& InLines, FString& OutUrl)`: the URL from the first line starting exactly `Endpoint=`, cut at the first space. False when absent.
- `FString RemoteRefName(const FString& InUpstreamFullName, const FString& InCurrentBranch)`: `refs/remotes/<remote>/<x>` → `refs/heads/<x>`, else `refs/heads/<InCurrentBranch>` when that is non-empty, else empty.
- `float TimeoutSeconds(int32 InPathCount)`: `FMath::Min(60.0f, 2.0f + 0.05f * InPathCount)`.

- [ ] **Step 1: Write the failing tests** in new `Private/Tests/GitLfsLockBatchTests.cpp`, group `GitSourceControl.LfsLockBatch`:
  - `Body.Lock`: paths `{"Content/A.uasset","Content/Ä b.uasset"}`, ref `refs/heads/main` → parsed `operation == "lock"`, both paths round-trip, `ref.name == "refs/heads/main"`, no `force`.
  - `Body.UnlockWithoutRef`: `operation == "unlock"`, no `ref`.
  - `Parse.MixedResults`: the README example (A ok with id `871` and owner `someone`, B failed 409 `already locked`) → two results, A `bOk`, `LockId == "871"`, `Owner == "someone"`. B not ok, `Status == 409`, `Message == "already locked"`.
  - `Parse.MissingItemsFail`: requested A, B, C. Response has C then A → results in order A, B, C, B not ok with the "no result" message.
  - `Parse.RejectsNonBatchBody`: `{"locks":[]}`, `[]` and `not json` all return false.
  - `Env.Endpoint`: lines `"git-lfs/3.6.0 (...)"`, `""`, `"Endpoint=https://lfs.example.com/github/studio/game.git/info/lfs (auth=basic)"`, `"Endpoint (upstream)=https://other/info/lfs (auth=none)"` → `https://lfs.example.com/github/studio/game.git/info/lfs`. No `Endpoint=` line → false.
  - `Ref.Name`: `("refs/remotes/origin/feature/x", "local")` → `refs/heads/feature/x`. `("", "local")` → `refs/heads/local`. `("", "")` → empty.
  - `Timeout.GrowsAndCaps`: 0 → 2.0, 100 → 7.0, 10000 → 60.0.
- [ ] **Step 2:** Run tests. Expected: compile failure on the missing header (RED).
- [ ] **Step 3:** Implement `GitLfsLockBatch.{h,cpp}`.
- [ ] **Step 4:** Run tests. Expected: 34/34.
- [ ] **Step 5:** Commit "Add batched lock request and response logic".

### Task 4: Batched lock client and call sites

**Interfaces:**
- `GitLfsLockBatchClient::TryRun(GitLfsLockBatch::EOperation InOperation, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<GitLfsLockBatch::FItemResult>& OutResults) -> bool`: false means "not answered, use the stock command".
- `GitSourceControlUtils::RunLfsLockOperation(bool bInLock, const FString& InRepositoryRoot, const FString& InPathToGitBinary, const TArray<FString>& InRelativeFiles, TArray<FString>& OutInfoMessages, TArray<FString>& OutErrorMessages) -> bool`, declared in `GitSourceControlUtils.h`.

- [ ] **Step 1:** Implement the client. Once per session, under a critical section: `git lfs env` through `RunLFSCommand` → `ParseLfsEnvEndpoint`, `git rev-parse --symbolic-full-name @{upstream}` plus the current branch → `RemoteRefName`, and `git config --get --bool lfs.setlockablereadonly` (anything but `false` means read-only on unlock). The endpoint must pass the allowed-URL check. The credential comes from `ParseHttpsRemote(Endpoint)` plus `RunCredentialFill`, cached per client. The POST goes to `Endpoint + "/locks/batch"` with `TimeoutSeconds(N)`. Outcomes follow Global Constraints. Log through `FGitScopedLogTimer("GitLfsCache locks/batch <op> (<N> files)")` and log each fallback reason once.
- [ ] **Step 2:** Implement `RunLfsLockOperation`. On `TryRun` true: per `bOk` result, `SetReadOnly(false)` after a lock, or `SetReadOnly(true)` after an unlock when read-only is enabled. Only touch files that exist. Failed items add `"<path>: <message>"` to `OutErrorMessages`. Return true only when every item is ok. On false: `RunLFSCommand(bInLock ? "lock" : "unlock", ...)` unchanged.
- [ ] **Step 3:** Switch the four sites, each inside `// [DIVERGENCE]` with the old call kept commented: `FGitCheckOutWorker::Execute` blocking lock, `GitBackgroundLock::LockFiles`, `FGitCheckInWorker` unlock, `FGitRevertWorker` unlock. The provider's sync loop uses `GitServiceHttp::IsRequestInFlight()`.
- [ ] **Step 4:** In `UpdateCachedStates`, next to the existing writable workaround: a state whose `LockState` is `Locked` (by this user) makes its file writable when it exists.
- [ ] **Step 5:** Build. Expected: both succeed. Run tests: 34/34. No automated test for I/O (ruling, as in the first plan).
- [ ] **Step 6:** Update the uncommitted `CLAUDE.md` section on the caches and the PR description (manual checklist from the spec's Verification). Commit "Lock and unlock through GitLfsCache locks/batch".

### Task 5: Manual verification

Joins the existing merge gate: the spec's Verification list, including the new batched locking and whole-tree checks. It needs the HTTPS test repository from Matt.
