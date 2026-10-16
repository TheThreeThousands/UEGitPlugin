// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlCommand.h"

#include "Modules/ModuleManager.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlUtils.h"
#include "GitSourceControlLog.h"

FGitSourceControlCommand::FGitSourceControlCommand(const TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe>& InOperation, const TSharedRef<class IGitSourceControlWorker, ESPMode::ThreadSafe>& InWorker, const FSourceControlOperationComplete& InOperationCompleteDelegate)
	: Operation(InOperation)
	, Worker(InWorker)
	, OperationCompleteDelegate(InOperationCompleteDelegate)
	, bExecuteProcessed(0)
	, bCancelled(0)
	, bCommandSuccessful(false)
	, bAutoDelete(true)
	, Concurrency(EConcurrency::Synchronous)
{
	// cache the providers settings here
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	const FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	PathToGitBinary = Provider.GetGitBinaryPath();
	bUsingGitLfsLocking = Provider.UsesCheckout();
	PathToRepositoryRoot = Provider.GetPathToRepositoryRoot();
	PathToGitRoot = Provider.GetPathToGitRoot();
}

void FGitSourceControlCommand::UpdateRepositoryRootIfSubmodule(TArray<FString>& AbsoluteFilePaths)
{
	PathToRepositoryRoot = GitSourceControlUtils::ChangeRepositoryRootIfSubmodule(AbsoluteFilePaths, PathToRepositoryRoot);
}

bool FGitSourceControlCommand::DoWork()
{
	const FString OperationName = Operation->GetName().ToString();
	UE_LOG(LogGitSourceControl, Log, TEXT("[%s] Operation %s started (%d files, %s)"), GitSourceControlLog::GetThreadLabel(), *OperationName, Files.Num(),
		Concurrency == EConcurrency::Synchronous ? TEXT("synchronous") : TEXT("asynchronous"));
	{
		FGitScopedLogTimer Timer(FString::Printf(TEXT("Operation %s"), *OperationName));
		bCommandSuccessful = Worker->Execute(*this);
		Timer.SetOutcome(bCommandSuccessful ? FString(TEXT("succeeded")) : FString::Printf(TEXT("FAILED (%d errors)"), ResultInfo.ErrorMessages.Num()));
	}
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);

	return bCommandSuccessful;
}

void FGitSourceControlCommand::Abandon()
{
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);
}

void FGitSourceControlCommand::DoThreadedWork()
{
	Concurrency = EConcurrency::Asynchronous;
	DoWork();
}

void FGitSourceControlCommand::Cancel()
{
	FPlatformAtomics::InterlockedExchange(&bCancelled, 1);
}

bool FGitSourceControlCommand::IsCanceled() const
{
	return bCancelled != 0;
}

ECommandResult::Type FGitSourceControlCommand::ReturnResults()
{
	// Save any messages that have accumulated
	for (const auto& String : ResultInfo.InfoMessages)
	{
		Operation->AddInfoMessge(FText::FromString(String));
	}
	for (const auto& String : ResultInfo.ErrorMessages)
	{
		Operation->AddErrorMessge(FText::FromString(String));
	}

	// run the completion delegate if we have one bound
	ECommandResult::Type Result = bCancelled ? ECommandResult::Cancelled : (bCommandSuccessful ? ECommandResult::Succeeded : ECommandResult::Failed);
	OperationCompleteDelegate.ExecuteIfBound(Operation, Result);

	return Result;
}
