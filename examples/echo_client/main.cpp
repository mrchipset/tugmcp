#include <tugmcp/client.hpp>
#include <tugmcp/transport/http.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: tugmcp_echo_client http://127.0.0.1:PORT\n";
    return 2;
  }

  mcp::v1::StreamableHttpClientOptions transport_options;
  transport_options.baseUrl = argv[1];
  transport_options.protocolVersion = "2026-07-28";
  auto transport =
      mcp::v1::makeStreamableHttpClientTransport(transport_options);
  if (!transport.hasValue()) {
    std::cerr << transport.error().message << "\n";
    return 1;
  }

  mcp::v1::ClientOptions client_options;
  client_options.protocolVersion = "2026-07-28";
  mcp::v1::McpClient client(client_options, transport.value());

  const mcp::v1::Status connected = client.connect();
  if (!connected.ok) {
    std::cerr << connected.error.message << "\n";
    client.close();
    return 1;
  }

  auto listed = client.listTools();
  if (!listed.hasValue()) {
    std::cerr << listed.error().message << "\n";
    client.close();
    return 1;
  }

  auto called = client.callTool("echo", mcp::v1::Json{{"text", "hello"}});
  if (!called.hasValue()) {
    std::cerr << called.error().message << "\n";
    client.close();
    return 1;
  }

  const auto& content = called.value().at("content");
  if (!content.is_array() || content.empty() ||
      !content.at(0).contains("text")) {
    std::cerr << "echo response missing text content\n";
    client.close();
    return 1;
  }

  std::cout << content.at(0).at("text").get<std::string>() << "\n";
  client.close();
  return 0;
}
