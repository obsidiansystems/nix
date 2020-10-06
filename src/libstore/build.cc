#include "build.hh"
#include "pathlocks.hh"
#include "references.hh"
#include "globals.hh"
#include "util.hh"
#include "archive.hh"
#include "affinity.hh"
#include "filetransfer.hh"
#include "finally.hh"
#include "json.hh"
#include "nar-info.hh"
#include "parsed-derivations.hh"
#include "machines.hh"
#include "daemon.hh"
#include "worker-protocol.hh"
#include "topo-sort.hh"
#include "callback.hh"

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#include <future>
#include <chrono>
#include <regex>
#include <queue>
#include <climits>

#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <termios.h>
#include <poll.h>

#include <pwd.h>
#include <grp.h>

#include <nlohmann/json.hpp>


namespace nix {

/* Common initialisation performed in child processes. */
void commonChildInit(Pipe & logPipe)
{
    restoreSignals();

    /* Put the child in a separate session (and thus a separate
       process group) so that it has no controlling terminal (meaning
       that e.g. ssh cannot open /dev/tty) and it doesn't receive
       terminal signals. */
    if (setsid() == -1)
        throw SysError("creating a new session");

    /* Dup the write side of the logger pipe into stderr. */
    if (dup2(logPipe.writeSide.get(), STDERR_FILENO) == -1)
        throw SysError("cannot pipe standard error into log file");

    /* Dup stderr to stdout. */
    if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1)
        throw SysError("cannot dup stderr into stdout");

    /* Reroute stdin to /dev/null. */
    int fdDevNull = open(pathNullDevice.c_str(), O_RDWR);
    if (fdDevNull == -1)
        throw SysError("cannot open '%1%'", pathNullDevice);
    if (dup2(fdDevNull, STDIN_FILENO) == -1)
        throw SysError("cannot dup null device into stdin");
    close(fdDevNull);
}

//////////////////////////////////////////////////////////////////////



//////////////////////////////////////////////////////////////////////


HookInstance::HookInstance()
{
    debug("starting build hook '%s'", settings.buildHook);

    /* Create a pipe to get the output of the child. */
    fromHook.create();

    /* Create the communication pipes. */
    toHook.create();

    /* Create a pipe to get the output of the builder. */
    builderOut.create();

    /* Fork the hook. */
    pid = startProcess([&]() {

        commonChildInit(fromHook);

        if (chdir("/") == -1) throw SysError("changing into /");

        /* Dup the communication pipes. */
        if (dup2(toHook.readSide.get(), STDIN_FILENO) == -1)
            throw SysError("dupping to-hook read side");

        /* Use fd 4 for the builder's stdout/stderr. */
        if (dup2(builderOut.writeSide.get(), 4) == -1)
            throw SysError("dupping builder's stdout/stderr");

        /* Hack: pass the read side of that fd to allow build-remote
           to read SSH error messages. */
        if (dup2(builderOut.readSide.get(), 5) == -1)
            throw SysError("dupping builder's stdout/stderr");

        Strings args = {
            std::string(baseNameOf(settings.buildHook.get())),
            std::to_string(verbosity),
        };

        execv(settings.buildHook.get().c_str(), stringsToCharPtrs(args).data());

        throw SysError("executing '%s'", settings.buildHook);
    });

    pid.setSeparatePG(true);
    fromHook.writeSide = -1;
    toHook.readSide = -1;

    sink = FdSink(toHook.writeSide.get());
    std::map<std::string, Config::SettingInfo> settings;
    globalConfig.getSettings(settings);
    for (auto & setting : settings)
        sink << 1 << setting.first << setting.second.value;
    sink << 0;
}


HookInstance::~HookInstance()
{
    try {
        toHook.writeSide = -1;
        if (pid != -1) pid.kill();
    } catch (...) {
        ignoreException();
    }
}


//////////////////////////////////////////////////////////////////////



//////////////////////////////////////////////////////////////////////


SubstitutionGoal::SubstitutionGoal(const StorePath & storePath, Worker & worker, RepairFlag repair, std::optional<ContentAddress> ca)
    : Goal(worker)
    , storePath(storePath)
    , repair(repair)
    , ca(ca)
{
    state = &SubstitutionGoal::init;
    name = fmt("substitution of '%s'", worker.store.printStorePath(this->storePath));
    trace("created");
    maintainExpectedSubstitutions = std::make_unique<MaintainCount<uint64_t>>(worker.expectedSubstitutions);
}


