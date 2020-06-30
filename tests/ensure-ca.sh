source common.sh

if [[ -z $(type -p jq) ]]; then
    echo "Jq not installed; skipping ensure-ca tests"
    exit 99
fi

# This are for ./fixed.nix
export IMPURE_VAR1=foo
export IMPURE_VAR2=bar

body=$(nix-build fixed.nix -A good.0 --no-out-link)

ca=$(nix path-info --json $body | jq -r .\[0\].ca)

path=$(nix ensure-ca full:fixed:refs,0:$ca)

[ $body = $path ]

body=$(nix-build dependencies.nix --no-out-link)

rewrite=$(nix --experimental-features 'nix-command ca-references' make-content-addressable --json -r $body | jq -r ".rewrites[\"$body\"]")

ca=$(nix path-info --json $rewrite | jq -r .\[0\].ca)
numRefs=$(nix-store -q --references $rewrite | wc -l)
refs=$(nix-store -q --references $rewrite | sed s,$rewrite,self, | sed s,$NIX_STORE_DIR/,, | tr \\n :)

path2=$(nix ensure-ca dependencies-top:refs,$numRefs:$refs$ca)
[ -d $path2 ]
