#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
try this:
python .\git-safe-push.py "https://github.com/thuyavansc/esp32s3_board.git"
python .\git-safe-push.py "https://github.com/thuyavansc/esp32s3_board_test.git"

"""
# python .\git-safe-push.py "https://github.com/thuyavansc/esp32s3_board.git"

"""
git_safe_push.py

Purpose:
    Safely initialize a local project as a Git repository (if needed),
    add files, commit changes, connect/update GitHub remote origin,
    rename branch to main, and push to GitHub.

How to run:
    1. Open PowerShell, Command Prompt, or VS Code terminal.
    2. Go to your project folder:
           cd "C:\\path\\to\\your project"
    3. Run:
           python git_safe_push.py "https://github.com/USERNAME/REPO.git"

       Or with a custom commit message:
           python git_safe_push.py "https://github.com/USERNAME/REPO.git" "Initial commit"

Examples:
    python git_safe_push.py "https://github.com/thuyavansc/esp32s3_board.git"
    python git_safe_push.py "https://github.com/thuyavansc/esp32s3_board.git" "Initial ESP32 S3 board commit"

What this script does:
    - Checks Git is installed
    - Validates the repo URL
    - Runs git init only if .git does not exist
    - Runs git add .
    - Commits only if there are staged changes
    - Adds origin if missing
    - Updates origin if already exists and URL differs
    - Renames branch to main
    - Pushes to origin/main
    - If push fails because remote already has history, it tries:
          git pull origin main --allow-unrelated-histories --no-rebase
      and then pushes again

Notes:
    - Run this from the ROOT of the project folder.
    - This script does not delete your files.
    - If GitHub asks you to sign in, complete the authentication prompt.
    - Make sure your .gitignore is correct before first push.
    - Do not commit secrets, API keys, passwords, certificates, or tokens.
