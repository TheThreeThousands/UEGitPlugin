#include "GitLfsLockBatch.h"

#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace GitLfsLockBatch
{

FString BuildBody(const EOperation InOperation, const TArray<FString>& InRelativePaths, const FString& InRefName)
{
	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("operation"), InOperation == EOperation::Lock ? TEXT("lock") : TEXT("unlock"));
	Writer->WriteArrayStart(TEXT("paths"));
	for (const FString& Path : InRelativePaths)
	{
		Writer->WriteValue(Path);
	}
	Writer->WriteArrayEnd();
	if (!InRefName.IsEmpty())
	{
		Writer->WriteObjectStart(TEXT("ref"));
		Writer->WriteValue(TEXT("name"), InRefName);
		Writer->WriteObjectEnd();
	}
	Writer->WriteObjectEnd();
	Writer->Close();
	return Body;
}

bool ParseResponse(const FString& InJson, const TArray<FString>& InRequestedPaths, TArray<FItemResult>& OutResults, FString& OutError)
{
	OutResults.Reset();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("response is not a JSON object");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Root->TryGetArrayField(TEXT("results"), Items))
	{
		OutError = TEXT("response has no results array");
		return false;
	}

	TMap<FString, FItemResult> ByPath;
	for (const TSharedPtr<FJsonValue>& Item : *Items)
	{
		const TSharedPtr<FJsonObject>* ItemObject = nullptr;
		FString Path;
		if (!Item.IsValid() || !Item->TryGetObject(ItemObject) || !(*ItemObject)->TryGetStringField(TEXT("path"), Path))
		{
			// An item released by id carries no path; this client only sends paths
			continue;
		}

		FItemResult Result;
		Result.Path = Path;
		(*ItemObject)->TryGetBoolField(TEXT("ok"), Result.bOk);
		(*ItemObject)->TryGetNumberField(TEXT("status"), Result.Status);
		(*ItemObject)->TryGetStringField(TEXT("message"), Result.Message);

		const TSharedPtr<FJsonObject>* Lock = nullptr;
		if ((*ItemObject)->TryGetObjectField(TEXT("lock"), Lock))
		{
			(*Lock)->TryGetStringField(TEXT("id"), Result.LockId);
			const TSharedPtr<FJsonObject>* Owner = nullptr;
			if ((*Lock)->TryGetObjectField(TEXT("owner"), Owner))
			{
				(*Owner)->TryGetStringField(TEXT("name"), Result.Owner);
			}
		}
		ByPath.Add(Path, MoveTemp(Result));
	}

	OutResults.Reserve(InRequestedPaths.Num());
	for (const FString& RequestedPath : InRequestedPaths)
	{
		if (const FItemResult* Found = ByPath.Find(RequestedPath))
		{
			OutResults.Add(*Found);
		}
		else
		{
			FItemResult Missing;
			Missing.Path = RequestedPath;
			Missing.Message = TEXT("no result from GitLfsCache");
			OutResults.Add(MoveTemp(Missing));
		}
	}
	return true;
}

bool ParseLfsEnvEndpoint(const TArray<FString>& InLines, FString& OutUrl)
{
	static const FString Prefix = TEXT("Endpoint=");
	for (const FString& Line : InLines)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (!Trimmed.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			continue;
		}

		FString Url = Trimmed.Mid(Prefix.Len());
		int32 Space = INDEX_NONE;
		if (Url.FindChar(TEXT(' '), Space))
		{
			Url.LeftInline(Space);
		}
		if (Url.IsEmpty())
		{
			return false;
		}
		OutUrl = Url;
		return true;
	}
	return false;
}

FString RemoteRefName(const FString& InUpstreamFullName, const FString& InCurrentBranch)
{
	static const FString RemotesPrefix = TEXT("refs/remotes/");
	const FString Upstream = InUpstreamFullName.TrimStartAndEnd();
	if (Upstream.StartsWith(RemotesPrefix, ESearchCase::CaseSensitive))
	{
		// refs/remotes/<remote>/<branch>: the remote name is the first segment after the prefix
		const FString RemoteAndBranch = Upstream.Mid(RemotesPrefix.Len());
		int32 Slash = INDEX_NONE;
		if (RemoteAndBranch.FindChar(TEXT('/'), Slash) && Slash + 1 < RemoteAndBranch.Len())
		{
			return TEXT("refs/heads/") + RemoteAndBranch.Mid(Slash + 1);
		}
	}

	const FString CurrentBranch = InCurrentBranch.TrimStartAndEnd();
	return CurrentBranch.IsEmpty() ? FString() : TEXT("refs/heads/") + CurrentBranch;
}

TArray<TArray<FString>> SplitIntoChunks(const TArray<FString>& InPaths)
{
	TArray<TArray<FString>> Chunks;
	for (int32 Start = 0; Start < InPaths.Num(); Start += MaxPathsPerRequest)
	{
		const int32 Count = FMath::Min(MaxPathsPerRequest, InPaths.Num() - Start);
		Chunks.Emplace(InPaths.GetData() + Start, Count);
	}
	return Chunks;
}

TArray<FString> RemainingAfterUnknownOutcome(const EOperation InOperation, const TArray<FString>& InRequestedPaths, const TSet<FString>& InPathsHeldByUser)
{
	// FString hashing and equality ignore case
	return InRequestedPaths.FilterByPredicate([InOperation, &InPathsHeldByUser](const FString& Path)
	{
		const bool bHeld = InPathsHeldByUser.Contains(Path);
		return InOperation == EOperation::Lock ? !bHeld : bHeld;
	});
}

float TimeoutSeconds(const int32 InPathCount)
{
	return FMath::Min(60.0f, 2.0f + 0.05f * FMath::Max(0, InPathCount));
}

}
