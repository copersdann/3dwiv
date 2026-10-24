@echo off
rem Publishes the current project to an existing GitHub repository.
rem Git Credential Manager handles authentication; never pass a token on the command line.
setlocal
if "%~1"=="" goto usage

set "REMOTE=%~1"

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
  git init -b main
  if errorlevel 1 goto failed
)

git add -A
if errorlevel 1 goto failed
git branch -M main
if errorlevel 1 goto failed

git diff --cached --quiet
if errorlevel 1 (
  git -c user.name="3Dwiv contributors" -c user.email="3dwiv@users.noreply.github.com" commit -m "Publish 3Dwiv Android 3D model viewer"
  if errorlevel 1 goto failed
)

git remote get-url origin >nul 2>&1
if errorlevel 1 (
  git remote add origin "%REMOTE%"
) else (
  git remote set-url origin "%REMOTE%"
)
if errorlevel 1 goto failed

git push -u origin main
if errorlevel 1 goto failed

echo.
echo OK: project pushed to %REMOTE%
endlocal
exit /b 0

:failed
echo.
echo ERROR: publish failed. Check the repository URL and GitHub authentication.
endlocal
exit /b 1

:usage
echo Usage: deploy.cmd "https://github.com/OWNER/REPOSITORY.git"
echo Create an empty GitHub repository first. Git Credential Manager will prompt for authentication if needed.
exit /b 2
