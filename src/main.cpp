#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <unistd.h>

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "grparse/document_parser_service.h"
#include "grparse/page_scheduler.h"

namespace {

volatile sig_atomic_t shutdown_signal_fd = -1;

void request_shutdown(int signal_number) {
  if (shutdown_signal_fd < 0) return;
  const unsigned char signal_byte = static_cast<unsigned char>(signal_number);
  const ssize_t ignored = write(static_cast<int>(shutdown_signal_fd), &signal_byte,
                                sizeof(signal_byte));
  (void)ignored;
}

size_t configured_size(const char* name, size_t fallback, size_t maximum = 1024) {
  const char* configured = std::getenv(name);
  if (configured != nullptr) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(configured, &end, 10);
    if (end != configured && *end == '\0' && parsed > 0 && parsed <= maximum) {
      return static_cast<size_t>(parsed);
    }
    throw std::invalid_argument(std::string(name) + " must be an integer between 1 and " +
                                std::to_string(maximum));
  }
  return fallback;
}

int configured_index(const char* name, int fallback, int maximum = 63) {
  const char* configured = std::getenv(name);
  if (configured == nullptr) return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(configured, &end, 10);
  if (end == configured || *end != '\0' || parsed < 0 || parsed > maximum) {
    throw std::invalid_argument(std::string(name) + " must be an integer between 0 and " +
                                std::to_string(maximum));
  }
  return static_cast<int>(parsed);
}

size_t page_worker_count() {
  const unsigned int hardware = std::thread::hardware_concurrency();
  return std::min<size_t>(2, hardware == 0 ? 1 : hardware);
}

void install_shutdown_signal_pipe(int* signal_pipe) {
  if (pipe2(signal_pipe, O_CLOEXEC) != 0) {
    throw std::runtime_error("Could not create graceful shutdown signal pipe");
  }
  shutdown_signal_fd = signal_pipe[1];
  struct sigaction action {};
  action.sa_handler = request_shutdown;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    shutdown_signal_fd = -1;
    throw std::runtime_error("Could not install graceful shutdown signal handlers");
  }
}

}  // namespace

int main() {
  const char* models = std::getenv("GRPARSE_MODELS_DIR");
  const char* address = std::getenv("GRPARSE_LISTEN_ADDRESS");
  const std::string listen_address = address == nullptr ? "0.0.0.0:50051" : address;
  try {
    int signal_pipe[2];
    install_shutdown_signal_pipe(signal_pipe);
    const size_t inference_workers = configured_size("GRPARSE_PAGE_WORKERS", page_worker_count(), 64);
    const unsigned int hardware = std::thread::hardware_concurrency();
    const size_t render_workers = configured_size(
        "GRPARSE_RENDER_WORKERS", std::min<size_t>(4, hardware == 0 ? 2 : hardware), 256);
    const int gpu_index = configured_index("GRPARSE_CUDA_DEVICE", 0);
    grparse::OcrEnginePool engines(models == nullptr ? "/models" : models, inference_workers,
                                   gpu_index);
    grparse::PageScheduler scheduler(
        engines, grparse::PageScheduler::Options{
                     configured_size("GRPARSE_DOCUMENT_QUEUE", 8),
                     configured_size("GRPARSE_RENDER_QUEUE", 8),
                     configured_size("GRPARSE_INFERENCE_QUEUE", 4),
                     configured_size("GRPARSE_ASSEMBLY_QUEUE", 8), render_workers, inference_workers,
                     configured_size("GRPARSE_ASSEMBLY_WORKERS", 2, 64),
                     configured_size("GRPARSE_PAGE_WINDOW", 4, 64),
                     configured_size("GRPARSE_MAX_ACTIVE_DOCUMENTS", 32, 1024)});
    grparse::DocumentParserService service(scheduler);
    grparse::DocumentStreamingService streaming_service(scheduler);
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    builder.SetMaxReceiveMessageSize(70 * 1024 * 1024);
    grpc::ResourceQuota quota;
    quota.Resize(configured_size("GRPARSE_GRPC_MEMORY_MIB", 256, 16384) * 1024U * 1024U);
    quota.SetMaxThreads(static_cast<int>(configured_size("GRPARSE_GRPC_MAX_THREADS", 64, 1024)));
    builder.SetResourceQuota(quota);
    builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS,
                               static_cast<int>(configured_size("GRPARSE_MAX_CONCURRENT_STREAMS", 32, 1024)));
    builder.RegisterService(&service);
    builder.RegisterService(&streaming_service);
    const auto server = builder.BuildAndStart();
    if (!server) {
      std::cerr << "Unable to listen on " << listen_address << '\n';
      return 1;
    }
    std::cout << "gRParse listening on " << listen_address
              << " (RapidOCR / ONNX Runtime CUDA)" << std::endl;
    std::thread shutdown_thread([&] {
      unsigned char received_signal = 0;
      if (read(signal_pipe[0], &received_signal, sizeof(received_signal)) ==
          static_cast<ssize_t>(sizeof(received_signal))) {
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(10));
      }
    });
    server->Wait();
    shutdown_thread.join();
    shutdown_signal_fd = -1;
    close(signal_pipe[0]);
    close(signal_pipe[1]);
  } catch (const std::exception& error) {
    std::cerr << "Startup failed: " << error.what() << '\n';
    return 1;
  }
}
