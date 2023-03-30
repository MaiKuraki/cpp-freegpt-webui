#include <functional>
#include <semaphore>
#include <string>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/scope_exit.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <spdlog/spdlog.h>

#include <inja/inja.hpp>

#include "cfg.h"
#include "free_gpt.h"
#include "helper.hpp"

constexpr auto use_nothrow_awaitable =
    boost::asio::as_tuple(boost::asio::use_awaitable);

inline std::string CHAT_PATH{"/chat"};
inline std::string ASSETS_PATH{"/assets"};
inline std::string API_PATH{"/backend-api/v2/conversation"};

using GptCallback = std::function<boost::asio::awaitable<void>(
    std::shared_ptr<FreeGpt::Channel>, nlohmann::json,
    FreeGpt::HttpClientPool::handle)>;
inline std::unordered_map<
    std::string,
    std::pair<std::shared_ptr<FreeGpt::HttpClientPool>, GptCallback>>
    gpt_function;

#define ADD_METHOD(name, function)                                             \
    gpt_function[name] = std::pair(                                            \
        std::make_shared<FreeGpt::HttpClientPool>(1, cfg.max_http_client_num), \
        std::bind_front(&function, app));

std::string create_index_html(const std::string& file) {
    boost::uuids::random_generator gen;
    inja::Environment env;
    nlohmann::json data;
    data["chat_id"] = boost::uuids::to_string(gen());
    for (auto&& m : std::views::keys(gpt_function))
        data["model_list"].emplace_back(m);
    return env.render_file(file, data);
}

boost::asio::awaitable<void> send_http_response(auto& stream, auto& request,
                                                auto status) {
    boost::beast::http::response<boost::beast::http::string_body> res{
        status, request.version()};
    res.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(boost::beast::http::field::content_type, "text/html");
    res.keep_alive(request.keep_alive());
    res.prepare_payload();
    boost::beast::http::message_generator rsp = std::move(res);
    co_await boost::beast::async_write(stream, std::move(rsp),
                                       use_nothrow_awaitable);
    co_return;
}

boost::asio::awaitable<void> start_session(boost::asio::ip::tcp::socket sock,
                                           Config& cfg,
                                           boost::asio::io_context& context) {
    boost::beast::tcp_stream stream{std::move(sock)};
    using namespace boost::asio::experimental::awaitable_operators;
    BOOST_SCOPE_EXIT(&stream) {
        boost::beast::error_code ec;
        stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                 ec);
    }
    BOOST_SCOPE_EXIT_END
    while (true) {
        boost::beast::flat_buffer buffer;
        boost::beast::http::request<boost::beast::http::string_body> request;
        auto result =
            co_await (boost::beast::http::async_read(stream, buffer, request,
                                                     use_nothrow_awaitable) ||
                      timeout(std::chrono::seconds(cfg.interval)));
        if (result.index() == 1) {
            SPDLOG_INFO("read timeout");
            co_return;
        }
        auto [ec, bytes_transferred] = std::get<0>(result);
        if (ec) {
            SPDLOG_INFO("async_read: {}", ec.message());
            co_return;
        }
        bool keep_alive = request.keep_alive();
        auto http_path = request.target();
        if (http_path.back() == '/')
            http_path.remove_suffix(1);
        if (http_path == cfg.chat_path) {
            auto html = create_index_html(
                fmt::format("{}/html/index.html", cfg.client_root_path));
            boost::beast::http::response<boost::beast::http::string_body> res{
                boost::beast::http::status::ok, request.version()};
            res.set(boost::beast::http::field::server,
                    BOOST_BEAST_VERSION_STRING);
            res.set(boost::beast::http::field::content_type, "text/html");
            res.keep_alive(request.keep_alive());
            res.body() = std::move(html);
            res.prepare_payload();
            boost::beast::http::message_generator rsp = std::move(res);
            co_await boost::beast::async_write(stream, std::move(rsp),
                                               use_nothrow_awaitable);
        } else if (request.target().starts_with(ASSETS_PATH)) {
            std::string req_path{request.target()};
            req_path.erase(req_path.find(ASSETS_PATH), ASSETS_PATH.length());
            auto file = fmt::format("{}{}", cfg.client_root_path, req_path);
            SPDLOG_INFO("load: {}", file);
            boost::beast::error_code ec;
            boost::beast::http::file_body::value_type body;
            body.open(file.c_str(), boost::beast::file_mode::scan, ec);
            if (ec == boost::beast::errc::no_such_file_or_directory) {
                co_await send_http_response(
                    stream, request, boost::beast::http::status::not_found);
                co_return;
            }
            auto const size = body.size();
            boost::beast::http::response<boost::beast::http::file_body> res{
                std::piecewise_construct, std::make_tuple(std::move(body)),
                std::make_tuple(boost::beast::http::status::ok,
                                request.version())};
            res.set(boost::beast::http::field::server,
                    BOOST_BEAST_VERSION_STRING);
            res.set(boost::beast::http::field::content_type,
                    req_path.contains("css") ? "text/css" : "text/javascript");
            res.content_length(size);
            res.keep_alive(request.keep_alive());
            boost::beast::http::message_generator rsp = std::move(res);
            co_await boost::beast::async_write(stream, std::move(rsp),
                                               use_nothrow_awaitable);
        } else if (request.target() == API_PATH) {
            std::string model;
            nlohmann::json request_body;
            bool flag = false;
            try {
                request_body = nlohmann::json::parse(request.body());
                model = request_body.at("model");
                auto prompt =
                    request_body.at("meta").at("content").at("parts").at(0).at(
                        "content");
            } catch (const nlohmann::json::exception& e) {
                SPDLOG_ERROR("nlohmann json: {}", e.what());
                flag = true;
            }
            if (flag) {
                co_await send_http_response(
                    stream, request, boost::beast::http::status::bad_request);
                co_return;
            }

            boost::beast::http::response<boost::beast::http::buffer_body> res;
            res.result(boost::beast::http::status::ok);
            res.version(request.version());
            res.set(boost::beast::http::field::server, "CppFreeGpt");
            res.set(boost::beast::http::field::transfer_encoding, "chunked");
            res.set(boost::beast::http::field::content_type,
                    "text/event-stream");
            res.body().data = nullptr;
            res.body().more = true;

            boost::beast::http::response_serializer<
                boost::beast::http::buffer_body, boost::beast::http::fields>
                sr{res};
            auto [ec, count] = co_await boost::beast::http::async_write_header(
                stream, sr, use_nothrow_awaitable);
            if (ec) {
                SPDLOG_ERROR("{}", ec.message());
                co_return;
            }
            if (!gpt_function.contains(model)) {
                SPDLOG_ERROR("Invalid request model: {}", model);
                co_await send_http_response(
                    stream, request, boost::beast::http::status::bad_request);
                co_return;
            }
            auto ch = std::make_shared<FreeGpt::Channel>(
                co_await boost::asio::this_coro::executor, 4096);
            auto& [pool, _] = gpt_function[model];
            pool->get_auto_waste(
                context,
                [=, request_body = std::move(request_body), &context](
                    const boost::system::error_code& ec,
                    FreeGpt::HttpClientPool::handle handle) mutable {
                    if (ec) {
                        SPDLOG_ERROR("get_auto_waste: {}", ec.message());
                        ch->close();
                        return;
                    }
                    boost::asio::co_spawn(
                        context,
                        [](auto ch, auto model, auto request_body,
                           auto handle) -> boost::asio::awaitable<void> {
                            auto& [_, func] = gpt_function[model];
                            co_await func(ch, std::move(request_body),
                                          std::move(handle));
                            co_return;
                        }(ch, std::move(model), std::move(request_body),
                                        std::move(handle)),
                        boost::asio::detached);
                },
                yamail::resource_pool::time_traits::duration::max());
            while (true) {
                auto [ec, str] =
                    co_await ch->async_receive(use_nothrow_awaitable);
                if (ec) {
                    break;
                }
                res.body().data = str.data();
                res.body().size = str.size();
                res.body().more = true;
                std::tie(ec, count) = co_await boost::beast::http::async_write(
                    stream, sr, use_nothrow_awaitable);
            }
            res.body().data = nullptr;
            res.body().more = false;
            std::tie(ec, count) = co_await boost::beast::http::async_write(
                stream, sr, use_nothrow_awaitable);
        } else {
            SPDLOG_ERROR("bad_request");
            co_await send_http_response(
                stream, request, boost::beast::http::status::bad_request);
            co_return;
        }
        if (!keep_alive)
            co_return;
    }
    co_return;
}

