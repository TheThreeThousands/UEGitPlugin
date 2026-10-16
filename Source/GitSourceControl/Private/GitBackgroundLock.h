#pragma once

#include "CoreMinimal.h"

/** Locks files with git lfs without blocking the caller, for check outs the status cache server already approved */
namespace GitBackgroundLock
{
	/**
	 * Runs git lfs lock on a thread pool task. If it fails, the cached lock states are corrected and the user is asked
	 * whether to revert the files, since they were already saved without a lock.
	 */
	void LockFiles(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& RelativeFiles);
}

