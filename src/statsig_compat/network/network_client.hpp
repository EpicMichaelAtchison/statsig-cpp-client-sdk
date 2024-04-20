#pragma once

#include <functional>
#include <optional>
#include <utility>

#include "httplib.h"
#include "statsig/internal/constants.h"

namespace statsig::internal {

struct HttpResponse {
  const std::string text;
  const int status = -1;
};

struct HttpRequest {
  const std::string api;
  const std::string &path;
  const std::unordered_map<std::string, std::string> &headers;
  const std::string &body;
};

#ifndef STATSIG_TESTS

class NetworkClient {
 public:
  static void Post(
      const HttpRequest &request,
      const std::function<void(HttpResponse)> &callback
  ) {
    httplib::Client client(request.api);
    client.set_compress(request.path == constants::kEndpointLogEvent);

    httplib::Headers compat_headers;
    for (const auto &kv : request.headers) {
      compat_headers.insert(kv);
    }

    httplib::Result result = client.Post(
        request.path,
        compat_headers,
        request.body,
        constants::kContentTypeJson
    );

    callback(HttpResponse{result->body, result->status});
  }

};


#else

class NetworkClient {
 public:
  static void Post(
      const HttpRequest &request,
      const std::function<void(HttpResponse)> &callback
  ) {
    GetInstance().PostImpl(request, callback);
  }

  static NetworkClient &GetInstance() {
    static NetworkClient instance;
    return instance;
  }

  std::function<HttpResponse(HttpRequest)> _test_func_;

 private:
  void PostImpl(
      const HttpRequest &request,
      const std::function<void(HttpResponse)> &callback
  ) const {
    callback(_test_func_(request));
  }
};

#endif

}



