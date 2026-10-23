#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

/** Plugin flow log: operations, git processes, branch state cache requests. Filter with "Log LogGitSourceControl Verbose" */
DECLARE_LOG_CATEGORY_EXTERN(LogGitSourceControl, Log, All);

/** Logs "<Name> took N ms" when it goes out of scope. Use SetOutcome to append a result. */
class FGitScopedLogTimer
{
public:
	explicit FGitScopedLogTimer(const FString& InName);
	~FGitScopedLogTimer();

	void SetOutcome(const FString& InOutcome);
	double GetElapsedMilliseconds() const;

private:
	FString Name;
	FString Outcome;
	double StartTimeSeconds;
};

namespace GitSourceControlLog
{
	/** Shortens long strings (e.g. git commands with hundreds of files) for logging */
	FString Truncate(const FString& Text, int32 MaxLength = 300);

	/** Short name of the thread for log lines, e.g. "GameThread" or "Worker" */
	const TCHAR* GetThreadLabel();

	/** Removes "user:password@" from every URL in a log line, e.g. a remote URL typed with a token in it */
	FString RedactUserInfo(const FString& Text);
}
