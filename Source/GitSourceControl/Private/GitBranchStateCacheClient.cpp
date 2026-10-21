#include "GitBranchStateCacheClient.h"

#include "GitBranchStateCache.h"
#include "GitSourceControlLog.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlUtils.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Runtime/Launch/Resources/Version.h"

// HTTP thread completion and EHttpFailureReason are used from 5.4. Older engines rely on the HTTP manager being ticked.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
#define GIT_BRANCH_STATE_MODERN_HTTP 1
#else
#define GIT_BRANCH_STATE_MODERN_HTTP 0
#endif

namespace GitBranchStateCacheClient
{

namespace
{
	/** The status check this sits on blocks the editor, so a slow service must cost little before the local path runs */
	constexpr float RequestTimeoutSeconds = 2.0f;

	/** Grace on top of the request timeout before the wait loop gives up on its own */
	constexpr double RequestWaitGraceSeconds = 1.0;

	/** git credential fill must never hang the status check, for example on a credential helper waiting for input */
	constexpr double CredentialTimeoutSeconds = 5.0;

	/** Matches GitSourceControlConstants::MaxFilesPerBatch, which is private to GitSourceControlUtils.cpp */
	constexpr int32 MaxFilesPerLsTree = 50;

	FCriticalSection StateCriticalSection;
	GitBranchStateCache::FBackoff Backoff;
	bool bDisabledForSession = false;
	bool bHasCredential = false;
	FString CachedUserName;
	FString CachedPassword;
	TSet<FString> LoggedReasons;

	FThreadSafeCounter RequestsInFlight;

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

	bool RunCredentialFill(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const GitBranchStateCache::FRemoteParts& InRemote, FString& OutUserName, FString& OutPassword)
	{
#if ENGINE_MAJOR_VERSION >= 5
		void* StdOutRead = nullptr;
		void* StdOutWrite = nullptr;
		void* StdInRead = nullptr;
		void* StdInWrite = nullptr;
		if (!FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite))
		{
			return false;
		}
		if (!FPlatformProcess::CreatePipe(StdInRead, StdInWrite, true))
		{
			FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
			return false;
		}
		ON_SCOPE_EXIT
		{
			FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
			FPlatformProcess::ClosePipe(StdInRead, StdInWrite);
		};

		// credential.interactive=never stops Git Credential Manager from showing a login window; the request only uses a
		// credential the artist already has. The credential itself only ever travels over the pipes, never the command line.
		const FString Params = FString::Printf(TEXT("-C \"%s\" -c credential.interactive=never credential fill"), *InRepositoryRoot);
		FProcHandle Process = FPlatformProcess::CreateProc(*InPathToGitBinary, *Params, false, true, true, nullptr, 0, nullptr, StdOutWrite, StdInRead);
		if (!Process.IsValid())
		{
			return false;
		}

		const FString Input = FString::Printf(TEXT("protocol=%s\nhost=%s\npath=%s\n\n"), *InRemote.Protocol, *InRemote.Host, *InRemote.Path);
		const FTCHARToUTF8 InputUtf8(*Input);
		FPlatformProcess::WritePipe(StdInWrite, reinterpret_cast<const uint8*>(InputUtf8.Get()), InputUtf8.Length());

		FString Output;
		bool bTimedOut = false;
		const double DeadlineSeconds = FPlatformTime::Seconds() + CredentialTimeoutSeconds;
		while (FPlatformProcess::IsProcRunning(Process))
		{
			Output += FPlatformProcess::ReadPipe(StdOutRead);
			if (FPlatformTime::Seconds() > DeadlineSeconds)
			{
				bTimedOut = true;
				FPlatformProcess::TerminateProc(Process, true);
				break;
			}
			FPlatformProcess::Sleep(0.01f);
		}
		Output += FPlatformProcess::ReadPipe(StdOutRead);

		int32 ReturnCode = -1;
		FPlatformProcess::GetProcReturnCode(Process, &ReturnCode);
		FPlatformProcess::CloseProc(Process);

		if (bTimedOut || ReturnCode != 0)
		{
			return false;
		}

		TArray<FString> Lines;
		Output.ParseIntoArrayLines(Lines);
		return GitBranchStateCache::ParseCredentialFill(Lines, OutUserName, OutPassword);
#else
		// UE4 lacks a writable stdin pipe for child processes, so the service is not used there
		return false;
#endif
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
		if (!RunCredentialFill(InPathToGitBinary, InRepositoryRoot, InRemote, UserName, Password))
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

