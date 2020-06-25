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

path=$(nix ensure-ca $ca fixed)

[ $body = $path ]
