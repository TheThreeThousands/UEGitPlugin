#include "GitBranchStateCache.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace GitBranchStateCache
{

namespace
{

/** The "user:password@host:port" part of a URL, given the index just past "://" */
int32 FindAuthorityEnd(const FString& InUrl, const int32 InAuthorityStart)
{
	for (int32 Index = InAuthorityStart; Index < InUrl.Len(); ++Index)
	{
		const TCHAR Char = InUrl[Index];
		if (Char == TEXT('/') || Char == TEXT('?') || Char == TEXT('#'))
		{
			return Index;
		}
	}
	return InUrl.Len();
}

/** Splits a URL into lower case scheme and authority. False when there is no "://". */
bool SplitSchemeAndAuthority(const FString& InUrl, FString& OutScheme, FString& OutAuthority, int32& OutAuthorityEnd)
{
	const int32 SchemeEnd = InUrl.Find(TEXT("://"), ESearchCase::CaseSensitive);
	if (SchemeEnd <= 0)
	{
		return false;
	}
	OutScheme = InUrl.Left(SchemeEnd).ToLower();
	const int32 AuthorityStart = SchemeEnd + 3;
	OutAuthorityEnd = FindAuthorityEnd(InUrl, AuthorityStart);
	OutAuthority = InUrl.Mid(AuthorityStart, OutAuthorityEnd - AuthorityStart);
	return true;
}

/** Host without a port. Handles bracketed IPv6 literals. */
FString HostWithoutPort(const FString& InAuthority)
{
	if (InAuthority.StartsWith(TEXT("[")))
	{
		const int32 Close = InAuthority.Find(TEXT("]"));
		return Close == INDEX_NONE ? InAuthority : InAuthority.Left(Close + 1);
	}
	int32 Colon = INDEX_NONE;
	return InAuthority.FindChar(TEXT(':'), Colon) ? InAuthority.Left(Colon) : InAuthority;
}

/**
 * Undoes git's C style path quoting ("a\"b", "\303\251"). Octal escapes are UTF-8 bytes, so the path is rebuilt as UTF-8
 * and converted once at the end. Unquoted paths are returned as is.
 */
FString UnquoteGitPath(const FString& InPath)
{
	if (InPath.Len() < 2 || !InPath.StartsWith(TEXT("\"")) || !InPath.EndsWith(TEXT("\"")))
	{
		return InPath;
	}

	const FTCHARToUTF8 Utf8(*InPath.Mid(1, InPath.Len() - 2));
	const ANSICHAR* Source = Utf8.Get();
	const int32 Length = Utf8.Length();

	TArray<ANSICHAR> Bytes;
	Bytes.Reserve(Length + 1);
	for (int32 Index = 0; Index < Length; ++Index)
	{
		const ANSICHAR Char = Source[Index];
		if (Char != '\\' || Index + 1 >= Length)
		{
			Bytes.Add(Char);
			continue;
		}

		const ANSICHAR Escaped = Source[++Index];
		if (Escaped >= '0' && Escaped <= '7' && Index + 2 < Length)
		{
			const int32 Value = (Escaped - '0') * 64 + (Source[Index + 1] - '0') * 8 + (Source[Index + 2] - '0');
			Bytes.Add(static_cast<ANSICHAR>(Value));
			Index += 2;
			continue;
		}

		switch (Escaped)
		{
		case 'a': Bytes.Add('\a'); break;
		case 'b': Bytes.Add('\b'); break;
		case 'f': Bytes.Add('\f'); break;
		case 'n': Bytes.Add('\n'); break;
		case 'r': Bytes.Add('\r'); break;
		case 't': Bytes.Add('\t'); break;
		case 'v': Bytes.Add('\v'); break;
		default: Bytes.Add(Escaped); break; // '\\' and '"'
		}
	}
	Bytes.Add('\0');

	return FString(UTF8_TO_TCHAR(Bytes.GetData()));
}

}

FString BuildStateRequestBody(const FString& InBase, const TArray<FString>& InBranches, const TArray<FString>& InRelativePaths)
{
	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("base"), InBase);
	Writer->WriteArrayStart(TEXT("branchPatterns"));
	for (const FString& Branch : InBranches)
	{
		Writer->WriteValue(Branch);
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("paths"));
	for (const FString& Path : InRelativePaths)
	{
		Writer->WriteValue(Path);
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();
	return Body;
}

bool ParseStateResponse(const FString& InJson, FStateResponse& OutResponse, FString& OutError)
{
	OutResponse = FStateResponse();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("response is not a JSON object");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Branches = nullptr;
	if (!Root->TryGetArrayField(TEXT("branches"), Branches))
	{
		OutError = TEXT("response has no \"branches\" array");
		return false;
	}

	const TSharedPtr<FJsonObject>* Paths = nullptr;
	if (!Root->TryGetObjectField(TEXT("paths"), Paths))
	{
		OutError = TEXT("response has no \"paths\" object");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& BranchValue : *Branches)
	{
		const TSharedPtr<FJsonObject>* Branch = nullptr;
		FString Name;
		if (!BranchValue.IsValid() || !BranchValue->TryGetObject(Branch) || !(*Branch)->TryGetStringField(TEXT("name"), Name))
		{
			OutError = TEXT("a branch entry has no \"name\"");
			return false;
		}
		OutResponse.BranchNames.Add(Name);

		// A null error fails TryGetStringField, so only a real message lands here
		FString BranchError;
		if ((*Branch)->TryGetStringField(TEXT("error"), BranchError) && !BranchError.IsEmpty())
		{
			OutResponse.BranchesWithErrors.Add(Name);
		}
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& PathEntry : (*Paths)->Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* ChangeValues = nullptr;
		if (!PathEntry.Value.IsValid() || !PathEntry.Value->TryGetArray(ChangeValues))
		{
			OutError = FString::Printf(TEXT("path \"%s\" does not hold an array"), *PathEntry.Key);
			return false;
		}

		TArray<FPathChange>& Changes = OutResponse.Paths.Add(PathEntry.Key);
		for (const TSharedPtr<FJsonValue>& ChangeValue : *ChangeValues)
		{
			const TSharedPtr<FJsonObject>* Change = nullptr;
			FPathChange PathChange;
			if (!ChangeValue.IsValid() || !ChangeValue->TryGetObject(Change) || !(*Change)->TryGetStringField(TEXT("branch"), PathChange.Branch))
			{
				OutError = FString::Printf(TEXT("a change to \"%s\" has no \"branch\""), *PathEntry.Key);
				return false;
			}
			// "blob" is null for a delete, which leaves Blob empty
			(*Change)->TryGetStringField(TEXT("blob"), PathChange.Blob);
			(*Change)->TryGetStringField(TEXT("status"), PathChange.Status);
			Changes.Add(MoveTemp(PathChange));
		}
	}

	Root->TryGetBoolField(TEXT("partial"), OutResponse.bPartial);
	Root->TryGetBoolField(TEXT("truncated"), OutResponse.bTruncated);
	return true;
}

bool IsUsable(const FStateResponse& InResponse, FString& OutReason)
{
	if (InResponse.bPartial)
	{
		OutReason = TEXT("the service answered with partial results");
		return false;
	}
	if (InResponse.bTruncated)
	{
		OutReason = TEXT("the service truncated its results");
		return false;
	}
	if (InResponse.BranchesWithErrors.Num() > 0)
	{
		OutReason = FString::Printf(TEXT("the service could not answer for %s"), *FString::Join(InResponse.BranchesWithErrors, TEXT(", ")));
		return false;
	}
	return true;
}

TMap<FString, FString> ParseLsTree(const FString& InOutput)
{
	TMap<FString, FString> Blobs;

	TArray<FString> Lines;
	InOutput.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		// "<mode> SP <type> SP <oid> TAB <path>"
		int32 Tab = INDEX_NONE;
		if (!Line.FindChar(TEXT('\t'), Tab))
		{
			continue;
		}
		TArray<FString> Fields;
		Line.Left(Tab).ParseIntoArray(Fields, TEXT(" "));
		if (Fields.Num() == 3 && Fields[1] == TEXT("blob"))
		{
			Blobs.Add(UnquoteGitPath(Line.Mid(Tab + 1)), Fields[2]);
		}
	}
	return Blobs;
}

