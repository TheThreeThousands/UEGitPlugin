#pragma once

#include "CoreMinimal.h"
#include "GitLfsLockBatch.h"

/**
 * Locks or unlocks many files in one request through ktsu.GitLfsCache's locks/batch extension, at the LFS endpoint git-lfs
 * itself uses. Every failure to get an answer returns false so the caller runs the stock git lfs command instead.
 */
namespace GitLfsLockBatchClient
{

/**
 * True when GitLfsCache answered for every path in OutResults (some items may still have failed, which is final).
 * False when it was not asked or did not answer, and nothing is known about the paths.
 */
bool TryRun(GitLfsLockBatch::EOperation InOperation, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles,
			TArray<GitLfsLockBatch::FItemResult>& OutResults);

/** False when lfs.setlockablereadonly is false, i.e. git-lfs would leave an unlocked file writable. Only valid after TryRun. */
bool SetsLockableReadOnly();

}
