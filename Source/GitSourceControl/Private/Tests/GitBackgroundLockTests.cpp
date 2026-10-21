#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GitBackgroundLock.h"
#include "GitSourceControlLog.h"
#include "GitSourceControlState.h"

#define GIT_BACKGROUND_LOCK_TEST_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitBackgroundLockFailureStatesTest, "GitSourceControl.BackgroundLock.FailureStatesTouchOnlyLockFields", GIT_BACKGROUND_LOCK_TEST_FLAGS)
bool FGitBackgroundLockFailureStatesTest::RunTest(const FString& Parameters)
{
	// What RefreshLocks leaves behind: FindOrAdd'ed entries with every non-lock field at its default
	TMap<const FString, FGitState> Refreshed;
	FGitState& Other = Refreshed.Add(TEXT("C:/Repo/Content/A.umap"));
	Other.LockState = ELockState::LockedOther;
	Other.LockUser = TEXT("bob");
	Refreshed.Add(TEXT("C:/Repo/Content/Unrelated.uasset")).LockState = ELockState::LockedOther;
	Refreshed.Add(TEXT("C:/Repo/Content/Mine.uasset")).LockState = ELockState::Locked;

	TArray<FString> FailedFiles;
	const TMap<const FString, FGitState> States = GitBackgroundLock::BuildLockFailureStates(Refreshed,
		{ TEXT("C:/Repo/Content/A.umap"), TEXT("C:/Repo/Content/Missing.uasset"), TEXT("C:/Repo/Content/Mine.uasset") }, FailedFiles);

	TestEqual(TEXT("Failed files"), FailedFiles, TArray<FString>{ TEXT("C:/Repo/Content/A.umap"), TEXT("C:/Repo/Content/Missing.uasset") });
	TestEqual(TEXT("Only failed files are pushed"), States.Num(), 2);
	TestFalse(TEXT("Unrelated locks are not pushed"), States.Contains(TEXT("C:/Repo/Content/Unrelated.uasset")));

	const FGitState* A = States.Find(TEXT("C:/Repo/Content/A.umap"));
	if (TestNotNull(TEXT("A pushed"), A))
	{
		TestEqual(TEXT("A lock"), static_cast<int32>(A->LockState), static_cast<int32>(ELockState::LockedOther));
		TestEqual(TEXT("A lock user"), A->LockUser, FString(TEXT("bob")));
		TestEqual(TEXT("A file state untouched"), static_cast<int32>(A->FileState), static_cast<int32>(EFileState::Unset));
		TestEqual(TEXT("A tree state untouched"), static_cast<int32>(A->TreeState), static_cast<int32>(ETreeState::Unset));
		TestEqual(TEXT("A remote state untouched"), static_cast<int32>(A->RemoteState), static_cast<int32>(ERemoteState::Unset));
	}

	const FGitState* Missing = States.Find(TEXT("C:/Repo/Content/Missing.uasset"));
	if (TestNotNull(TEXT("Missing pushed"), Missing))
	{
		TestEqual(TEXT("Missing lock"), static_cast<int32>(Missing->LockState), static_cast<int32>(ELockState::NotLocked));
		TestEqual(TEXT("Missing tree state untouched"), static_cast<int32>(Missing->TreeState), static_cast<int32>(ETreeState::Unset));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLogRedactTest, "GitSourceControl.Log.RedactsUserInfo", GIT_BACKGROUND_LOCK_TEST_FLAGS)
bool FGitSourceControlLogRedactTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Token in a remote URL"), GitSourceControlLog::RedactUserInfo(TEXT("remote add origin \"https://user:pat@host/x.git\"")),
		FString(TEXT("remote add origin \"https://host/x.git\"")));
	TestEqual(TEXT("Two URLs"), GitSourceControlLog::RedactUserInfo(TEXT("a https://u:p@h1/x b http://u@h2/y")), FString(TEXT("a https://h1/x b http://h2/y")));
	TestEqual(TEXT("At sign in the path is kept"), GitSourceControlLog::RedactUserInfo(TEXT("fetch https://host/a@b")), FString(TEXT("fetch https://host/a@b")));
	TestEqual(TEXT("No URL"), GitSourceControlLog::RedactUserInfo(TEXT("status --porcelain \"Content/A@B.uasset\"")), FString(TEXT("status --porcelain \"Content/A@B.uasset\"")));
	return true;
}

#undef GIT_BACKGROUND_LOCK_TEST_FLAGS

#endif
