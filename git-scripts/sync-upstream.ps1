if (Test-Path -Path "../.git") {
    cd ..
}

if (Test-Path -Path ".git") {
    git remote add upstream https://github.com/ProjectBorealis/UEGitPlugin.git
    git fetch --all
    git checkout -B dev --track origin/dev
    git pull
    git merge upstream/dev
    git push origin dev
    git checkout -B dev-3k --track origin/dev-3k
    git pull
    git merge origin/dev
    git push origin dev-3k
}
else {
    Write-Host "Unable to locate the git repository"
}
