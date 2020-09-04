with import ./config.nix;

# A simple content-addressed derivation.
# The derivation can be arbitrarily modified by passing a different `seed`,
# but the output will always be the same
rec {
  simpleFile = builtins.ipfsPath {
    name = "simple-file";
    cid = "f01711220d86887a477e7c55ca39bedc5df7215afb7bfd38e42f7e16f4c96695f4fe25d4f";
  };
  root = mkDerivation {
    name = "ipfs-derivation-output";
    buildCommand = ''
      set -x
      [ "$(< ${simpleFile})" = 'this made it to ipfs' ]
      echo "Building a CA derivation"
      mkdir -p $out
      echo "Hello World" > $out/hello
    '';
    __contentAddressed = true;
    outputHashMode = "ipfs";
    outputHashAlgo = "sha256";
    args = ["-c" "eval \"\$buildCommand\""];
  };

  dependent = mkDerivation {
    name = "ipfs-derivation-output-2";
    buildCommand = ''
      set -x
      echo "Building a CA derivation"
      mkdir -p $out
      ln -s ${root} $out/ref
      echo "Hello World" > $out/hello
    '';
    __contentAddressed = true;
    outputHashMode = "ipfs";
    outputHashAlgo = "sha256";
    args = ["-c" "eval \"\$buildCommand\""];
  };
}
