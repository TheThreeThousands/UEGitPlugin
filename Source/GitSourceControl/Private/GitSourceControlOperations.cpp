// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlOperations.h"

#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SourceControlOperations.h"
#include "ISourceControlModule.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlCommand.h"
#include "GitSourceControlUtils.h"
#include "SourceControlHelpers.h"
#include "Logging/MessageLog.h"
#include "Misc/MessageDialog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformFile.h"
#if ENGINE_MAJOR_VERSION >= 5
#include "HAL/PlatformFileManager.h"
#else
#include "HAL/PlatformFilemanager.h"
#endif

#include <thread>

#define LOCTEXT_NAMESPACE "GitSourceControl"

FName FGitConnectWorker::GetName() const
{
	return "Connect";
}

bool FGitConnectWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// The connect worker checks if we are connected to the remote server.
	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FConnect, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FConnect>(InCommand.Operation);

	// Skip login operations, since Git does not have to login.
	// It's not a big deal for async commands though, so let those go through.
	// More information: this is a heuristic for cases where UE is trying to create
	// a valid Perforce connection as a side effect for the connect worker. For Git,
	// the connect worker has no side effects. It is simply a query to retrieve information
	// to be displayed to the user, like in the revision control settings or on init.
	// Therefore, there is no need for synchronously establishing a connection if not there.
	if (InCommand.Concurrency == EConcurrency::Synchronous)
	{
		InCommand.bCommandSuccessful = true;
		return true;
	}

	// Check Git availability
	// We already know that Git is available if PathToGitBinary is not empty, since it is validated then.
	if (InCommand.PathToGitBinary.IsEmpty())
	{
		const FText& NotFound = LOCTEXT("GitNotFound", "Failed to enable Git revision control. You need to install Git and ensure the plugin has a valid path to the git executable.");
		InCommand.ResultInfo.ErrorMessages.Add(NotFound.ToString());
		Operation->SetErrorText(NotFound);
		InCommand.bCommandSuccessful = false;
		return false;
	}

	// Get default branch: git remote show
	
	TArray<FString> Parameters {
		TEXT("-h"), // Only limit to branches
		TEXT("-q") // Skip printing out remote URL, we don't use it
	};
	
	// Check if remote matches our refs.
	// Could be useful in the future, but all we want to know right now is if connection is up.
	// Parameters.Add("--exit-code");
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("ls-remote"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (!InCommand.bCommandSuccessful)
	{
		const FText& NotFound = LOCTEXT("GitRemoteFailed", "Failed Git remote connection. Ensure your repo is initialized, and check your connection to the Git host.");
		InCommand.ResultInfo.ErrorMessages.Add(NotFound.ToString());
		Operation->SetErrorText(NotFound);
	}

	// TODO: always return true, and enter an offline mode if could not connect to remote
	return InCommand.bCommandSuccessful;
}

bool FGitConnectWorker::UpdateStates() const
{
	return false;
}

FName FGitCheckOutWorker::GetName() const
{
	return "CheckOut";
}

