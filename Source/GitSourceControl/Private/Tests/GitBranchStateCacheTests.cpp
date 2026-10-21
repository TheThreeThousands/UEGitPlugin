#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GitBranchStateCache.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

using namespace GitBranchStateCache;

#define GIT_BRANCH_STATE_TEST_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace GitBranchStateCacheTests
{
	static const TCHAR* ReadmeResponse = TEXT(R"({
		"base": "a1b2c3",
		"branches": [
			{ "name": "origin/main", "tip": "d4e5f6", "mergeBase": "a1b2c3", "error": null }
		],
		"paths": {
			"Content/Chars/Bar.uasset": [
				{ "branch": "origin/main", "blob": "9f8e7d", "status": "M" }
			]
		},
		"refsAsOf": "2026-08-19T09:47:00Z",
		"partial": false,
		"truncated": false
	})");

	static FStateResponse MakeResponse(const TArray<FString>& InBranches, const FString& InPath, const TArray<FPathChange>& InChanges)
	{
		FStateResponse Response;
		Response.BranchNames = InBranches;
		Response.Paths.Add(InPath, InChanges);
		return Response;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheRequestBodyTest, "GitSourceControl.BranchStateCache.Request.Body", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheRequestBodyTest::RunTest(const FString& Parameters)
{
	const FString Body = BuildStateRequestBody(TEXT("a1b2"), { TEXT("origin/main"), TEXT("origin/KO-1") }, { TEXT("Content/A.uasset") });

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!TestTrue(TEXT("Body is JSON"), FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("base"), Root->GetStringField(TEXT("base")), FString(TEXT("a1b2")));

	TArray<FString> Branches;
	Root->TryGetStringArrayField(TEXT("branchPatterns"), Branches);
	TestEqual(TEXT("branchPatterns"), Branches, TArray<FString>{ TEXT("origin/main"), TEXT("origin/KO-1") });

	TArray<FString> Paths;
	Root->TryGetStringArrayField(TEXT("paths"), Paths);
	TestEqual(TEXT("paths"), Paths, TArray<FString>{ TEXT("Content/A.uasset") });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheParseValidTest, "GitSourceControl.BranchStateCache.Parse.Valid", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheParseValidTest::RunTest(const FString& Parameters)
{
	FStateResponse Response;
	FString Error;
	if (!TestTrue(TEXT("Parses"), ParseStateResponse(GitBranchStateCacheTests::ReadmeResponse, Response, Error)))
	{
		return false;
	}

	TestEqual(TEXT("BranchNames"), Response.BranchNames, TArray<FString>{ TEXT("origin/main") });
	const TArray<FPathChange>* Changes = Response.Paths.Find(TEXT("Content/Chars/Bar.uasset"));
	if (!TestNotNull(TEXT("Path present"), Changes) || !TestEqual(TEXT("One change"), Changes->Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("Branch"), (*Changes)[0].Branch, FString(TEXT("origin/main")));
	TestEqual(TEXT("Blob"), (*Changes)[0].Blob, FString(TEXT("9f8e7d")));
	TestEqual(TEXT("Status"), (*Changes)[0].Status, FString(TEXT("M")));

	FString Reason;
	TestTrue(TEXT("Usable"), IsUsable(Response, Reason));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheParseDeleteTest, "GitSourceControl.BranchStateCache.Parse.DeleteHasEmptyBlob", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheParseDeleteTest::RunTest(const FString& Parameters)
{
	const FString Json = TEXT(R"({ "branches": [ { "name": "origin/KO-1", "error": null } ],
		"paths": { "Content/A.uasset": [ { "branch": "origin/KO-1", "blob": null, "status": "D" } ] },
		"partial": false, "truncated": false })");

	FStateResponse Response;
	FString Error;
	if (!TestTrue(TEXT("Parses"), ParseStateResponse(Json, Response, Error)))
	{
		return false;
	}
	const TArray<FPathChange>* Changes = Response.Paths.Find(TEXT("Content/A.uasset"));
	if (!TestNotNull(TEXT("Path present"), Changes) || !TestEqual(TEXT("One change"), Changes->Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("Blob empty"), (*Changes)[0].Blob.IsEmpty());
	TestEqual(TEXT("Status"), (*Changes)[0].Status, FString(TEXT("D")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheParseMalformedTest, "GitSourceControl.BranchStateCache.Parse.Malformed", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheParseMalformedTest::RunTest(const FString& Parameters)
{
	for (const FString& Json : { FString(TEXT("{")), FString(), FString(TEXT(R"({"base":"x"})")) })
	{
		FStateResponse Response;
		FString Error;
		TestFalse(FString::Printf(TEXT("Rejects '%s'"), *Json), ParseStateResponse(Json, Response, Error));
		TestFalse(FString::Printf(TEXT("Error for '%s'"), *Json), Error.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheParseNotUsableTest, "GitSourceControl.BranchStateCache.Parse.NotUsable", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheParseNotUsableTest::RunTest(const FString& Parameters)
{
	const TCHAR* Partial = TEXT(R"({ "branches": [], "paths": {}, "partial": true, "truncated": false })");
	const TCHAR* Truncated = TEXT(R"({ "branches": [], "paths": {}, "partial": false, "truncated": true })");
	const TCHAR* BranchError = TEXT(R"({ "branches": [ { "name": "origin/KO-1", "error": "fetch failed" } ], "paths": {}, "partial": false, "truncated": false })");

	for (const TCHAR* Json : { Partial, Truncated, BranchError })
	{
		FStateResponse Response;
		FString Error;
		FString Reason;
		if (TestTrue(FString::Printf(TEXT("Parses %s"), Json), ParseStateResponse(Json, Response, Error)))
		{
			TestFalse(FString::Printf(TEXT("Not usable %s"), Json), IsUsable(Response, Reason));
			TestFalse(TEXT("Reason given"), Reason.IsEmpty());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheUnicodePathTest, "GitSourceControl.BranchStateCache.Parse.UnicodeAndSpacesInPaths", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheUnicodePathTest::RunTest(const FString& Parameters)
{
	const FString Path = TEXT("Content/Maps/Café Level/Foo Bar.umap");

	const FString Body = BuildStateRequestBody(TEXT("a1"), { TEXT("origin/main") }, { Path });
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	TArray<FString> Paths;
	if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
	{
		Root->TryGetStringArrayField(TEXT("paths"), Paths);
	}
	TestEqual(TEXT("Request path round trips"), Paths, TArray<FString>{ Path });

	const FString Json = FString::Printf(TEXT(R"({ "branches": [ { "name": "origin/main", "error": null } ], "paths": { "%s": [ { "branch": "origin/main", "blob": "abc", "status": "M" } ] }, "partial": false, "truncated": false })"), *Path);
	FStateResponse Response;
	FString Error;
	TestTrue(TEXT("Parses"), ParseStateResponse(Json, Response, Error));
	TestTrue(TEXT("Response path round trips"), Response.Paths.Contains(Path));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheLsTreeTest, "GitSourceControl.BranchStateCache.LsTree.ParsesLines", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheLsTreeTest::RunTest(const FString& Parameters)
{
	// core.quotePath=false leaves spaces and non-ASCII alone, git still quotes names holding '"', '\' or control characters
	const FString Output = TEXT("100644 blob aaa\tContent/A.uasset\n")
		TEXT("100644 blob bbb\tContent/Dir With Space/Café.umap\r\n")
		TEXT("100644 blob ccc\t\"Content/Odd\\\"Name\\\\x\\303\\251.uasset\"\n")
		TEXT("160000 commit ddd\tPlugins/Submodule\n");

	const TMap<FString, FString> Blobs = ParseLsTree(Output);
	TestEqual(TEXT("Three blob entries"), Blobs.Num(), 3);
	const FString* A = Blobs.Find(TEXT("Content/A.uasset"));
	const FString* B = Blobs.Find(TEXT("Content/Dir With Space/Café.umap"));
	const FString* C = Blobs.Find(TEXT("Content/Odd\"Name\\xé.uasset"));
	TestTrue(TEXT("A blob"), A && *A == TEXT("aaa"));
	TestTrue(TEXT("B blob, CRLF tolerated"), B && *B == TEXT("bbb"));
	TestTrue(TEXT("C blob, quoted path unescaped"), C && *C == TEXT("ccc"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheRemoteHttpsTest, "GitSourceControl.BranchStateCache.Remote.Https", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheRemoteHttpsTest::RunTest(const FString& Parameters)
{
	FRemoteParts Parts;
	if (TestTrue(TEXT("Https parses"), ParseHttpsRemote(TEXT("https://user@dev.azure.com/org/proj/_git/game"), Parts)))
	{
		TestEqual(TEXT("Protocol"), Parts.Protocol, FString(TEXT("https")));
		TestEqual(TEXT("Host"), Parts.Host, FString(TEXT("dev.azure.com")));
		TestEqual(TEXT("Path"), Parts.Path, FString(TEXT("org/proj/_git/game")));
	}

	for (const TCHAR* Url : { TEXT("git@github.com:studio/game.git"), TEXT("ssh://git@host/x"), TEXT("http://host/x") })
	{
		FRemoteParts Ignored;
		TestFalse(FString::Printf(TEXT("Rejects %s"), Url), ParseHttpsRemote(Url, Ignored));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheAllowedUrlTest, "GitSourceControl.BranchStateCache.Remote.AllowedStateUrl", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheAllowedUrlTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("https from project config"), IsAllowedStateUrl(TEXT("https://branchstate.example.com/v1/ado/p/_git/game"), false));
	TestTrue(TEXT("HTTPS scheme is case insensitive"), IsAllowedStateUrl(TEXT("HTTPS://branchstate.example.com/v1/x"), false));
	TestFalse(TEXT("http from project config"), IsAllowedStateUrl(TEXT("http://branchstate.example.com/v1/x"), false));
	TestFalse(TEXT("http localhost from project config"), IsAllowedStateUrl(TEXT("http://localhost:8080/v1/x"), false));
	TestTrue(TEXT("http localhost from user override"), IsAllowedStateUrl(TEXT("http://localhost:8080/v1/x"), true));
	TestTrue(TEXT("http 127.0.0.1 from user override"), IsAllowedStateUrl(TEXT("http://127.0.0.1:8080/v1/x"), true));
	TestFalse(TEXT("http other host from user override"), IsAllowedStateUrl(TEXT("http://branchstate.example.com/v1/x"), true));
	TestFalse(TEXT("localhost lookalike from user override"), IsAllowedStateUrl(TEXT("http://localhost.evil.com/v1/x"), true));
	TestFalse(TEXT("userinfo"), IsAllowedStateUrl(TEXT("https://user:pw@branchstate.example.com/v1/x"), false));
	TestFalse(TEXT("other scheme"), IsAllowedStateUrl(TEXT("ftp://branchstate.example.com/v1/x"), true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheStripUserInfoTest, "GitSourceControl.BranchStateCache.Remote.StripUserInfo", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheStripUserInfoTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Stripped"), StripUserInfo(TEXT("https://user:secret@host/x")), FString(TEXT("https://host/x")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheCredentialTest, "GitSourceControl.BranchStateCache.Credential.Parse", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheCredentialTest::RunTest(const FString& Parameters)
{
	FString UserName;
	FString Password;
	TestTrue(TEXT("Parses"), ParseCredentialFill({ TEXT("protocol=https"), TEXT("host=h"), TEXT("username=u"), TEXT("password=p") }, UserName, Password));
	TestEqual(TEXT("User"), UserName, FString(TEXT("u")));
	TestEqual(TEXT("Password"), Password, FString(TEXT("p")));

	FString NoUser;
	FString NoPassword;
	TestFalse(TEXT("No password"), ParseCredentialFill({ TEXT("protocol=https"), TEXT("host=h"), TEXT("username=u") }, NoUser, NoPassword));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheNewerDifferentBlobTest, "GitSourceControl.BranchStateCache.NewerFiles.DifferentBlob", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheNewerDifferentBlobTest::RunTest(const FString& Parameters)
{
	const FStateResponse Response = GitBranchStateCacheTests::MakeResponse({ TEXT("origin/KO-1") }, TEXT("Content/A.uasset"), { { TEXT("origin/KO-1"), TEXT("bbb"), TEXT("M") } });
	const TMap<FString, FString> Newer = ComputeNewerFiles(Response, { { TEXT("Content/A.uasset"), TEXT("aaa") } }, TEXT("origin/main"));

	TestEqual(TEXT("One newer file"), Newer.Num(), 1);
	const FString* Branch = Newer.Find(TEXT("Content/A.uasset"));
	TestTrue(TEXT("On KO-1"), Branch && *Branch == TEXT("origin/KO-1"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheNewerRevertedTest, "GitSourceControl.BranchStateCache.NewerFiles.RevertedChangeIsUpToDate", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheNewerRevertedTest::RunTest(const FString& Parameters)
{
	const FStateResponse Response = GitBranchStateCacheTests::MakeResponse({ TEXT("origin/KO-1") }, TEXT("Content/A.uasset"), { { TEXT("origin/KO-1"), TEXT("aaa"), TEXT("M") } });
	const TMap<FString, FString> Newer = ComputeNewerFiles(Response, { { TEXT("Content/A.uasset"), TEXT("aaa") } }, TEXT("origin/main"));

	TestEqual(TEXT("Nothing newer"), Newer.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheNewerDeleteTest, "GitSourceControl.BranchStateCache.NewerFiles.DeleteOnBranch", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheNewerDeleteTest::RunTest(const FString& Parameters)
{
	const FStateResponse Response = GitBranchStateCacheTests::MakeResponse({ TEXT("origin/KO-1") }, TEXT("Content/A.uasset"), { { TEXT("origin/KO-1"), FString(), TEXT("D") } });
	const TMap<FString, FString> Newer = ComputeNewerFiles(Response, { { TEXT("Content/A.uasset"), TEXT("aaa") } }, TEXT("origin/main"));

	const FString* Branch = Newer.Find(TEXT("Content/A.uasset"));
	TestTrue(TEXT("Delete reported on KO-1"), Branch && *Branch == TEXT("origin/KO-1"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheNewerCurrentWinsTest, "GitSourceControl.BranchStateCache.NewerFiles.CurrentBranchWins", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheNewerCurrentWinsTest::RunTest(const FString& Parameters)
{
	const FStateResponse Response = GitBranchStateCacheTests::MakeResponse({ TEXT("origin/KO-1"), TEXT("origin/main") }, TEXT("Content/A.uasset"),
		{ { TEXT("origin/KO-1"), TEXT("bbb"), TEXT("M") }, { TEXT("origin/main"), TEXT("ccc"), TEXT("M") } });
	const TMap<FString, FString> Newer = ComputeNewerFiles(Response, { { TEXT("Content/A.uasset"), TEXT("aaa") } }, TEXT("origin/main"));

	const FString* Branch = Newer.Find(TEXT("Content/A.uasset"));
	TestTrue(TEXT("Current remote branch wins"), Branch && *Branch == TEXT("origin/main"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheNewerRemoteAheadTest, "GitSourceControl.BranchStateCache.NewerFiles.RemoteAheadOfHead", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheNewerRemoteAheadTest::RunTest(const FString& Parameters)
{
	const FStateResponse Response = GitBranchStateCacheTests::MakeResponse({ TEXT("origin/main") }, TEXT("Content/A.uasset"), { { TEXT("origin/main"), TEXT("ccc"), TEXT("M") } });
	const TMap<FString, FString> Newer = ComputeNewerFiles(Response, { { TEXT("Content/A.uasset"), TEXT("aaa") } }, TEXT("origin/main"));

	const FString* Branch = Newer.Find(TEXT("Content/A.uasset"));
	TestTrue(TEXT("Newer on current remote"), Branch && *Branch == TEXT("origin/main"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheBackoffSkipTest, "GitSourceControl.BranchStateCache.Backoff.SkipsAfterFailure", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheBackoffSkipTest::RunTest(const FString& Parameters)
{
	FBackoff Backoff;
	TestFalse(TEXT("Not skipping before any failure"), Backoff.ShouldSkip(100.0));
	Backoff.RecordConnectionFailure(100.0);
	TestTrue(TEXT("Skipping inside 30 seconds"), Backoff.ShouldSkip(129.9));
	TestFalse(TEXT("Not skipping at 30 seconds"), Backoff.ShouldSkip(130.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheBackoffSuccessTest, "GitSourceControl.BranchStateCache.Backoff.SuccessClears", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheBackoffSuccessTest::RunTest(const FString& Parameters)
{
	FBackoff Backoff;
	Backoff.RecordConnectionFailure(100.0);
	Backoff.RecordSuccess();
	TestFalse(TEXT("Success clears the backoff"), Backoff.ShouldSkip(101.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheBackoffTimeoutsTest, "GitSourceControl.BranchStateCache.Backoff.RepeatedTimeouts", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheBackoffTimeoutsTest::RunTest(const FString& Parameters)
{
	FBackoff Backoff;
	Backoff.RecordTimeout(90.0);
	Backoff.RecordTimeout(95.0);
	TestFalse(TEXT("Two timeouts do not back off"), Backoff.ShouldSkip(96.0));
	Backoff.RecordTimeout(100.0);
	TestTrue(TEXT("Third timeout in a row backs off"), Backoff.ShouldSkip(129.9));
	TestFalse(TEXT("For 30 seconds"), Backoff.ShouldSkip(130.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheBackoffTimeoutResetTest, "GitSourceControl.BranchStateCache.Backoff.SuccessResetsTimeouts", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheBackoffTimeoutResetTest::RunTest(const FString& Parameters)
{
	FBackoff Backoff;
	Backoff.RecordTimeout(90.0);
	Backoff.RecordTimeout(95.0);
	Backoff.RecordSuccess();
	Backoff.RecordTimeout(100.0);
	TestFalse(TEXT("A success in between resets the count"), Backoff.ShouldSkip(101.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheBackoffUnauthorizedTest, "GitSourceControl.BranchStateCache.Backoff.RepeatedUnauthorized", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheBackoffUnauthorizedTest::RunTest(const FString& Parameters)
{
	FBackoff Backoff;
	TestFalse(TEXT("First 401 retries with a fresh credential"), Backoff.RecordUnauthorized());
	TestTrue(TEXT("Second 401 in a row gives up"), Backoff.RecordUnauthorized());

	FBackoff Reset;
	Reset.RecordUnauthorized();
	Reset.RecordSuccess();
	TestFalse(TEXT("A success in between resets the count"), Reset.RecordUnauthorized());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBranchStateCacheMissingCurrentBranchTest, "GitSourceControl.BranchStateCache.Parse.MissingCurrentBranch", GIT_BRANCH_STATE_TEST_FLAGS)
bool FGitBranchStateCacheMissingCurrentBranchTest::RunTest(const FString& Parameters)
{
	FStateResponse Response;
	Response.BranchNames = { TEXT("origin/KO-1") };
	TestFalse(TEXT("Current remote branch missing from the answer"), AnswersForBranch(Response, TEXT("origin/main")));
	TestTrue(TEXT("Listed branch"), AnswersForBranch(Response, TEXT("origin/KO-1")));
	TestTrue(TEXT("No current remote branch to check"), AnswersForBranch(Response, FString()));
	return true;
}

#undef GIT_BRANCH_STATE_TEST_FLAGS

#endif
