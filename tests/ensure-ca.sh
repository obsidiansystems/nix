source common.sh

if [[ -z $(type -p jq) ]]; then
    echo "Jq not installed; skipping ensure-ca tests"
    exit 99
fi

# This are for ./fixed.nix
export IMPURE_VAR1=foo
export IMPURE_VAR2=bar

body=$(nix-build fixed.nix -A good.0 --no-out-link)

jqCaHash='.[0].ca | split(":") | .[1:] | join(":")'

caHash=$(nix path-info --json $body | jq -r "$jqCaHash")

path=$(nix ensure-ca fixed:fixed:refs:0:$caHash)

[ $body = $path ]

body=$(nix-build dependencies.nix --no-out-link)

rewrite=$(nix --experimental-features 'nix-command ca-references' make-content-addressable --json -r $body | jq -r ".rewrites[\"$body\"]")

caHash=$(nix path-info --json $rewrite | jq -r "$jqCaHash")
numRefs=$(nix-store -q --references $rewrite | grep -v $rewrite | wc -l)
refs=$(nix-store -q --references $rewrite | sed s,$rewrite,self, | sed s,$NIX_STORE_DIR/,, | tr \\n :)

path2=$(nix ensure-ca dependencies-top:fixed:refs:$numRefs:$refs$caHash)
[ -d $path2 ]
[ fixed:$caHash = $(nix path-info --json $path2 | jq -r .\[0\].ca) ]
