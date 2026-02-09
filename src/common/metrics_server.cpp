#include "common/metrics_server.h"
#include "common/logging.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace anycache {

MetricsHttpServer::MetricsHttpServer(uint16_t port) : port_(port) {}

MetricsHttpServer::~MetricsHttpServer() { Stop(); }

void MetricsHttpServer::Start() {
  if (running_)
    return;

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    LOG_WARN("MetricsHttpServer: failed to create socket");
    return;
  }

  int opt = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (::bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0) {
    LOG_WARN("MetricsHttpServer: bind failed on port {}", port_);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return;
  }

  if (::listen(listen_fd_, 8) < 0) {
    LOG_WARN("MetricsHttpServer: listen failed");
    ::close(listen_fd_);
    listen_fd_ = -1;
    return;
  }

  running_ = true;
  thread_ = std::thread(&MetricsHttpServer::ServeLoop, this);
  LOG_INFO("MetricsHttpServer listening on :{}/metrics", port_);
}

void MetricsHttpServer::Stop() {
  if (!running_)
    return;
  running_ = false;

  // Interrupt accept() by closing the listen socket
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }

  if (thread_.joinable()) {
    thread_.join();
  }
}

void MetricsHttpServer::ServeLoop() {
  while (running_) {
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd =
        ::accept(listen_fd_, reinterpret_cast<struct sockaddr *>(&client_addr),
                 &client_len);
    if (client_fd < 0) {
      if (!running_)
        break; // Expected during shutdown
      continue;
    }

    // Read the HTTP request (we only need the first line)
    char req_buf[1024];
    ssize_t n = ::recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
    if (n <= 0) {
      ::close(client_fd);
      continue;
    }
    req_buf[n] = '\0';

    // Generate response
    std::string body = Metrics::Instance().ExportText();
    std::string response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain; version=0.0.4\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\n"
                           "\r\n" +
                           body;

    ::send(client_fd, response.data(), response.size(), 0);
    ::close(client_fd);
  }
}

} // namespace anycache
