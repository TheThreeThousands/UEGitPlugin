#include "GitBackgroundLock.h"

#include "Async/Async.h"
#include "GitSourceControlLog.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlState.h"
#include "GitSourceControlUtils.h"
#include "ISourceControlModule.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Runtime/Launch/Resources/Version.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitBackgroundLock
{
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
#if ENGINE_MAJOR_VERSION >= 5
			// Unload first and reload after, or the next save writes the in-memory change straight back
			USourceControlHelpers::RevertAndReloadPackages(FailedFiles);
#else
			ISourceControlModule::Get().GetProvider().Execute(ISourceControlOperation::Create<FRevert>(), FailedFiles);
#endif
		}
	}

	/** Our optimistic locked state is wrong once the lock fails, so ask git lfs who really holds the locks and correct the cache */
	static void HandleLockFailure(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& RelativeFiles, const TArray<FString>& ErrorMessages)
	{
		TMap<const FString, FGitState> RefreshedStates;
		TArray<FString> RefreshErrors;
		GitSourceControlUtils::RefreshLocks(FGitSourceControlModule::GetEmptyStringArray(), PathToGitRoot, PathToGitBinary, RefreshErrors, RefreshedStates);

		TArray<FString> FailedFiles;
		TMap<const FString, FGitState> FailureStates = BuildLockFailureStates(RefreshedStates, ToAbsoluteFilenames(RelativeFiles, PathToGitRoot), FailedFiles);

		const FString Reason = FString::Join(ErrorMessages, TEXT("\n"));
		AsyncTask(ENamedThreads::GameThread, [FailedFiles, Reason, FailureStates = MoveTemp(FailureStates)]()
		{
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

	void LockFiles(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& RelativeFiles)
	{
		Async(EAsyncExecution::ThreadPool, [PathToGitRoot, PathToGitBinary, RelativeFiles]()
		{
			TArray<FString> InfoMessages;
			TArray<FString> ErrorMessages;
			const bool bLocked = GitSourceControlUtils::RunLfsLockOperation(true, PathToGitRoot, PathToGitBinary, RelativeFiles, InfoMessages, ErrorMessages);
			UE_LOG(LogGitSourceControl, Log, TEXT("[%s] Background lock of %d files %s"), GitSourceControlLog::GetThreadLabel(), RelativeFiles.Num(), bLocked ? TEXT("succeeded") : TEXT("failed"));
			if (!bLocked)
			{
				HandleLockFailure(PathToGitRoot, PathToGitBinary, RelativeFiles, ErrorMessages);
			}
		});
	}
}

#undef LOCTEXT_NAMESPACE
