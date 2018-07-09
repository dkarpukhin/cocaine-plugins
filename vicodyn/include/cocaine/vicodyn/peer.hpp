#pragma once

#include "cocaine/idl/node.hpp"

#include <cocaine/executor/asio.hpp>
#include <cocaine/forwards.hpp>
#include <cocaine/locked_ptr.hpp>
#include <cocaine/logging.hpp>
#include <cocaine/rpc/session.hpp>
#include <cocaine/rpc/upstream.hpp>

#include <asio/ip/tcp.hpp>

#include <unordered_set>

namespace cocaine {
namespace vicodyn {

class peer_t : public std::enable_shared_from_this<peer_t> {
public:
    using app_streaming_tag = io::stream_of<std::string>::tag;

    using endpoints_t = std::vector<asio::ip::tcp::endpoint>;

    ~peer_t();

    peer_t(context_t& context, asio::io_service& loop, endpoints_t endpoints, std::string uuid, dynamic_t::object_t extra);

    template<class Event, class ...Args>
    auto open_stream(std::shared_ptr<io::basic_dispatch_t> dispatch, Args&& ...args) -> io::upstream_ptr_t {
        auto locked = session_.synchronize();
        auto session = *locked;
        if(!session) {
            schedule_reconnect(session);
            throw error_t(error::not_connected, "session is not connected");
        }
        d_.last_active = std::chrono::system_clock::now();
        auto stream = session->fork(std::move(dispatch));
        stream->send<Event>(std::forward<Args>(args)...);
        return stream;
    }

    auto connect() -> void;

    auto schedule_reconnect() -> void;

    auto uuid() const -> const std::string&;

    auto hostname() const -> const std::string&;

    auto endpoints() const -> const std::vector<asio::ip::tcp::endpoint>&;

    auto connected() const -> bool;

    auto last_active() const -> std::chrono::system_clock::time_point;

    auto extra() const -> const dynamic_t::object_t&;

    auto x_cocaine_cluster() const -> const std::string&;

private:
    auto schedule_reconnect(std::shared_ptr<cocaine::session_t>& session) -> void;

    context_t& context_;
    asio::io_service& loop_;
    asio::deadline_timer timer_;
    std::unique_ptr<logging::logger_t> logger_;
    synchronized<std::shared_ptr<cocaine::session_t>> session_;
    bool connecting_{false};

    struct {
        std::string uuid;
        std::vector<asio::ip::tcp::endpoint> endpoints;
        std::chrono::system_clock::time_point last_active;
        dynamic_t::object_t extra;
        std::string x_cocaine_cluster;
        std::string hostname;
    } d_;

};

// thread safe wrapper on map of peers indexed by uuid
class peers_t {
public:
    struct app_service_t { };

    using endpoints_t = std::vector<asio::ip::tcp::endpoint>;
    // peer_uuid -> peer_ptr
    using peers_data_t = std::unordered_map<std::string, std::shared_ptr<peer_t>>;
    // peer_uuid -> app_service
    using app_services_t = std::unordered_map<std::string, app_service_t>;
    // app_name -> app_services
    using app_data_t = std::unordered_map<std::string, app_services_t>;

    struct data_t {
        peers_data_t peers;
        app_data_t apps;
    };


private:
    context_t& context;
    std::unique_ptr<logging::logger_t> logger;
    executor::owning_asio_t executor;
    data_t data;
    mutable boost::shared_mutex mutex;


public:
    template<class F>
    auto apply_shared(F&& f) const -> decltype(f(std::declval<const data_t&>())) {
        boost::shared_lock<boost::shared_mutex> lock(mutex);
        return f(data);
    }

    template<class F>
    auto apply(F&& f) -> decltype(f(std::declval<data_t&>())) {
        boost::unique_lock<boost::shared_mutex> lock(mutex);
        return f(data);
    }


    peers_t(context_t& context);

    auto register_peer(const std::string& uuid, const endpoints_t& endpoints, dynamic_t::object_t extra) -> std::shared_ptr<peer_t>;

    auto register_peer(const std::string& uuid, std::shared_ptr<peer_t> peer) -> void;

    auto erase_peer(const std::string& uuid) -> void;

    auto register_app( const std::string& uuid, const std::string& name) -> void;

    auto erase_app(const std::string& uuid, const std::string& name) -> void;

    auto erase(const std::string& uuid) -> void;

    auto peer(const std::string& uuid) -> std::shared_ptr<peer_t>;
};

} // namespace vicodyn
} // namespace cocaine
