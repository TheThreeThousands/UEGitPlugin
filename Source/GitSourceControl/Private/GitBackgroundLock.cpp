#include "GitBackgroundLock.h"

#include "Async/Async.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlState.h"
#include "GitSourceControlUtils.h"
#include "ISourceControlModule.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
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
			ISourceControlModule::Get().GetProvider().Execute(ISourceControlOperation::Create<FRevert>(), FailedFiles);
		}
	}

	/** Our optimistic locked state is wrong once the lock fails, so ask git lfs who really holds the locks and correct the cache */
	static void HandleLockFailure(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& RelativeFiles, const TArray<FString>& ErrorMessages)
	{
		TMap<const FString, FGitState> RefreshedStates;
		TArray<FString> RefreshErrors;
		GitSourceControlUtils::RefreshLocks(FGitSourceControlModule::GetEmptyStringArray(), PathToGitRoot, PathToGitBinary, RefreshErrors, RefreshedStates);

		TArray<FString> FailedFiles;
		for (const FString& AbsoluteFile : ToAbsoluteFilenames(RelativeFiles, PathToGitRoot))
		{
			const FGitState* RefreshedState = RefreshedStates.Find(AbsoluteFile);
			if (RefreshedState && RefreshedState->LockState == ELockState::Locked)
			{
				continue;
			}
			if (!RefreshedState)
			{
				RefreshedStates.Add(AbsoluteFile).LockState = ELockState::NotLocked;
			}
			FailedFiles.Add(AbsoluteFile);
		}

		const FString Reason = FString::Join(ErrorMessages, TEXT("\n"));
		AsyncTask(ENamedThreads::GameThread, [FailedFiles, Reason, RefreshedStates = MoveTemp(RefreshedStates)]()
		{
			GitSourceControlUtils::UpdateCachedStates(RefreshedStates);
			if (!FailedFiles.IsEmpty())
			{
				AskToRevertUnlockedFiles(FailedFiles, Reason);
			}
		});
	}

	void LockFiles(const FString& PathToGitRoot, const FString& PathToGitBinary, const TArray<FString>& RelativeFiles)
	{
		Async(EAsyncExecution::ThreadPool, [PathToGitRoot, PathToGitBinary, RelativeFiles]()
		{
			TArray<FString> InfoMessages;
			TArray<FString> ErrorMessages;
			const bool bLocked = GitSourceControlUtils::RunLFSCommand(TEXT("lock"), PathToGitRoot, PathToGitBinary, FGitSourceControlModule::GetEmptyStringArray(), RelativeFiles, InfoMessages, ErrorMessages);
			if (!bLocked)
			{
				HandleLockFailure(PathToGitRoot, PathToGitBinary, RelativeFiles, ErrorMessages);
			}
		});
	}
}

#undef LOCTEXT_NAMESPACE

