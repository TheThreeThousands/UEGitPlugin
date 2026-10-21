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
}

