#pragma once
///@file

#include "parsed-derivations.hh"
#include "derivation-options.hh"
#ifndef _WIN32
#  include "user-lock.hh"
#endif
#include "outputs-spec.hh"
#include "store-api.hh"
#include "pathlocks.hh"
#include "goal.hh"

namespace nix {

using std::map;

#ifndef _WIN32 // TODO enable build hook on Windows
struct HookInstance;
#endif

typedef enum {rpAccept, rpDecline, rpPostpone} HookReply;

/**
 * Unless we are repairing, we don't both to test validity and just assume it,
 * so the choices are `Absent` or `Valid`.
 */
enum struct PathStatus {
    Corrupt,
    Absent,
    Valid,
};

struct InitialOutputStatus {
    StorePath path;
    PathStatus status;
    /**
     * Valid in the store, and additionally non-corrupt if we are repairing
     */
    bool isValid() const {
        return status == PathStatus::Valid;
    }
    /**
     * Merely present, allowed to be corrupt
     */
    bool isPresent() const {
        return status == PathStatus::Corrupt
            || status == PathStatus::Valid;
    }
};

struct InitialOutput {
    bool wanted;
    Hash outputHash;
    std::optional<InitialOutputStatus> known;
};

/** Used internally */
void runPostBuildHook(
    Store & store,
    Logger & logger,
    const StorePath & drvPath,
    const StorePathSet & outputPaths);

/**
 * Members shared, for now, between `DerivationGoal` and
 * `DerivationBuilder`.
 *
 * This should not depend on any `Goal` infrastructure, because the goal
 * is for `DerivationBuilder` to be separate from the scheduler / goals.
 *
 * There are some virtual members here, but that is to be avoided where
 * possible, because we would like `DerivationBuilder` to be
 * self-contained.
 *
 * @todo in the long term, `DerivationBuilder` should just do local
 * building building, some thing else should do remote building, and
 * `DerivationGoal` should should do scheduling, deciding which method
 * (local or remote building) to use at the appropriate moment. This
 * class should go away in that case because there really shouldn't be
 * so many fields/methods in common worth deduplicating this between the
 * scheduler and the two ways of building.
 */
struct DerivationGoalBuilderShared
{
    Store & store;

    /** The path of the derivation. */
    StorePath drvPath;

    /**
     * The specific outputs that we need to build.
     */
    OutputsSpec wantedOutputs;

    /**
     * The derivation stored at drvPath.
     */
    std::unique_ptr<Derivation> drv;

    std::unique_ptr<ParsedDerivation> parsedDrv;
    std::unique_ptr<DerivationOptions> drvOptions;

    /**
     * The remainder is state held during the build.
     */

    /**
     * Locks on (fixed) output paths.
     */
    PathLocks outputLocks;

    /**
     * All input paths (that is, the union of FS closures of the
     * immediate input paths).
     */
    StorePathSet inputPaths;

    std::map<std::string, InitialOutput> initialOutputs;

    /**
     * The sort of derivation we are building.
     */
    std::optional<DerivationType> derivationType;

    BuildMode buildMode;

    DerivationGoalBuilderShared(
        Store & store,
        StorePath drvPath,
        OutputsSpec wantedOutputs,
        BuildMode buildMode)
        : store{store}
        , drvPath{drvPath}, wantedOutputs{wantedOutputs}, buildMode{buildMode}
    { }

    /**
     * Activity that denotes waiting for a lock.
     */
    std::unique_ptr<Activity> actLock;

    /**
     * Open a log file and a pipe to it.
     */
    virtual Path openLogFile() = 0;

    /**
     * Close the log file.
     */
    virtual void closeLogFile() = 0;

    /**
     * Aborts if any output is not valid or corrupt, and otherwise
     * returns a 'SingleDrvOutputs' structure containing all outputs.
     *
     * @todo Probably should just be in `DerivationGoal`.
     */
    virtual SingleDrvOutputs assertPathValidity() = 0;

    virtual void started() = 0;

    virtual void appendLogTailErrorMsg(std::string & msg) = 0;
};

/**
 * A goal for building some or all of the outputs of a derivation.
 */
struct DerivationGoal : public Goal, virtual DerivationGoalBuilderShared
{
    /**
     * Whether to use an on-disk .drv file.
     */
    bool useDerivation;

    /**
     * The goal for the corresponding resolved derivation
     */
    std::shared_ptr<DerivationGoal> resolvedDrvGoal;

    /**
     * See `needRestart`; just for that field.
     */
    enum struct NeedRestartForMoreOutputs {
        /**
         * The goal state machine is progressing based on the current value of
         * `wantedOutputs. No actions are needed.
         */
        OutputsUnmodifedDontNeed,
        /**
         * `wantedOutputs` has been extended, but the state machine is
         * proceeding according to its old value, so we need to restart.
         */
        OutputsAddedDoNeed,
        /**
         * The goal state machine has progressed to the point of doing a build,
         * in which case all outputs will be produced, so extensions to
         * `wantedOutputs` no longer require a restart.
         */
        BuildInProgressWillNotNeed,
    };