"""

import os
import sys
import shutil
import subprocess
from urllib.parse import urlparse


def print_line():
    print("=" * 70)


def run_git_command(args, cwd, check=True):
    """
    Run a git command in the given working directory.

    Args:
        args: list of command arguments, for example ["git", "status"]
        cwd: folder where command should run
        check: if True, raise error on failure

    Returns:
        subprocess.CompletedProcess
    """
    print(f"[CMD] {' '.join(args)}")
    result = subprocess.run(
        args,
        cwd=cwd,
        text=True,
        capture_output=True,
        shell=False
    )

    if result.stdout and result.stdout.strip():
        print(result.stdout.strip())

    if result.stderr and result.stderr.strip():
        print(result.stderr.strip())

    if check and result.returncode != 0:
        raise subprocess.CalledProcessError(
            result.returncode, args, output=result.stdout, stderr=result.stderr
        )

    return result


def is_valid_repo_url(url):
    """
    Accepts:
      - https://github.com/USER/REPO.git
      - git@github.com:USER/REPO.git
    """
    if not url or not url.endswith(".git"):
        return False

    if url.startswith("https://github.com/"):
        return True

    if url.startswith("git@github.com:"):
        return True

    return False


def git_installed():
    return shutil.which("git") is not None


def get_remote_url(cwd, remote_name="origin"):
    result = subprocess.run(
        ["git", "remote", "get-url", remote_name],
        cwd=cwd,
        text=True,
        capture_output=True,
        shell=False
    )
    if result.returncode == 0:
        return result.stdout.strip()
    return None


def has_staged_changes(cwd):
    """
    git diff --cached --quiet
    returncode:
      0 -> no staged changes
      1 -> staged changes exist
    """
    result = subprocess.run(
        ["git", "diff", "--cached", "--quiet"],
        cwd=cwd,
        text=True,
        capture_output=True,
        shell=False
    )
    return result.returncode == 1


def main():
    if len(sys.argv) < 2:
        print("ERROR: Missing GitHub repository URL.")
        print("Usage:")
        print('  python git_safe_push.py "https://github.com/USERNAME/REPO.git"')
        print('  python git_safe_push.py "https://github.com/USERNAME/REPO.git" "Initial commit"')
        sys.exit(1)

    remote_url = sys.argv[1].strip()
    commit_message = sys.argv[2].strip() if len(sys.argv) >= 3 else "Initial commit"
    cwd = os.getcwd()

    print_line()
    print("GIT SAFE PUSH - PYTHON")
    print_line()
    print(f"Project folder : {cwd}")
    print(f"Remote URL     : {remote_url}")
    print(f"Commit message : {commit_message}")
    print_line()

    if not git_installed():
        print("ERROR: Git is not installed or not found in PATH.")
        print("Install Git for Windows first: https://git-scm.com/download/win")
        sys.exit(1)

    if not is_valid_repo_url(remote_url):
        print("ERROR: Repository URL does not look valid.")
        print("Expected examples:")
        print("  https://github.com/USERNAME/REPO.git")
        print("  git@github.com:USERNAME/REPO.git")
        sys.exit(1)

    try:
        # Step 1: Initialize git only if needed
        git_folder = os.path.join(cwd, ".git")
        if os.path.isdir(git_folder):
            print("[INFO] Existing Git repository detected. Skipping git init.")
        else:
            print("[INFO] No .git folder found. Initializing repository...")
            run_git_command(["git", "init"], cwd)

        # Step 2: Stage all files
        print("[INFO] Staging all files...")
        run_git_command(["git", "add", "."], cwd)

        # Step 3: Commit only if there are staged changes
        if has_staged_changes(cwd):
            print("[INFO] Staged changes detected. Creating commit...")
            run_git_command(["git", "commit", "-m", commit_message], cwd)
        else:
            print("[INFO] No staged changes to commit. Skipping commit step.")

        # Step 4: Add or update remote origin
        current_remote = get_remote_url(cwd, "origin")
        if current_remote is None:
            print("[INFO] Remote origin not found. Adding origin...")
            run_git_command(["git", "remote", "add", "origin", remote_url], cwd)
        else:
            if current_remote == remote_url:
                print("[INFO] Remote origin already points to the same URL.")
            else:
                print(f"[INFO] Existing origin URL found: {current_remote}")
                print(f"[INFO] Updating origin URL to: {remote_url}")
                run_git_command(["git", "remote", "set-url", "origin", remote_url], cwd)

        # Step 5: Rename branch to main
        print("[INFO] Setting branch name to main...")
        run_git_command(["git", "branch", "-M", "main"], cwd)

        # Step 6: First push attempt
        print("[INFO] Pushing to GitHub...")
        push_result = subprocess.run(
            ["git", "push", "-u", "origin", "main"],
            cwd=cwd,
            text=True,
            capture_output=True,
            shell=False
        )

        if push_result.stdout and push_result.stdout.strip():
            print(push_result.stdout.strip())
        if push_result.stderr and push_result.stderr.strip():
            print(push_result.stderr.strip())

        if push_result.returncode == 0:
            print("[SUCCESS] Push completed successfully.")
            sys.exit(0)

        # Step 7: Retry with pull then push
        print("[WARN] First push failed.")
        print("[INFO] Trying pull + push to sync with remote history...")
        run_git_command(
            ["git", "pull", "origin", "main", "--allow-unrelated-histories", "--no-rebase"],
            cwd
        )

        print("[INFO] Retrying push...")
        run_git_command(["git", "push", "-u", "origin", "main"], cwd)

        print("[SUCCESS] Push completed successfully after syncing remote history.")
        sys.exit(0)

    except subprocess.CalledProcessError as e:
        print_line()
        print("ERROR: Git command failed.")
        print(f"Command     : {' '.join(e.cmd)}")
        print(f"Exit code   : {e.returncode}")
        if e.output:
            print("Stdout:")
            print(e.output.strip())
        if e.stderr:
            print("Stderr:")
            print(e.stderr.strip())
        print_line()
        print("Possible reasons:")
        print("- GitHub authentication failed")
        print("- Merge conflict needs manual resolution")
        print("- Git user.name or user.email is not configured")
        print("- Remote branch protection rules prevented push")
        print("- .gitignore is missing and too many generated files were staged")
        print()
        print("Useful commands to inspect manually:")
        print("  git status")
        print("  git remote -v")
        print("  git branch -a")
        print("  git log --oneline --graph --decorate --all")
        sys.exit(1)

    except Exception as ex:
        print_line()
        print("ERROR: Unexpected failure.")
        print(str(ex))
        print_line()
        sys.exit(1)


if __name__ == "__main__":
    main()