bool ParseHttpsRemote(const FString& InRemoteUrl, FRemoteParts& OutParts)
{
	FString Scheme;
	FString Authority;
	int32 AuthorityEnd = 0;
	if (!SplitSchemeAndAuthority(InRemoteUrl.TrimStartAndEnd(), Scheme, Authority, AuthorityEnd) || Scheme != TEXT("https"))
	{
		return false;
	}

	int32 At = INDEX_NONE;
	if (Authority.FindLastChar(TEXT('@'), At))
	{
		Authority.RightChopInline(At + 1);
	}
	if (Authority.IsEmpty())
	{
		return false;
	}

	FString Path = InRemoteUrl.TrimStartAndEnd().Mid(AuthorityEnd);
	while (Path.StartsWith(TEXT("/")))
	{
		Path.RightChopInline(1);
	}
	while (Path.EndsWith(TEXT("/")))
	{
		Path.LeftChopInline(1);
	}

	OutParts.Protocol = Scheme;
	OutParts.Host = Authority;
	OutParts.Path = Path;
	return true;
}

bool IsAllowedStateUrl(const FString& InUrl, const bool bInFromUserOverride)
{
	FString Scheme;
	FString Authority;
	int32 AuthorityEnd = 0;
	if (!SplitSchemeAndAuthority(InUrl.TrimStartAndEnd(), Scheme, Authority, AuthorityEnd) || Authority.IsEmpty())
	{
		return false;
	}

	// Never send the forge credential alongside credentials embedded in the URL itself
	if (Authority.Contains(TEXT("@")))
	{
		return false;
	}

	if (Scheme == TEXT("https"))
	{
		return true;
	}

	if (Scheme == TEXT("http") && bInFromUserOverride)
	{
		const FString Host = HostWithoutPort(Authority).ToLower();
		return Host == TEXT("localhost") || Host == TEXT("127.0.0.1") || Host == TEXT("[::1]");
	}

	return false;
}

