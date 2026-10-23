#include "GitBackgroundLock.h"

#include "Async/Async.h"
#include "GitSourceControlLog.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlState.h"
#include "GitSourceControlUtils.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "ISourceControlModule.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Runtime/Launch/Resources/Version.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitBackgroundLock
{
	static FCriticalSection RegistryCriticalSection;
	static FLockOperationRegistry Registry;
	static FThreadSafeCounter RunningLocks;

	/** How long an unlock waits for a background lock of the same file before going ahead anyway */
	static constexpr double MaxWaitForBackgroundLockSeconds = 120.0;

	uint64 FLockOperationRegistry::BeginBackgroundLock(const TArray<FString>& InPaths)
	{
		const uint64 Ticket = NextTicket++;
		for (const FString& Path : InPaths)
		{
			LatestTicketByPath.Add(Path, Ticket);
		}
		PendingPathsByTicket.Add(Ticket, InPaths);
		return Ticket;
	}

	void FLockOperationRegistry::EndBackgroundLock(const uint64 InTicket)
	{
		PendingPathsByTicket.Remove(InTicket);
	}

	void FLockOperationRegistry::BeginForegroundOperation(const TArray<FString>& InPaths)
	{
		const uint64 Ticket = NextTicket++;
		for (const FString& Path : InPaths)
		{
			LatestTicketByPath.Add(Path, Ticket);
		}
	}

	bool FLockOperationRegistry::IsAnyPending(const TArray<FString>& InPaths) const
	{
		// FString equality and hashing ignore case
		for (const TPair<uint64, TArray<FString>>& Pending : PendingPathsByTicket)
		{
			for (const FString& Path : InPaths)
			{
				if (Pending.Value.Contains(Path))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool FLockOperationRegistry::IsAnyPending() const
	{
		return PendingPathsByTicket.Num() > 0;
	}

	bool FLockOperationRegistry::IsLatest(const FString& InPath, const uint64 InTicket) const
	{
		const uint64* Latest = LatestTicketByPath.Find(InPath);
		return Latest != nullptr && *Latest == InTicket;
	}

	void FLockOperationRegistry::Reset()
	{
		// NextTicket keeps counting, so a result from before the reset never looks current
		LatestTicketByPath.Reset();
		PendingPathsByTicket.Reset();
	}

	static TArray<FString> ToAbsoluteFilenames(const TArray<FString>& RelativeFiles, const FString& PathToGitRoot)
	{
		TArray<FString> AbsoluteFiles;
		AbsoluteFiles.Reserve(RelativeFiles.Num());
		for (const FString& RelativeFile : RelativeFiles)
		{
			FString AbsoluteFile = FPaths::Combine(PathToGitRoot, RelativeFile);
			FPaths::NormalizeFilename(AbsoluteFile);
			AbsoluteFiles.Add(AbsoluteFile);
		}
		return AbsoluteFiles;
	}

	/** Runs on the game thread. The files are saved without a lock, so the user decides between discarding their changes or carrying on unprotected. */
	static void AskToRevertUnlockedFiles(const TArray<FString>& FailedFiles, const FString& Reason)
	{
		const FText Message = FText::Format(LOCTEXT("BackgroundLockFailed",
			"Could not lock:\n{0}\n\n{1}\n\n"
			"Your changes are saved without a lock and may conflict with someone else's.\n\n"
			"Yes: revert the file and discard your changes (reload the asset or level afterwards)\n"
			"No: keep your changes without a lock (this is cowboy, please understand what you're doing)"),
			FText::FromString(FString::Join(FailedFiles, TEXT("\n"))), FText::FromString(Reason));
		if (FMessageDialog::Open(EAppMsgType::YesNo, Message) == EAppReturnType::Yes)
		{
			// Revert picks what to restore from the cached state, which may predate the save that modified the file
			ISourceControlModule::Get().GetProvider().Execute(ISourceControlOperation::Create<FUpdateStatus>(), FailedFiles, EConcurrency::Synchronous);
#if ENGINE_MAJOR_VERSION >= 5
			// Unload first and reload after, or the next save writes the in-memory change straight back
			USourceControlHelpers::RevertAndReloadPackages(FailedFiles);
#else
			ISourceControlModule::Get().GetProvider().Execute(ISourceControlOperation::Create<FRevert>(), FailedFiles);
#endif
		}
	}

	/** Our optimistic locked state is wrong once the lock fails, so ask git lfs who really holds the locks and correct the cache */
	static void HandleLockFailure(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& RelativeFiles, const TArray<FString>& ErrorMessages, const uint64 Ticket)
	{
		TMap<const FString, FGitState> RefreshedStates;
		TArray<FString> RefreshErrors;
		GitSourceControlUtils::RefreshLocks(FGitSourceControlModule::GetEmptyStringArray(), PathToGitRoot, PathToGitBinary, RefreshErrors, RefreshedStates);

		const TArray<FString> AbsoluteFiles = ToAbsoluteFilenames(RelativeFiles, PathToGitRoot);
		TArray<FString> FailedFiles;
		TMap<const FString, FGitState> FailureStates = BuildLockFailureStates(RefreshedStates, AbsoluteFiles, FailedFiles);

		const FString Reason = FString::Join(ErrorMessages, TEXT("\n"));
		AsyncTask(ENamedThreads::GameThread, [RelativeFiles, AbsoluteFiles, Ticket, FailedFiles, Reason, FailureStates = MoveTemp(FailureStates)]() mutable
		{
			FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
			if (GitSourceControl == nullptr || !GitSourceControl->GetProvider().IsAvailable())
			{
				return;
			}

			// A revert, check in or lock that ran since this lock owns the file's lock state now
			{
				FScopeLock Lock(&RegistryCriticalSection);
				for (int32 Index = 0; Index < RelativeFiles.Num(); ++Index)
				{
					if (!Registry.IsLatest(RelativeFiles[Index], Ticket))
					{
						FailureStates.Remove(AbsoluteFiles[Index]);
						FailedFiles.Remove(AbsoluteFiles[Index]);
					}
				}
			}

			GitSourceControlUtils::UpdateCachedStates(FailureStates);
			if (FailedFiles.Num() > 0)
			{
				AskToRevertUnlockedFiles(FailedFiles, Reason);
			}
		});
	}

	bool CanLockInBackground(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& AbsoluteFiles, FString& OutReason)
	{
		FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
		if (GitSourceControl == nullptr)
		{
			OutReason = TEXT("plugin shutting down");
			return false;
		}
		FGitSourceControlProvider& Provider = GitSourceControl->GetProvider();

		// Up to date: trust the remote state from the last status check, whose freshness GitBranchStateCache decides.
		// RunUpdateStatus reads the state cache from worker threads the same way.
		TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> States;
		Provider.GetState(AbsoluteFiles, States, EStateCacheUsage::Use);
		for (const TSharedRef<ISourceControlState, ESPMode::ThreadSafe>& State : States)
		{
			const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> GitState = StaticCastSharedRef<FGitSourceControlState>(State);
			if (GitState->State.RemoteState == ERemoteState::NotAtHead || GitState->State.RemoteState == ERemoteState::NotLatest)
			{
				OutReason = FString::Printf(TEXT("%s has a newer version on %s"), *GitState->GetFilename(), *GitState->State.HeadBranch);
				return false;
			}
		}

		// Unlocked: ask the LFS server (GitLfsCache answers this from its lock snapshot). Not RefreshLocks, which falls back
		// to the local lock cache when the server is unreachable, and an unconfirmed lock must take the blocking path.
		const TArray<FString> RelativeFiles = GitSourceControlUtils::RelativeFilenames(AbsoluteFiles, PathToGitRoot);
		const TArray<FString> PathParameters = RelativeFiles.Num() == 1 ? TArray<FString>{ TEXT("-p") } : TArray<FString>();
		const TArray<FString>& PathFiles = RelativeFiles.Num() == 1 ? RelativeFiles : FGitSourceControlModule::GetEmptyStringArray();
		TArray<FString> Locks;
		TArray<FString> Errors;
		if (!GitSourceControlUtils::RunLFSCommand(TEXT("locks"), PathToGitRoot, PathToGitBinary, PathParameters, PathFiles, Locks, Errors))
		{
			OutReason = FString::Printf(TEXT("could not list locks: %s"), *FString::Join(Errors, TEXT(" ")));
			return false;
		}

		const TSet<FString> RequestedFiles(AbsoluteFiles);
		const FString LockUser = Provider.GetLockUser();
		for (const FString& Lock : Locks)
		{
			const GitSourceControlUtils::FGitLfsLocksParser LockInfo(PathToGitRoot, Lock);
			if (LockInfo.LockUser != LockUser && RequestedFiles.Contains(LockInfo.LocalFilename))
			{
				OutReason = FString::Printf(TEXT("%s is locked by %s"), *LockInfo.LocalFilename, *LockInfo.LockUser);
				return false;
			}
		}
		return true;
	}

	TMap<const FString, FGitState> BuildLockFailureStates(const TMap<const FString, FGitState>& InRefreshedLocks, const TArray<FString>& InAbsoluteFiles, TArray<FString>& OutFailedFiles)
	{
		TMap<const FString, FGitState> States;
		for (const FString& AbsoluteFile : InAbsoluteFiles)
		{
			const FGitState* Refreshed = InRefreshedLocks.Find(AbsoluteFile);
			if (Refreshed && Refreshed->LockState == ELockState::Locked)
			{
				continue;
			}

			// Only the lock fields are known here. Defaults elsewhere (Unknown, NotInRepo, UpToDate) would overwrite the
			// file's real status in the cache, and a revert skips files that no longer look modified.
			FGitState& State = States.Add(AbsoluteFile);
			State.FileState = EFileState::Unset;
			State.TreeState = ETreeState::Unset;
			State.RemoteState = ERemoteState::Unset;
			State.LockState = Refreshed && Refreshed->LockState == ELockState::LockedOther ? ELockState::LockedOther : ELockState::NotLocked;
			State.LockUser = Refreshed ? Refreshed->LockUser : FString();
			OutFailedFiles.Add(AbsoluteFile);
		}
		return States;
	}

	uint64 RegisterLock(const TArray<FString>& RelativeFiles)
	{
		FScopeLock Lock(&RegistryCriticalSection);
		return Registry.BeginBackgroundLock(RelativeFiles);
	}

	void LockFiles(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& RelativeFiles, const uint64 Ticket)
	{
		RunningLocks.Increment();
		Async(EAsyncExecution::ThreadPool, [PathToGitRoot, PathToGitBinary, RelativeFiles, Ticket]()
		{
			TArray<FString> InfoMessages;
			TArray<FString> ErrorMessages;
			const bool bLocked = GitSourceControlUtils::RunLfsLockOperation(true, PathToGitRoot, PathToGitBinary, RelativeFiles, InfoMessages, ErrorMessages);
			UE_LOG(LogGitSourceControl, Log, TEXT("[%s] Background lock of %d files %s"), GitSourceControlLog::GetThreadLabel(), RelativeFiles.Num(), bLocked ? TEXT("succeeded") : TEXT("failed"));
			{
				FScopeLock Lock(&RegistryCriticalSection);
				Registry.EndBackgroundLock(Ticket);
			}
			if (!bLocked)
			{
				HandleLockFailure(PathToGitRoot, PathToGitBinary, RelativeFiles, ErrorMessages, Ticket);
			}
			RunningLocks.Decrement();
		});
	}

	void WaitForBackgroundLocks(const TArray<FString>& RelativeFiles)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		for (;;)
		{
			{
				FScopeLock Lock(&RegistryCriticalSection);
				const bool bPending = Registry.IsAnyPending(RelativeFiles);
				if (!bPending || FPlatformTime::Seconds() - StartSeconds > MaxWaitForBackgroundLockSeconds)
				{
					if (bPending)
					{
						UE_LOG(LogGitSourceControl, Warning, TEXT("[%s] Gave up waiting for a background lock of the same files after %.0f s"), GitSourceControlLog::GetThreadLabel(), MaxWaitForBackgroundLockSeconds);
					}
					Registry.BeginForegroundOperation(RelativeFiles);
					return;
				}
			}
			FPlatformProcess::Sleep(0.02f);
		}
	}

	int32 NumRunningLocks()
	{
		return RunningLocks.GetValue();
	}

	void ForgetLocks()
	{
		FScopeLock Lock(&RegistryCriticalSection);
		Registry.Reset();
	}
}

#undef LOCTEXT_NAMESPACE
