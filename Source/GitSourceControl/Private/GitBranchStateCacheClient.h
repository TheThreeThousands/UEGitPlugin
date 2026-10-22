#pragma once

#include "CoreMinimal.h"

/**
 * Asks ktsu.GitBranchStateCache which files are newer on which status branch, in place of running git log and one
 * git diff per branch locally. Every failure returns false so the caller runs the local path instead.
 */
namespace GitBranchStateCacheClient
{

/** The per user override if set, else [GitSourceControl] BranchStateCacheUrl from the editor ini, else empty. No trailing slash. */
FString GetStateUrl();

/**
 * The service diffs each branch from its merge base with InBase. When a status branch holds commits from InBase..HEAD (local
 * commits that reached that branch but not the upstream), the true merge base is newer, and a file that branch later changed
 * back to InBase's content would look up to date. bOutAmbiguous says so. False when git could not answer.
 */
bool IsBaseAmbiguous(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InBase, const TArray<FString>& InBranches, bool& bOutAmbiguous);

/**
 * Fills OutChangedRelativeFilesByBranch with, per branch, the repository relative files that branch has a different
 * version of than the local HEAD. True only when the service gave a complete, usable answer.
 *
 * @param InAbsoluteFiles        files or directories to check; a directory asks the service for the whole tree
 * @param InBranches             concrete remote branch names, e.g. "origin/main"
 * @param InCurrentRemoteBranch  the current branch's remote, or empty when it has none
 */
bool TryGetChangedFilesByBranch(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InAbsoluteFiles,
								const TArray<FString>& InBranches, const FString& InCurrentRemoteBranch, TMap<FString, TArray<FString>>& OutChangedRelativeFilesByBranch);

}
