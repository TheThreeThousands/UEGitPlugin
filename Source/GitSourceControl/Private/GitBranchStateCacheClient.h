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
 * Fills OutChangedRelativeFilesByBranch with, per branch, the repository relative files that branch has a different
 * version of than the local HEAD. True only when the service gave a complete, usable answer.
 *
 * @param InAbsoluteFiles        files to check, no directories
 * @param InBranches             concrete remote branch names, e.g. "origin/main"
 * @param InCurrentRemoteBranch  the current branch's remote, or empty when it has none
 */
bool TryGetChangedFilesByBranch(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InAbsoluteFiles,
								const TArray<FString>& InBranches, const FString& InCurrentRemoteBranch, TMap<FString, TArray<FString>>& OutChangedRelativeFilesByBranch);

}
