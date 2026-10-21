#pragma once

#include "CoreMinimal.h"

/**
 * Pure logic for talking to ktsu.GitBranchStateCache: building the request, reading the answer, and deciding which files
 * are newer on which branch by comparing blob ids. No I/O here, see GitBranchStateCacheClient for that.
 */
namespace GitBranchStateCache
{

/** One branch's change to one path, as reported by the service */
struct FPathChange
{
	/** Branch name as sent in the request, e.g. "origin/main" */
	FString Branch;

	/** Blob id at the branch tip, empty when the path was deleted on that branch */
	FString Blob;

	/** git's raw status letter, e.g. "M", "A", "D" */
	FString Status;
};

/** The parts of a /state response the plugin uses */
struct FStateResponse
{
	/** Every branch the service answered for, in response order */
	TArray<FString> BranchNames;

	/** Branches the service could not answer for. Their paths are unknown, not unchanged. */
	TArray<FString> BranchesWithErrors;

	/** Repository relative path (forward slashes) to its changes, one per branch that changed it */
	TMap<FString, TArray<FPathChange>> Paths;

	bool bPartial = false;
	bool bTruncated = false;
};

/** JSON body for POST .../state. Branches are sent as concrete names in branchPatterns. */
FString BuildStateRequestBody(const FString& InBase, const TArray<FString>& InBranches, const TArray<FString>& InRelativePaths);

/** Reads a /state response. False on malformed JSON or a missing "branches" or "paths" field. */
bool ParseStateResponse(const FString& InJson, FStateResponse& OutResponse, FString& OutError);

/** False when the answer cannot be trusted as complete: partial, truncated, or any branch carrying an error */
bool IsUsable(const FStateResponse& InResponse, FString& OutReason);

/**
 * Reads `git -c core.quotePath=false ls-tree -r HEAD -- <paths>` output into repository relative path to blob id.
 * Not -z: older engines cut process output at the first NUL. Paths git still quotes are unescaped.
 */
TMap<FString, FString> ParseLsTree(const FString& InOutput);

struct FRemoteParts
{
	FString Protocol;
	FString Host;
	FString Path;
};

/** Splits an https remote URL for `git credential fill`. False for anything that is not https (SSH, http, local paths). */
bool ParseHttpsRemote(const FString& InRemoteUrl, FRemoteParts& OutParts);

/**
 * True when the artist's forge credential may be sent to this service URL: https always, plain http only to localhost and
 * only from the per user override (local testing). Never true for a URL carrying userinfo.
 */
bool IsAllowedStateUrl(const FString& InUrl, bool bInFromUserOverride);

/** Removes "user:password@" from a URL so it can be logged */
FString StripUserInfo(const FString& InUrl);

/** Reads `git credential fill` output. False when there is no password. */
bool ParseCredentialFill(const TArray<FString>& InLines, FString& OutUserName, FString& OutPassword);

/**
 * Repository relative path to the branch carrying a version the local HEAD does not have. A path is newer on a branch
 * when its blob there differs from the HEAD blob (a missing HEAD entry counts as empty). When several branches qualify
 * the current remote branch wins, otherwise the first in BranchNames order.
 */
TMap<FString, FString> ComputeNewerFiles(const FStateResponse& InResponse, const TMap<FString, FString>& InHeadBlobs, const FString& InCurrentRemoteBranch);

/**
 * False when the answer does not list InBranch (the current branch's remote), for example when the service's mirror has
 * not fetched it yet. The service drops unknown branch names silently, which would otherwise hide every NotAtHead.
 * True for an empty InBranch.
 */
bool AnswersForBranch(const FStateResponse& InResponse, const FString& InBranch);

/** Stops asking an unreachable service for a while, so a dead service does not cost every status check a connection attempt */
class FBackoff
{
public:
	explicit FBackoff(double InBackoffSeconds = 30.0, int32 InTimeoutsBeforeBackoff = 3);

	bool ShouldSkip(double InNowSeconds) const;
	void RecordConnectionFailure(double InNowSeconds);

	/** One slow answer is not a dead service, but several in a row (a host that drops packets) back off like a refused connection */
	void RecordTimeout(double InNowSeconds);

	/** True on the second 401 in a row: a fresh credential did not help, so stop sending it */
	bool RecordUnauthorized();

	void RecordSuccess();

private:
	double BackoffSeconds;
	int32 TimeoutsBeforeBackoff;
	double SkipUntilSeconds = 0.0;
	int32 ConsecutiveTimeouts = 0;
	int32 ConsecutiveUnauthorized = 0;
};

}
