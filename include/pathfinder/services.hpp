#pragma once

#include <pathfinder/order_books.hpp>

#include <xrpl/basics/Log.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/core/ServiceRegistry.h>
#include <xrpl/server/LoadFeeTrack.h>

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <optional>
#include <stdexcept>
#include <string>

namespace pathfinder {

/**
 * Minimal ServiceRegistry for Pathfinder + RippleCalc.
 *
 * RippleCalc only needs getJournal(). Pathfinder needs getOrderBookDB(),
 * getFeeTrack(), and getJournal(). Every other accessor throws.
 */
class PathServices final : public xrpl::ServiceRegistry
{
public:
    explicit PathServices(boost::asio::io_context& io, beast::Severity level = beast::Severity::Error)
        : io_(io)
        , logs_(level)
        , feeTrack_(logs_.journal("LoadFeeTrack"))
        , books_(std::make_unique<LocalOrderBooks>())
    {
        // Flow logs every unfunded/degenerate AMM at Error; path_find
        // walks thousands of them. Keep the process usable.
        logs_.get("Flow").threshold(beast::Severity::Fatal);
    }

    LocalOrderBooks&
    books()
    {
        return *books_;
    }

    LocalOrderBooks const&
    books() const
    {
        return *books_;
    }

    void
    stop()
    {
        stopping_.store(true, std::memory_order_release);
    }

    xrpl::CollectorManager&
    getCollectorManager() override
    {
        throw unused("getCollectorManager");
    }

    xrpl::Family&
    getNodeFamily() override
    {
        throw unused("getNodeFamily");
    }

    xrpl::TimeKeeper&
    getTimeKeeper() override
    {
        throw unused("getTimeKeeper");
    }

    xrpl::JobQueue&
    getJobQueue() override
    {
        throw unused("getJobQueue");
    }

    xrpl::NodeCache&
    getTempNodeCache() override
    {
        throw unused("getTempNodeCache");
    }

    xrpl::CachedSLEs&
    getCachedSLEs() override
    {
        throw unused("getCachedSLEs");
    }

    xrpl::NetworkIDService&
    getNetworkIDService() override
    {
        throw unused("getNetworkIDService");
    }

    xrpl::AmendmentTable&
    getAmendmentTable() override
    {
        throw unused("getAmendmentTable");
    }

    xrpl::HashRouter&
    getHashRouter() override
    {
        throw unused("getHashRouter");
    }

    xrpl::LoadFeeTrack&
    getFeeTrack() override
    {
        return feeTrack_;
    }

    xrpl::LoadManager&
    getLoadManager() override
    {
        throw unused("getLoadManager");
    }

    xrpl::RCLValidations&
    getValidations() override
    {
        throw unused("getValidations");
    }

    xrpl::ValidatorList&
    getValidators() override
    {
        throw unused("getValidators");
    }

    xrpl::ValidatorSite&
    getValidatorSites() override
    {
        throw unused("getValidatorSites");
    }

    xrpl::ManifestCache&
    getValidatorManifests() override
    {
        throw unused("getValidatorManifests");
    }

    xrpl::ManifestCache&
    getPublisherManifests() override
    {
        throw unused("getPublisherManifests");
    }

    xrpl::Overlay&
    getOverlay() override
    {
        throw unused("getOverlay");
    }

    xrpl::Cluster&
    getCluster() override
    {
        throw unused("getCluster");
    }

    xrpl::PeerReservationTable&
    getPeerReservations() override
    {
        throw unused("getPeerReservations");
    }

    xrpl::resource::Manager&
    getResourceManager() override
    {
        throw unused("getResourceManager");
    }

    xrpl::node_store::Database&
    getNodeStore() override
    {
        throw unused("getNodeStore");
    }

    xrpl::SHAMapStore&
    getSHAMapStore() override
    {
        throw unused("getSHAMapStore");
    }

    xrpl::RelationalDatabase&
    getRelationalDatabase() override
    {
        throw unused("getRelationalDatabase");
    }

    xrpl::InboundLedgers&
    getInboundLedgers() override
    {
        throw unused("getInboundLedgers");
    }

    xrpl::InboundTransactions&
    getInboundTransactions() override
    {
        throw unused("getInboundTransactions");
    }

    xrpl::TaggedCache<xrpl::uint256, xrpl::AcceptedLedger>&
    getAcceptedLedgerCache() override
    {
        throw unused("getAcceptedLedgerCache");
    }

    xrpl::LedgerMaster&
    getLedgerMaster() override
    {
        throw unused("getLedgerMaster");
    }

    xrpl::LedgerCleaner&
    getLedgerCleaner() override
    {
        throw unused("getLedgerCleaner");
    }

    xrpl::LedgerReplayer&
    getLedgerReplayer() override
    {
        throw unused("getLedgerReplayer");
    }

    xrpl::PendingSaves&
    getPendingSaves() override
    {
        throw unused("getPendingSaves");
    }

    xrpl::OpenLedger&
    getOpenLedger() override
    {
        throw unused("getOpenLedger");
    }

    xrpl::OpenLedger const&
    getOpenLedger() const override
    {
        throw unused("getOpenLedger");
    }

    xrpl::NetworkOPs&
    getOPs() override
    {
        throw unused("getOPs");
    }

    xrpl::OrderBookDB&
    getOrderBookDB() override
    {
        return *books_;
    }

    xrpl::TransactionMaster&
    getMasterTransaction() override
    {
        throw unused("getMasterTransaction");
    }

    xrpl::TxQ&
    getTxQ() override
    {
        throw unused("getTxQ");
    }

    xrpl::PathRequestManager&
    getPathRequestManager() override
    {
        throw unused("getPathRequestManager");
    }

    xrpl::ServerHandler&
    getServerHandler() override
    {
        throw unused("getServerHandler");
    }

    xrpl::perf::PerfLog&
    getPerfLog() override
    {
        throw unused("getPerfLog");
    }

    [[nodiscard]] bool
    isStopping() const override
    {
        return stopping_.load(std::memory_order_acquire);
    }

    beast::Journal
    getJournal(std::string const& name) override
    {
        return logs_.journal(name);
    }

    boost::asio::io_context&
    getIOContext() override
    {
        return io_;
    }

    xrpl::Logs&
    getLogs() override
    {
        return logs_;
    }

    [[nodiscard]] std::optional<xrpl::uint256> const&
    getTrapTxID() const override
    {
        return trapTxID_;
    }

    xrpl::DatabaseCon&
    getWalletDB() override
    {
        throw unused("getWalletDB");
    }

    xrpl::Application&
    getApp() override
    {
        throw unused("getApp");
    }

private:
    [[noreturn]] static std::logic_error
    unused(char const* name)
    {
        throw std::logic_error(std::string("PathServices::") + name + " is not used by local path_find");
    }

    boost::asio::io_context& io_;
    xrpl::Logs logs_;
    xrpl::LoadFeeTrack feeTrack_;
    std::unique_ptr<LocalOrderBooks> books_;
    std::atomic<bool> stopping_{false};
    std::optional<xrpl::uint256> trapTxID_;
};

}  // namespace pathfinder
