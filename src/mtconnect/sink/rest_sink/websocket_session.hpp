//
// Copyright Copyright 2009-2022, AMT – The Association For Manufacturing Technology (“AMT”)
// All rights reserved.
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
//

#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include "mtconnect/config.hpp"
#include "mtconnect/configuration/config_options.hpp"
#include "mtconnect/utilities.hpp"
#include "session.hpp"

namespace mtconnect::sink::rest_sink {
  namespace beast = boost::beast;

  struct WebsocketRequest
  {
    std::string m_requestId;
    std::optional<boost::asio::streambuf> m_streamBuffer;
    Complete m_complete;
    bool m_streaming {false};
  };

  /// @brief A websocket session that provides a pubsub interface using REST parameters
  template <class Derived>
  class WebsocketSession : public Session
  {
  public:
    using RequestMessage = boost::beast::http::request<boost::beast::http::string_body>;

    WebsocketSession(RequestPtr &&request, RequestMessage &&msg, Dispatch dispatch,
                     ErrorFunction func)
      : Session(dispatch, func), m_request(std::move(request)), m_msg(std::move(msg))
    {}

    /// @brief Session cannot be copied.
    WebsocketSession(const WebsocketSession &) = delete;
    ~WebsocketSession() = default;

    /// @brief get this as the `Derived` type
    /// @return the subclass
    Derived &derived() { return static_cast<Derived &>(*this); }

    void run() override
    {
      using namespace boost::beast;

      // Set suggested timeout settings for the websocket
      derived().stream().set_option(
          websocket::stream_base::timeout::suggested(beast::role_type::server));

      // Set a decorator to change the Server of the handshake
      derived().stream().set_option(
          websocket::stream_base::decorator([](websocket::response_type &res) {
            res.set(http::field::server, GetAgentVersion() + " MTConnectAgent");
          }));

      // Accept the websocket handshake
      derived().stream().async_accept(
          m_msg, beast::bind_front_handler(&WebsocketSession::onAccept, derived().shared_ptr()));
    }

    void writeResponse(ResponsePtr &&response, Complete complete = nullptr) override
    {
      if (response->m_requestId)
      {
        auto id = *(response->m_requestId);
        auto it = m_requests.find(id);
        if (it != m_requests.end())
        {
          using namespace std::placeholders;

          auto &req = it->second;
          req.m_complete = complete;
          req.m_streamBuffer.emplace();
          std::ostream str(&req.m_streamBuffer.value());

          str << response->m_body;

          derived().stream().text(derived().stream().got_text());
          derived().stream().async_write(
              req.m_streamBuffer->data(),
              beast::bind_handler(
                  [this, id](beast::error_code ec, std::size_t len) { sent(ec, len, id); }, _1,
                  _2));
        }
        else
        {
          LOG(error) << "WebsocketSession::writeResponse: Cannot find request for id: " << id;
        }
      }
      else
      {
        LOG(error) << "WebsocketSession::writeResponse: No request id for websocket";
      }
    }

    void writeFailureResponse(ResponsePtr &&response, Complete complete = nullptr) override {}

    void beginStreaming(const std::string &mimeType, Complete complete) override {}

    void writeChunk(const std::string &chunk, Complete complete) override {}

    void closeStream() override {}

  protected:
    void onAccept(boost::beast::error_code ec)
    {
      if (ec)
      {
        fail(status::internal_server_error, "Error occurred in accpet", ec);
        return;
      }

      derived().stream().async_read(
          m_buffer, beast::bind_front_handler(&WebsocketSession::onRead, derived().shared_ptr()));
    }

    void sent(beast::error_code ec, std::size_t len, const std::string &id)
    {
      auto it = m_requests.find(id);
      if (it != m_requests.end())
      {
        auto &req = it->second;
        if (req.m_complete)
        {
          req.m_complete();
        }

        if (!req.m_streaming)
        {
          m_requests.erase(id);
        }
      }
      else
      {
        LOG(error) << "WebsocketSession::sent: Cannot find request for id: " << id;
      }
    }

