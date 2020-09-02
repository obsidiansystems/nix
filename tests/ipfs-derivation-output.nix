with import ./config.nix;

# A simple content-addressed derivation.
# The derivation can be arbitrarily modified by passing a different `seed`,
# but the output will always be the same
rec {
  root = mkDerivation {
    name = "ipfs-derivation-output";
    buildCommand = ''
      set -x
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