FString StripUserInfo(const FString& InUrl)
{
	FString Scheme;
	FString Authority;
	int32 AuthorityEnd = 0;
	if (!SplitSchemeAndAuthority(InUrl, Scheme, Authority, AuthorityEnd))
	{
		return InUrl;
	}

	int32 At = INDEX_NONE;
	if (!Authority.FindLastChar(TEXT('@'), At))
	{
		return InUrl;
	}

	const int32 AuthorityStart = AuthorityEnd - Authority.Len();
	return InUrl.Left(AuthorityStart) + InUrl.Mid(AuthorityStart + At + 1);
}

bool ParseCredentialFill(const TArray<FString>& InLines, FString& OutUserName, FString& OutPassword)
{
	bool bHasPassword = false;
	for (const FString& Line : InLines)
	{
		FString Key;
		FString Value;
		if (!Line.Split(TEXT("="), &Key, &Value))
		{
			continue;
		}
		if (Key == TEXT("username"))
		{
			OutUserName = Value;
		}
		else if (Key == TEXT("password"))
		{
			OutPassword = Value;
			bHasPassword = true;
		}
	}
	return bHasPassword;
}

TMap<FString, FString> ComputeNewerFiles(const FStateResponse& InResponse, const TMap<FString, FString>& InHeadBlobs, const FString& InCurrentRemoteBranch)
{
	TMap<FString, FString> NewerFiles;
	for (const TPair<FString, TArray<FPathChange>>& PathEntry : InResponse.Paths)
	{
		const FString* HeadBlob = InHeadBlobs.Find(PathEntry.Key);
		const FString& LocalBlob = HeadBlob ? *HeadBlob : FString();

		// The branch tip differs from what this checkout has. A change that was later reverted on the branch has the same blob.
		TArray<FString> QualifyingBranches;
		for (const FPathChange& Change : PathEntry.Value)
		{
			if (Change.Blob != LocalBlob)
			{
				QualifyingBranches.AddUnique(Change.Branch);
			}
		}
		if (QualifyingBranches.Num() == 0)
		{
			continue;
		}

		// Same priority as the local CheckRemote loop: the current branch's remote wins, otherwise the first branch in request order
		FString Winner;
		if (!InCurrentRemoteBranch.IsEmpty() && QualifyingBranches.Contains(InCurrentRemoteBranch))
		{
			Winner = InCurrentRemoteBranch;
		}
		else
		{
			for (const FString& BranchName : InResponse.BranchNames)
			{
				if (QualifyingBranches.Contains(BranchName))
				{
					Winner = BranchName;
					break;
				}
			}
			if (Winner.IsEmpty())
			{
				Winner = QualifyingBranches[0];
			}
		}
		NewerFiles.Add(PathEntry.Key, Winner);
	}
	return NewerFiles;
}

FBackoff::FBackoff(const double InBackoffSeconds)
	: BackoffSeconds(InBackoffSeconds)
{
}

bool FBackoff::ShouldSkip(const double InNowSeconds) const
{
	return InNowSeconds < SkipUntilSeconds;
}

void FBackoff::RecordConnectionFailure(const double InNowSeconds)
{
	SkipUntilSeconds = InNowSeconds + BackoffSeconds;
}

void FBackoff::RecordSuccess()
{
	SkipUntilSeconds = 0.0;
}

}
