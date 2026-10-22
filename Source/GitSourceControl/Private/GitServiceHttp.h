#pragma once

#include "CoreMinimal.h"
#include "GitBranchStateCache.h"

/**
 * The HTTP and credential plumbing shared by the clients of the cache services (GitBranchStateCache, GitLfsCache).
 * Callers decide what a failure means, this only reports it.
 */
namespace GitServiceHttp
{

struct FPostResult
{
	bool bResponded = false;
	bool bConnectionFailure = false;
	bool bTimedOut = false;
	int32 ResponseCode = 0;
	FString Body;
	FString Error;
};

/** POSTs a JSON body and waits for the answer on the calling thread, never longer than about InTimeoutSeconds */
FPostResult PostJson(const FString& InUrl, const FString& InAuthorization, const FString& InBody, float InTimeoutSeconds);

/**
 * Asks `git credential fill` for a credential the artist already has, without ever showing a login window. The credential
 * only travels over pipes. Always false on UE4, which lacks a writable stdin pipe for child processes.
 */
bool RunCredentialFill(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const GitBranchStateCache::FRemoteParts& InRemote, FString& OutUserName, FString& OutPassword);

/** "Basic <base64 of user:password>" */
FString BasicAuthorization(const FString& InUserName, const FString& InPassword);

/** True while a request is waiting for a response. Engines without HTTP thread completion need the HTTP manager ticked meanwhile. */
bool IsRequestInFlight();

}
