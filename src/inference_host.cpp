// Native OpenVINO in a supervised child process (the openvino flavor).
//
// The GPU plugin and compute runtime can take a process down on their own:
// after a device loss on the Arc Pro B70 their objects throw from
// destructors and background threads (std::terminate, exit 139, gRParse#352),
// and a wedged engine can block a call forever.  So the server keeps no
// OpenVINO state at all: `grparse-server --grparse-inference-host <fd>` owns
// the core, the compiled shapes and the GPU context, and the server's models
// are proxies that send each call over a SOCK_SEQPACKET socketpair, the
// tensors in a memfd per call.  When the host dies, or a call outlives
// GRPARSE_OPENVINO_CALL_TIMEOUT_S (default 120; the host is then killed),
// every call in flight fails with InferenceDeviceError (gRPC UNAVAILABLE),
// the host is respawned, and each model reopens and re-prewarms on its next
// call (from the kernel cache).  Counted in openvino_host_restart_count.

#include "grparse_inference_model.h"

#ifdef GRPARSE_NATIVE_OPENVINO

#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <print>
#include <thread>

extern char** environ;

namespace grparse {

std::unique_ptr<InferenceModel> make_native_openvino_model(const std::filesystem::path& ir,
                                                           std::string what,
                                                           ModelShapePolicy shapes,
                                                           OrtPrecision precision);

namespace {

std::atomic<uint64_t> host_restarts{0};

// ---------------------------------------------------------------------------
// Wire format: one datagram per message, little-endian PODs and length-
// prefixed strings, plus at most one memfd (SCM_RIGHTS) carrying tensor
// bytes at the offsets the message names.
// ---------------------------------------------------------------------------

enum class Kind : uint32_t { kConfigure = 1, kOpen = 2, kPrewarm = 3, kRun = 4 };
enum class Outcome : uint32_t { kOk = 0, kError = 1, kDeviceError = 2, kInvalid = 3 };

constexpr size_t kMaxMessage = 1 << 20;

class Writer {
 public:
  template <typename T>
  void pod(const T& value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    data_.insert(data_.end(), bytes, bytes + sizeof(T));
  }
  void text(std::string_view value) {
    pod(static_cast<uint32_t>(value.size()));
    const auto* bytes = reinterpret_cast<const std::byte*>(value.data());
    data_.insert(data_.end(), bytes, bytes + value.size());
  }
  void spec(TensorElement element, const std::vector<int64_t>& shape) {
    pod(static_cast<uint32_t>(element));
    pod(static_cast<uint32_t>(shape.size()));
    for (const int64_t dim : shape) pod(dim);
  }
  const std::vector<std::byte>& bytes() const { return data_; }

 private:
  std::vector<std::byte> data_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> data) : data_(data) {}
  template <typename T>
  T pod() {
    if (position_ + sizeof(T) > data_.size()) throw std::runtime_error("truncated host message");
    T value;
    std::memcpy(&value, data_.data() + position_, sizeof(T));
    position_ += sizeof(T);
    return value;
  }
  std::string text() {
    const auto size = pod<uint32_t>();
    if (position_ + size > data_.size()) throw std::runtime_error("truncated host message");
    std::string value(reinterpret_cast<const char*>(data_.data() + position_), size);
    position_ += size;
    return value;
  }
  TensorSpec spec() {
    TensorSpec spec;
    spec.element = static_cast<TensorElement>(pod<uint32_t>());
    const auto rank = pod<uint32_t>();
    if (rank > 8) throw std::runtime_error("host message tensor rank out of range");
    for (uint32_t i = 0; i < rank; ++i) spec.shape.push_back(pod<int64_t>());
    return spec;
  }

 private:
  std::span<const std::byte> data_;
  size_t position_ = 0;
};

size_t bytes_of(const TensorSpec& spec) {
  size_t count = 1;
  for (const int64_t dim : spec.shape) count *= static_cast<size_t>(dim);
  return count * tensor_element_size(spec.element);
}

bool send_message(int socket, const std::vector<std::byte>& payload, int fd) {
  iovec io{const_cast<std::byte*>(payload.data()), payload.size()};
  msghdr message{};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  if (fd >= 0) {
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &fd, sizeof(int));
  }
  return ::sendmsg(socket, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size());
}

