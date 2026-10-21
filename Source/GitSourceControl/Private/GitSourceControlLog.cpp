#include "GitSourceControlLog.h"

#include "HAL/PlatformTime.h"
#include "Misc/CoreMisc.h"

DEFINE_LOG_CATEGORY(LogGitSourceControl);

FGitScopedLogTimer::FGitScopedLogTimer(const FString& InName)
	: Name(InName)
	, StartTimeSeconds(FPlatformTime::Seconds())
{
}

FGitScopedLogTimer::~FGitScopedLogTimer()
{
	const FString OutcomeSuffix = Outcome.IsEmpty() ? FString() : FString::Printf(TEXT(" -> %s"), *Outcome);
	UE_LOG(LogGitSourceControl, Log, TEXT("[%s] %s took %.1f ms%s"), GitSourceControlLog::GetThreadLabel(), *Name, GetElapsedMilliseconds(), *OutcomeSuffix);
}

void FGitScopedLogTimer::SetOutcome(const FString& InOutcome)
{
	Outcome = InOutcome;
}

double FGitScopedLogTimer::GetElapsedMilliseconds() const
{
	return (FPlatformTime::Seconds() - StartTimeSeconds) * 1000.0;
}

namespace GitSourceControlLog
{
	FString Truncate(const FString& Text, const int32 MaxLength)
	{
		if (Text.Len() <= MaxLength)
		{
			return Text;
		}
		return FString::Printf(TEXT("%s... (%d chars)"), *Text.Left(MaxLength), Text.Len());
	}

	const TCHAR* GetThreadLabel()
	{
		return IsInGameThread() ? TEXT("GameThread") : TEXT("Worker");
	}

	FString RedactUserInfo(const FString& Text)
	{
		FString Result;
		Result.Reserve(Text.Len());
		int32 Copied = 0;
		int32 SchemeEnd = Text.Find(TEXT("://"), ESearchCase::CaseSensitive);
		while (SchemeEnd != INDEX_NONE)
		{
			// The authority runs to the first '/', quote or whitespace; userinfo is everything up to its last '@'
			const int32 AuthorityStart = SchemeEnd + 3;
			int32 AuthorityEnd = AuthorityStart;
			while (AuthorityEnd < Text.Len() && Text[AuthorityEnd] != TEXT('/') && Text[AuthorityEnd] != TEXT('"') && Text[AuthorityEnd] != TEXT('\'')
				&& !FChar::IsWhitespace(Text[AuthorityEnd]))
			{
				++AuthorityEnd;
			}
			int32 At = INDEX_NONE;
			for (int32 Index = AuthorityStart; Index < AuthorityEnd; ++Index)
			{
				if (Text[Index] == TEXT('@'))
				{
					At = Index;
				}
			}
			if (At != INDEX_NONE)
			{
				Result += Text.Mid(Copied, AuthorityStart - Copied);
				Copied = At + 1;
			}
			SchemeEnd = Text.Find(TEXT("://"), ESearchCase::CaseSensitive, ESearchDir::FromStart, AuthorityEnd);
		}
		Result += Text.Mid(Copied);
		return Result;
	}
}

