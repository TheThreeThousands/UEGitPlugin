#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GitLfsLockBatch.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

using namespace GitLfsLockBatch;

#define GIT_LFS_LOCK_BATCH_TEST_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace GitLfsLockBatchTests
{
	TSharedPtr<FJsonObject> ParseObject(const FString& InJson)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InJson);
		FJsonSerializer::Deserialize(Reader, Root);
		return Root;
	}

	/** The example from the GitLfsCache README, "Batched locking" */
	const TCHAR* const ReadmeResponse = TEXT(R"({ "results": [
		{ "path": "Content/A.uasset", "ok": true,  "lock": { "id": "871", "owner": { "name": "someone" } } },
		{ "path": "Content/B.uasset", "ok": false, "status": 409, "message": "already locked" }
	] })");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchBodyLockTest, "GitSourceControl.LfsLockBatch.Body.Lock", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchBodyLockTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Paths{ TEXT("Content/A.uasset"), TEXT("Content/Ä b.uasset") };
	const TSharedPtr<FJsonObject> Root = GitLfsLockBatchTests::ParseObject(BuildBody(EOperation::Lock, Paths, TEXT("refs/heads/main")));
	if (!TestTrue(TEXT("Body is JSON"), Root.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("operation"), Root->GetStringField(TEXT("operation")), FString(TEXT("lock")));
	TArray<FString> SentPaths;
	Root->TryGetStringArrayField(TEXT("paths"), SentPaths);
	TestEqual(TEXT("paths round trip"), SentPaths, Paths);
	const TSharedPtr<FJsonObject>* Ref = nullptr;
	if (TestTrue(TEXT("ref present"), Root->TryGetObjectField(TEXT("ref"), Ref)))
	{
		TestEqual(TEXT("ref name"), (*Ref)->GetStringField(TEXT("name")), FString(TEXT("refs/heads/main")));
	}
	TestFalse(TEXT("force never sent"), Root->HasField(TEXT("force")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchBodyUnlockTest, "GitSourceControl.LfsLockBatch.Body.UnlockWithoutRef", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchBodyUnlockTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Root = GitLfsLockBatchTests::ParseObject(BuildBody(EOperation::Unlock, { TEXT("Content/A.uasset") }, FString()));
	if (!TestTrue(TEXT("Body is JSON"), Root.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("operation"), Root->GetStringField(TEXT("operation")), FString(TEXT("unlock")));
	TestFalse(TEXT("no ref"), Root->HasField(TEXT("ref")));
	TestFalse(TEXT("force never sent"), Root->HasField(TEXT("force")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchParseMixedTest, "GitSourceControl.LfsLockBatch.Parse.MixedResults", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchParseMixedTest::RunTest(const FString& Parameters)
{
	TArray<FItemResult> Results;
	FString Error;
	if (!TestTrue(TEXT("Parses"), ParseResponse(GitLfsLockBatchTests::ReadmeResponse, { TEXT("Content/A.uasset"), TEXT("Content/B.uasset") }, Results, Error))
		|| !TestEqual(TEXT("one result per path"), Results.Num(), 2))
	{
		return false;
	}

	TestEqual(TEXT("A path"), Results[0].Path, FString(TEXT("Content/A.uasset")));
	TestTrue(TEXT("A ok"), Results[0].bOk);
	TestEqual(TEXT("A id"), Results[0].LockId, FString(TEXT("871")));
	TestEqual(TEXT("A owner"), Results[0].Owner, FString(TEXT("someone")));
	TestEqual(TEXT("B path"), Results[1].Path, FString(TEXT("Content/B.uasset")));
	TestFalse(TEXT("B failed"), Results[1].bOk);
	TestEqual(TEXT("B status"), Results[1].Status, 409);
	TestEqual(TEXT("B message"), Results[1].Message, FString(TEXT("already locked")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchParseMissingTest, "GitSourceControl.LfsLockBatch.Parse.MissingItemsFail", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchParseMissingTest::RunTest(const FString& Parameters)
{
	const FString Json = TEXT(R"({ "results": [
		{ "path": "C.uasset", "ok": true, "lock": { "id": "3", "owner": { "name": "me" } } },
		{ "path": "A.uasset", "ok": true, "lock": { "id": "1", "owner": { "name": "me" } } }
	] })");

	TArray<FItemResult> Results;
	FString Error;
	if (!TestTrue(TEXT("Parses"), ParseResponse(Json, { TEXT("A.uasset"), TEXT("B.uasset"), TEXT("C.uasset") }, Results, Error))
		|| !TestEqual(TEXT("one result per requested path"), Results.Num(), 3))
	{
		return false;
	}

	TestEqual(TEXT("request order 0"), Results[0].Path, FString(TEXT("A.uasset")));
	TestTrue(TEXT("A ok"), Results[0].bOk);
	TestEqual(TEXT("request order 1"), Results[1].Path, FString(TEXT("B.uasset")));
	TestFalse(TEXT("B missing counts as failed"), Results[1].bOk);
	TestEqual(TEXT("B message"), Results[1].Message, FString(TEXT("no result from GitLfsCache")));
	TestEqual(TEXT("request order 2"), Results[2].Path, FString(TEXT("C.uasset")));
	TestTrue(TEXT("C ok"), Results[2].bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchParseRejectsTest, "GitSourceControl.LfsLockBatch.Parse.RejectsNonBatchBody", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchParseRejectsTest::RunTest(const FString& Parameters)
{
	TArray<FItemResult> Results;
	FString Error;
	TestFalse(TEXT("a lock listing"), ParseResponse(TEXT(R"({"locks":[]})"), { TEXT("A.uasset") }, Results, Error));
	TestFalse(TEXT("an array"), ParseResponse(TEXT("[]"), { TEXT("A.uasset") }, Results, Error));
	TestFalse(TEXT("not JSON"), ParseResponse(TEXT("not json"), { TEXT("A.uasset") }, Results, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchEnvTest, "GitSourceControl.LfsLockBatch.Env.Endpoint", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchEnvTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Lines{
		TEXT("git-lfs/3.6.0 (GitHub; windows amd64; go 1.23.3)"),
		TEXT(""),
		TEXT("Endpoint=https://lfs.example.com/github/studio/game.git/info/lfs (auth=basic)"),
		TEXT("Endpoint (upstream)=https://other/info/lfs (auth=none)"),
	};

	FString Url;
	TestTrue(TEXT("found"), ParseLfsEnvEndpoint(Lines, Url));
	TestEqual(TEXT("url"), Url, FString(TEXT("https://lfs.example.com/github/studio/game.git/info/lfs")));

	FString Missing;
	TestFalse(TEXT("no Endpoint= line"), ParseLfsEnvEndpoint({ TEXT("Endpoint (upstream)=https://other/info/lfs (auth=none)") }, Missing));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchRefTest, "GitSourceControl.LfsLockBatch.Ref.Name", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchRefTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("upstream branch"), RemoteRefName(TEXT("refs/remotes/origin/feature/x"), TEXT("local")), FString(TEXT("refs/heads/feature/x")));
	TestEqual(TEXT("no upstream"), RemoteRefName(FString(), TEXT("local")), FString(TEXT("refs/heads/local")));
	TestEqual(TEXT("nothing known"), RemoteRefName(FString(), FString()), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchTimeoutTest, "GitSourceControl.LfsLockBatch.Timeout.GrowsAndCaps", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchTimeoutTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("no paths"), TimeoutSeconds(0), 2.0f);
	TestEqual(TEXT("100 paths"), TimeoutSeconds(100), 7.0f, 0.001f);
	TestEqual(TEXT("capped"), TimeoutSeconds(10000), 60.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchChunksTest, "GitSourceControl.LfsLockBatch.Chunks.StayUnderServerLimit", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchChunksTest::RunTest(const FString& Parameters)
{
	TArray<FString> Paths;
	for (int32 Index = 0; Index < 1201; ++Index)
	{
		Paths.Add(FString::Printf(TEXT("Content/__ExternalActors__/A%d.uasset"), Index));
	}

	const TArray<TArray<FString>> Chunks = SplitIntoChunks(Paths);
	if (!TestEqual(TEXT("chunk count"), Chunks.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("first chunk"), Chunks[0].Num(), 500);
	TestEqual(TEXT("second chunk"), Chunks[1].Num(), 500);
	TestEqual(TEXT("last chunk"), Chunks[2].Num(), 201);
	TestEqual(TEXT("order kept"), Chunks[2].Last(), Paths.Last());
	TestEqual(TEXT("empty input"), SplitIntoChunks(TArray<FString>()).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchUnknownTest, "GitSourceControl.LfsLockBatch.Unknown.OnlyWhatIsLeft", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchUnknownTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Requested{ TEXT("A.uasset"), TEXT("B.uasset"), TEXT("C.uasset") };
	const TSet<FString> HeldByMe{ TEXT("a.uasset"), TEXT("C.uasset"), TEXT("Z.uasset") };

	// A timed out batch may have locked some files already; git lfs lock would report those as failures
	TestEqual(TEXT("lock what is not yet held"), RemainingAfterUnknownOutcome(EOperation::Lock, Requested, HeldByMe), TArray<FString>{ TEXT("B.uasset") });
	// and git lfs unlock would fail on files the batch already released
	TestEqual(TEXT("unlock what is still held"), RemainingAfterUnknownOutcome(EOperation::Unlock, Requested, HeldByMe), TArray<FString>{ TEXT("A.uasset"), TEXT("C.uasset") });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchClassifyTest, "GitSourceControl.LfsLockBatch.Response.AmbiguousRepliesAreUnknown", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchClassifyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("answered"), ClassifyResponse(200, true) == EResponseOutcome::Answered);
	// Refused before any item ran, so git lfs can safely do them all
	TestTrue(TEXT("no route"), ClassifyResponse(404, false) == EResponseOutcome::NotAsked);
	TestTrue(TEXT("too many paths"), ClassifyResponse(413, false) == EResponseOutcome::NotAsked);
	TestTrue(TEXT("credential refused"), ClassifyResponse(401, false) == EResponseOutcome::NotAsked);
	TestTrue(TEXT("bad request"), ClassifyResponse(400, false) == EResponseOutcome::NotAsked);
	// A gateway or server error can arrive after some items were granted or released
	TestTrue(TEXT("bad gateway"), ClassifyResponse(502, false) == EResponseOutcome::Unknown);
	TestTrue(TEXT("server error"), ClassifyResponse(500, false) == EResponseOutcome::Unknown);
	TestTrue(TEXT("unreadable 200"), ClassifyResponse(200, false) == EResponseOutcome::Unknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsLockBatchItemUnauthorizedTest, "GitSourceControl.LfsLockBatch.Response.ItemUnauthorized", GIT_LFS_LOCK_BATCH_TEST_FLAGS)
bool FGitLfsLockBatchItemUnauthorizedTest::RunTest(const FString& Parameters)
{
	TArray<FItemResult> Results;
	FString Error;
	ParseResponse(GitLfsLockBatchTests::ReadmeResponse, { TEXT("Content/A.uasset"), TEXT("Content/B.uasset") }, Results, Error);
	TestFalse(TEXT("a 409 is not a credential problem"), HasUnauthorizedItem(Results));

	// GitLfsCache relays each upstream failure inside a 200, so an expired token shows up per item
	ParseResponse(TEXT(R"({ "results": [ { "path": "Content/A.uasset", "ok": false, "status": 401, "message": "Unauthorized" } ] })"), { TEXT("Content/A.uasset") }, Results, Error);
	TestTrue(TEXT("a 401 item"), HasUnauthorizedItem(Results));
	return true;
}

#undef GIT_LFS_LOCK_BATCH_TEST_FLAGS

#endif
