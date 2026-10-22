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

	enum class EChunkOutcome : uint8
	{
		Answered,
		NotAsked,
		Unknown,
	};

	/** Sends one chunk. Without a well-formed answer, the outcome says whether upstream may have acted on it anyway. */
	EChunkOutcome PostChunk(const FString& InBatchUrl, const FString& InAuthorization, const GitLfsLockBatch::EOperation InOperation, const TArray<FString>& InChunk,
							const FString& InRef, TArray<GitLfsLockBatch::FItemResult>& OutResults)
	{
		const FString Body = GitLfsLockBatch::BuildBody(InOperation, InChunk, InRef);
		const GitServiceHttp::FPostResult Post = GitServiceHttp::PostJson(InBatchUrl, InAuthorization, Body, GitLfsLockBatch::TimeoutSeconds(InChunk.Num()));
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
			LogFallbackOnce(FString::Printf(TEXT("%s: %s"), *GitBranchStateCache::StripUserInfo(InBatchUrl), *Post.Error));
			// A refused connection never reached GitLfsCache, but a request that timed out may already have been fanned out
			return Post.bConnectionFailure ? EChunkOutcome::NotAsked : EChunkOutcome::Unknown;
		}

		if (Post.ResponseCode == 404)
		{
			// No such route: lfs.url is not GitLfsCache, or its locking is switched off
			DisableForSession(FString::Printf(TEXT("%s answered HTTP 404"), *GitBranchStateCache::StripUserInfo(InBatchUrl)));
			return EChunkOutcome::NotAsked;
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
				LogFallbackOnce(FString::Printf(TEXT("%s refused the stored credential twice, disabled for this session"), *GitBranchStateCache::StripUserInfo(InBatchUrl)));
			}
		}
		if (Post.ResponseCode != 200)
		{
			// Refused as a whole (bad request, too many paths, credential), so no item was attempted
			LogFallbackOnce(FString::Printf(TEXT("%s answered HTTP %d"), *GitBranchStateCache::StripUserInfo(InBatchUrl), Post.ResponseCode));
			return EChunkOutcome::NotAsked;
		}

		FString Error;
		if (!GitLfsLockBatch::ParseResponse(Post.Body, InChunk, OutResults, Error))
		{
			// Not GitLfsCache, which would have answered this route with results; it will answer the same way every time
			DisableForSession(FString::Printf(TEXT("%s is not a locks/batch answer: %s"), *GitBranchStateCache::StripUserInfo(InBatchUrl), *Error));
			return EChunkOutcome::NotAsked;
		}
		return EChunkOutcome::Answered;
	}
}

void Run(const GitLfsLockBatch::EOperation InOperation, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, FOutcome& OutOutcome)
{
	OutOutcome = FOutcome();
	if (InRelativeFiles.Num() == 0)
	{
		return;
	}
	OutOutcome.NotAsked = InRelativeFiles;

	const TCHAR* const OperationName = InOperation == GitLfsLockBatch::EOperation::Lock ? TEXT("lock") : TEXT("unlock");
	FGitScopedLogTimer Timer(FString::Printf(TEXT("GitLfsCache locks/batch %s (%d files)"), OperationName, InRelativeFiles.Num()));

	bool bNeedsDiscovery = false;
	{
		FScopeLock Lock(&StateCriticalSection);
		if (bDisabledForSession)
		{
			Timer.SetOutcome(TEXT("disabled for this session"));
			return;
		}
		if (Backoff.ShouldSkip(FPlatformTime::Seconds()))
		{
			Timer.SetOutcome(TEXT("skipped, service recently unreachable"));
			return;
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
		return;
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
		return;
	}

	const FString BatchUrl = Url + TEXT("/locks/batch");
	const FString Authorization = GitServiceHttp::BasicAuthorization(UserName, Password);
	OutOutcome.NotAsked.Reset();

	// Chunks stay under GitLfsCache's per request limit. After the first chunk without an answer, the rest are not sent.
	const TArray<TArray<FString>> Chunks = GitLfsLockBatch::SplitIntoChunks(InRelativeFiles);
	for (int32 ChunkIndex = 0; ChunkIndex < Chunks.Num(); ++ChunkIndex)
	{
		TArray<GitLfsLockBatch::FItemResult> ChunkResults;
		const EChunkOutcome ChunkOutcome = PostChunk(BatchUrl, Authorization, InOperation, Chunks[ChunkIndex], Ref, ChunkResults);
		if (ChunkOutcome == EChunkOutcome::Answered)
		{
			OutOutcome.Answered.Append(MoveTemp(ChunkResults));
			continue;
		}

		(ChunkOutcome == EChunkOutcome::Unknown ? OutOutcome.Unknown : OutOutcome.NotAsked).Append(Chunks[ChunkIndex]);
		for (int32 Remaining = ChunkIndex + 1; Remaining < Chunks.Num(); ++Remaining)
		{
			OutOutcome.NotAsked.Append(Chunks[Remaining]);
		}
		break;
	}

	int32 Succeeded = 0;
	for (const GitLfsLockBatch::FItemResult& Result : OutOutcome.Answered)
	{
		Succeeded += Result.bOk ? 1 : 0;
	}
	if (OutOutcome.Answered.Num() > 0)
	{
		FScopeLock Lock(&StateCriticalSection);
		Backoff.RecordSuccess();
		LoggedReasons.Reset();
	}
	Timer.SetOutcome(FString::Printf(TEXT("%d of %d answered succeeded, %d unanswered, %d not sent"), Succeeded, OutOutcome.Answered.Num(), OutOutcome.Unknown.Num(), OutOutcome.NotAsked.Num()));
}

bool SetsLockableReadOnly()
{
	FScopeLock Lock(&StateCriticalSection);
	return bSetsLockableReadOnly;
}

}
