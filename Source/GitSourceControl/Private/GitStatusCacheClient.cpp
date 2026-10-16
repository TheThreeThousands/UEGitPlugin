#include "GitStatusCacheClient.h"

#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlLog.h"
#include "HAL/PlatformProcess.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <atomic>

namespace GitStatusCacheClient
{
	struct FGetResult
	{
		bool bSucceeded = false;
		FString Body;
		FString Error;
		int32 HttpStatusCode = 0;
	};

	static std::atomic<bool> bServerHealthy = true;

	static FString GetBaseUrl()
	{
		const FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
		return GitSourceControl ? GitSourceControl->AccessSettings().GetStatusCacheServerUrl() : FString();
	}

	static FString BuildUrl(const FString& Endpoint, const FString& UserName, const FString& BranchName)
	{
		return FString::Printf(TEXT("%s/%s?user=%s&branch=%s"),
			*GetBaseUrl(),
			*Endpoint,
			*FGenericPlatformHttp::UrlEncode(UserName),
			*FGenericPlatformHttp::UrlEncode(BranchName));
	}

	static void WaitForCompletion(const FHttpRequestPtr& Request, const double TimeoutSeconds)
	{
		const double StartTime = FPlatformTime::Seconds();
		while (Request->GetStatus() == EHttpRequestStatus::Processing && FPlatformTime::Seconds() - StartTime < TimeoutSeconds)
		{
			if (IsInGameThread())
			{
				FHttpModule::Get().GetHttpManager().Tick(0.0f);
			}
			FPlatformProcess::Sleep(0.005f);
		}
	}

	static void UpdateServerHealth(const bool bHealthy)
	{
		if (bServerHealthy.exchange(bHealthy) != bHealthy)
		{
			UE_LOG(LogGitSourceControl, Log, TEXT("GitStatusCacheServer is now considered %s"), bHealthy ? TEXT("healthy (server mode on)") : TEXT("unreachable (requests will fall back to git)"));
		}
	}

	static FGetResult SendGetRequest(const FString& Url, const double TimeoutSeconds)
	{
		FGitScopedLogTimer Timer(FString::Printf(TEXT("GitStatusCacheServer GET %s"), *Url));
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(static_cast<float>(TimeoutSeconds));
		Request->ProcessRequest();
		WaitForCompletion(Request, TimeoutSeconds + 1.0);

		FGetResult Result;
		const FHttpResponsePtr Response = Request->GetResponse();
		UpdateServerHealth(Response.IsValid());
		if (Response.IsValid())
		{
			Result.HttpStatusCode = Response->GetResponseCode();
		}
		if (!Response.IsValid())
		{
			Result.Error = FString::Printf(TEXT("GitStatusCacheServer unreachable at %s"), *GetBaseUrl());
			Timer.SetOutcome(TEXT("no response (unreachable or timed out)"));
		}
		else if (!EHttpResponseCodes::IsOk(Response->GetResponseCode()))
		{
			Result.Error = FString::Printf(TEXT("GitStatusCacheServer returned HTTP %d: %s"), Response->GetResponseCode(), *Response->GetContentAsString());
			Timer.SetOutcome(FString::Printf(TEXT("HTTP %d: %s"), Response->GetResponseCode(), *GitSourceControlLog::Truncate(Response->GetContentAsString())));
		}
		else
		{
			Result.bSucceeded = true;
			Result.Body = Response->GetContentAsString();
			Timer.SetOutcome(FString::Printf(TEXT("HTTP %d, %d bytes: %s"), Response->GetResponseCode(), Result.Body.Len(), *GitSourceControlLog::Truncate(Result.Body, 150)));
		}
		return Result;
	}