    /**
     * Whether additional wanted outputs have been added.
     */
    NeedRestartForMoreOutputs needRestart = NeedRestartForMoreOutputs::OutputsUnmodifedDontNeed;

    /**
     * See `retrySubstitution`; just for that field.
     */
    enum RetrySubstitution {
        /**
         * No issues have yet arose, no need to restart.
         */
        NoNeed,
        /**
         * Something failed and there is an incomplete closure. Let's retry
         * substituting.
         */
        YesNeed,
        /**
         * We are current or have already retried substitution, and whether or
         * not something goes wrong we will not retry again.
         */
        AlreadyRetried,
    };

    /**
     * Whether to retry substituting the outputs after building the
     * inputs. This is done in case of an incomplete closure.
     */
    RetrySubstitution retrySubstitution = RetrySubstitution::NoNeed;

    /**
     * The remainder is state held during the build.
     */

    /**
     * File descriptor for the log file.
     */
    AutoCloseFD fdLogFile;
    std::shared_ptr<BufferedSink> logFileSink, logSink;

    /**
     * Number of bytes received from the builder's stdout/stderr.
     */
    unsigned long logSize;

    /**
     * The most recent log lines.
     */
    std::list<std::string> logTail;

    std::string currentLogLine;
    size_t currentLogLinePos = 0; // to handle carriage return

    std::string currentHookLine;

#ifndef _WIN32 // TODO enable build hook on Windows
    /**
     * The build hook.
     */
    std::unique_ptr<HookInstance> hook;
#endif

    std::unique_ptr<MaintainCount<uint64_t>> mcExpectedBuilds, mcRunningBuilds;

    std::unique_ptr<Activity> act;

    std::map<ActivityId, Activity> builderActivities;

    /**
     * The remote machine on which we're building.
     */
    std::string machineName;

    DerivationGoal(const StorePath & drvPath,
        const OutputsSpec & wantedOutputs, Worker & worker,
        BuildMode buildMode = bmNormal);
    DerivationGoal(const StorePath & drvPath, const BasicDerivation & drv,
        const OutputsSpec & wantedOutputs, Worker & worker,
        BuildMode buildMode = bmNormal);
    virtual ~DerivationGoal();

    void timedOut(Error && ex) override;

    std::string key() override;

    /**
     * Add wanted outputs to an already existing derivation goal.
     */
    void addWantedOutputs(const OutputsSpec & outputs);

    /**
     * The states.
     */
    Co init() override;
    Co haveDerivation();
    Co gaveUpOnSubstitution();
    Co tryToBuild();
    virtual Co tryLocalBuild();
    Co hookDone();

    Co resolvedFinished();

    /**
     * Is the build hook willing to perform the build?
     */
    HookReply tryBuildHook();

    /**
     * Open a log file and a pipe to it.
     */
    Path openLogFile() override;

    /**
     * Close the log file.
     */
    void closeLogFile() override;

    virtual bool isReadDesc(Descriptor fd);

    /**
     * Callback used by the worker to write to the log.
     */
    void handleChildOutput(Descriptor fd, std::string_view data) override;
    void handleEOF(Descriptor fd) override;
    void flushLine();

    /**
     * Wrappers around the corresponding Store methods that first consult the
     * derivation.  This is currently needed because when there is no drv file
     * there also is no DB entry.
     */
    std::map<std::string, std::optional<StorePath>> queryPartialDerivationOutputMap();
    OutputPathMap queryDerivationOutputMap();

    /**
     * Update 'initialOutputs' to determine the current status of the
     * outputs of the derivation. Also returns a Boolean denoting
     * whether all outputs are valid and non-corrupt, and a
     * 'SingleDrvOutputs' structure containing the valid outputs.
     */
    std::pair<bool, SingleDrvOutputs> checkPathValidity();

    /**
     * Aborts if any output is not valid or corrupt, and otherwise
     * returns a 'SingleDrvOutputs' structure containing all outputs.
     */
    SingleDrvOutputs assertPathValidity() override;

    /**
     * Forcibly kill the child process, if any.
     */
    virtual void killChild();

    Co repairClosure();

    void started() override;

    Done done(
        BuildResult::Status status,
        SingleDrvOutputs builtOutputs = {},
        std::optional<Error> ex = {});

    void appendLogTailErrorMsg(std::string & msg) override;

    StorePathSet exportReferences(const StorePathSet & storePaths);

    JobCategory jobCategory() const override {
        return JobCategory::Build;
    };
};

MakeError(NotDeterministic, BuildError);

}
