let
  pkgs = import <nixpkgs> {};

  unshare_test = pkgs.runCommandNoCC "unshare_break" {} ''
    mkdir -p $out
    ${pkgs.util-linux}/bin/unshare -cn -- ${pkgs.bash}/bin/bash -e -c "${layer1}/bin/layer"
  '';

  layer1 = pkgs.writeShellScriptBin "layer" ''
    ${pkgs.util-linux}/bin/unshare -cn -- ${pkgs.bash}/bin/bash -e -c "${layer2}/bin/layer"
  '';

  layer2 = pkgs.writeShellScriptBin "layer" ''
    set -eux
    mkdir -p tmp_store
    export STORE=tmp_store
    bashPath=$(echo ${pkgs.bash.outPath} | sed 's%/nix/store/%%g')
    echo $bashPath
    ${pkgs.nix}/bin/nix-build \
      --store $TMP \
      --expr 'derivation { name = "test"; builder = ${pkgs.bash}/bin/bash; system = "x86_64-linux"; args = [ "-c" "echo hello > $out" ]; }'
    echo done > $out/log
  '';
in unshare_test


# Unshare -> Layer 1 -> Layer 2 :: FAILS no dervation output
# Unshare -> Layer 2 :: Outputs store contents
# Store :: ??
