#include "check.hpp"

#include <edgy/engine.hpp>
#include <edgy/session.hpp>

#include <xrpl/protocol/UintTypes.h>

int
main()
{
    using namespace edgy;
    using edgy::test::expect;

    expect(pathDeepenBudget(false) == 0, "mid-close ticks do not FastPathFinder");
    expect(pathDeepenBudget(true) == 2, "closed ledger rediscovers at most two sockets");

    expect(shouldReplayCachedQuote(true, false), "idle mid-close replays the last quote");
    expect(!shouldReplayCachedQuote(true, true), "apply/close still reprices");
    expect(!shouldReplayCachedQuote(false, false), "create never replays a cache");
    expect(allowRevalidateFlow(10, 0), "first quote may Flow");
    expect(allowRevalidateFlow(11, 10), "new ledger may Flow if CLOB is dry");
    expect(!allowRevalidateFlow(10, 10), "same seq does not Flow again");

    auto const created = pathRepricePolicy(PathRepriceWave::Create);
    expect(!created.replayCache, "create does not replay a cache");
    expect(created.allowFlow, "create may Flow after a dry CLOB");
    expect(!created.allowDeepen, "create is not a deepen wave");
    expect(created.booksMoved, "create always quotes the live view");

    auto const idle = pathRepricePolicy(PathRepriceWave::MidCloseIdle);
    expect(idle.replayCache, "idle mid-close policy is replay");
    expect(!idle.allowFlow, "idle mid-close must not Flow");
    expect(!idle.allowDeepen, "idle mid-close must not FastPathFinder");
    expect(!idle.booksMoved, "idle mid-close booksMoved is false");
    expect(
        shouldReplayCachedQuote(true, idle.booksMoved),
        "idle mid-close args replay the last quote");

    auto const dirty = pathRepricePolicy(PathRepriceWave::MidCloseDirty);
    expect(!dirty.replayCache, "apply on the open ledger still reprices");
    expect(!dirty.allowFlow, "same-seq apply is CLOB-only, not convert-all Flow");
    expect(!dirty.allowDeepen, "apply mid-close must not FastPathFinder");
    expect(dirty.booksMoved, "apply mid-close booksMoved is true");
    expect(
        !shouldReplayCachedQuote(true, dirty.booksMoved),
        "apply mid-close does not replay");
    expect(
        !allowRevalidateFlow(100, 100),
        "same-seq dirty tick does not Flow convert-all");

    auto const closed = pathRepricePolicy(PathRepriceWave::LedgerClose);
    expect(!closed.replayCache, "ledger close never replays a cached dest");
    expect(closed.allowFlow, "ledger close may Flow if the CLOB is dry");
    expect(closed.allowDeepen, "ledger close may rediscover hops");
    expect(closed.booksMoved, "ledger close booksMoved is true");
    expect(
        !shouldReplayCachedQuote(true, closed.booksMoved),
        "ledger close notify args do not replay");
    expect(
        mustRecalcAfterLedgerClose(101, 100),
        "new closed seq after a quote on 100 must recalc");
    expect(
        mustRecalcAfterLedgerClose(1, 0),
        "first closed ledger after snapshot must recalc");
    expect(
        !mustRecalcAfterLedgerClose(100, 100),
        "already-quoted closed seq does not Force another Flow");
    expect(
        mustRecalcAfterLedgerClose(102, 101) &&
            !shouldReplayCachedQuote(true, true) &&
            allowRevalidateFlow(102, 101),
        "close + new seq is CLOB then Flow-if-dry, never cache replay");

    // The engine close path must use LedgerClose policy. If someone
    // later passes booksMoved=false on close, this combination fails.
    expect(
        pathRepricePolicy(PathRepriceWave::LedgerClose).booksMoved &&
            !pathRepricePolicy(PathRepriceWave::LedgerClose).replayCache,
        "Engine ledger-close wave cannot be configured to skip recalc");

    {
        auto idleCycle = planApplyCycle(false, true, true);
        expect(!idleCycle.exit && !idleCycle.hold && !idleCycle.takeBatch && idleCycle.tick,
               "ready + empty queue still ticks (100ms path_find updates)");
        auto work = planApplyCycle(false, true, false);
        expect(!work.exit && !work.hold && work.takeBatch && work.tick,
               "ready + queued apply takes a batch and ticks");
        auto snap = planApplyCycle(false, false, false);
        expect(!snap.exit && snap.hold && !snap.takeBatch && !snap.tick,
               "snapshot hold does not apply or tick");
        auto snapIdle = planApplyCycle(false, false, true);
        expect(!snapIdle.exit && snapIdle.hold && !snapIdle.tick,
               "snapshot hold with empty queue does not tick");
        auto halt = planApplyCycle(true, true, true);
        expect(halt.exit, "stop + empty queue exits the apply loop");
        auto drain = planApplyCycle(true, true, false);
        expect(!drain.exit && drain.takeBatch && !drain.tick,
               "stop + queued apply drains then does not tick");
    }

    expect(shouldApplyStreamTx(100, 100), "tx for the just-closed ledger applies");
    expect(!shouldApplyStreamTx(101, 100), "next ledger does not apply early");
    expect(shouldDeferStreamTx(101, 100), "next ledger is held until its close");
    expect(!shouldApplyStreamTx(99, 100), "tx for an older ledger is skipped");
    expect(shouldApplyStreamTx(0, 100), "tx with no ledger_index still applies");
    expect(!shouldApplyStreamTx(100, 100, 100), "snapshot ledger txs are not replayed");
    expect(shouldApplyStreamTx(101, 101, 100), "held tx applies once that ledger is current");
    expect(shouldDeferStreamTx(101, 100, 100), "tx after the snapshot is held until its close");

    return edgy::test::finish("reprice");
}
