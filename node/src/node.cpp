/*
    Copyright (c) 2011-2014 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2014 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "cocaine/service/node.hpp"
#include "cocaine/service/node/app/event.hpp"
#include "cocaine/service/node/slave/error.hpp"
#include "cocaine/detail/service/node/dispatch/client.hpp"

#include <cocaine/api/authorization/event.hpp>
#include <cocaine/api/storage.hpp>
#include <cocaine/context.hpp>
#include <cocaine/context/signal.hpp>
#include <cocaine/format/vector.hpp>
#include <cocaine/logging.hpp>
#include <cocaine/middleware/auth.hpp>
#include <cocaine/middleware/headers.hpp>
#include <cocaine/traits/dynamic.hpp>
#include <cocaine/traits/endpoint.hpp>
#include <cocaine/traits/graph.hpp>
#include <cocaine/traits/tuple.hpp>
#include <cocaine/traits/vector.hpp>

#include "cocaine/service/node/app.hpp"

#include <blackhole/logger.hpp>
#include <blackhole/scope/holder.hpp>
#include <blackhole/wrapper.hpp>

#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>

#include "cocaine/service/node/overseer.hpp"

using namespace cocaine;
using namespace cocaine::service;

using cocaine::service::node::overseer_t;

namespace ph = std::placeholders;

namespace {

class control_slot_t:
    public io::basic_slot<io::node::control_app>
{
    struct dispatch_t:
        public io::basic_slot<io::node::control_app>::dispatch_type
    {
        typedef io::basic_slot<io::node::control_app>::dispatch_type super;

        typedef io::event_traits<io::node::control_app>::dispatch_type dispatch_type;
        typedef io::protocol<dispatch_type>::scope protocol;

        using backward_protocol = io::aux::protocol_impl<typename io::event_traits<io::node::control_app>::upstream_type>::type;

        node_t& node;
        std::string app_name;
        streamed<bool> upstream;

        class catcher_t {
            streamed<bool> upstream;
        public:
            explicit
            catcher_t(streamed<bool> upstream) :
                upstream(upstream)
            {}

            template<typename Event, typename F, typename... Args>
            auto
            operator()(F fn, Event, const hpack::headers_t& headers, Args&&... args) -> typename result_of<F>::type {
                try {
                    return fn(headers, std::forward<Args>(args)...);
                } catch(const std::system_error& e) {
                    upstream.abort(e.code(), e.what());
                } catch(const std::exception& e) {
                    upstream.abort(error::uncaught_error, e.what());
                }
                if(io::is_recursed<Event>::value) {
                    return boost::none;
                } else {
                    using R = typename result_of<F>::type;
                    return R(nullptr);
                }
            }
        };


        dispatch_t(const std::string& name, node_t& node, streamed<bool> _upstream):
            super(format("controlling/{}", name)),
            node(node),
            app_name(name),
            upstream(_upstream)
        {
            try {
                if(!node.overseer(name)) {
                    throw cocaine::error_t("no overseer");
                }
            } catch(const std::system_error& e) {
                upstream.abort(error::not_running,
                    cocaine::format("app is not running, {}", e.what()));
            }

            catcher_t catcher(upstream);

            using chunk = protocol::chunk;
            using choke = protocol::choke;
            using error = protocol::error;
            using chunk_slot = io::basic_slot<chunk>;
            using choke_slot = io::basic_slot<choke>;
            using error_slot = io::basic_slot<error>;
            on<chunk>().with_middleware(catcher).execute(
                [&](const hpack::headers_t&, chunk_slot::tuple_type&& args, chunk_slot::upstream_type&&) -> chunk_slot::result_type {
                    auto size = std::get<0>(args);
                    if (size >= 0) {
                        control_population(boost::make_optional(std::size_t(size)));
                    } else {
                        control_population(boost::none);
                    }
                    upstream.write(true);
                    return boost::none;
                }
            );
            on<error>().with_middleware(catcher).execute(
                [&](const hpack::headers_t&, error_slot::tuple_type&&, error_slot::upstream_type&&) -> error_slot::result_type {
                    control_population(boost::none);
                    upstream.close();
                    return error_slot::result_type(nullptr);
                }
            );
            on<choke>().with_middleware(catcher).execute(
                [&](const hpack::headers_t&, choke_slot::tuple_type&&, choke_slot::upstream_type&&) -> choke_slot::result_type {
                    control_population(boost::none);
                    upstream.close();
                    return choke_slot::result_type(nullptr);
                }
            );
        }

        void
        control_population(boost::optional<std::size_t> population) {
            try {
                auto ovs = node.overseer(app_name);
                if (!ovs) {
                    throw cocaine::error_t("no overseer");
                }
                ovs->control_population(population);
            } catch(const std::exception& e) {
                upstream.abort(error::node_errors::not_running, cocaine::format("app is not running, {}", e.what()));
            }
        }

        void
        discard(const std::error_code&) override {
            control_population(boost::none);
        }
    };

    typedef std::vector<hpack::header_t> meta_type;
    typedef std::shared_ptr<io::basic_slot<io::node::control_app>::dispatch_type> result_type;

    node_t& parent;

public:
    explicit control_slot_t(node_t& parent):
        parent(parent)
    {}

    boost::optional<result_type>
    operator()(tuple_type&& args, upstream_type&& upstream) {
        return operator()({}, std::move(args), std::move(upstream));
    }

    boost::optional<result_type>
    operator()(const meta_type&, tuple_type&& args, upstream_type&& upstream) {
        streamed<bool> up;
        up.attach(upstream);

        const auto dispatch = tuple::invoke(std::move(args), [&](const std::string& name) -> result_type {
            return std::make_shared<dispatch_t>(name, parent, std::move(up));
        });

        return boost::make_optional(dispatch);
    }
};

struct event_middleware_t {
    std::shared_ptr<logging::logger_t> logger;
    std::shared_ptr<api::authorization::event_t> authorization;

    template<typename F, typename Event, typename... Args>
    auto
    operator()(F fn, Event, Args&&... args) ->
        decltype(fn(std::forward<Args>(args)..., logger))
    {
        auto identity = extract_identity(args...);
        auto cids = identity.cids();
        auto uids = identity.uids();
        auto log = std::make_shared<blackhole::wrapper_t>(*logger, blackhole::attributes_t{
            {"event", std::string(Event::alias())},
            {"cids", cocaine::format("{}", cids)},
            {"uids", cocaine::format("{}", uids)},
        });

        try {
            authorization->verify(Event::alias(), identity);
            return fn(std::forward<Args>(args)..., log);
        } catch (const std::system_error& err) {
            COCAINE_LOG_WARNING(log, "failed to complete '{}' operation", Event::alias(), blackhole::attribute_list{
                {"code", err.code().value()},
                {"error", error::to_string(err)},
            });
            throw;
        }
    }

private:
    template<typename... Args>
    auto
    extract_identity(const Args&... args) -> const auth::identity_t& {
        return std::get<sizeof...(Args) - 1>(std::tie(args...));
    }
};

}  // namespace

node_t::node_t(context_t& context, asio::io_service& asio, const std::string& name, const dynamic_t& args):
    category_type(context, asio, name, args),
    dispatch<io::node_tag>(name),
    log(context.log(name)),
    context(context)
{
    auto audit = std::shared_ptr<logging::logger_t>(context.log("audit", {{"service", name}}));
    auto middleware = middleware::auth_t(context, name);
    auto authorization = api::authorization::event(context, name);

    on<io::node::start_app>()
        .with_middleware(middleware)
        .with_middleware(middleware::drop_headers_t())
        .with_middleware(event_middleware_t{audit, authorization})
        .execute([&](
            const std::string& name,
            const std::string& profile,
            const auth::identity_t&,
            const std::shared_ptr<logging::logger_t>& log)
        {
            cocaine::deferred<void> deferred;

            start_app(name, profile, [=](std::future<void> future) mutable {
                try {
                    future.get();

                    COCAINE_LOG_INFO(log, "completed 'start_app' operation");
                    deferred.close();
                } catch (const std::system_error& err) {
                    COCAINE_LOG_WARNING(log, "failed to complete 'start_app' operation", {
                        {"code", err.code().value()},
                        {"error", error::to_string(err)},
                    });
                    deferred.abort(err.code(), error::to_string(err));
                }
            });

            return deferred;
        });
    on<io::node::pause_app>()
        .with_middleware(middleware)
        .with_middleware(middleware::drop_headers_t())
        .with_middleware(event_middleware_t{audit, authorization})
        .execute([&](
            const std::string& name,
            const auth::identity_t&,
            const std::shared_ptr<logging::logger_t>& log)
        {
            pause_app(name);
            COCAINE_LOG_INFO(log, "completed 'pause_app' operation");
        });
    on<io::node::list>(std::bind(&node_t::list, this));
    on<io::node::info>(std::bind(&node_t::info, this, ph::_1, ph::_2));
    on<io::node::control_app>(std::make_shared<control_slot_t>(*this));


    using enqueue_slot_t = io::basic_slot<io::node::enqueue>;
    on<io::node::enqueue>(
        [&](const hpack::headers_t& headers, enqueue_slot_t::tuple_type&& args, enqueue_slot_t::upstream_type&& upstream)
            -> enqueue_slot_t::result_type
        {
            std::string app_name, event_name;
            std::tie(app_name, event_name) = args;
            using app_protocol = io::protocol<io::stream_of<std::string>::tag>::scope;
            auto empty = [&](){
                auto dispatch = std::make_shared<enqueue_slot_t::dispatch_type>(format("{}/{}/empty", app_name, event_name));
                dispatch->on<app_protocol::error>([this](std::error_code, std::string){});
                dispatch->on<app_protocol::chunk>([this](std::string){});
                dispatch->on<app_protocol::choke>([this](){});
                return dispatch;
            };
            auto ovs = apps.apply([&](std::map<std::string, std::shared_ptr<node::app_t>>& apps) -> std::shared_ptr<overseer_t> {
                auto it = apps.find(app_name);
                if(it == apps.end()) {
                    return nullptr;
                } else {
                    return it->second->overseer();
                }
            });
            if(!ovs) {
                upstream.send<app_protocol::error>(make_error_code(error::not_running),
                                                   format("application {} is not found", app_name));
                return empty();
            } else {
                try {
                    node::app::event_t event(event_name, headers);
                    auto dispatch = ovs->enqueue(event, upstream);
                    return std::shared_ptr<enqueue_slot_t::dispatch_type>(dispatch);
                } catch (const std::system_error& e) {
                    upstream.send<app_protocol::error>(e.code(), e.what());
                    return empty();
                }
            }
        }
    );

    // Context signal/slot.
    signal = std::make_shared<dispatch<io::context_tag>>(name);
    signal->on<io::context::shutdown>(std::bind(&node_t::on_context_shutdown, this));

    const auto runname = args.as_object().at("runlist", "").as_string();

    if(runname.empty()) {
        context.signal_hub().listen(signal, asio);
        return;
    }

    COCAINE_LOG_INFO(log, "reading '{}' runlist", runname);

    typedef std::map<std::string, std::string> runlist_t;
    runlist_t runlist;

    const auto storage = api::storage(context, "core");

    try {
        // TODO: Perform request to a special service, like "storage->runlist(runname)".
        runlist = storage->get<runlist_t>("runlists", runname).get();
    } catch(const std::system_error& err) {
        COCAINE_LOG_WARNING(log, "unable to read '{}' runlist: {}", runname, err.what());
    }

    if(runlist.empty()) {
        context.signal_hub().listen(signal, asio);
        return;
    }

    COCAINE_LOG_INFO(log, "starting {} app(s)", runlist.size());

    std::vector<std::string> errored;

    std::string app;
    std::string profile;
    for (const auto& run : runlist) {
        std::tie(app, profile) = run;
        const blackhole::scope::holder_t scoped(*log, {{ "app", app }});

        try {
            start_app(app, profile);
        } catch(const std::exception& err) {
            COCAINE_LOG_WARNING(log, "unable to initialize app: {}", err.what());
            errored.push_back(app);
        }
    }

    if(!errored.empty()) {
        COCAINE_LOG_WARNING(log, "couldn't start {} app(s): {}", errored.size(), boost::join(errored, ", "));
    }

    context.signal_hub().listen(signal, asio);
}

node_t::~node_t() = default;

auto
node_t::prototype() -> io::basic_dispatch_t&{
    return *this;
}

void
node_t::on_context_shutdown() {
    // TODO: In fact this method may not be invoked during context shutdown - race - node service
    // can be terminated earlier than this completion handler be invoked.
    COCAINE_LOG_DEBUG(log, "shutting down apps");

    apps->clear();

    signal = nullptr;
}

auto
node_t::start_app(const std::string& name, const std::string& profile) -> deferred<void> {
    cocaine::deferred<void> deferred;

    start_app(name, profile, [=](std::future<void> future) mutable {
        try {
            future.get();
            deferred.close();
        } catch (const std::system_error& err) {
            deferred.abort(err.code(), error::to_string(err));
        }
    });

    return deferred;
}

auto
node_t::start_app(const std::string& name, const std::string& profile, callback_type callback) -> void {
    COCAINE_LOG_DEBUG(log, "processing `start_app` request, app: '{}'", name);

    apps.apply([&](std::map<std::string, std::shared_ptr<node::app_t>>& apps) {
        auto it = apps.find(name);

        if(it != apps.end()) {
            const auto info = it->second->info(io::node::info::brief).as_object();
            throw std::system_error(error::already_started,
                cocaine::format("app '{}' is {}", name, info["state"].as_string()));
        }

        apps.insert({
            name,
            std::make_shared<node::app_t>(context, name, profile, std::move(callback))
        });
    });
}

auto
node_t::pause_app(const std::string& name) -> void {
    COCAINE_LOG_DEBUG(log, "processing `pause_app` request, app: '{}'", name);

    apps.apply([&](std::map<std::string, std::shared_ptr<node::app_t>>& apps) {
        auto it = apps.find(name);

        if(it == apps.end()) {
            throw std::system_error(error::not_running,
                cocaine::format("app '{}' is not running", name));
        }

        auto app = it->second;
        apps.erase(it);
        return std::move(app);
    });
}

auto
node_t::list() const -> dynamic_t {
    dynamic_t::array_t result;
    apps.apply([&](const std::map<std::string, std::shared_ptr<node::app_t>>& apps) {
        boost::copy(apps | boost::adaptors::map_keys, std::back_inserter(result));
    });

    return result;
}

dynamic_t
node_t::info(const std::string& name, io::node::info::flags_t flags) const {
    auto app = apps.apply([&](const std::map<std::string, std::shared_ptr<node::app_t>>& apps) -> std::shared_ptr<node::app_t> {
        auto it = apps.find(name);

        if(it != apps.end()) {
            return it->second;
        }

        return nullptr;
    });

    if (!app) {
        throw cocaine::error_t("app '{}' is not running", name);
    }

    return app->info(flags);
}

std::shared_ptr<node::overseer_t>
node_t::overseer(const std::string& name) const {
    auto app = apps.apply([&](const std::map<std::string, std::shared_ptr<node::app_t>>& apps) -> std::shared_ptr<node::app_t> {
        auto it = apps.find(name);

        if(it != apps.end()) {
            return it->second;
        }

        return nullptr;
    });

    if (!app) {
        throw cocaine::error_t("app '{}' is not running", name);
    }

    return app->overseer();
}
