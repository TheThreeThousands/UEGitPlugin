#pragma once

#include "CoreMinimal.h"

/**
 * Locks files with git lfs without blocking the caller. Used for check outs that a quick pre-check found safe: nobody else
 * holds the lock and the cached remote state shows the file up to date, so the lock is expected to succeed.
 */
namespace GitBackgroundLock
{
	/**
	 * True when every file may be locked in the background: the last status check found no newer version on a status
	 * branch, and the LFS server (not the local lock cache) reports no lock held by someone else. OutReason says why not.
	 */
	bool CanLockInBackground(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& AbsoluteFiles, FString& OutReason);

	/**
	 * Runs git lfs lock on a thread pool task. If it fails, the cached lock states are corrected and the user is asked
	 * whether to revert the files, since they were already saved without a lock.
	 */
	void LockFiles(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& RelativeFiles);
}