// Returns false on EOF or error; *fd is -1 when the message carried none.
bool receive_message(int socket, std::vector<std::byte>* payload, int* fd) {
  payload->resize(kMaxMessage);
  iovec io{payload->data(), payload->size()};
  msghdr message{};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  const ssize_t got = ::recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
  if (got <= 0) return false;
  payload->resize(static_cast<size_t>(got));
  *fd = -1;
  for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
       header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
      std::memcpy(fd, CMSG_DATA(header), sizeof(int));
    }
  }
  return true;
}

// A memfd holding `size` bytes, mapped read-write.
struct SharedBytes {
  int fd = -1;
  std::byte* data = nullptr;
  size_t size = 0;

  static SharedBytes create(size_t size) {
    SharedBytes shared;
    shared.size = size;
    shared.fd = ::memfd_create("grparse-tensors", MFD_CLOEXEC);
    if (shared.fd < 0 || ::ftruncate(shared.fd, static_cast<off_t>(std::max<size_t>(size, 1))) != 0) {
      throw std::runtime_error("inference host: memfd for tensors failed");
    }
    shared.map();
    return shared;
  }
  static SharedBytes adopt(int fd) {
    SharedBytes shared;
    shared.fd = fd;
    const off_t end = ::lseek(fd, 0, SEEK_END);
    shared.size = end < 0 ? 0 : static_cast<size_t>(end);
    shared.map();
    return shared;
  }
  void map() {
    if (size == 0) return;
    void* address = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) throw std::runtime_error("inference host: mmap of tensors failed");
    data = static_cast<std::byte*>(address);
  }
  SharedBytes() = default;
  SharedBytes(SharedBytes&& other) noexcept { *this = std::move(other); }
  SharedBytes& operator=(SharedBytes&& other) noexcept {
    std::swap(fd, other.fd);
    std::swap(data, other.data);
    std::swap(size, other.size);
    return *this;
  }
  ~SharedBytes() {
    if (data != nullptr) ::munmap(data, size);
    if (fd >= 0) ::close(fd);
  }
};

// ---------------------------------------------------------------------------
// The host (child) side.
// ---------------------------------------------------------------------------

class Host {
 public:
  explicit Host(int socket) : socket_(socket) {}

  int serve() {
    std::vector<std::byte> payload;
    int fd = -1;
    while (receive_message(socket_, &payload, &fd)) {
      // The device selection applies before anything else is read; every
      // other call runs on its own thread, so the page workers' calls
      // overlap on the device as they do in process.
      if (Reader(payload).pod<uint32_t>() == static_cast<uint32_t>(Kind::kConfigure)) {
        handle(payload, fd);
        continue;
      }
      std::thread([this, payload, fd]() mutable { handle(payload, fd); }).detach();
    }
    // The server closed its end (shutdown): leave without running any
    // OpenVINO destructor.
    std::fflush(stdout);
    std::fflush(stderr);
    ::_exit(0);
  }

