#pragma once

#include "CoreMinimal.h"
#include "GitSourceControlState.h"

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
	 * Which lock operation last touched each path, and which background locks have not finished. Paths compare without case.
	 * Not thread safe on its own; the functions below guard the one the plugin uses.
	 */
	class FLockOperationRegistry
	{
	public:
		/** Records a background lock on InPaths that has not finished, and returns its ticket */
		uint64 BeginBackgroundLock(const TArray<FString>& InPaths);

		/** The background lock with this ticket has finished, successfully or not */
		void EndBackgroundLock(uint64 InTicket);

		/** Records a lock or unlock that runs in the foreground, after the background locks on its paths finished */
		void BeginForegroundOperation(const TArray<FString>& InPaths);

		bool IsAnyPending(const TArray<FString>& InPaths) const;
		bool IsAnyPending() const;

		/** True when no lock or unlock on InPath started after the one with InTicket */
		bool IsLatest(const FString& InPath, uint64 InTicket) const;

		void Reset();

	private:
		uint64 NextTicket = 1;
		TMap<FString, uint64> LatestTicketByPath;
		TMap<uint64, TArray<FString>> PendingPathsByTicket;
	};

	/** Records a background lock for a check out, before its optimistic Locked states are published. Returns the ticket for LockFiles. */
	uint64 RegisterLock(const TArray<FString>& RelativeFiles);

	/**
	 * Runs the lock registered under Ticket on a thread pool task. Call it after the check out's Locked states are in the
	 * cache, so a failure's correction lands after them. If it fails, the cached lock states are corrected and the user is
	 * asked whether to revert the files, since they were already saved without a lock, unless a later lock or unlock of a
	 * file has run since.
	 */
	void LockFiles(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& RelativeFiles, uint64 Ticket);

	/** Waits for background locks on RelativeFiles to finish, then records a foreground lock or unlock of them. Worker threads only. */
	void WaitForBackgroundLocks(const TArray<FString>& RelativeFiles);

	/** Background locks still running on the thread pool, which the provider waits for when it closes */
	int32 NumRunningLocks();

	/** Starts handling failed background locks on the game thread. Called when the provider initializes. */
	void Start();

	/**
	 * Cancels the HTTP requests of background locks still running, so they finish quickly without falling back to git lfs,
	 * and stops handling failures. The provider then waits for NumRunningLocks to reach zero.
	 */
	void BeginShutdown();

	/** Forgets every registered lock and failure, including any whose check out never reached LockFiles. Called when the provider closes. */
	void ForgetLocks();

	/**
	 * The cache update after a failed background lock: one entry per requested file not locked by us, carrying only the lock
	 * fields (everything else Unset, so UpdateCachedStates leaves the file's status alone and a revert still sees it modified).
	 * Locks on other files are left out. OutFailedFiles lists those requested files, in request order.
	 */
	TMap<const FString, FGitState> BuildLockFailureStates(const TMap<const FString, FGitState>& InRefreshedLocks, const TArray<FString>& InAbsoluteFiles, TArray<FString>& OutFailedFiles);
}