SubstitutionGoal::~SubstitutionGoal()
{
    try {
        if (thr.joinable()) {
            // FIXME: signal worker thread to quit.
            thr.join();
            worker.childTerminated(this);
        }
    } catch (...) {
        ignoreException();
    }
}


void SubstitutionGoal::work()
{
    (this->*state)();
}


void SubstitutionGoal::init()
{
    trace("init");

    worker.store.addTempRoot(storePath);

    /* If the path already exists we're done. */
    if (!repair && worker.store.isValidPath(storePath)) {
        amDone(ecSuccess);
        return;
    }

    if (settings.readOnlyMode)
        throw Error("cannot substitute path '%s' - no write access to the Nix store", worker.store.printStorePath(storePath));

    subs = settings.useSubstitutes ? getDefaultSubstituters() : std::list<ref<Store>>();

    tryNext();
}


void SubstitutionGoal::tryNext()
{
    trace("trying next substituter");

    if (subs.size() == 0) {
        /* None left.  Terminate this goal and let someone else deal
           with it. */
        debug("path '%s' is required, but there is no substituter that can build it", worker.store.printStorePath(storePath));

        /* Hack: don't indicate failure if there were no substituters.
           In that case the calling derivation should just do a
           build. */
        amDone(substituterFailed ? ecFailed : ecNoSubstituters);

        if (substituterFailed) {
            worker.failedSubstitutions++;
            worker.updateProgress();
        }

        return;
    }

    sub = subs.front();
    subs.pop_front();

    if (ca) {
        subPath = sub->makeFixedOutputPathFromCA(storePath.name(), *ca);
        if (sub->storeDir == worker.store.storeDir)
            assert(subPath == storePath);
    } else if (sub->storeDir != worker.store.storeDir) {
        tryNext();
        return;
    }

    try {
        // FIXME: make async
        info = sub->queryPathInfo(subPath ? *subPath : storePath);
    } catch (InvalidPath &) {
        tryNext();
        return;
    } catch (SubstituterDisabled &) {
        if (settings.tryFallback) {
            tryNext();
            return;
        }
        throw;
    } catch (Error & e) {
        if (settings.tryFallback) {
            logError(e.info());
            tryNext();
            return;
        }
        throw;
    }

    if (info->path != storePath) {
        if (info->isContentAddressed(*sub) && info->references.empty()) {
            auto info2 = std::make_shared<ValidPathInfo>(*info);
            info2->path = storePath;
            info = info2;
        } else {
            printError("asked '%s' for '%s' but got '%s'",
                sub->getUri(), worker.store.printStorePath(storePath), sub->printStorePath(info->path));
            tryNext();
            return;
        }
    }

    /* Update the total expected download size. */
    auto narInfo = std::dynamic_pointer_cast<const NarInfo>(info);

    maintainExpectedNar = std::make_unique<MaintainCount<uint64_t>>(worker.expectedNarSize, info->narSize);

    maintainExpectedDownload =
        narInfo && narInfo->fileSize
        ? std::make_unique<MaintainCount<uint64_t>>(worker.expectedDownloadSize, narInfo->fileSize)
        : nullptr;

    worker.updateProgress();

    /* Bail out early if this substituter lacks a valid
       signature. LocalStore::addToStore() also checks for this, but
       only after we've downloaded the path. */
    if (worker.store.requireSigs
        && !sub->isTrusted
        && !info->checkSignatures(worker.store, worker.store.getPublicKeys()))
    {
        logWarning({
            .name = "Invalid path signature",
            .hint = hintfmt("substituter '%s' does not have a valid signature for path '%s'",
                sub->getUri(), worker.store.printStorePath(storePath))
        });
        tryNext();
        return;
    }

    /* To maintain the closure invariant, we first have to realise the
       paths referenced by this one. */
    for (auto & i : info->references)
        if (i != storePath) /* ignore self-references */
            addWaitee(worker.makeSubstitutionGoal(i));

    if (waitees.empty()) /* to prevent hang (no wake-up event) */
        referencesValid();
    else
        state = &SubstitutionGoal::referencesValid;
}