bool FGitCheckOutWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	if (!InCommand.bUsingGitLfsLocking)
	{
		InCommand.bCommandSuccessful = false;
		return InCommand.bCommandSuccessful;
	}

	// lock files: execute the LFS command on relative filenames
	const TArray<FString>& RelativeFiles = GitSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToGitRoot);

	const TArray<FString>& LockableRelativeFiles = RelativeFiles.FilterByPredicate(GitSourceControlUtils::IsFileLFSLockable);

	if (LockableRelativeFiles.Num() < 1)
	{
		InCommand.bCommandSuccessful = true;
		return InCommand.bCommandSuccessful;
	}

	const bool bSuccess = GitSourceControlUtils::RunLFSCommand(TEXT("lock"), InCommand.PathToGitRoot, InCommand.PathToGitBinary, FGitSourceControlModule::GetEmptyStringArray(), LockableRelativeFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	InCommand.bCommandSuccessful = bSuccess;
	const FString& LockUser = FGitSourceControlModule::Get().GetProvider().GetLockUser();
	if (bSuccess)
	{
		TArray<FString> AbsoluteFiles;
		for (const auto& RelativeFile : RelativeFiles)
		{
			FString AbsoluteFile = FPaths::Combine(InCommand.PathToGitRoot, RelativeFile);
			FGitLockedFilesCache::AddLockedFile(AbsoluteFile, LockUser);
			FPaths::NormalizeFilename(AbsoluteFile);
			AbsoluteFiles.Add(AbsoluteFile);
		}

		GitSourceControlUtils::CollectNewStates(AbsoluteFiles, States, EFileState::Unset, ETreeState::Unset, ELockState::Locked);
		for (auto& State : States)
		{
			State.Value.LockUser = LockUser;
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitCheckOutWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

static FText ParseCommitResults(const TArray<FString>& InResults)
{
	if (InResults.Num() >= 1)
	{
		const FString& FirstLine = InResults[0];
		return FText::Format(LOCTEXT("CommitMessage", "Commited {0}."), FText::FromString(FirstLine));
	}
	return LOCTEXT("CommitMessageUnknown", "Submitted revision.");
}

FName FGitCheckInWorker::GetName() const
{
	return "CheckIn";
}

const FText EmptyCommitMsg;

bool FGitCheckInWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);

	// make a temp file to place our commit message in
	bool bDoCommit = InCommand.Files.Num() > 0;
	const FText& CommitMsg = bDoCommit ? Operation->GetDescription() : EmptyCommitMsg;
	FGitScopedTempFile CommitMsgFile(CommitMsg);
	if (CommitMsgFile.GetFilename().Len() > 0)
	{
		FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();

		// Fetch so we can check if there are pending commits to pull.
		bool success = GitSourceControlUtils::FetchRemote(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, false,
			InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

		// We don't want to allow UE to pull changes, it might be something we add support for in future, but right now this git plugin rebases, and it's causing content teams lots of grief.
		// so if we've got pending commits on remote, just bail.
		int NumRevisionsBehind = 0;
		success = GitSourceControlUtils::GetNumRevisionsBehindOrigin(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, NumRevisionsBehind, InCommand.ResultInfo.ErrorMessages);

		if (!success)
		{
			FText CommitFailedMessage(LOCTEXT("GitCommit_FetchFailed_Msg", "Git Commit failed to check for remote changes\nPlease check your internet connection and try again."));
			FText CommitFailedTitle(LOCTEXT("GitCommit_FetchFailed_Title", "Git Commit failed"));
			FMessageDialog::Open(EAppMsgType::Ok, CommitFailedMessage, CommitFailedTitle);
			UE_LOG(LogSourceControl, Log, TEXT("Commit failed because we couldn't fetch remote status."));

			InCommand.bCommandSuccessful = false;
			return false;
		}

		if (NumRevisionsBehind > 0)
		{
			FText CommitFailedMessage(LOCTEXT("GitCommit_OutOfDate_Msg", "Git Commit failed because there are newer revisions available on remote.\nPlease close Unreal Engine, update through GitHub Desktop and try again."));
			FText CommitFailedTitle(LOCTEXT("GitCommit_OutOfDate_Title", "Git Commit failed - Out Of Date"));
			FMessageDialog::Open(EAppMsgType::Ok, CommitFailedMessage, CommitFailedTitle);
			UE_LOG(LogSourceControl, Log, TEXT("Commit failed because there are newer revisions available on remote."));
			InCommand.bCommandSuccessful = false;
			return false;
		}

		if (bDoCommit)
		{
			FString ParamCommitMsgFilename = TEXT("--file=\"");
			ParamCommitMsgFilename += FPaths::ConvertRelativePathToFull(CommitMsgFile.GetFilename());
			ParamCommitMsgFilename += TEXT("\"");
			TArray<FString> CommitParameters {ParamCommitMsgFilename};
			const TArray<FString>& FilesToCommit = GitSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);

			// If no files were committed, this is false, so we treat it as if we never wanted to commit in the first place.
			bDoCommit = GitSourceControlUtils::RunCommit(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, CommitParameters,
														FilesToCommit, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}

		// If we commit, we can push up the deleted state to gone
		if (bDoCommit)
		{
			// Remove any deleted files from status cache
			TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
			Provider.GetState(InCommand.Files, LocalStates, EStateCacheUsage::Use);
			for (const auto& State : LocalStates)
			{
				if (State->IsDeleted())
				{
					Provider.RemoveFileFromCache(State->GetFilename());
				}
			}
			Operation->SetSuccessMessage(ParseCommitResults(InCommand.ResultInfo.InfoMessages));
			const FString& Message = (InCommand.ResultInfo.InfoMessages.Num() > 0) ? InCommand.ResultInfo.InfoMessages[0] : TEXT("");
			UE_LOG(LogSourceControl, Log, TEXT("commit successful: %s"), *Message);
			GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);
		}

		// Collect difference between the remote and what we have on top of remote locally. This is to handle unpushed commits other than the one we just did.
		// Doesn't matter that we're not synced. Because our local branch is always based on the remote.
		TArray<FString> CommittedFiles;
		FString BranchName;
		bool bDiffSuccess;
		if (GitSourceControlUtils::GetRemoteBranchName(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, BranchName))
		{
			TArray<FString> Parameters {"--name-only", FString::Printf(TEXT("%s...HEAD"), *BranchName), "--"};
			bDiffSuccess = GitSourceControlUtils::RunCommand(TEXT("diff"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parameters,
															  FGitSourceControlModule::GetEmptyStringArray(), CommittedFiles, InCommand.ResultInfo.ErrorMessages);
		}
		else
		{
			// Get all non-remote commits and list out their files
			TArray<FString> Parameters {"--branches", "--not" "--remotes", "--name-only", "--pretty="};
			bDiffSuccess = GitSourceControlUtils::RunCommand(TEXT("log"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), CommittedFiles, InCommand.ResultInfo.ErrorMessages);
			// Dedup files list between commits
			CommittedFiles = TSet<FString>{CommittedFiles}.Array();
		}

		bool bUnpushedFiles;
		TSet<FString> FilesToCheckIn {InCommand.Files};
		if (bDiffSuccess)
		{
			// Only push if we have a difference (any commits at all, not just the one we just did)
			bUnpushedFiles = CommittedFiles.Num() > 0;
			CommittedFiles = GitSourceControlUtils::AbsoluteFilenames(CommittedFiles, InCommand.PathToRepositoryRoot);
			FilesToCheckIn.Append(CommittedFiles.FilterByPredicate(GitSourceControlUtils::IsFileLFSLockable));
		}
		else
		{
			// Be cautious, try pushing anyway
			bUnpushedFiles = true;
		}

		TArray<FString> PulledFiles;

		// If we have unpushed files, push
		if (bUnpushedFiles)
		{
			// TODO: configure remote
			TArray<FString> PushParameters {TEXT("-u"), TEXT("origin"), TEXT("HEAD")};
			InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("push"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot,
																			 PushParameters, FGitSourceControlModule::GetEmptyStringArray(),
																			 InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

			if (!InCommand.bCommandSuccessful)
			{
				// if out of date, pull first, then try again
				bool bWasOutOfDate = false;
				for (const auto& PushError : InCommand.ResultInfo.ErrorMessages)
				{
					if ((PushError.Contains(TEXT("[rejected]")) && (PushError.Contains(TEXT("non-fast-forward")) || PushError.Contains(TEXT("fetch first")))) ||
						PushError.Contains(TEXT("cannot lock ref")))
					{
						// Don't do it during iteration, want to append pull results to InCommand.ResultInfo.ErrorMessages
						bWasOutOfDate = true;
						break;
					}
				}
				if (bWasOutOfDate)
				{
					// Get latest
					const bool bFetched = GitSourceControlUtils::FetchRemote(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, false,
																			 InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
					if (bFetched)
					{
						// Update local with latest
						const bool bPulled = GitSourceControlUtils::PullOrigin(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot,
																			   FGitSourceControlModule::GetEmptyStringArray(), PulledFiles,
																			   InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
						if (bPulled)
						{
							InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(
								TEXT("push"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, PushParameters,
								FGitSourceControlModule::GetEmptyStringArray(), InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
						}
					}

					// Our push still wasn't successful
					if (!InCommand.bCommandSuccessful)
					{
						if (!Provider.bPendingRestart)
						{
							// If it fails, just let the user do it
							FText PushFailMessage(LOCTEXT("GitPush_OutOfDate_Msg", "Git Push failed because there are changes you need to pull.\n\n"
																				   "An attempt was made to pull, but failed, because while the Unreal Editor is "
																				   "open, files cannot always be updated.\n\n"
																				   "Please exit the editor, and update the project again."));
							FText PushFailTitle(LOCTEXT("GitPush_OutOfDate_Title", "Git Pull Required"));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
							FMessageDialog::Open(EAppMsgType::Ok, PushFailMessage, PushFailTitle);
#else
							FMessageDialog::Open(EAppMsgType::Ok, PushFailMessage, &PushFailTitle);
#endif
							UE_LOG(LogSourceControl, Log, TEXT("Push failed because we're out of date, prompting user to resolve manually"));
						}
					}
				}
			}
		}
		else
		{
			InCommand.bCommandSuccessful = true;
		}

		// git-lfs: unlock files
		if (InCommand.bUsingGitLfsLocking)
		{
			// If we successfully pushed (or didn't need to push), unlock the files marked for check in
			if (InCommand.bCommandSuccessful)
			{
				// unlock files: execute the LFS command on relative filenames
				// (unlock only locked files, that is, not Added files)
				TArray<FString> LockedFiles;
				GitSourceControlUtils::GetLockedFiles(FilesToCheckIn.Array(), LockedFiles);
				if (LockedFiles.Num() > 0)
				{
					const TArray<FString>& FilesToUnlock = GitSourceControlUtils::RelativeFilenames(LockedFiles, InCommand.PathToGitRoot);

					if (FilesToUnlock.Num() > 0)
					{
						// Not strictly necessary to succeed, so don't update command success
						const bool bUnlockSuccess = GitSourceControlUtils::RunLFSCommand(TEXT("unlock"), InCommand.PathToGitRoot, InCommand.PathToGitBinary,
																						 FGitSourceControlModule::GetEmptyStringArray(), FilesToUnlock,
																						 InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
						if (bUnlockSuccess)
						{
							for (const auto& File : LockedFiles)
							{
								FGitLockedFilesCache::RemoveLockedFile(File);
							}
						}
					}
				}
#if 0
				for (const FString& File : FilesToCheckIn.Array())
				{
					FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File, true);
				}
#endif
			}
		}

		// Collect all the files we touched through the pull update
		if (bUnpushedFiles && PulledFiles.Num())
		{
			FilesToCheckIn.Append(PulledFiles);
		}
		// Before, we added only lockable files from CommittedFiles. But now, we want to update all files, not just lockables.
		FilesToCheckIn.Append(CommittedFiles);

		// now update the status of our files
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking,
															   FilesToCheckIn.Array(), InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		return InCommand.bCommandSuccessful;
	}

	InCommand.bCommandSuccessful = false;

	return false;
}

bool FGitCheckInWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitMarkForAddWorker::GetName() const
{
	return "MarkForAdd";
}

bool FGitMarkForAddWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Added, ETreeState::Staged);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	}

	return InCommand.bCommandSuccessful;
}

bool FGitMarkForAddWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitDeleteWorker::GetName() const
{
	return "Delete";
}

bool FGitDeleteWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Deleted, ETreeState::Staged);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	}

	return InCommand.bCommandSuccessful;
}

bool FGitDeleteWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}