 private:
  void handle(const std::vector<std::byte>& payload, int fd) {
    SharedBytes input;
    if (fd >= 0) input = SharedBytes::adopt(fd);
    Reader reader(payload);
    const auto kind = static_cast<Kind>(reader.pod<uint32_t>());
    const auto call = reader.pod<uint64_t>();
    Writer reply;
    reply.pod(call);
    SharedBytes output;
    try {
      switch (kind) {
        case Kind::kConfigure: {
          OrtEpSelection selection;
          selection.ep = OrtEp::kOpenVino;
          selection.openvino_device = reader.text();
          selection.openvino_cache_dir = reader.text();
          set_ort_ep_selection(selection);
          reply.pod(Outcome::kOk);
          break;
        }
        case Kind::kOpen: {
          const std::filesystem::path ir = reader.text();
          std::string what = reader.text();
          ModelShapePolicy shapes;
          const auto rungs = reader.pod<uint32_t>();
          for (uint32_t i = 0; i < rungs; ++i) shapes.batch_ladder.push_back(reader.pod<int64_t>());
          const auto precision = static_cast<OrtPrecision>(reader.pod<uint32_t>());
          auto model = make_native_openvino_model(ir, what, shapes, precision);
          reply.pod(Outcome::kOk);
          std::lock_guard<std::mutex> lock(mutex_);
          const auto id = static_cast<uint32_t>(models_.size());
          reply.pod(id);
          reply.pod(static_cast<uint32_t>(model->input_names().size()));
          for (const auto& name : model->input_names()) reply.text(name);
          reply.pod(static_cast<uint32_t>(model->output_names().size()));
          for (const auto& name : model->output_names()) reply.text(name);
          models_.push_back(std::move(model));
          break;
        }
        case Kind::kPrewarm: {
          InferenceModel& model = model_at(reader.pod<uint32_t>());
          std::vector<std::vector<TensorSpec>> sets(reader.pod<uint32_t>());
          for (auto& set : sets) {
            set.resize(reader.pod<uint32_t>());
            for (auto& spec : set) spec = reader.spec();
          }
          model.prewarm(sets);
          reply.pod(Outcome::kOk);
          break;
        }
        case Kind::kRun: {
          InferenceModel& model = model_at(reader.pod<uint32_t>());
          std::vector<TensorView> views(reader.pod<uint32_t>());
          for (auto& view : views) {
            const TensorSpec spec = reader.spec();
            const auto offset = reader.pod<uint64_t>();
            if (offset + bytes_of(spec) > input.size) throw std::runtime_error("tensor outside memfd");
            view = {spec.element, input.data + offset, spec.shape};
          }
          const std::vector<Tensor> outputs = model.run(views);
          size_t total = 0;
          for (const Tensor& tensor : outputs) total += tensor.bytes.size();
          output = SharedBytes::create(total);
          reply.pod(Outcome::kOk);
          reply.pod(static_cast<uint32_t>(outputs.size()));
          size_t offset = 0;
          for (const Tensor& tensor : outputs) {
            reply.spec(tensor.element, tensor.shape);
            reply.pod(static_cast<uint64_t>(offset));
            std::memcpy(output.data + offset, tensor.bytes.data(), tensor.bytes.size());
            offset += tensor.bytes.size();
          }
          break;
        }
        default:
          throw std::invalid_argument("unknown inference host message");
      }
    } catch (const InferenceDeviceError& error) {
      reply = Writer();
      reply.pod(call);
      reply.pod(Outcome::kDeviceError);
      reply.text(error.what());
    } catch (const std::invalid_argument& error) {
      reply = Writer();
      reply.pod(call);
      reply.pod(Outcome::kInvalid);
      reply.text(error.what());
    } catch (const std::exception& error) {
      reply = Writer();
      reply.pod(call);
      reply.pod(Outcome::kError);
      reply.text(error.what());
    }
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_message(socket_, reply.bytes(), output.fd);
  }

  InferenceModel& model_at(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= models_.size()) throw std::invalid_argument("unknown model id");
    return *models_[id];
  }

  int socket_;
  std::mutex mutex_;
  std::mutex send_mutex_;
  std::vector<std::unique_ptr<InferenceModel>> models_;
};

// ---------------------------------------------------------------------------
// The server (parent) side.
// ---------------------------------------------------------------------------

struct Reply {
  Outcome outcome = Outcome::kOk;
  std::string message;
  std::vector<std::byte> payload;  // after call id and outcome
  int fd = -1;
};

int call_timeout_seconds() {
  static const int seconds = [] {
    const char* value = std::getenv("GRPARSE_OPENVINO_CALL_TIMEOUT_S");
    if (value == nullptr || *value == '\0') return 120;
    const int parsed = std::atoi(value);
    if (parsed < 1 || parsed > 3600) {
      throw std::invalid_argument("GRPARSE_OPENVINO_CALL_TIMEOUT_S must be 1..3600");
    }
    return parsed;
  }();
  return seconds;
}