    void onRead(beast::error_code ec, std::size_t len)
    {
      if (ec)
        return fail(boost::beast::http::status::internal_server_error, "shutdown", ec);

      using namespace rapidjson;
      using namespace std;

      // Parse the buffer as a JSON request with parameters matching
      // REST API
      derived().stream().text(derived().stream().got_text());
      auto buffer = beast::buffers_to_string(m_buffer.data());
      m_buffer.consume(m_buffer.size());

      Document doc;
      doc.Parse(buffer.c_str(), len);

      if (doc.HasParseError())
      {
        LOG(warning) << "Websocket Read Error(offset (" << doc.GetErrorOffset()
                     << "): " << GetParseError_En(doc.GetParseError());
        LOG(warning) << "  " << buffer;
      }
      if (!doc.IsObject())
      {
        LOG(warning) << "Websocket Read Error: JSON message does not have a top level object";
        LOG(warning) << "  " << buffer;
      }
      else
      {
        // Extract the parameters from the json doc to map them to the REST
        // protocol parameters
        m_request->m_verb = beast::http::verb::get;
        const auto &object = doc.GetObject();

        for (auto &it : object)
        {
          switch (it.value.GetType())
          {
            case rapidjson::kNullType:
              // Skip nulls
              break;
            case rapidjson::kFalseType:
              m_request->m_parameters.emplace(make_pair(it.name.GetString(), false));
              break;
            case rapidjson::kTrueType:
              m_request->m_parameters.emplace(make_pair(it.name.GetString(), true));
              break;
            case rapidjson::kObjectType:
              break;
            case rapidjson::kArrayType:
              break;
            case rapidjson::kStringType:
              m_request->m_parameters.emplace(
                  make_pair(it.name.GetString(), string(it.value.GetString())));

              break;
            case rapidjson::kNumberType:
              if (it.value.Is<int>())
                m_request->m_parameters.emplace(
                    make_pair(it.name.GetString(), it.value.Get<int>()));
              else if (it.value.Is<unsigned>())
                m_request->m_parameters.emplace(
                    make_pair(it.name.GetString(), it.value.Get<unsigned>()));
              else if (it.value.Is<int64_t>())
                m_request->m_parameters.emplace(
                    make_pair(it.name.GetString(), (uint64_t)it.value.Get<int64_t>()));
              else if (it.value.Is<uint64_t>())
                m_request->m_parameters.emplace(
                    make_pair(it.name.GetString(), it.value.Get<uint64_t>()));
              else if (it.value.Is<double>())
                m_request->m_parameters.emplace(
                    make_pair(it.name.GetString(), it.value.Get<double>()));

              break;
          }
        }

        if (m_request->m_parameters.count("request") > 0)
        {
          m_request->m_command = get<string>(m_request->m_parameters["request"]);
          m_request->m_parameters.erase("request");
        }
        if (m_request->m_parameters.count("id") > 0)
        {
          auto &v = m_request->m_parameters["id"];
          string id = visit(overloaded {[](monostate m) { return ""s; },
                                        [](auto v) { return boost::lexical_cast<string>(v); }},
                            v);
          m_request->m_requestId = id;
          m_request->m_parameters.erase("id");
        }

        auto &req = m_requests[*(m_request->m_requestId)];
        req.m_requestId = *(m_request->m_requestId);

        if (!m_dispatch(derived().shared_ptr(), m_request))
        {
          ostringstream txt;
          txt << "Failed to find handler for " << buffer;
          LOG(error) << txt.str();
        }
      }

      derived().stream().async_read(
          m_buffer, beast::bind_front_handler(&WebsocketSession::onRead, derived().shared_ptr()));
    }

  protected:
    RequestPtr m_request;
    RequestMessage m_msg;
    beast::flat_buffer m_buffer;
    std::map<std::string, WebsocketRequest> m_requests;
  };

  template <class Derived>
  using WebsocketSessionPtr = std::shared_ptr<WebsocketSession<Derived>>;

  class PlainWebsocketSession : public WebsocketSession<PlainWebsocketSession>
  {
  public:
    using Stream = beast::websocket::stream<beast::tcp_stream>;

    PlainWebsocketSession(beast::tcp_stream &&stream, RequestPtr &&request, RequestMessage &&msg,
                          Dispatch dispatch, ErrorFunction func)
      : WebsocketSession(std::move(request), std::move(msg), dispatch, func),
        m_stream(std::move(stream))
    {}
    ~PlainWebsocketSession() { close(); }

    void close() override
    {
      NAMED_SCOPE("PlainWebsocketSession::close");

      m_request.reset();
      if (m_stream.is_open())
        m_stream.close(beast::websocket::close_code::none);
    }

    auto &stream() { return m_stream; }

    /// @brief Get a pointer cast as an Websocket Session
    /// @return shared pointer to an Websocket session
    std::shared_ptr<PlainWebsocketSession> shared_ptr()
    {
      return std::dynamic_pointer_cast<PlainWebsocketSession>(shared_from_this());
    }

  protected:
    Stream m_stream;
  };

  class TlsWebsocketSession : public WebsocketSession<TlsWebsocketSession>
  {
  public:
    using Stream = beast::websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    TlsWebsocketSession(beast::ssl_stream<beast::tcp_stream> &&stream, RequestPtr &&request,
                        RequestMessage &&msg, Dispatch dispatch, ErrorFunction func)
      : WebsocketSession(std::move(request), std::move(msg), dispatch, func),
        m_stream(std::move(stream))
    {}
    ~TlsWebsocketSession() { close(); }

    auto &stream() { return m_stream; }

    void close() override
    {
      NAMED_SCOPE("TlsWebsocketSession::close");

      m_request.reset();
      if (m_stream.is_open())
        m_stream.close(beast::websocket::close_code::none);
    }

    /// @brief Get a pointer cast as an TLS Websocket Session
    /// @return shared pointer to an TLS Websocket session
    std::shared_ptr<TlsWebsocketSession> shared_ptr()
    {
      return std::dynamic_pointer_cast<TlsWebsocketSession>(shared_from_this());
    }

  protected:
    Stream m_stream;
  };

}  // namespace mtconnect::sink::rest_sink