void GroupFileCommandsForRevert(const TArray<FString>& InFiles, TArray<FString>& FilesToRemove, TArray<FString>& FilesToCheckout, TArray<FString>& FilesToReset, TArray<FString>& FilesToDelete)
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

	TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
	Provider.GetState(InFiles, LocalStates, EStateCacheUsage::Use);
	for (const auto& State : LocalStates)
	{
		if (State->IsAdded())
		{
			if (FPaths::FileExists(State->GetFilename()))
			{
				// Git rm won't delete the file because the engine still has it in use, and reset won't work on a file which doesn't exist on disk
				// so we have to delete it ourselves, and then remove it from the index.
				if (USourceControlPreferences::ShouldDeleteNewFilesOnRevert())
				{
					FilesToDelete.Add(State->GetFilename());
					FilesToRemove.Add(State->GetFilename());
				}
				else
				{
					FilesToReset.Add(State->GetFilename());
				}
			}
			else
			{
				// When you delete a file through content browser, UE will send a revert command to allow us to clean up the stage state.
				FilesToRemove.Add(State->GetFilename());
			}
		}
		// Checkout to head will reset the file back to what it is in your current commit, and reset the index.
		else if (State->CanRevert())
		{
			FilesToCheckout.Add(State->GetFilename());
		}
	}
}