class HostClient {
 public:
  static HostClient& instance() {
    static HostClient* client = new HostClient();  // lives to process exit
    return *client;
  }

  // The host generation a model was opened in; a call made after a
  // restart reopens the model first.
  uint64_t generation() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_running_locked();
    return generation_;
  }

  Reply call(Kind kind, const Writer& body, int fd) {
    std::shared_ptr<std::promise<Reply>> promise = std::make_shared<std::promise<Reply>>();
    std::future<Reply> future = promise->get_future();
    uint64_t id = 0;
    uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ensure_running_locked();
      id = next_call_++;
      generation = generation_;
      pending_[id] = promise;
      Writer message;
      message.pod(static_cast<uint32_t>(kind));
      message.pod(id);
      std::vector<std::byte> bytes = message.bytes();
      bytes.insert(bytes.end(), body.bytes().begin(), body.bytes().end());
      if (!send_message(socket_, bytes, fd)) {
        pending_.erase(id);
        throw InferenceDeviceError("OpenVINO inference host is gone (send failed)");
      }
    }
    if (future.wait_for(std::chrono::seconds(call_timeout_seconds())) != std::future_status::ready) {
      // A wedged device: kill the host; the reader fails every pending call.
      std::println(stderr, "gRParse OpenVINO inference host: a call outlived {} s; killing host {}",
                   call_timeout_seconds(), pid_of(generation));
      kill_generation(generation);
    }
    return future.get();
  }

 private:
  HostClient() = default;

  pid_t pid_of(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation == generation_ ? pid_ : -1;
  }

  void kill_generation(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == generation_ && pid_ > 0) ::kill(pid_, SIGKILL);
  }

  void ensure_running_locked() {
    if (pid_ > 0) return;
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
      throw InferenceDeviceError("OpenVINO inference host: socketpair failed");
    }
    // The child's end survives exec; ours does not.
    ::fcntl(sockets[1], F_SETFD, 0);
    const std::string fd_text = std::to_string(sockets[1]);
    std::string self = std::filesystem::read_symlink("/proc/self/exe").string();
    char* argv[] = {self.data(), const_cast<char*>("--grparse-inference-host"),
                    const_cast<char*>(fd_text.c_str()), nullptr};
    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, self.c_str(), nullptr, nullptr, argv, environ);
    ::close(sockets[1]);
    if (spawned != 0) {
      ::close(sockets[0]);
      throw InferenceDeviceError("OpenVINO inference host: spawn failed");
    }
    pid_ = pid;
    socket_ = sockets[0];
    ++generation_;
    if (generation_ > 1) host_restarts.fetch_add(1);
    std::println("gRParse OpenVINO inference host: pid {} (generation {})", pid_, generation_);
    std::thread(&HostClient::read_replies, this, socket_, pid_, generation_).detach();
    // The host takes the server's device selection first.
    const OrtEpSelection selection = ort_ep_selection();
    Writer configure;
    configure.pod(static_cast<uint32_t>(Kind::kConfigure));
    configure.pod(uint64_t{0});
    configure.text(selection.openvino_device);
    configure.text(selection.openvino_cache_dir);
    send_message(socket_, configure.bytes(), -1);
  }

  void read_replies(int socket, pid_t pid, uint64_t generation) {
    std::vector<std::byte> payload;
    int fd = -1;
    while (receive_message(socket, &payload, &fd)) {
      Reader reader(payload);
      const auto id = reader.pod<uint64_t>();
      Reply reply;
      reply.outcome = reader.pod<Outcome>();
      const size_t header = sizeof(uint64_t) + sizeof(Outcome);
      reply.payload.assign(payload.begin() + static_cast<ptrdiff_t>(header), payload.end());
      reply.fd = fd;
      if (reply.outcome != Outcome::kOk) {
        Reader message(reply.payload);
        reply.message = message.text();
      }
      std::shared_ptr<std::promise<Reply>> promise;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = pending_.find(id);
        if (found == pending_.end()) {
          if (fd >= 0) ::close(fd);
          continue;  // the configure reply, or a call that already failed
        }
        promise = found->second;
        pending_.erase(found);
      }
      promise->set_value(std::move(reply));
    }
    // The host is gone: reap it, fail everything in flight, respawn on the
    // next call.
    int status = 0;
    ::waitpid(pid, &status, 0);
    const std::string how = WIFSIGNALED(status) ? "signal " + std::to_string(WTERMSIG(status))
                                                : "exit " + std::to_string(WEXITSTATUS(status));
    std::map<uint64_t, std::shared_ptr<std::promise<Reply>>> failed;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation == generation_) {
        failed.swap(pending_);
        ::close(socket_);
        socket_ = -1;
        pid_ = -1;
      }
    }
    std::println(stderr, "gRParse OpenVINO inference host {} died ({}); {} call(s) failed; it "
                 "restarts on the next call", pid, how, failed.size());
    for (auto& [id, promise] : failed) {
      Reply reply;
      reply.outcome = Outcome::kDeviceError;
      reply.message = "OpenVINO inference host died (" + how + ")";
      promise->set_value(std::move(reply));
    }
  }

  std::mutex mutex_;
  pid_t pid_ = -1;
  int socket_ = -1;
  uint64_t generation_ = 0;
  uint64_t next_call_ = 1;
  std::map<uint64_t, std::shared_ptr<std::promise<Reply>>> pending_;
};

