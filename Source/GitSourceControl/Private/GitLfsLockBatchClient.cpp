#include "GitLfsLockBatchClient.h"

#include "GitBranchStateCache.h"
#include "GitServiceHttp.h"
#include "GitSourceControlLog.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlUtils.h"

#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace GitLfsLockBatchClient
{

namespace
{
	FCriticalSection StateCriticalSection;
	GitBranchStateCache::FBackoff Backoff;
	bool bDisabledForSession = false;
	bool bDiscovered = false;
	FString Endpoint;
	FString RefName;
	bool bSetsLockableReadOnly = true;
	bool bHasCredential = false;
	FString CachedUserName;
	FString CachedPassword;
	TSet<FString> LoggedReasons;

	void LogFallbackOnce(const FString& InReason)
	{
		FScopeLock Lock(&StateCriticalSection);
		if (!LoggedReasons.Contains(InReason))
		{
			LoggedReasons.Add(InReason);
			UE_LOG(LogGitSourceControl, Log, TEXT("[%s] GitLfsCache locks/batch not used, falling back to git lfs: %s"), GitSourceControlLog::GetThreadLabel(), *InReason);
		}
	}

	void DisableForSession(const FString& InReason)
	{
		{
			FScopeLock Lock(&StateCriticalSection);
			bDisabledForSession = true;
		}
		LogFallbackOnce(InReason + TEXT(", disabled for this session"));
	}

	/** Reads once per session what git-lfs itself would use: the endpoint, the ref it sends, and whether it makes unlocked files read-only */
	void Discover(const FString& InPathToGitBinary, const FString& InRepositoryRoot)
	{
		TArray<FString> EnvLines;
		TArray<FString> Errors;
		FString FoundEndpoint;
		if (GitSourceControlUtils::RunLFSCommand(TEXT("env"), InRepositoryRoot, InPathToGitBinary, FGitSourceControlModule::GetEmptyStringArray(), FGitSourceControlModule::GetEmptyStringArray(), EnvLines, Errors))
		{
			GitLfsLockBatch::ParseLfsEnvEndpoint(EnvLines, FoundEndpoint);
		}
		while (FoundEndpoint.EndsWith(TEXT("/")))
		{
			FoundEndpoint.LeftChopInline(1);
		}

		TArray<FString> Upstream;
		Errors.Reset();
		GitSourceControlUtils::RunCommand(TEXT("rev-parse"), InPathToGitBinary, InRepositoryRoot, { TEXT("--symbolic-full-name"), TEXT("@{upstream}") }, FGitSourceControlModule::GetEmptyStringArray(), Upstream, Errors);
		FString CurrentBranch;
		GitSourceControlUtils::GetBranchName(InPathToGitBinary, InRepositoryRoot, CurrentBranch);

		TArray<FString> ReadOnly;
		Errors.Reset();
		GitSourceControlUtils::RunCommand(TEXT("config"), InPathToGitBinary, InRepositoryRoot, { TEXT("--get"), TEXT("--bool"), TEXT("lfs.setlockablereadonly") }, FGitSourceControlModule::GetEmptyStringArray(), ReadOnly, Errors);

		FScopeLock Lock(&StateCriticalSection);
		Endpoint = FoundEndpoint;
		RefName = GitLfsLockBatch::RemoteRefName(Upstream.Num() > 0 ? Upstream[0] : FString(), CurrentBranch);
		bSetsLockableReadOnly = !(ReadOnly.Num() > 0 && ReadOnly[0].TrimStartAndEnd() == TEXT("false"));
		bDiscovered = true;
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
}

bool TryRun(const GitLfsLockBatch::EOperation InOperation, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles,
			TArray<GitLfsLockBatch::FItemResult>& OutResults)
{
	if (InRelativeFiles.Num() == 0)
	{
		return false;
	}

	const TCHAR* const OperationName = InOperation == GitLfsLockBatch::EOperation::Lock ? TEXT("lock") : TEXT("unlock");
	FGitScopedLogTimer Timer(FString::Printf(TEXT("GitLfsCache locks/batch %s (%d files)"), OperationName, InRelativeFiles.Num()));

	bool bNeedsDiscovery = false;
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
		bNeedsDiscovery = !bDiscovered;
	}
	if (bNeedsDiscovery)
	{
		Discover(InPathToGitBinary, InRepositoryRoot);
	}

	FString Url;
	FString Ref;
	{
		FScopeLock Lock(&StateCriticalSection);
		Url = Endpoint;
		Ref = RefName;
	}

	// git-lfs already sends this credential to the same endpoint. Plain http is still refused, except to a local test instance.
	const bool bPlainHttp = Url.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase);
	GitBranchStateCache::FRemoteParts Remote;
	if (Url.IsEmpty() || !GitBranchStateCache::IsAllowedStateUrl(Url, true)
		|| !GitBranchStateCache::ParseHttpsRemote(bPlainHttp ? TEXT("https://") + Url.Mid(7) : Url, Remote))
	{
		DisableForSession(FString::Printf(TEXT("LFS endpoint is not https (%s)"), *GitBranchStateCache::StripUserInfo(Url)));
		Timer.SetOutcome(TEXT("endpoint not usable"));
		return false;
	}
	if (bPlainHttp)
	{
		Remote.Protocol = TEXT("http");
	}

	FString UserName;
	FString Password;
	if (!GetCredential(InPathToGitBinary, InRepositoryRoot, Remote, UserName, Password))
	{
		DisableForSession(FString::Printf(TEXT("no stored git credential for %s"), *Remote.Host));
		Timer.SetOutcome(TEXT("no credential"));
		return false;
	}

	const FString BatchUrl = Url + TEXT("/locks/batch");
	const FString Body = GitLfsLockBatch::BuildBody(InOperation, InRelativeFiles, Ref);
	const GitServiceHttp::FPostResult Post = GitServiceHttp::PostJson(BatchUrl, GitServiceHttp::BasicAuthorization(UserName, Password), Body, GitLfsLockBatch::TimeoutSeconds(InRelativeFiles.Num()));
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
		LogFallbackOnce(FString::Printf(TEXT("%s: %s"), *GitBranchStateCache::StripUserInfo(BatchUrl), *Post.Error));
		Timer.SetOutcome(Post.Error);
		return false;
	}

	if (Post.ResponseCode == 404)
	{
		// No such route: lfs.url is not GitLfsCache, or its locking is switched off
		DisableForSession(FString::Printf(TEXT("%s answered HTTP 404"), *GitBranchStateCache::StripUserInfo(BatchUrl)));
		Timer.SetOutcome(TEXT("HTTP 404"));
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
			LogFallbackOnce(FString::Printf(TEXT("%s refused the stored credential twice, disabled for this session"), *GitBranchStateCache::StripUserInfo(BatchUrl)));
		}
	}
	if (Post.ResponseCode != 200)
	{
		LogFallbackOnce(FString::Printf(TEXT("%s answered HTTP %d"), *GitBranchStateCache::StripUserInfo(BatchUrl), Post.ResponseCode));
		Timer.SetOutcome(FString::Printf(TEXT("HTTP %d"), Post.ResponseCode));
		return false;
	}

	FString Error;
	if (!GitLfsLockBatch::ParseResponse(Post.Body, InRelativeFiles, OutResults, Error))
	{
		DisableForSession(FString::Printf(TEXT("%s is not a locks/batch answer: %s"), *GitBranchStateCache::StripUserInfo(BatchUrl), *Error));
		Timer.SetOutcome(Error);
		return false;
	}

	int32 Succeeded = 0;
	for (const GitLfsLockBatch::FItemResult& Result : OutResults)
	{
		Succeeded += Result.bOk ? 1 : 0;
	}
	{
		FScopeLock Lock(&StateCriticalSection);
		Backoff.RecordSuccess();
		LoggedReasons.Reset();
	}
	Timer.SetOutcome(FString::Printf(TEXT("%d of %d succeeded"), Succeeded, OutResults.Num()));
	return true;
}

bool SetsLockableReadOnly()
{
	FScopeLock Lock(&StateCriticalSection);
	return bSetsLockableReadOnly;
}

}
