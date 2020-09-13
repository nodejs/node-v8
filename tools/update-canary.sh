#!/usr/bin/env bash

set -ex

cd node-v8

git remote add upstream https://github.com/nodejs/node.git
git fetch upstream master
git fetch upstream canary-base

git config user.name github-actions
git config user.email github-actions@github.com

git reset --hard upstream/master

# Update V8 to the lkgr branch
git-node v8 major --branch=lkgr --base-dir="$GITHUB_WORKSPACE"

# Cherry-pick the floating V8 patches Node.js maintains on master.
# Canary-base is the last good version of canary, and is manually updated with any V8 patches or backports.
git cherry-pick `git log upstream/canary-base -1 --format=format:%H --grep "src: update NODE_MODULE_VERSION"`...upstream/canary-base

# Verify that Node.js can be compiled and executed
python ./configure
make -j $(getconf _NPROCESSORS_ONLN) V=
out/Release/node test/parallel/test-process-versions.js

# Force-push to the canary branch.
git push --force origin HEAD:canary