[[noreturn]] void throw_reply(const Reply& reply, std::string_view what) {
  const std::string message = "gRParse " + std::string(what) + ": " + reply.message;
  switch (reply.outcome) {
    case Outcome::kDeviceError: throw InferenceDeviceError(message);
    case Outcome::kInvalid: throw std::invalid_argument(message);
    default: throw std::runtime_error(message);
  }
}

class RemoteModel final : public InferenceModel {
 public:
  RemoteModel(std::filesystem::path ir, std::string what, ModelShapePolicy shapes,
              OrtPrecision precision)
      : ir_(std::move(ir)), what_(std::move(what)), shapes_(std::move(shapes)),
        precision_(precision) {
    std::lock_guard<std::mutex> lock(open_mutex_);
    open();  // boot fails here, naming the model, if the host cannot load it
  }

  std::vector<Tensor> run(std::span<const TensorView> inputs) override {
    const uint32_t id = current_id();
    size_t total = 0;
    for (const TensorView& view : inputs) total += bytes_of({view.element, view.shape});
    SharedBytes shared = SharedBytes::create(total);
    Writer body;
    body.pod(id);
    body.pod(static_cast<uint32_t>(inputs.size()));
    size_t offset = 0;
    for (const TensorView& view : inputs) {
      const size_t bytes = bytes_of({view.element, view.shape});
      body.spec(view.element, view.shape);
      body.pod(static_cast<uint64_t>(offset));
      std::memcpy(shared.data + offset, view.data, bytes);
      offset += bytes;
    }
    Reply reply = HostClient::instance().call(Kind::kRun, body, shared.fd);
    if (reply.outcome != Outcome::kOk) throw_reply(reply, what_);
    SharedBytes output = reply.fd >= 0 ? SharedBytes::adopt(reply.fd) : SharedBytes();
    Reader reader(reply.payload);
    std::vector<Tensor> tensors(reader.pod<uint32_t>());
    for (Tensor& tensor : tensors) {
      const TensorSpec spec = reader.spec();
      const auto at = reader.pod<uint64_t>();
      const size_t bytes = bytes_of(spec);
      if (at + bytes > output.size) throw std::runtime_error("host output outside memfd");
      tensor.element = spec.element;
      tensor.shape = spec.shape;
      tensor.bytes.assign(output.data + at, output.data + at + bytes);
    }
    return tensors;
  }

  void prewarm(std::span<const std::vector<TensorSpec>> input_sets) override {
    std::lock_guard<std::mutex> lock(open_mutex_);
    warm_sets_.assign(input_sets.begin(), input_sets.end());
    send_prewarm();
  }

  const std::vector<std::string>& input_names() const override { return input_names_; }
  const std::vector<std::string>& output_names() const override { return output_names_; }
  std::string_view backend_name() const override { return "openvino"; }

