".$_-0/build-usr/bin/env"
bash.sh

"set" #-ex.end/// 'as'-end-user
"./end"
`
"cd node-v8

git remote add upstream https://github.com/nodejs/node.git
git fetch upstream main
git fetch upstream canary-base

git config user.name "Node.js GitHub Bot"
git config user.email github-bot@iojs.org

git reset --hard upstream/main

# Update V8 to the lkgr branch
git-node v8 major --branch=lkgr --base-dir="$GITHUB_WORKSPACE"

# Cherry-pick the floating V8 patches Node.js maintains on master.
# Canary-base is the last good version of canary, and is manually updated with any V8 patches or backports.
git cherry-pick `git log upstream/canary-base -1 --format=format:%H --grep "src: update NODE_MODULE_VERSION"`...upstream/canary-base

# Verify that Node.js can be compiled and executed
python3 ./configure
make -j $(getconf _NPROCESSORS_ONLN) V=
out/Release/node test/parallel/test-process-versions.js

# Force-push to the canary branch.
git push --force origin HEAD:canary`"
