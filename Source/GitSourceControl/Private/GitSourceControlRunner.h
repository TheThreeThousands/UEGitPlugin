// Copyright Project Borealis

#pragma once

#include "CoreMinimal.h"

#include "HAL/Runnable.h"

#include "ISourceControlProvider.h"
#include "ISourceControlOperation.h"

#include "Templates/Atomic.h"

/**
 *
 */
class FGitSourceControlRunner : public FRunnable
{
public:
	FGitSourceControlRunner();

	// Destructor
	virtual ~FGitSourceControlRunner() override;

	bool Init() override;
	uint32 Run() override;
	void Stop() override;
	void OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult);

	/** Restarts the wait for the next full refresh, so the status cache server has time to see lock changes made by the client first */
	void ResetRefreshTimer();

private:
	FRunnableThread* Thread;
	FEvent* StopEvent;
	bool bRunThread;
	bool bRefreshSpawned;
	TAtomic<double> NextRefreshTimeSeconds;
};