boost::asio::awaitable<void> do_session(
    boost::asio::ip::tcp::acceptor& acceptor, IoContextPool& pool,
    Config& cfg) {
    for (;;) {
        auto& context = pool.getIoContext();
        boost::asio::ip::tcp::socket socket(context);
        auto [ec] =
            co_await acceptor.async_accept(socket, use_nothrow_awaitable);
        if (ec) {
            if (ec == boost::asio::error::operation_aborted)
                break;
            SPDLOG_ERROR("Accept failed, error: {}", ec.message());
            continue;
        }
        boost::asio::co_spawn(context,
                              start_session(std::move(socket), cfg, context),
                              boost::asio::detached);
    }
    co_return;
}

int main(int argc, char** argv) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e][thread %t][%!][%s:%#][%l] %v");
    auto [config, error] = yaml_cpp_struct::from_yaml<Config>(argv[1]);
    if (!config) {
        SPDLOG_ERROR("{}", error);
        return EXIT_FAILURE;
    }
    auto& cfg = config.value();
    SPDLOG_INFO("cfg.max_http_client_num: {}", cfg.max_http_client_num);
    SPDLOG_INFO("cfg.work_thread_num: {}", cfg.work_thread_num);
    FreeGpt app{cfg};

    ADD_METHOD("gpt-3.5-turbo-aitianhu", FreeGpt::aitianhu);
    ADD_METHOD("gpt-3.5-turbo-aichat", FreeGpt::aichat);
    ADD_METHOD("gpt-3.5-turbo-stream-deepai", FreeGpt::deepai);
    ADD_METHOD("gpt-3.5-turbo-stream-getgpt", FreeGpt::getgpt);

    IoContextPool pool{cfg.work_thread_num};
    pool.start();
    auto& context = pool.getIoContext();
    boost::asio::ip::tcp::acceptor acceptor(context);

    boost::asio::ip::tcp::resolver resolver(context);
    boost::asio::ip::tcp::endpoint endpoint =
        *resolver.resolve(cfg.host, cfg.port).begin();
    std::stringstream ss;
    ss << endpoint;

    acceptor.open(endpoint.protocol());
    boost::system::error_code ec;

    acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    SPDLOG_INFO("server start accept at {} ...", ss.str());
    acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        SPDLOG_ERROR("{}", ec.message());
        return EXIT_FAILURE;
    }

    boost::asio::co_spawn(context, do_session(acceptor, pool, cfg),
                          boost::asio::detached);
    boost::asio::signal_set sigset(context, SIGINT, SIGTERM);
    std::binary_semaphore smph_signal_main_to_thread{0};
    sigset.async_wait([&](const boost::system::error_code&, int) {
        acceptor.close();
        smph_signal_main_to_thread.release();
    });
    smph_signal_main_to_thread.acquire();
    pool.stop();
    return EXIT_SUCCESS;
}