	bool GetHeadBlobs(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TMap<FString, FString>& OutBlobs)
	{
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

	struct FPostResult
	{
		bool bResponded = false;
		bool bConnectionFailure = false;
		int32 ResponseCode = 0;
		FString Body;
		FString Error;
	};

	FPostResult PostState(const FString& InUrl, const FString& InAuthorization, const FString& InBody)
	{
		FPostResult Result;

		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(InUrl);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("Authorization"), InAuthorization);
		Request->SetContentAsString(InBody);
		Request->SetTimeout(RequestTimeoutSeconds);
#if GIT_BRANCH_STATE_MODERN_HTTP
		// The waiting thread may be a worker while the game thread is busy in a synchronous source control command
		Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);
#endif

		RequestsInFlight.Increment();
		ON_SCOPE_EXIT
		{
			RequestsInFlight.Decrement();
		};

		if (!Request->ProcessRequest())
		{
			Result.Error = TEXT("request could not be started");
			return Result;
		}

		const double DeadlineSeconds = FPlatformTime::Seconds() + RequestTimeoutSeconds + RequestWaitGraceSeconds;
		while (Request->GetStatus() == EHttpRequestStatus::Processing || Request->GetStatus() == EHttpRequestStatus::NotStarted)
		{
			if (IsInGameThread())
			{
				FHttpModule::Get().GetHttpManager().Tick(0.0f);
			}
			if (FPlatformTime::Seconds() > DeadlineSeconds)
			{
				Request->CancelRequest();
				Result.Error = TEXT("timed out");
				return Result;
			}
			FPlatformProcess::Sleep(0.005f);
		}

		if (Request->GetStatus() != EHttpRequestStatus::Succeeded)
		{
#if GIT_BRANCH_STATE_MODERN_HTTP
			const EHttpFailureReason FailureReason = Request->GetFailureReason();
			Result.bConnectionFailure = FailureReason == EHttpFailureReason::ConnectionError;
			Result.Error = FailureReason == EHttpFailureReason::TimedOut ? TEXT("timed out") : (Result.bConnectionFailure ? TEXT("could not connect") : TEXT("request failed"));
#else
			Result.bConnectionFailure = Request->GetStatus() == EHttpRequestStatus::Failed_ConnectionError;
			Result.Error = Result.bConnectionFailure ? TEXT("could not connect") : TEXT("request failed or timed out");
#endif
			return Result;
		}

		const FHttpResponsePtr Response = Request->GetResponse();
		if (!Response.IsValid())
		{
			Result.Error = TEXT("no response");
			return Result;
		}
		Result.bResponded = true;
		Result.ResponseCode = Response->GetResponseCode();
		Result.Body = Response->GetContentAsString();
		return Result;
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

	FGitScopedLogTimer Timer(FString::Printf(TEXT("GitBranchStateCache (%d files, %d branches)"), InAbsoluteFiles.Num(), InBranches.Num()));

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
		LogFallbackOnce(FString::Printf(TEXT("no stored git credential for %s"), *Remote.Host));
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

	const FTCHARToUTF8 UserPassUtf8(*FString::Printf(TEXT("%s:%s"), *UserName, *Password));
	const FString Authorization = TEXT("Basic ") + FBase64::Encode(reinterpret_cast<const uint8*>(UserPassUtf8.Get()), UserPassUtf8.Length());
	const FString Body = GitBranchStateCache::BuildStateRequestBody(Base, InBranches, RelativeFiles);

	const FPostResult Post = PostState(StateUrl + TEXT("/state"), Authorization, Body);
	if (!Post.bResponded)
	{
		if (Post.bConnectionFailure)
		{
			FScopeLock Lock(&StateCriticalSection);
			Backoff.RecordConnectionFailure(FPlatformTime::Seconds());
		}
		LogFallbackOnce(FString::Printf(TEXT("%s: %s"), *GitBranchStateCache::StripUserInfo(StateUrl), *Post.Error));
		Timer.SetOutcome(Post.Error);
		return false;
	}

	if (Post.ResponseCode == 401)
	{
		ForgetCredential();
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

	const TMap<FString, FString> NewerFiles = GitBranchStateCache::ComputeNewerFiles(Response, HeadBlobs, InCurrentRemoteBranch);
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

bool IsRequestInFlight()
{
	return RequestsInFlight.GetValue() > 0;
}

}

#undef GIT_BRANCH_STATE_MODERN_HTTP
