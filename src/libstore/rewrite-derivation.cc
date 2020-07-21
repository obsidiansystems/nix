#include "derivations.hh"
#include "parsed-derivations.hh"
#include "util.hh"

namespace nix {

void recomputeOutputs(Store & store, Derivation & drv) {

    /* In this function we assert that the derivation is input addressed,
       otherwise there's nothing to recompute */
    assert(drv.type() == DerivationType::Regular);

    for (auto & i : drv.outputs) {
        debug("Rewriting env var %s", i.first);
        auto outputEnvVar = drv.env.find(i.first);
        if (outputEnvVar != drv.env.end()) {
            outputEnvVar->second = "";
            debug("Rewrote env var %s", outputEnvVar->first);
        }
        i.second = DerivationOutput {
            .output = DerivationOutputFloating {
                .method = FileIngestionMethod::Recursive,
                .hashType = htSHA256,
            },
        };
    }

    /* Use the masked derivation expression to compute the output path. */
    DrvHashModulo drvHM = hashDerivationModulo(store, drv, true);

    /* Since drv is input addressed, we know that the resulting DrvHashModulo
       has to be a hash */
    assert (drvHM.index() == 0);
    Hash h = std::get<Hash>(drvHM);

    // XXX: There's certainly a better and less error-prone way
    // of getting the name than to look it up in the drv environment
    string name = ParsedDerivation(StorePath::dummy, drv).getStringAttr("name").value_or("");

    for (auto & i : drv.outputs) {
        StorePath outPath = store.makeOutputPath(i.first, h, name);
        auto outputEnvVar = drv.env.find(i.first);
        if (outputEnvVar != drv.env.end())
            outputEnvVar->second = store.printStorePath(outPath);
        debug("Rewrote output %s to %s"
            , store.printStorePath(drv.outputs.at(i.first).path(store, name))
            , store.printStorePath(outPath));
        i.second = DerivationOutput {
            .output = DerivationOutputInputAddressed {
                .path = outPath,
            },
        };
    }
}

void rewriteDerivation(Store & store, Derivation & drv, const StringMap & rewrites) {

    debug("Rewriting the derivation");

    for (auto &rewrite: rewrites) {
        debug("rewriting %s as %s", rewrite.first, rewrite.second);
    }

    drv.builder = rewriteStrings(drv.builder, rewrites);
    for (auto & arg: drv.args) {
        arg = rewriteStrings(arg, rewrites);
    }

    StringPairs newEnv;
    for (auto & envVar: drv.env) {
        auto envName = rewriteStrings(envVar.first, rewrites);
        auto envValue = rewriteStrings(envVar.second, rewrites);
        newEnv.emplace(envName, envValue);
    }
    drv.env = newEnv;

    if (!derivationIsFixed(drv.type())) {
        recomputeOutputs(store, drv);
    }
}

bool BasicDerivation::resolve(Store & store) {
    return false;
}

bool Derivation::resolve(Store & store) {
    // Input paths that we'll want to rewrite in the derivation
    std::map<Path, Path> inputRewrites;

    DerivationInputs newInputs;

    for (auto & input : inputDrvs) {
        auto inputDrv = store.readDerivation(input.first);
        auto inputDrvOutputs = store.queryDerivationOutputMap(input.first);
        StringSet newOutputNames;
        for (auto & outputName : input.second) {
            auto actualPath = inputDrvOutputs.at(outputName);
            if (actualPath != inputDrv.findOutput(store, outputName)) {
                inputRewrites.emplace(
                        store.printStorePath(inputDrv.outputs.at(outputName).path(store, outputName)),
                        store.printStorePath(actualPath)
                );
                inputSrcs.emplace(std::move(actualPath));
            } else {
                newOutputNames.emplace(outputName);
            }
        }
        if (!newOutputNames.empty()) {
            newInputs.emplace(input.first, newOutputNames);
        }
    }
    inputDrvs = std::move(newInputs);
    if (!inputRewrites.empty()) {
        rewriteDerivation(store, *this, inputRewrites);
    }

    return (! inputRewrites.empty());
}

}
