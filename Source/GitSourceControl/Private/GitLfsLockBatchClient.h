#pragma once

#include "CoreMinimal.h"
#include "GitLfsLockBatch.h"

/**
 * Locks or unlocks many files per request through ktsu.GitLfsCache's locks/batch extension, at the LFS endpoint git-lfs
 * itself uses. Paths it could not get an answer for are handed back so the caller can run the stock git lfs command.
 */
namespace GitLfsLockBatchClient
{

/** Where each requested path ended up */
struct FOutcome
{
	/** GitLfsCache answered for these, which is final even for failed items */
	TArray<GitLfsLockBatch::FItemResult> Answered;

	/** Never reached GitLfsCache (not configured, unreachable, refused as a whole): the stock git lfs command handles them */
	TArray<FString> NotAsked;

	/** Sent but not answered (a timeout): upstream may already have locked or unlocked some of them */
	TArray<FString> Unknown;
};

/** Sends InRelativeFiles to GitLfsCache's locks/batch in chunks, and reports which paths it answered for */
void Run(GitLfsLockBatch::EOperation InOperation, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, FOutcome& OutOutcome);

/** False when lfs.setlockablereadonly is false, i.e. git-lfs would leave an unlocked file writable. Only valid after Run. */
bool SetsLockableReadOnly();

}
