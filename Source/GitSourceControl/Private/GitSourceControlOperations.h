// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "IGitSourceControlWorker.h"
#include "GitSourceControlState.h"

#include "ISourceControlOperation.h"

/**
 * Internal operation used to fetch from remote
 */
class FGitFetch : public ISourceControlOperation
{
public:
	// ISourceControlOperation interface
	virtual FName GetName() const override;

	virtual FText GetInProgressString() const override;

	bool bUpdateStatus = false;
};

class FGitLFSRefreshLocks : public ISourceControlOperation
{
	// ISourceControlOperation interface
	virtual FName GetName() const override;

	virtual FText GetInProgressString() const override;
};

class FGitSourceControlWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitSourceControlWorker() {}

	virtual bool UpdateStates() const final;

	protected:
	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Called when first activated on a project, and then at project load time.
 *  Look for the root directory of the git repository (where the ".git/" subdirectory is located). */
class FGitConnectWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitConnectWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
};

/** Lock (check-out) a set of files using Git LFS 2. */
class FGitCheckOutWorker : public FGitSourceControlWorker
{
public:
	virtual ~FGitCheckOutWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
};

/** Commit (check-in) a set of files to the local depot. */
class FGitCheckInWorker : public FGitSourceControlWorker
{
public:
	virtual ~FGitCheckInWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
};

/** Add an untracked file to revision control (so only a subset of the git add command). */
class FGitMarkForAddWorker : public FGitSourceControlWorker
{
public:
	virtual ~FGitMarkForAddWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;;
};

/** Delete a file and remove it from revision control. */
class FGitDeleteWorker : public FGitSourceControlWorker
{
public:
	virtual ~FGitDeleteWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
};

/** Revert any change to a file to its state on the local depot. */
class FGitRevertWorker : public FGitSourceControlWorker
{
public:
	virtual ~FGitRevertWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
};

/** Git pull --rebase to update branch from its configured remote */
class FGitSyncWorker : public FGitSourceControlWorker
{
public:
	virtual ~FGitSyncWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
};

/** Get revision control status of files on local working copy. */
class FGitUpdateStatusWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitUpdateStatusWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	/** Temporary states for results */
	TMap<const FString, FGitState> States;

	/** Map of filenames to history */
	TMap<FString, TGitSourceControlHistory> Histories;
};

/** Copy or Move operation on a single file */
class FGitCopyWorker : public FGitSourceControlWorker
{
public:
	virtual ~FGitCopyWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
};

/** git add to mark a conflict as resolved */
class FGitResolveWorker : public FGitSourceControlWorker
{
public:
	virtual ~FGitResolveWorker() {}
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
};

/** Git push to publish branch for its configured remote */
class FGitFetchWorker : public FGitSourceControlWorker
{
public:
	virtual ~FGitFetchWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
};

#if ENGINE_MAJOR_VERSION == 5
class FGitMoveToChangelistWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitMoveToChangelistWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	
	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

class FGitUpdateStagingWorker: public IGitSourceControlWorker
{
public:
	virtual ~FGitUpdateStagingWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	
	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

class FGitRefreshLockStateWorker : public FGitSourceControlWorker
{
public:
	virtual ~FGitRefreshLockStateWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
};
#endif