FName FGitRevertWorker::GetName() const
{
	return "Revert";
}

bool FGitRevertWorker::Execute(FGitSourceControlCommand& InCommand)
{
	InCommand.bCommandSuccessful = true;

	// Filter files by status
	const bool bRevertAll = InCommand.Files.Num() == 0;
	if (bRevertAll)
	{
		TArray<FString> Params;
		Params.Add(TEXT("--hard"));
		InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Params, FGitSourceControlModule::GetEmptyStringArray(), InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

		Params.Reset(2);
		Params.Add(TEXT("-f")); // force
		Params.Add(TEXT("-d")); // remove directories
		InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("clean"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Params, FGitSourceControlModule::GetEmptyStringArray(), InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	else
	{
		TArray<FString> FilesToRemove;
		TArray<FString> FilesToCheckout;
		TArray<FString> FilesToReset;
		TArray<FString> FilesToDelete;
		GroupFileCommandsForRevert(InCommand.Files, FilesToRemove, FilesToCheckout, FilesToReset, FilesToDelete);

		// Verify we haven't missed performing an operation on any file passed on for revert
		ensure(FilesToRemove.Num() + FilesToCheckout.Num() + FilesToReset.Num() == InCommand.Files.Num());

		for (const FString& FileName : FilesToDelete)
		{
			bool RequireExists = true;
			bool EvenReadOnly = true;
			IFileManager::Get().Delete(*FileName, RequireExists, EvenReadOnly);
		}
		if (FilesToReset.Num() > 0)
		{
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), FilesToReset, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}
		if (FilesToRemove.Num() > 0)
		{
			// "Added" files that have been deleted needs to be removed from revision control
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), FilesToRemove, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}
		if (FilesToCheckout.Num() > 0)
		{
			// HEAD param allows us to re-pull files which have been deleted.
			TArray<FString> Params;
			Params.Add(TEXT("HEAD"));
			// Checkout back to the last commit for any modified files.
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("checkout"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Params, FilesToCheckout, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}
	}

	// This is all the files we "asked" to revert, in the case of InCommand.Files everything *should* be changed
	// in the case where we didn't pass in any files, we ran a revert on the repository root, so we should refresh everything
	const TArray<FString>& RequestedReverts = bRevertAll ? FGitSourceControlModule::Get().GetProvider().GetFilesInCache() : InCommand.Files;
	if (InCommand.bUsingGitLfsLocking)
	{
		// unlock files: execute the LFS command on relative filenames
		TArray<FString> LockedFiles;
		GitSourceControlUtils::GetLockedFiles(RequestedReverts, LockedFiles);
		if (LockedFiles.Num() > 0)
		{
			const TArray<FString>& RelativeFiles = GitSourceControlUtils::RelativeFilenames(LockedFiles, InCommand.PathToGitRoot);
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunLFSCommand(TEXT("unlock"), InCommand.PathToGitRoot, InCommand.PathToGitBinary, FGitSourceControlModule::GetEmptyStringArray(), RelativeFiles,
																				 InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
			if (InCommand.bCommandSuccessful)
			{
				for (const auto& File : LockedFiles)
				{
					FGitLockedFilesCache::RemoveLockedFile(File);
				}
			}
		}
	}

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, RequestedReverts, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));

	return InCommand.bCommandSuccessful;
}

