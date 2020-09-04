source common.sh

# This are for ./fixed.nix
export IMPURE_VAR1=foo
export IMPURE_VAR2=bar

################################################################################
## Check that the ipfs daemon is present and  enabled in your environment
################################################################################

if [[ -z $(type -p ipfs) ]]; then
    echo "Ipfs not installed; skipping ipfs tests"
    exit 99
fi

startIpfs

################################################################################
## Create the folders for the source and destination stores
################################################################################

IPFS_TESTS=$TEST_ROOT/ipfs_tests
mkdir $IPFS_TESTS

# Here we define some store locations, one for the initial store we upload, and
# the other three for the destination stores to which we'll copy (one for each
# method)
IPFS_SRC_STORE=$IPFS_TESTS/ipfs_source_store

IPFS_DST_HTTP_STORE=$IPFS_TESTS/ipfs_dest_http_store
IPFS_DST_HTTP_LOCAL_STORE=$IPFS_TESTS/ipfs_dest_http_local_store
IPFS_DST_IPFS_STORE=$IPFS_TESTS/ipfs_dest_ipfs_store
IPFS_DST_IPNS_STORE=$IPFS_TESTS/ipfs_dest_ipns_store

EMPTY_HASH=$(echo {} | ipfs dag put)

################################################################################
## Check that fetchurl works directly with the ipfs store
################################################################################

TEST_FILE=test-file.txt
touch $TEST_FILE

# We try to do the evaluation with a known wrong hash to get the suggestion for
# the correct one
CORRECT_ADDRESS=$( \
    nix eval --impure --raw --expr '(builtins.fetchurl 'file://$PWD/$TEST_FILE')' --store ipfs://$EMPTY_HASH?allow-modify=true \
    |& grep '^warning: created new store' \
    | sed "s/^warning: created new store at '\(.*\)'\. .*$/\1/")

# Then we eval and get back the hash-name part of the store path
RESULT=$(nix eval --impure --expr '(builtins.fetchurl 'file://$PWD/$TEST_FILE')' --store "$CORRECT_ADDRESS" --json \
    | jq -r | awk -F/ '{print $NF}')

# Finally, we ask back the info from IPFS (formatting the address the right way
# beforehand)
ADDRESS_IPFS_FORMATTED=$(echo $CORRECT_ADDRESS | awk -F/ '{print $3}')
ipfs dag get /ipfs/$ADDRESS_IPFS_FORMATTED/nar/$RESULT

################################################################################
## Generate the keys to sign the store
################################################################################

SIGNING_KEY_NAME=nixcache.for.ipfs-1
SIGNING_KEY_PRI_FILE=$IPFS_TESTS/nix-cache-key.sec
SIGNING_KEY_PUB_FILE=$IPFS_TESTS/nix-cache-key.pub

nix-store --generate-binary-cache-key $SIGNING_KEY_NAME $SIGNING_KEY_PRI_FILE $SIGNING_KEY_PUB_FILE

################################################################################
## Create and sign the source store
################################################################################

mkdir -p $IPFS_SRC_STORE
storePaths=$(nix-build ./fixed.nix -A good --no-out-link)

nix sign-paths -k $SIGNING_KEY_PRI_FILE $storePaths

################################################################################
## Manually upload the source store
################################################################################

# Hack around https://github.com/NixOS/nix/issues/3695
for path in $storePaths; do
  nix copy --to file://$IPFS_SRC_STORE $path
done
unset path

MANUAL_IPFS_HASH=$(ipfs add -r $IPFS_SRC_STORE 2>/dev/null | tail -n 1 | awk '{print $2}')

################################################################################
## Create the local http store and download the derivation there
################################################################################

mkdir $IPFS_DST_HTTP_LOCAL_STORE

IPFS_HTTP_LOCAL_PREFIX="http://localhost:$IPFS_GATEWAY_PORT/ipfs"

nix-build ./fixed.nix -A good \
  --option substituters $IPFS_HTTP_LOCAL_PREFIX/$MANUAL_IPFS_HASH \
  --store $IPFS_DST_HTTP_LOCAL_STORE \
  --no-out-link \
  -j0 \
  --option trusted-public-keys $(cat $SIGNING_KEY_PUB_FILE)

################################################################################
## Create the ipfs store and download the derivation there
################################################################################

# Try to upload the content to the empty directory, fail but grab the right hash
# HERE do the same thing but expect failure
IPFS_ADDRESS=$(nix copy --to ipfs://$EMPTY_HASH?allow-modify=true $(nix-build ./fixed.nix -A good --no-out-link) --experimental-features nix-command |& \
               grep '^warning: created new store' | sed "s/^warning: created new store at '\(.*\)'\. .*$/\1/")

# We want to check that the `allow-modify` flag is required for the command to
# succeed. This is an invocation of the same command without that flag that we
# expect to fail
! nix copy --to ipfs://$EMPTY_HASH $(nix-build ./fixed.nix -A good --no-out-link) --experimental-features nix-command

# Verify that new path is valid.
nix copy --to $IPFS_ADDRESS $(nix-build ./fixed.nix -A good --no-out-link) --experimental-features nix-command

mkdir $IPFS_DST_IPFS_STORE

nix-build ./fixed.nix -A good \
  --option substituters $IPFS_ADDRESS \
  --store $IPFS_DST_IPFS_STORE \
  --no-out-link \
  -j0 \
  --option trusted-public-keys $(cat $SIGNING_KEY_PUB_FILE)


