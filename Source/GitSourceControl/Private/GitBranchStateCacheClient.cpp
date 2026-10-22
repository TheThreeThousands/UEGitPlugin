#include "GitBranchStateCacheClient.h"

#include "GitBranchStateCache.h"
#include "GitSourceControlLog.h"
#include "GitSourceControlModule.h"
#include "GitServiceHttp.h"
#include "GitSourceControlUtils.h"

#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace GitBranchStateCacheClient
{

namespace
{
	/** The status check this sits on blocks the editor, so a slow service must cost little before the local path runs */
	constexpr float RequestTimeoutSeconds = 2.0f;

	/** Matches GitSourceControlConstants::MaxFilesPerBatch, which is private to GitSourceControlUtils.cpp */
	constexpr int32 MaxFilesPerLsTree = 50;

	FCriticalSection StateCriticalSection;
	GitBranchStateCache::FBackoff Backoff;
	bool bDisabledForSession = false;
	bool bHasCredential = false;
	FString CachedUserName;
	FString CachedPassword;
	TSet<FString> LoggedReasons;

	/** Logs a reason for falling back once, rather than on every status check while the service stays unavailable */
	void LogFallbackOnce(const FString& InReason)
	{
		FScopeLock Lock(&StateCriticalSection);
		if (!LoggedReasons.Contains(InReason))
		{
			LoggedReasons.Add(InReason);
			UE_LOG(LogGitSourceControl, Log, TEXT("[%s] GitBranchStateCache not used, falling back to local git: %s"), GitSourceControlLog::GetThreadLabel(), *InReason);
		}
	}

	bool GetCredential(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const GitBranchStateCache::FRemoteParts& InRemote, FString& OutUserName, FString& OutPassword)
	{
		{
			FScopeLock Lock(&StateCriticalSection);
			if (bHasCredential)
			{
				OutUserName = CachedUserName;
				OutPassword = CachedPassword;
				return true;
			}
		}

		FString UserName;
		FString Password;
		if (!GitServiceHttp::RunCredentialFill(InPathToGitBinary, InRepositoryRoot, InRemote, UserName, Password))
		{
			return false;
		}

		FScopeLock Lock(&StateCriticalSection);
		CachedUserName = UserName;
		CachedPassword = Password;
		bHasCredential = true;
		OutUserName = MoveTemp(UserName);
		OutPassword = MoveTemp(Password);
		return true;
	}

	void ForgetCredential()
	{
		FScopeLock Lock(&StateCriticalSection);
		bHasCredential = false;
		CachedUserName.Empty();
		CachedPassword.Empty();
	}

	/** An ancestor of HEAD the remote has: the fork point with the upstream, or with the first status branch for a local only branch */
	bool GetBaseCommit(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InBranches, FString& OutBase)
	{
		TArray<FString> Results;
		TArray<FString> Errors;
		if (GitSourceControlUtils::RunCommand(TEXT("merge-base"), InPathToGitBinary, InRepositoryRoot, { TEXT("HEAD"), TEXT("@{upstream}") }, FGitSourceControlModule::GetEmptyStringArray(), Results, Errors)
			&& Results.Num() > 0)
		{
			OutBase = Results[0].TrimStartAndEnd();
			return !OutBase.IsEmpty();
		}

		Results.Reset();
		Errors.Reset();
		if (InBranches.Num() > 0
			&& GitSourceControlUtils::RunCommand(TEXT("merge-base"), InPathToGitBinary, InRepositoryRoot, { TEXT("HEAD"), InBranches[0] }, FGitSourceControlModule::GetEmptyStringArray(), Results, Errors)
			&& Results.Num() > 0)
		{
			OutBase = Results[0].TrimStartAndEnd();
			return !OutBase.IsEmpty();
		}
		return false;
	}

	/** True for the repository root as RelativeFilenames spells it */
	bool IsRepositoryRoot(const FString& InRelativePath)
	{
		return InRelativePath.IsEmpty() || InRelativePath == TEXT(".") || InRelativePath == TEXT("./") || InRelativePath == TEXT("/");
	}

	/** Directories in InRelativeFiles recurse, and the repository root reads the whole tree in one call */
	bool GetHeadBlobs(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TMap<FString, FString>& OutBlobs)
	{
		if (InRelativeFiles.ContainsByPredicate(&IsRepositoryRoot))
		{
			FString Output;
			FString Errors;
			if (!GitSourceControlUtils::RunCommandInternalRaw(TEXT("-c core.quotePath=false ls-tree"), InPathToGitBinary, InRepositoryRoot, { TEXT("-r"), TEXT("HEAD") }, FGitSourceControlModule::GetEmptyStringArray(), Output, Errors))
			{
				return false;
			}
			OutBlobs = GitBranchStateCache::ParseLsTree(Output);
			return true;
		}

		for (int32 Start = 0; Start < InRelativeFiles.Num(); Start += MaxFilesPerLsTree)
		{
			const int32 Count = FMath::Min(MaxFilesPerLsTree, InRelativeFiles.Num() - Start);
			const TArray<FString> Batch(InRelativeFiles.GetData() + Start, Count);

			// Not -z: older engines cut process output at the first NUL. core.quotePath=false keeps non-ASCII paths readable,
			// and ParseLsTree unescapes the few names git still quotes.
			FString Output;
			FString Errors;
			if (!GitSourceControlUtils::RunCommandInternalRaw(TEXT("-c core.quotePath=false ls-tree"), InPathToGitBinary, InRepositoryRoot, { TEXT("-r"), TEXT("HEAD"), TEXT("--") }, Batch, Output, Errors))
			{
				return false;
			}
			OutBlobs.Append(GitBranchStateCache::ParseLsTree(Output));
		}
		return true;
	}

}

static FString GetStateUrl(bool& bOutFromUserOverride)
{
	FString Url;
	if (FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe())
	{
		Url = GitSourceControl->AccessSettings().GetBranchStateCacheUrlOverride();
	}
	bOutFromUserOverride = !Url.TrimStartAndEnd().IsEmpty();
	if (!bOutFromUserOverride && GConfig)
	{
		GConfig->GetString(TEXT("GitSourceControl"), TEXT("BranchStateCacheUrl"), Url, GEditorIni);
	}
	Url.TrimStartAndEndInline();
	while (Url.EndsWith(TEXT("/")))
	{
		Url.LeftChopInline(1);
	}
	return Url;
}

FString GetStateUrl()
{
	bool bFromUserOverride = false;
	return GetStateUrl(bFromUserOverride);
}

bool TryGetChangedFilesByBranch(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InAbsoluteFiles,
								const TArray<FString>& InBranches, const FString& InCurrentRemoteBranch, TMap<FString, TArray<FString>>& OutChangedRelativeFilesByBranch)
{
	bool bFromUserOverride = false;
	const FString StateUrl = GetStateUrl(bFromUserOverride);
	if (StateUrl.IsEmpty() || InAbsoluteFiles.Num() == 0 || InBranches.Num() == 0)
	{
		return false;
	}

	// A directory asks for every changed path (the service matches paths exactly), and the answer is cut down to the request below
	const bool bWholeTree = InAbsoluteFiles.ContainsByPredicate([](const FString& Path) { return FPaths::DirectoryExists(Path); });

	FGitScopedLogTimer Timer(FString::Printf(TEXT("GitBranchStateCache (%d %s, %d branches)"), InAbsoluteFiles.Num(), bWholeTree ? TEXT("paths, whole tree") : TEXT("files"), InBranches.Num()));

	// The artist's forge credential goes to this URL, so never over plain http except to a local test instance
	if (!GitBranchStateCache::IsAllowedStateUrl(StateUrl, bFromUserOverride))
	{
		LogFallbackOnce(FString::Printf(TEXT("BranchStateCacheUrl must be https (%s)"), *GitBranchStateCache::StripUserInfo(StateUrl)));
		Timer.SetOutcome(TEXT("URL not allowed"));
		return false;
	}

	{
		FScopeLock Lock(&StateCriticalSection);
		if (bDisabledForSession)
		{
			Timer.SetOutcome(TEXT("disabled for this session"));
			return false;
		}
		if (Backoff.ShouldSkip(FPlatformTime::Seconds()))
		{
			Timer.SetOutcome(TEXT("skipped, service recently unreachable"));
			return false;
		}
	}

	FString RemoteUrl;
	GitBranchStateCache::FRemoteParts Remote;
	if (!GitSourceControlUtils::GetRemoteUrl(InPathToGitBinary, InRepositoryRoot, RemoteUrl) || !GitBranchStateCache::ParseHttpsRemote(RemoteUrl, Remote))
	{
		{
			FScopeLock Lock(&StateCriticalSection);
			bDisabledForSession = true;
		}
		LogFallbackOnce(FString::Printf(TEXT("origin is not an https remote (%s), disabled for this session"), *GitBranchStateCache::StripUserInfo(RemoteUrl)));
		Timer.SetOutcome(TEXT("origin is not https"));
		return false;
	}

	FString UserName;
	FString Password;
	if (!GetCredential(InPathToGitBinary, InRepositoryRoot, Remote, UserName, Password))
	{
		// Asking again on every move would cost a credential helper start (or its full timeout) each time
		{
			FScopeLock Lock(&StateCriticalSection);
			bDisabledForSession = true;
		}
		LogFallbackOnce(FString::Printf(TEXT("no stored git credential for %s, disabled for this session"), *Remote.Host));
		Timer.SetOutcome(TEXT("no credential"));
		return false;
	}

	FString Base;
	if (!GetBaseCommit(InPathToGitBinary, InRepositoryRoot, InBranches, Base))
	{
		LogFallbackOnce(TEXT("could not find a pushed ancestor of HEAD"));
		Timer.SetOutcome(TEXT("no base commit"));
		return false;
	}

	const TArray<FString> RelativeFiles = GitSourceControlUtils::RelativeFilenames(InAbsoluteFiles, InRepositoryRoot);
	TMap<FString, FString> HeadBlobs;
	if (!GetHeadBlobs(InPathToGitBinary, InRepositoryRoot, RelativeFiles, HeadBlobs))
	{
		LogFallbackOnce(TEXT("could not read local blob ids"));
		Timer.SetOutcome(TEXT("ls-tree failed"));
		return false;
	}

	const FString Authorization = GitServiceHttp::BasicAuthorization(UserName, Password);
	const FString Body = GitBranchStateCache::BuildStateRequestBody(Base, InBranches, RelativeFiles, !bWholeTree);

	const GitServiceHttp::FPostResult Post = GitServiceHttp::PostJson(StateUrl + TEXT("/state"), Authorization, Body, RequestTimeoutSeconds);
	if (!Post.bResponded)
	{
		{
			FScopeLock Lock(&StateCriticalSection);
			if (Post.bConnectionFailure)
			{
				Backoff.RecordConnectionFailure(FPlatformTime::Seconds());
			}
			else if (Post.bTimedOut)
			{
				Backoff.RecordTimeout(FPlatformTime::Seconds());
			}
		}
		LogFallbackOnce(FString::Printf(TEXT("%s: %s"), *GitBranchStateCache::StripUserInfo(StateUrl), *Post.Error));
		Timer.SetOutcome(Post.Error);
		return false;
	}

	if (Post.ResponseCode == 401)
	{
		ForgetCredential();
		bool bGiveUp = false;
		{
			FScopeLock Lock(&StateCriticalSection);
			bGiveUp = Backoff.RecordUnauthorized();
			bDisabledForSession |= bGiveUp;
		}
		if (bGiveUp)
		{
			LogFallbackOnce(FString::Printf(TEXT("%s refused the stored credential twice, disabled for this session"), *GitBranchStateCache::StripUserInfo(StateUrl)));
		}
	}
	if (Post.ResponseCode < 200 || Post.ResponseCode >= 300)
	{
		LogFallbackOnce(FString::Printf(TEXT("%s answered HTTP %d"), *GitBranchStateCache::StripUserInfo(StateUrl), Post.ResponseCode));
		Timer.SetOutcome(FString::Printf(TEXT("HTTP %d"), Post.ResponseCode));
		return false;
	}

	GitBranchStateCache::FStateResponse Response;
	FString Reason;
	if (!GitBranchStateCache::ParseStateResponse(Post.Body, Response, Reason) || !GitBranchStateCache::IsUsable(Response, Reason))
	{
		LogFallbackOnce(FString::Printf(TEXT("unusable answer: %s"), *Reason));
		Timer.SetOutcome(Reason);
		return false;
	}
	if (!GitBranchStateCache::AnswersForBranch(Response, InCurrentRemoteBranch))
	{
		// Status branches missing from the mirror are fine to skip, but the current branch carries every NotAtHead
		LogFallbackOnce(FString::Printf(TEXT("the service does not know %s yet"), *InCurrentRemoteBranch));
		Timer.SetOutcome(TEXT("current branch missing"));
		return false;
	}

	TMap<FString, FString> NewerFiles = GitBranchStateCache::ComputeNewerFiles(Response, HeadBlobs, InCurrentRemoteBranch);
	if (bWholeTree && !RelativeFiles.ContainsByPredicate(&IsRepositoryRoot))
	{
		for (auto It = NewerFiles.CreateIterator(); It; ++It)
		{
			if (!GitBranchStateCache::IsUnderRequestedPath(It.Key(), RelativeFiles))
			{
				It.RemoveCurrent();
			}
		}
	}
	for (const TPair<FString, FString>& NewerFile : NewerFiles)
	{
		OutChangedRelativeFilesByBranch.FindOrAdd(NewerFile.Value).Add(NewerFile.Key);
	}

	{
		FScopeLock Lock(&StateCriticalSection);
		Backoff.RecordSuccess();
		LoggedReasons.Reset();
	}
	Timer.SetOutcome(FString::Printf(TEXT("%d newer files"), NewerFiles.Num()));
	return true;
}

}