bool FGitRevertWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitSyncWorker::GetName() const
{
	return "Sync";
}

bool FGitSyncWorker::Execute(FGitSourceControlCommand& InCommand)
{
	FText SyncNotAllowedMessage(LOCTEXT("GitSync_NotAllowed_Msg", "Please exit Unreal Engine and update through GitHub Desktop to get latest changes."));
	FText SyncNotAllowedTitle(LOCTEXT("GitSync_NotAllowed_Title", "Synching is not allowed"));
	FMessageDialog::Open(EAppMsgType::Ok, SyncNotAllowedMessage, SyncNotAllowedTitle);
	UE_LOG(LogSourceControl, Log, TEXT("Sync cancelled - not allowed."));

	InCommand.bCommandSuccessful = false;
	return false;

	/*
	TArray<FString> Results;
	const bool bFetched = GitSourceControlUtils::FetchRemote(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, false, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	if (!bFetched)
	{
		return false;
	}

	InCommand.bCommandSuccessful = GitSourceControlUtils::PullOrigin(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.Files, InCommand.Files, Results, InCommand.ResultInfo.ErrorMessages);

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	const bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking,
																 InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);

	return InCommand.bCommandSuccessful;
	*/
}

bool FGitSyncWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitFetch::GetName() const
{
	return "Fetch";
}

FText FGitFetch::GetInProgressString() const
{
	// TODO Configure origin
	return LOCTEXT("SourceControl_Push", "Fetching from remote origin...");
}

FName FGitFetchWorker::GetName() const
{
	return "Fetch";
}

