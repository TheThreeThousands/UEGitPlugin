#pragma once

#include "CoreMinimal.h"

namespace GitStatusCacheClient
{

/** Mirrors the server's CheckoutCode enum */
enum class ECheckoutCode : uint8
{
	/** Missing or unrecognised code, e.g. an older server */
	Unknown,
	Available,
	AlreadyLockedByYou,
	LockedByOther,
	ModifiedInOtherBranches,
	StaleOnOwnBranch,
};

struct FCanCheckoutResult
{
	/** True if the server was reached and returned a valid answer */
	bool bServerResponded = false;

	/** Server verdict, only meaningful if bServerResponded */
	bool bCanCheckout = false;

	/** Server supplied reason, or the error description if the server failed */
	FString Reason;

	/** Server supplied reason code, only meaningful if bServerResponded */
	ECheckoutCode Code = ECheckoutCode::Unknown;
};

struct FRemoteChange
{
	/** Branch holding the newer version of the file */
	FString HeadBranch;

	/** True if the newer version is on the remote of the current branch (needs a pull), false if it is only on another tracked branch */
	bool bNotAtHead = false;
};

/** Lock and remote state of the whole project as known by the server. Keys are absolute paths. */
struct FStatusSnapshot
{
	/** True if the server was reached and returned a valid snapshot */
	bool bValid = false;

	/** Description of what went wrong if not bValid */
	FString Error;

	/** HTTP status of the response, 0 if the server could not be reached */
	int32 HttpStatusCode = 0;

	/** True if newer non lockable binaries (code, plugins) are pending on the current branch */
	bool bPendingRestart = false;

	/** Absolute path to lock owner */
	TMap<FString, FString> LockOwners;

	/** Absolute path to remote change, files not in here are up to date */
	TMap<FString, FRemoteChange> RemoteChanges;
};

/** True if the user enabled the status cache server. The server is always tried first, and the fallback flows (git refresh, check out prompt) handle it being down. */
bool IsActive();

/** Converts an absolute or repo relative path to a repo relative path with forward slashes */
FString MakeServerPath(const FString& FilePath, const FString& PathToGitRoot);

/** Asks the cache server whether a repo relative file can be checked out. HeadCommitId is the local HEAD sha, pass empty to skip the outdated branch check. Blocks until a response or timeout. */
FCanCheckoutResult SendCanCheckoutRequest(const FString& ServerRelativePath, const FString& UserName, const FString& BranchName, const FString& HeadCommitId);

/** Asks the cache server for the lock and remote state of the whole project. BranchName has no origin/ prefix, HeadCommitId is the local HEAD sha. Blocks until a response or timeout. */
FStatusSnapshot FetchStatusSnapshot(const FString& PathToGitRoot, const FString& UserName, const FString& BranchName, const FString& HeadCommitId);

}