	static TSharedPtr<FJsonObject> ParseJsonObject(const FString& Body)
	{
		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject))
		{
			return nullptr;
		}
		return JsonObject;
	}

	static FCanCheckoutResult MakeCanCheckoutFailure(const FString& Reason)
	{
		FCanCheckoutResult Result;
		Result.Reason = Reason;
		return Result;
	}

	static ECheckoutCode ParseCheckoutCode(const FString& CodeName)
	{
		static const TMap<FString, ECheckoutCode> CodesByName = {
			{TEXT("available"), ECheckoutCode::Available},
			{TEXT("alreadylockedbyyou"), ECheckoutCode::AlreadyLockedByYou},
			{TEXT("lockedbyother"), ECheckoutCode::LockedByOther},
			{TEXT("modifiedinotherbranches"), ECheckoutCode::ModifiedInOtherBranches},
			{TEXT("staleonownbranch"), ECheckoutCode::StaleOnOwnBranch},
		};
		const ECheckoutCode* Code = CodesByName.Find(CodeName.ToLower());
		return Code ? *Code : ECheckoutCode::Unknown;
	}

	static ECheckoutCode ParseCheckoutCodeField(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FString CodeName;
		JsonObject->TryGetStringField(TEXT("code"), CodeName);
		return ParseCheckoutCode(CodeName);
	}

	static FCanCheckoutResult ParseCanCheckoutResponse(const FString& Body)
	{
		const TSharedPtr<FJsonObject> JsonObject = ParseJsonObject(Body);
		if (!JsonObject.IsValid() || !JsonObject->HasTypedField<EJson::Boolean>(TEXT("canCheckout")))
		{
			return MakeCanCheckoutFailure(FString::Printf(TEXT("Unexpected response: %s"), *Body));
		}
		FCanCheckoutResult Result;
		Result.bServerResponded = true;
		Result.bCanCheckout = JsonObject->GetBoolField(TEXT("canCheckout"));
		JsonObject->TryGetStringField(TEXT("reason"), Result.Reason);
		Result.Code = ParseCheckoutCodeField(JsonObject);
		return Result;
	}

	static FString MakeAbsolutePath(const FString& ServerRelativePath, const FString& PathToGitRoot)
	{
		return FPaths::ConvertRelativePathToFull(PathToGitRoot, ServerRelativePath);
	}

	static void ParseLocks(const TSharedPtr<FJsonObject>& JsonObject, const FString& PathToGitRoot, FStatusSnapshot& OutSnapshot)
	{
		const TArray<TSharedPtr<FJsonValue>>* Locks = nullptr;
		if (!JsonObject->TryGetArrayField(TEXT("locks"), Locks))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& LockValue : *Locks)
		{
			const TSharedPtr<FJsonObject> Lock = LockValue->AsObject();
			FString Path;
			FString Owner;
			if (Lock.IsValid() && Lock->TryGetStringField(TEXT("path"), Path) && Lock->TryGetStringField(TEXT("owner"), Owner))
			{
				OutSnapshot.LockOwners.Add(MakeAbsolutePath(Path, PathToGitRoot), Owner);
			}
		}
	}

	static void ParseRemoteChanges(const TSharedPtr<FJsonObject>& JsonObject, const FString& PathToGitRoot, FStatusSnapshot& OutSnapshot)
	{
		const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
		if (!JsonObject->TryGetArrayField(TEXT("remoteChanges"), Changes))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& ChangeValue : *Changes)
		{
			const TSharedPtr<FJsonObject> Change = ChangeValue->AsObject();
			FString Path;
			FRemoteChange RemoteChange;
			if (Change.IsValid() && Change->TryGetStringField(TEXT("path"), Path) && Change->TryGetStringField(TEXT("headBranch"), RemoteChange.HeadBranch))
			{
				Change->TryGetBoolField(TEXT("notAtHead"), RemoteChange.bNotAtHead);
				OutSnapshot.RemoteChanges.Add(MakeAbsolutePath(Path, PathToGitRoot), RemoteChange);
			}
		}
	}

	static FStatusSnapshot ParseStatusResponse(const FString& Body, const FString& PathToGitRoot)
	{
		FStatusSnapshot Snapshot;
		const TSharedPtr<FJsonObject> JsonObject = ParseJsonObject(Body);
		if (!JsonObject.IsValid())
		{
			Snapshot.Error = TEXT("GitStatusCacheServer returned an unparseable status response");
			return Snapshot;
		}
		JsonObject->TryGetBoolField(TEXT("pendingRestart"), Snapshot.bPendingRestart);
		ParseLocks(JsonObject, PathToGitRoot, Snapshot);
		ParseRemoteChanges(JsonObject, PathToGitRoot, Snapshot);
		Snapshot.bValid = true;
		return Snapshot;
	}

	bool IsActive()
	{
		const FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
		return bServerHealthy && GitSourceControl && GitSourceControl->AccessSettings().IsUsingStatusCacheServer();
	}

	FString MakeServerPath(const FString& FilePath, const FString& PathToGitRoot)
	{
		FString Path = FilePath;
		FPaths::NormalizeFilename(Path);
		FString Root = PathToGitRoot;
		FPaths::NormalizeFilename(Root);
		if (!Root.EndsWith(TEXT("/")))
		{
			Root += TEXT("/");
		}
		if (Path.StartsWith(Root, ESearchCase::IgnoreCase))
		{
			Path.RightChopInline(Root.Len());
		}
		return Path.Replace(TEXT("\\"), TEXT("/"));
	}

	FCanCheckoutResult SendCanCheckoutRequest(const FString& ServerRelativePath, const FString& UserName, const FString& BranchName, const FString& HeadCommitId)
	{
		FString Url = BuildUrl(TEXT("canCheckout"), UserName, BranchName) + TEXT("&path=") + FGenericPlatformHttp::UrlEncode(ServerRelativePath);
		if (!HeadCommitId.IsEmpty())
		{
			Url += TEXT("&head=") + FGenericPlatformHttp::UrlEncode(HeadCommitId);
		}
		const FGetResult Result = SendGetRequest(Url, 5.0);
		return Result.bSucceeded ? ParseCanCheckoutResponse(Result.Body) : MakeCanCheckoutFailure(Result.Error);
	}

	FStatusSnapshot FetchStatusSnapshot(const FString& PathToGitRoot, const FString& UserName, const FString& BranchName, const FString& HeadCommitId)
	{
		const FString Url = BuildUrl(TEXT("status"), UserName, BranchName) + TEXT("&head=") + FGenericPlatformHttp::UrlEncode(HeadCommitId);
		const FGetResult Result = SendGetRequest(Url, 15.0);
		if (!Result.bSucceeded)
		{
			FStatusSnapshot Snapshot;
			Snapshot.Error = Result.Error;
			Snapshot.HttpStatusCode = Result.HttpStatusCode;
			return Snapshot;
		}
		return ParseStatusResponse(Result.Body, PathToGitRoot);
	}
}

