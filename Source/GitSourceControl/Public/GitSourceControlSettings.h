// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "Containers/UnrealString.h"
#include "HAL/CriticalSection.h"

class GITSOURCECONTROL_API FGitSourceControlSettings
{
public:
	/** Get the Git Binary Path */
	const FString & GetBinaryPath() const;

	/** Set the Git Binary Path */
	bool SetBinaryPath(const FString& InString);

	/** Tell if using the Git LFS file Locking workflow */
	bool IsUsingGitLfsLocking() const;

	/** Configure the usage of Git LFS file Locking workflow */
	bool SetUsingGitLfsLocking(const bool InUsingGitLfsLocking);

	/** Get the username used by the Git LFS 2 File Locks server */
	const FString GetLfsUserName() const;

	/** Set the username used by the Git LFS 2 File Locks server */
	bool SetLfsUserName(const FString& InString);

	/** Tell if file check outs are validated by the GitStatusCacheServer */
	bool IsUsingStatusCacheServer() const;

	/** Configure usage of the GitStatusCacheServer for file check outs */
	bool SetUsingStatusCacheServer(const bool InUsingStatusCacheServer);

	/** Base url of the GitStatusCacheServer, without a trailing slash */
	FString GetStatusCacheServerUrl() const;

	/** Set the base url of the GitStatusCacheServer */
	bool SetStatusCacheServerUrl(const FString& InString);

	/** Load settings from ini file */
	void LoadSettings();

	/** Save settings to ini file */
	void SaveSettings() const;

private:
	/** A critical section for settings access */
	mutable FCriticalSection CriticalSection;

	/** Git binary path */
	FString BinaryPath;

	/** Tells if using the Git LFS file Locking workflow */
	bool bUsingGitLfsLocking = true;

	/** Username used by the Git LFS 2 File Locks server */
	FString LfsUserName;

	/** Experimental: ask the GitStatusCacheServer whether files can be checked out */
	bool bUsingStatusCacheServer = false;

	/** Base url of the GitStatusCacheServer. Example: http://localhost:5050 */
	FString StatusCacheServerUrl = TEXT("http://localhost:5050");
};