################################################################################
## Create the ipns store and download the derivation there
################################################################################

# First I have to publish:
IPNS_ID=$(ipfs name publish $EMPTY_HASH --allow-offline | awk '{print substr($3,1,length($3)-1)}')

# Check that we can upload the ipns store directly
nix copy --to ipns://$IPNS_ID $(nix-build ./fixed.nix -A good --no-out-link) --experimental-features nix-command

mkdir $IPFS_DST_IPNS_STORE

nix-build ./fixed.nix -A good \
  --option substituters 'ipns://'$IPNS_ID \
  --store $IPFS_DST_IPNS_STORE \
  --no-out-link \
  -j0 \
  --option trusted-public-keys $(cat $SIGNING_KEY_PUB_FILE)

# Verify we can copy something with dependencies
outPath=$(nix-build dependencies.nix --no-out-link)

nix copy $outPath --to ipns://$IPNS_ID --experimental-features nix-command

# and copy back
nix copy $outPath --store file://$IPFS_DST_IPNS_STORE --from ipns://$IPNS_ID --experimental-features nix-command

# Verify git objects can be substituted correctly

if [[ -n $(type -p git) ]]; then
    repo=$TEST_ROOT/git

    rm -rf $repo $TEST_HOME/.cache/nix

    git init $repo
    git -C $repo config user.email "foobar@example.com"
    git -C $repo config user.name "Foobar"

    echo hello > $repo/hello
    git -C $repo add hello
    git -C $repo commit -m 'Bla1'

    treeHash=$(git -C $repo rev-parse HEAD:)

    path=$(nix eval --raw --expr "(builtins.fetchTree { type = \"git\"; url = file://$repo; treeHash = \"$treeHash\"; }).outPath")

    # copy to ipfs in trustless mode, doesn’t require syncing
    nix copy $path --to ipfs:// --experimental-features nix-command

    helloBlob=$(git -C $repo rev-parse HEAD:hello)

    ipfs block stat f01781114$treeHash
    ipfs block get f01781114$treeHash > tree1
    (printf "tree %s\0" $(git -C $repo cat-file tree HEAD: | wc -c); git -C $repo cat-file tree HEAD:) > tree2
    diff tree1 tree2

    ipfs block stat f01781114$helloBlob
    ipfs block get f01781114$helloBlob > blob1
    (printf "blob %s\0" $(git -C $repo cat-file blob HEAD:hello | wc -c); git -C $repo cat-file blob HEAD:hello) > blob2
    diff blob1 blob2

    clearStore

    # verify we can substitute from global ipfs store
    nix eval --raw --expr "(builtins.fetchTree { type = \"git\"; url = file:///no-such-repo; treeHash = \"$helloBlob\"; }).outPath" --substituters ipfs:// --option substitute true
    path2=$(nix eval --raw --expr "(builtins.fetchTree { type = \"git\"; url = file:///no-such-repo; treeHash = \"$helloBlob\"; }).outPath" --substituters ipfs:// --option substitute true)
    [[ "$(cat $path2)" = hello ]]

    path3=$(nix eval --raw --expr "(builtins.fetchTree { type = \"git\"; url = file:///no-such-repo; treeHash = \"$treeHash\"; }).outPath" --substituters ipfs:// --option substitute true)

    [[ "$(ls $path3)" = hello ]]
    diff $path2 $path3/hello

    path4=$(nix eval --store ipfs:// --raw --expr "(builtins.fetchTree { type = \"git\"; url = file://$repo; treeHash = \"$treeHash\"; }).outPath")
    [[ "$(ls $path4)" = hello ]]
    diff $path2 $path4/hello
else
    echo "Git not installed; skipping IPFS/Git tests"
fi

# Try copying content addressable stuff

body=$(nix-build dependencies.nix --no-out-link)
nix --experimental-features 'nix-command ca-references' make-content-addressable --ipfs --json -r $body
rewrite=$(nix --experimental-features 'nix-command ca-references' make-content-addressable --ipfs --json -r $body | jq -r ".rewrites[\"$body\"]")
nix path-info $rewrite --json | jq

ca=$(nix path-info --json $rewrite | jq -r '.[0].ca')
numRefs=$(nix-store -q --references $rewrite | wc -l)

nix copy $rewrite --to ipfs://

# verify ipfs has it
cid=$(echo $ca | sed s,^ipfs:,,)
[ $(ipfs dag get $cid | jq -r .qtype) = ipfs ]
[ $(ipfs dag get $cid | jq -r .name) = dependencies-top ]

nix-store --delete $rewrite

path5=$(nix --experimental-features 'nix-command ca-references' ensure-ca dependencies-top:$ca --substituters ipfs:// --option substitute true)

[ $(nix-store -q --references $path5 | wc -l) = $numRefs ]
[ $(readlink -f $path5/self) = $path5 ]

nix-store --delete $path5

path6=$(nix --experimental-features 'nix-command ca-references' eval --expr "builtins.ipfsPath { name = \"dependencies-top\"; cid = \"$cid\"; }" --substituters ipfs:// --option substitute true)

[ "$path5" == "$path6" ]

source ./ipld-derivations.sh # TEMPORARY, to avoid spining up too IPFS deamons