void SubstitutionGoal::referencesValid()
{
    trace("all references realised");

    if (nrFailed > 0) {
        debug("some references of path '%s' could not be realised", worker.store.printStorePath(storePath));
        amDone(nrNoSubstituters > 0 || nrIncompleteClosure > 0 ? ecIncompleteClosure : ecFailed);
        return;
    }

    for (auto & i : info->references)
        if (i != storePath) /* ignore self-references */
            assert(worker.store.isValidPath(i));

    state = &SubstitutionGoal::tryToRun;
    worker.wakeUp(shared_from_this());
}


void SubstitutionGoal::tryToRun()
{
    trace("trying to run");

    /* Make sure that we are allowed to start a build.  Note that even
       if maxBuildJobs == 0 (no local builds allowed), we still allow
       a substituter to run.  This is because substitutions cannot be
       distributed to another machine via the build hook. */
    if (worker.getNrLocalBuilds() >= std::max(1U, (unsigned int) settings.maxBuildJobs)) {
        worker.waitForBuildSlot(shared_from_this());
        return;
    }

    maintainRunningSubstitutions = std::make_unique<MaintainCount<uint64_t>>(worker.runningSubstitutions);
    worker.updateProgress();

    outPipe.create();

    promise = std::promise<void>();

    thr = std::thread([this]() {
        try {
            /* Wake up the worker loop when we're done. */
            Finally updateStats([this]() { outPipe.writeSide = -1; });

            Activity act(*logger, actSubstitute, Logger::Fields{worker.store.printStorePath(storePath), sub->getUri()});
            PushActivity pact(act.id);

            copyStorePath(ref<Store>(sub), ref<Store>(worker.store.shared_from_this()),
                subPath ? *subPath : storePath, repair, sub->isTrusted ? NoCheckSigs : CheckSigs);

            promise.set_value();
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });

    worker.childStarted(shared_from_this(), {outPipe.readSide.get()}, true, false);

    state = &SubstitutionGoal::finished;
}


void SubstitutionGoal::finished()
{
    trace("substitute finished");

    thr.join();
    worker.childTerminated(this);

    try {
        promise.get_future().get();
    } catch (std::exception & e) {
        printError(e.what());

        /* Cause the parent build to fail unless --fallback is given,
           or the substitute has disappeared. The latter case behaves
           the same as the substitute never having existed in the
           first place. */
        try {
            throw;
        } catch (SubstituteGone &) {
        } catch (...) {
            substituterFailed = true;
        }

        /* Try the next substitute. */
        state = &SubstitutionGoal::tryNext;
        worker.wakeUp(shared_from_this());
        return;
    }

    worker.markContentsGood(storePath);

    printMsg(lvlChatty, "substitution of path '%s' succeeded", worker.store.printStorePath(storePath));

    maintainRunningSubstitutions.reset();

    maintainExpectedSubstitutions.reset();
    worker.doneSubstitutions++;

    if (maintainExpectedDownload) {
        auto fileSize = maintainExpectedDownload->delta;
        maintainExpectedDownload.reset();
        worker.doneDownloadSize += fileSize;
    }

    worker.doneNarSize += maintainExpectedNar->delta;
    maintainExpectedNar.reset();

    worker.updateProgress();

    amDone(ecSuccess);
}


void SubstitutionGoal::handleChildOutput(int fd, const string & data)
{
}


void SubstitutionGoal::handleEOF(int fd)
{
    if (fd == outPipe.readSide.get()) worker.wakeUp(shared_from_this());
}

//////////////////////////////////////////////////////////////////////


Worker::Worker(LocalStore & store)
    : act(*logger, actRealise)
    , actDerivations(*logger, actBuilds)
    , actSubstitutions(*logger, actCopyPaths)
    , store(store)
{
    /* Debugging: prevent recursive workers. */
    nrLocalBuilds = 0;
    lastWokenUp = steady_time_point::min();
    permanentFailure = false;
    timedOut = false;
    hashMismatch = false;
    checkMismatch = false;
}


Worker::~Worker()
{
    /* Explicitly get rid of all strong pointers now.  After this all
       goals that refer to this worker should be gone.  (Otherwise we
       are in trouble, since goals may call childTerminated() etc. in
       their destructors). */
    topGoals.clear();

    assert(expectedSubstitutions == 0);
    assert(expectedDownloadSize == 0);
    assert(expectedNarSize == 0);
}


std::shared_ptr<DerivationGoal> Worker::makeDerivationGoalCommon(
    const StorePath & drvPath,
    const StringSet & wantedOutputs,
    std::function<std::shared_ptr<DerivationGoal>()> mkDrvGoal)
{
    WeakGoalPtr & abstract_goal_weak = derivationGoals[drvPath];
    GoalPtr abstract_goal = abstract_goal_weak.lock(); // FIXME
    std::shared_ptr<DerivationGoal> goal;
    if (!abstract_goal) {
        goal = mkDrvGoal();
        abstract_goal_weak = goal;
        wakeUp(goal);
    } else {
        goal = std::dynamic_pointer_cast<DerivationGoal>(abstract_goal);
        assert(goal);
        goal->addWantedOutputs(wantedOutputs);
    }
    return goal;
}


std::shared_ptr<DerivationGoal> Worker::makeDerivationGoal(const StorePath & drvPath,
    const StringSet & wantedOutputs, BuildMode buildMode)
{
    return makeDerivationGoalCommon(drvPath, wantedOutputs, [&]() {
        return std::make_shared<DerivationGoal>(drvPath, wantedOutputs, *this, buildMode);
    });
}


std::shared_ptr<DerivationGoal> Worker::makeBasicDerivationGoal(const StorePath & drvPath,
    const BasicDerivation & drv, const StringSet & wantedOutputs, BuildMode buildMode)
{
    return makeDerivationGoalCommon(drvPath, wantedOutputs, [&]() {
        return std::make_shared<DerivationGoal>(drvPath, drv, wantedOutputs, *this, buildMode);
    });
}


GoalPtr Worker::makeSubstitutionGoal(const StorePath & path, RepairFlag repair, std::optional<ContentAddress> ca)
{
    WeakGoalPtr & goal_weak = substitutionGoals[path];
    GoalPtr goal = goal_weak.lock(); // FIXME
    if (!goal) {
        goal = std::make_shared<SubstitutionGoal>(path, *this, repair, ca);
        goal_weak = goal;
        wakeUp(goal);
    }
    return goal;
}


static void removeGoal(GoalPtr goal, WeakGoalMap & goalMap)
{
    /* !!! inefficient */
    for (WeakGoalMap::iterator i = goalMap.begin();
         i != goalMap.end(); )
        if (i->second.lock() == goal) {
            WeakGoalMap::iterator j = i; ++j;
            goalMap.erase(i);
            i = j;
        }
        else ++i;
}


void Worker::removeGoal(GoalPtr goal)
{
    nix::removeGoal(goal, derivationGoals);
    nix::removeGoal(goal, substitutionGoals);
    if (topGoals.find(goal) != topGoals.end()) {
        topGoals.erase(goal);
        /* If a top-level goal failed, then kill all other goals
           (unless keepGoing was set). */
        if (goal->exitCode == Goal::ecFailed && !settings.keepGoing)
            topGoals.clear();
    }

    /* Wake up goals waiting for any goal to finish. */
    for (auto & i : waitingForAnyGoal) {
        GoalPtr goal = i.lock();
        if (goal) wakeUp(goal);
    }

    waitingForAnyGoal.clear();
}


void Worker::wakeUp(GoalPtr goal)
{
    goal->trace("woken up");
    addToWeakGoals(awake, goal);
}


unsigned Worker::getNrLocalBuilds()
{
    return nrLocalBuilds;
}


void Worker::childStarted(GoalPtr goal, const set<int> & fds,
    bool inBuildSlot, bool respectTimeouts)
{
    Child child;
    child.goal = goal;
    child.goal2 = goal.get();
    child.fds = fds;
    child.timeStarted = child.lastOutput = steady_time_point::clock::now();
    child.inBuildSlot = inBuildSlot;
    child.respectTimeouts = respectTimeouts;
    children.emplace_back(child);
    if (inBuildSlot) nrLocalBuilds++;
}


void Worker::childTerminated(Goal * goal, bool wakeSleepers)
{
    auto i = std::find_if(children.begin(), children.end(),
        [&](const Child & child) { return child.goal2 == goal; });
    if (i == children.end()) return;

    if (i->inBuildSlot) {
        assert(nrLocalBuilds > 0);
        nrLocalBuilds--;
    }

    children.erase(i);

    if (wakeSleepers) {

        /* Wake up goals waiting for a build slot. */
        for (auto & j : wantingToBuild) {
            GoalPtr goal = j.lock();
            if (goal) wakeUp(goal);
        }

        wantingToBuild.clear();
    }
}


void Worker::waitForBuildSlot(GoalPtr goal)
{
    debug("wait for build slot");
    if (getNrLocalBuilds() < settings.maxBuildJobs)
        wakeUp(goal); /* we can do it right away */
    else
        addToWeakGoals(wantingToBuild, goal);
}


void Worker::waitForAnyGoal(GoalPtr goal)
{
    debug("wait for any goal");
    addToWeakGoals(waitingForAnyGoal, goal);
}


void Worker::waitForAWhile(GoalPtr goal)
{
    debug("wait for a while");
    addToWeakGoals(waitingForAWhile, goal);
}


void Worker::run(const Goals & _topGoals)
{
    for (auto & i : _topGoals) topGoals.insert(i);

    debug("entered goal loop");

    while (1) {

        checkInterrupt();

        store.autoGC(false);

        /* Call every wake goal (in the ordering established by
           CompareGoalPtrs). */
        while (!awake.empty() && !topGoals.empty()) {
            Goals awake2;
            for (auto & i : awake) {
                GoalPtr goal = i.lock();
                if (goal) awake2.insert(goal);
            }
            awake.clear();
            for (auto & goal : awake2) {
                checkInterrupt();
                goal->work();
                if (topGoals.empty()) break; // stuff may have been cancelled
            }
        }

        if (topGoals.empty()) break;

        /* Wait for input. */
        if (!children.empty() || !waitingForAWhile.empty())
            waitForInput();
        else {
            if (awake.empty() && 0 == settings.maxBuildJobs)
            {
                if (getMachines().empty())
                   throw Error("unable to start any build; either increase '--max-jobs' "
                            "or enable remote builds."
                            "\nhttps://nixos.org/nix/manual/#chap-distributed-builds");
                else
                   throw Error("unable to start any build; remote machines may not have "
                            "all required system features."
                            "\nhttps://nixos.org/nix/manual/#chap-distributed-builds");

            }
            assert(!awake.empty());
        }
    }

    /* If --keep-going is not set, it's possible that the main goal
       exited while some of its subgoals were still active.  But if
       --keep-going *is* set, then they must all be finished now. */
    assert(!settings.keepGoing || awake.empty());
    assert(!settings.keepGoing || wantingToBuild.empty());
    assert(!settings.keepGoing || children.empty());
}

void Worker::waitForInput()
{
    printMsg(lvlVomit, "waiting for children");

    /* Process output from the file descriptors attached to the
       children, namely log output and output path creation commands.
       We also use this to detect child termination: if we get EOF on
       the logger pipe of a build, we assume that the builder has
       terminated. */

    bool useTimeout = false;
    long timeout = 0;
    auto before = steady_time_point::clock::now();

    /* If we're monitoring for silence on stdout/stderr, or if there
       is a build timeout, then wait for input until the first
       deadline for any child. */
    auto nearest = steady_time_point::max(); // nearest deadline
    if (settings.minFree.get() != 0)
        // Periodicallty wake up to see if we need to run the garbage collector.
        nearest = before + std::chrono::seconds(10);
    for (auto & i : children) {
        if (!i.respectTimeouts) continue;
        if (0 != settings.maxSilentTime)
            nearest = std::min(nearest, i.lastOutput + std::chrono::seconds(settings.maxSilentTime));
        if (0 != settings.buildTimeout)
            nearest = std::min(nearest, i.timeStarted + std::chrono::seconds(settings.buildTimeout));
    }
    if (nearest != steady_time_point::max()) {
        timeout = std::max(1L, (long) std::chrono::duration_cast<std::chrono::seconds>(nearest - before).count());
        useTimeout = true;
    }

    /* If we are polling goals that are waiting for a lock, then wake
       up after a few seconds at most. */
    if (!waitingForAWhile.empty()) {
        useTimeout = true;
        if (lastWokenUp == steady_time_point::min() || lastWokenUp > before) lastWokenUp = before;
        timeout = std::max(1L,
            (long) std::chrono::duration_cast<std::chrono::seconds>(
                lastWokenUp + std::chrono::seconds(settings.pollInterval) - before).count());
    } else lastWokenUp = steady_time_point::min();

    if (useTimeout)
        vomit("sleeping %d seconds", timeout);

    /* Use select() to wait for the input side of any logger pipe to
       become `available'.  Note that `available' (i.e., non-blocking)
       includes EOF. */
    std::vector<struct pollfd> pollStatus;
    std::map <int, int> fdToPollStatus;
    for (auto & i : children) {
        for (auto & j : i.fds) {
            pollStatus.push_back((struct pollfd) { .fd = j, .events = POLLIN });
            fdToPollStatus[j] = pollStatus.size() - 1;
        }
    }

    if (poll(pollStatus.data(), pollStatus.size(),
            useTimeout ? timeout * 1000 : -1) == -1) {
        if (errno == EINTR) return;
        throw SysError("waiting for input");
    }

    auto after = steady_time_point::clock::now();

    /* Process all available file descriptors. FIXME: this is
       O(children * fds). */
    decltype(children)::iterator i;
    for (auto j = children.begin(); j != children.end(); j = i) {
        i = std::next(j);

        checkInterrupt();

        GoalPtr goal = j->goal.lock();
        assert(goal);

        set<int> fds2(j->fds);
        std::vector<unsigned char> buffer(4096);
        for (auto & k : fds2) {
            if (pollStatus.at(fdToPollStatus.at(k)).revents) {
                ssize_t rd = ::read(k, buffer.data(), buffer.size());
                // FIXME: is there a cleaner way to handle pt close
                // than EIO? Is this even standard?
                if (rd == 0 || (rd == -1 && errno == EIO)) {
                    debug("%1%: got EOF", goal->getName());
                    goal->handleEOF(k);
                    j->fds.erase(k);
                } else if (rd == -1) {
                    if (errno != EINTR)
                        throw SysError("%s: read failed", goal->getName());
                } else {
                    printMsg(lvlVomit, "%1%: read %2% bytes",
                        goal->getName(), rd);
                    string data((char *) buffer.data(), rd);
                    j->lastOutput = after;
                    goal->handleChildOutput(k, data);
                }
            }
        }

        if (goal->exitCode == Goal::ecBusy &&
            0 != settings.maxSilentTime &&
            j->respectTimeouts &&
            after - j->lastOutput >= std::chrono::seconds(settings.maxSilentTime))
        {
            goal->timedOut(Error(
                    "%1% timed out after %2% seconds of silence",
                    goal->getName(), settings.maxSilentTime));
        }

        else if (goal->exitCode == Goal::ecBusy &&
            0 != settings.buildTimeout &&
            j->respectTimeouts &&
            after - j->timeStarted >= std::chrono::seconds(settings.buildTimeout))
        {
            goal->timedOut(Error(
                    "%1% timed out after %2% seconds",
                    goal->getName(), settings.buildTimeout));
        }
    }

    if (!waitingForAWhile.empty() && lastWokenUp + std::chrono::seconds(settings.pollInterval) <= after) {
        lastWokenUp = after;
        for (auto & i : waitingForAWhile) {
            GoalPtr goal = i.lock();
            if (goal) wakeUp(goal);
        }
        waitingForAWhile.clear();
    }
}


unsigned int Worker::exitStatus()
{
    /*
     * 1100100
     *    ^^^^
     *    |||`- timeout
     *    ||`-- output hash mismatch
     *    |`--- build failure
     *    `---- not deterministic
     */
    unsigned int mask = 0;
    bool buildFailure = permanentFailure || timedOut || hashMismatch;
    if (buildFailure)
        mask |= 0x04;  // 100
    if (timedOut)
        mask |= 0x01;  // 101
    if (hashMismatch)
        mask |= 0x02;  // 102
    if (checkMismatch) {
        mask |= 0x08;  // 104
    }

    if (mask)
        mask |= 0x60;
    return mask ? mask : 1;
}


bool Worker::pathContentsGood(const StorePath & path)
{
    auto i = pathContentsGoodCache.find(path);
    if (i != pathContentsGoodCache.end()) return i->second;
    printInfo("checking path '%s'...", store.printStorePath(path));
    auto info = store.queryPathInfo(path);
    bool res;
    if (!pathExists(store.printStorePath(path)))
        res = false;
    else {
        HashResult current = hashPath(info->narHash.type, store.printStorePath(path));
        Hash nullHash(htSHA256);
        res = info->narHash == nullHash || info->narHash == current.first;
    }
    pathContentsGoodCache.insert_or_assign(path, res);
    if (!res)
        logError({
            .name = "Corrupted path",
            .hint = hintfmt("path '%s' is corrupted or missing!", store.printStorePath(path))
        });
    return res;
}


void Worker::markContentsGood(const StorePath & path)
{
    pathContentsGoodCache.insert_or_assign(path, true);
}


//////////////////////////////////////////////////////////////////////


static void primeCache(Store & store, const std::vector<StorePathWithOutputs> & paths)
{
    StorePathSet willBuild, willSubstitute, unknown;
    uint64_t downloadSize, narSize;
    store.queryMissing(paths, willBuild, willSubstitute, unknown, downloadSize, narSize);

    if (!willBuild.empty() && 0 == settings.maxBuildJobs && getMachines().empty())
        throw Error(
            "%d derivations need to be built, but neither local builds ('--max-jobs') "
            "nor remote builds ('--builders') are enabled", willBuild.size());
}


void LocalStore::buildPaths(const std::vector<StorePathWithOutputs> & drvPaths, BuildMode buildMode)
{
    Worker worker(*this);

    primeCache(*this, drvPaths);

    Goals goals;
    for (auto & path : drvPaths) {
        if (path.path.isDerivation())
            goals.insert(worker.makeDerivationGoal(path.path, path.outputs, buildMode));
        else
            goals.insert(worker.makeSubstitutionGoal(path.path, buildMode == bmRepair ? Repair : NoRepair));
    }

    worker.run(goals);

    StorePathSet failed;
    std::optional<Error> ex;
    for (auto & i : goals) {
        if (i->ex) {
            if (ex)
                logError(i->ex->info());
            else
                ex = i->ex;
        }
        if (i->exitCode != Goal::ecSuccess) {
            DerivationGoal * i2 = dynamic_cast<DerivationGoal *>(i.get());
            if (i2) failed.insert(i2->getDrvPath());
            else failed.insert(dynamic_cast<SubstitutionGoal *>(i.get())->getStorePath());
        }
    }

    if (failed.size() == 1 && ex) {
        ex->status = worker.exitStatus();
        throw *ex;
    } else if (!failed.empty()) {
        if (ex) logError(ex->info());
        throw Error(worker.exitStatus(), "build of %s failed", showPaths(failed));
    }
}

BuildResult LocalStore::buildDerivation(const StorePath & drvPath, const BasicDerivation & drv,
    BuildMode buildMode)
{
    Worker worker(*this);
    auto goal = worker.makeBasicDerivationGoal(drvPath, drv, {}, buildMode);

    BuildResult result;

    try {
        worker.run(Goals{goal});
        result = goal->getResult();
    } catch (Error & e) {
        result.status = BuildResult::MiscFailure;
        result.errorMsg = e.msg();
    }

    return result;
}


void LocalStore::ensurePath(const StorePath & path)
{
    /* If the path is already valid, we're done. */
    if (isValidPath(path)) return;

    primeCache(*this, {{path}});

    Worker worker(*this);
    GoalPtr goal = worker.makeSubstitutionGoal(path);
    Goals goals = {goal};

    worker.run(goals);

    if (goal->exitCode != Goal::ecSuccess) {
        if (goal->ex) {
            goal->ex->status = worker.exitStatus();
            throw *goal->ex;
        } else
            throw Error(worker.exitStatus(), "path '%s' does not exist and cannot be created", printStorePath(path));
    }
}


void LocalStore::repairPath(const StorePath & path)
{
    Worker worker(*this);
    GoalPtr goal = worker.makeSubstitutionGoal(path, Repair);
    Goals goals = {goal};

    worker.run(goals);

    if (goal->exitCode != Goal::ecSuccess) {
        /* Since substituting the path didn't work, if we have a valid
           deriver, then rebuild the deriver. */
        auto info = queryPathInfo(path);
        if (info->deriver && isValidPath(*info->deriver)) {
            goals.clear();
            goals.insert(worker.makeDerivationGoal(*info->deriver, StringSet(), bmRepair));
            worker.run(goals);
        } else
            throw Error(worker.exitStatus(), "cannot repair path '%s'", printStorePath(path));
    }
}


}