bool FGitFetchWorker::Execute(FGitSourceControlCommand& InCommand)
{
	InCommand.bCommandSuccessful = GitSourceControlUtils::FetchRemote(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking,
																	  InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	if (!InCommand.bCommandSuccessful)
	{
		return false;
	}

	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FGitFetch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FGitFetch>(InCommand.Operation);

	if (Operation->bUpdateStatus)
	{
		// Now update the status of all our files
		const TArray<FString> ProjectDirs { FString(FPlatformProcess::BaseDir()) };
		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking,
																			  ProjectDirs, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitFetchWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitUpdateStatusWorker::GetName() const
{
	return "UpdateStatus";
}

bool FGitUpdateStatusWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);

	if(InCommand.Files.Num() > 0)
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
			if (Operation->ShouldUpdateHistory())
			{
				for (const auto& State : UpdatedStates)
				{
					const FString& File = State.Key;
					TGitSourceControlHistory History;

					if (State.Value.IsConflicted())
					{
						// In case of a merge conflict, we first need to get the tip of the "remote branch" (MERGE_HEAD)
						GitSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, File, true,
															 InCommand.ResultInfo.ErrorMessages, History);
					}
					// Get the history of the file in the current branch
					InCommand.bCommandSuccessful &= GitSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, File, false,
																						 InCommand.ResultInfo.ErrorMessages, History);
					Histories.Add(*File, History);
				}
			}
		}
	}
	else
	{
		// no path provided: only update the status of assets in Content/ directory and also Config files
		const TArray<FString> ProjectDirs {FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()), FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()),
										   FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())};
		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, ProjectDirs, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);

	// don't use the ShouldUpdateModifiedState() hint here as it is specific to Perforce: the above normal Git status has already told us this information (like Git and Mercurial)

	return InCommand.bCommandSuccessful;
}

bool FGitUpdateStatusWorker::UpdateStates() const
{
	bool bUpdated = GitSourceControlUtils::UpdateCachedStates(States);

	FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>( "GitSourceControl" );
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	const bool bUsingGitLfsLocking = Provider.UsesCheckout();

	// TODO without LFS : Workaround a bug with the Source Control Module not updating file state after a simple "Save" with no "Checkout" (when not using File Lock)
	const FDateTime Now = bUsingGitLfsLocking ? FDateTime::Now() : FDateTime::MinValue();

	// add history, if any
	for(const auto& History : Histories)
	{
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(History.Key);
		State->History = History.Value;
		State->TimeStamp = Now;
		bUpdated = true;
	}

	return bUpdated;
}

FName FGitCopyWorker::GetName() const
{
	return "Copy";
}

bool FGitCopyWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	// Copy or Move operation on a single file : Git does not need an explicit copy nor move,
	// but after a Move the Editor create a redirector file with the old asset name that points to the new asset.
	// The redirector needs to be committed with the new asset to perform a real rename.
	// => the following is to "MarkForAdd" the redirector, but it still need to be committed by selecting the whole directory and "check-in"
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Added, ETreeState::Staged);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		const bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitCopyWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitResolveWorker::GetName() const
{
	return "Resolve";
}

bool FGitResolveWorker::Execute( class FGitSourceControlCommand& InCommand )
{
	check(InCommand.Operation->GetName() == GetName());

	// mark the conflicting files as resolved:
	TArray<FString> Results;
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, Results, InCommand.ResultInfo.ErrorMessages);

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	const bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}

	return InCommand.bCommandSuccessful;
}

bool FGitResolveWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitMoveToChangelistWorker::GetName() const
{
	return "MoveToChangelist";
}

bool FGitMoveToChangelistWorker::UpdateStates() const
{
	return true;
}

bool FGitMoveToChangelistWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	FGitSourceControlChangelist DestChangelist = InCommand.Changelist;
	bool bResult = false;
	if(DestChangelist.GetName().Equals(TEXT("Staged")))
	{
		bResult = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	else if(DestChangelist.GetName().Equals(TEXT("Working")))
	{
		TArray<FString> Parameter;
		Parameter.Add(TEXT("--staged"));
		bResult = GitSourceControlUtils::RunCommand(TEXT("restore"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parameter, InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	
	if (bResult)
	{
		TMap<FString, FGitSourceControlState> DummyStates;
		GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.InfoMessages, DummyStates);
	}
	return bResult;
}

FName FGitUpdateStagingWorker::GetName() const
{
	return "UpdateChangelistsStatus";
}

bool FGitUpdateStagingWorker::Execute(FGitSourceControlCommand& InCommand)
{
	return GitSourceControlUtils::UpdateChangelistStateByCommand();
}

bool FGitUpdateStagingWorker::UpdateStates() const
{
	return true;
}

#undef LOCTEXT_NAMESPACE
