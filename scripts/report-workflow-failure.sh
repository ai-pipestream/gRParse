#!/bin/sh
# Opens (or comments on) one GitHub issue when a workflow that nobody waits
# on fails on main, so a red publish cannot hide behind a green CI run.
#
#   sh scripts/report-workflow-failure.sh
#
# Runs inside GitHub Actions with GH_TOKEN (issues: write). On a push to
# main ci.yml skips the image legs (#240) because publish.yml builds them,
# which left a red publish invisible: 46e84fa's CI was green while every
# production image and deps seed failed (runs 44398650884, 44398650887).
set -eu

: "${GITHUB_REPOSITORY:?}" "${GITHUB_WORKFLOW:?}" "${GITHUB_RUN_ID:?}" "${GITHUB_SHA:?}"
title="$GITHUB_WORKFLOW is failing on main"
run="${GITHUB_SERVER_URL:-https://github.com}/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID"
body="$GITHUB_WORKFLOW failed on main at $GITHUB_SHA: $run

Opened by scripts/report-workflow-failure.sh. Close this issue once the workflow is green again; the next failure opens a new one."

number=$(gh issue list --repo "$GITHUB_REPOSITORY" --state open --search "in:title \"$title\"" \
  --json number,title --jq ".[] | select(.title == \"$title\") | .number" | head -n1)
if [ -n "$number" ]; then
  gh issue comment "$number" --repo "$GITHUB_REPOSITORY" --body "Failed again at $GITHUB_SHA: $run"
else
  gh issue create --repo "$GITHUB_REPOSITORY" --title "$title" --body "$body"
fi
