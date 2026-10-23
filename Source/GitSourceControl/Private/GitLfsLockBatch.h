#pragma once

#include "CoreMinimal.h"

/**
 * Pure logic for ktsu.GitLfsCache's batched locking extension, POST {lfs endpoint}/locks/batch, which takes many paths in
 * one request where git-lfs sends one request per path. No I/O here, see GitLfsLockBatchClient for that.
 */
namespace GitLfsLockBatch
{

enum class EOperation : uint8
{
	Lock,
	Unlock,
};

/** The outcome for one requested path */
struct FItemResult
{
	/** Repository relative path, forward slashes */
	FString Path;
	bool bOk = false;

	/** Upstream's HTTP status for a failed item, e.g. 409 when someone else holds the lock */
	int32 Status = 0;
	FString Message;
	FString LockId;
	FString Owner;
};

/** {"operation":"lock"|"unlock","paths":[...],"ref":{"name":...}}. No ref when InRefName is empty, and never "force". */
FString BuildBody(EOperation InOperation, const TArray<FString>& InRelativePaths, const FString& InRefName);

/**
 * False when the body is not a locks/batch answer (an object with a "results" array), e.g. from a server that is not
 * GitLfsCache. Otherwise one result per requested path in request order, matched by path. A requested path without a
 * result counts as failed: upstream may already have acted on it, so it is never retried through git lfs.
 */
bool ParseResponse(const FString& InJson, const TArray<FString>& InRequestedPaths, TArray<FItemResult>& OutResults, FString& OutError);

/** What one locks/batch reply means for the paths it carried */
enum class EResponseOutcome : uint8
{
	/** A results array: each item's result is final */
	Answered,
	/** Refused before any item ran (no such route, too many paths, a refused credential), so git lfs can do them all */
	NotAsked,
	/** Upstream may have acted on some items (a server or gateway error, a 200 that cannot be read), so check the locks first */
	Unknown,
};

/** Classifies a reply by its HTTP status, and for a 200 by whether ParseResponse could read it */
EResponseOutcome ClassifyResponse(int32 InResponseCode, bool bInParsed);

/** True when upstream refused an item's credential (status 401), which GitLfsCache reports inside a 200 */
bool HasUnauthorizedItem(const TArray<FItemResult>& InResults);

/** The URL on the `Endpoint=<url> (auth=...)` line of `git lfs env`. False when there is none. */
bool ParseLfsEnvEndpoint(const TArray<FString>& InLines, FString& OutUrl);

/**
 * The ref git-lfs sends with a lock: the upstream branch as refs/heads/..., from `git rev-parse --symbolic-full-name
 * @{upstream}` (refs/remotes/<remote>/<branch>), else the current branch, else empty.
 */
FString RemoteRefName(const FString& InUpstreamFullName, const FString& InCurrentBranch);

/** GitLfsCache refuses a request carrying more than Locks:MaxFanOutPaths (1000 by default), so large batches go in chunks */
constexpr int32 MaxPathsPerRequest = 500;

/** InPaths in order, at most MaxPathsPerRequest per chunk */
TArray<TArray<FString>> SplitIntoChunks(const TArray<FString>& InPaths);

/**
 * After a batch was sent but never answered (a timeout), some of its locks may already be granted or released. Returns
 * the requested paths the stock git lfs command still has to handle, given the paths the user holds locks on now:
 * for a lock the ones not held, for an unlock the ones still held. Paths compare without case, as git-lfs on Windows does.
 */
TArray<FString> RemainingAfterUnknownOutcome(EOperation InOperation, const TArray<FString>& InRequestedPaths, const TSet<FString>& InPathsHeldByUser);

/** Each path is one upstream call, fanned out under a rate limiter, so the wait grows with the batch: 2 s + 50 ms a path, at most 60 s */
float TimeoutSeconds(int32 InPathCount);

}