 private:
  // Called with open_mutex_ held.  The generation is published last, so a
  // call that sees it current also sees this generation's model id.
  void open() {
    const uint64_t generation = HostClient::instance().generation();
    Writer body;
    body.text(ir_.string());
    body.text(what_);
    body.pod(static_cast<uint32_t>(shapes_.batch_ladder.size()));
    for (const int64_t rung : shapes_.batch_ladder) body.pod(rung);
    body.pod(static_cast<uint32_t>(precision_));
    Reply reply = HostClient::instance().call(Kind::kOpen, body, -1);
    if (reply.outcome != Outcome::kOk) throw_reply(reply, what_);
    Reader reader(reply.payload);
    id_.store(reader.pod<uint32_t>());
    // Names are read once (the first open): the IR does not change across
    // host generations, and other threads read them without the lock.
    std::vector<std::string> inputs(reader.pod<uint32_t>());
    for (auto& name : inputs) name = reader.text();
    std::vector<std::string> outputs(reader.pod<uint32_t>());
    for (auto& name : outputs) name = reader.text();
    if (generation_.load() == 0) {
      input_names_ = std::move(inputs);
      output_names_ = std::move(outputs);
    }
    generation_.store(generation);
  }

  // Called with open_mutex_ held.
  void send_prewarm() {
    if (warm_sets_.empty()) return;
    Writer body;
    body.pod(id_.load());
    body.pod(static_cast<uint32_t>(warm_sets_.size()));
    for (const auto& set : warm_sets_) {
      body.pod(static_cast<uint32_t>(set.size()));
      for (const TensorSpec& spec : set) body.spec(spec.element, spec.shape);
    }
    Reply reply = HostClient::instance().call(Kind::kPrewarm, body, -1);
    if (reply.outcome != Outcome::kOk) throw_reply(reply, what_);
  }

  // Returns the model id valid for the current host generation.
  uint32_t current_id() {
    if (HostClient::instance().generation() == generation_.load()) return id_.load();
    std::lock_guard<std::mutex> lock(open_mutex_);
    if (HostClient::instance().generation() != generation_.load()) {
      std::println(stderr, "gRParse {}: reopening on the restarted OpenVINO inference host", what_);
      open();
      send_prewarm();
    }
    return id_.load();
  }

  std::filesystem::path ir_;
  std::string what_;
  ModelShapePolicy shapes_;
  OrtPrecision precision_ = OrtPrecision::kProviderDefault;
  std::mutex open_mutex_;
  std::atomic<uint64_t> generation_{0};
  std::atomic<uint32_t> id_{0};
  std::vector<std::string> input_names_;
  std::vector<std::string> output_names_;
  std::vector<std::vector<TensorSpec>> warm_sets_;
};

}  // namespace

bool openvino_process_isolation() {
  static const bool isolated = [] {
    const char* value = std::getenv("GRPARSE_OPENVINO_ISOLATION");
    const std::string mode = value == nullptr || *value == '\0' ? "process" : value;
    if (mode == "process") return true;
    if (mode == "none") return false;
    throw std::invalid_argument("GRPARSE_OPENVINO_ISOLATION must be process or none");
  }();
  return isolated;
}

std::unique_ptr<InferenceModel> make_remote_openvino_model(const std::filesystem::path& ir,
                                                           std::string what,
                                                           ModelShapePolicy shapes,
                                                           OrtPrecision precision) {
  return std::make_unique<RemoteModel>(ir, std::move(what), std::move(shapes), precision);
}

uint64_t openvino_host_restart_count() { return host_restarts.load(); }

bool maybe_run_inference_host(int argc, char** argv) {
  if (argc != 3 || std::string_view(argv[1]) != "--grparse-inference-host") return false;
  Host host(std::atoi(argv[2]));
  host.serve();
  return true;
}

}  // namespace grparse

#else

namespace grparse {
bool openvino_process_isolation() { return false; }
uint64_t openvino_host_restart_count() { return 0; }
bool maybe_run_inference_host(int, char**) { return false; }
}  // namespace grparse

#endif
