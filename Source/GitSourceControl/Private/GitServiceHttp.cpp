#include "GitServiceHttp.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "Runtime/Launch/Resources/Version.h"

// HTTP thread completion and EHttpFailureReason are used from 5.4. Older engines rely on the HTTP manager being ticked.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
#define GIT_SERVICE_HTTP_MODERN 1
#else
#define GIT_SERVICE_HTTP_MODERN 0
#endif

namespace GitServiceHttp
{

namespace
{
	/** Grace on top of the request timeout before the wait loop gives up on its own */
	constexpr double RequestWaitGraceSeconds = 1.0;

	/** git credential fill must never hang a status check, for example on a credential helper waiting for input */
	constexpr double CredentialTimeoutSeconds = 5.0;

	FThreadSafeCounter RequestsInFlight;

	FThreadSafeBool bRequestsCancelled;
}

FPostResult PostJson(const FString& InUrl, const FString& InAuthorization, const FString& InBody, const float InTimeoutSeconds)
{
	FPostResult Result;

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(InUrl);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Authorization"), InAuthorization);
	Request->SetContentAsString(InBody);
	Request->SetTimeout(InTimeoutSeconds);
#if GIT_SERVICE_HTTP_MODERN
	// The waiting thread may be a worker while the game thread is busy in a synchronous source control command
	Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);
#endif

	RequestsInFlight.Increment();
	ON_SCOPE_EXIT
	{
		RequestsInFlight.Decrement();
	};

	if (bRequestsCancelled || !Request->ProcessRequest())
	{
		Result.Error = bRequestsCancelled ? TEXT("cancelled") : TEXT("request could not be started");
		return Result;
	}

	const double DeadlineSeconds = FPlatformTime::Seconds() + InTimeoutSeconds + RequestWaitGraceSeconds;
	while (Request->GetStatus() == EHttpRequestStatus::Processing || Request->GetStatus() == EHttpRequestStatus::NotStarted)
	{
		if (IsInGameThread())
		{
			FHttpModule::Get().GetHttpManager().Tick(0.0f);
		}
		if (bRequestsCancelled)
		{
			Request->CancelRequest();
			Result.Error = TEXT("cancelled");
			return Result;
		}
		if (FPlatformTime::Seconds() > DeadlineSeconds)
		{
			Request->CancelRequest();
			Result.bTimedOut = true;
			Result.Error = TEXT("timed out");
			return Result;
		}
		FPlatformProcess::Sleep(0.005f);
	}

	if (Request->GetStatus() != EHttpRequestStatus::Succeeded)
	{
#if GIT_SERVICE_HTTP_MODERN
		const EHttpFailureReason FailureReason = Request->GetFailureReason();
		Result.bConnectionFailure = FailureReason == EHttpFailureReason::ConnectionError;
		Result.bTimedOut = FailureReason == EHttpFailureReason::TimedOut;
		Result.Error = Result.bTimedOut ? TEXT("timed out") : (Result.bConnectionFailure ? TEXT("could not connect") : TEXT("request failed"));
#else
		// Older engines do not tell a timeout from other failures, so count both towards the timeout backoff
		Result.bConnectionFailure = Request->GetStatus() == EHttpRequestStatus::Failed_ConnectionError;
		Result.bTimedOut = !Result.bConnectionFailure;
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
	// UE4 lacks a writable stdin pipe for child processes, so the services are not used there
	return false;
#endif
}

FString BasicAuthorization(const FString& InUserName, const FString& InPassword)
{
	const FTCHARToUTF8 UserPassUtf8(*FString::Printf(TEXT("%s:%s"), *InUserName, *InPassword));
	return TEXT("Basic ") + FBase64::Encode(reinterpret_cast<const uint8*>(UserPassUtf8.Get()), UserPassUtf8.Length());
}

bool IsRequestInFlight()
{
	return RequestsInFlight.GetValue() > 0;
}

void SetRequestsCancelled(const bool bInCancelled)
{
	bRequestsCancelled = bInCancelled;
}

bool AreRequestsCancelled()
{
	return bRequestsCancelled;
}

}

#undef GIT_SERVICE_HTTP_MODERN
