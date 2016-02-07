#include "inspector_agent.h"

#include "platform/v8_inspector/public/V8Inspector.h"
#include "platform/inspector_protocol/FrontendChannel.h"
#include "platform/inspector_protocol/String16.h"
#include "platform/inspector_protocol/Values.h"

// Avoid conflicts between Blink and Node macros
// TODO: Remove once the Blink code switches to STL pointers
#undef ASSERT
#undef NO_RETURN

#include "env.h"
#include "env-inl.h"
#include "node.h"
#include "node_version.h"
#include "v8-platform.h"
#include "util.h"

#include <string.h>

// We need pid to use as ID with Chrome
#if defined(_MSC_VER)
#include <direct.h>
#include <io.h>
#define getpid GetCurrentProcessId
#else
#include <unistd.h>  // setuid, getuid
#endif


namespace node {
namespace inspector {

using blink::protocol::DictionaryValue;
using blink::protocol::String16;

static const char DEVTOOLS_PATH[] = "/node";

static double CurrentTime() {
  return 0.0;
}

static void DisposeAsyncCb(uv_handle_t* handle) {
  free(handle);
}

static void DisposeInspector(inspector_socket_t* socket, int status) {
  free(socket);
}

static void DisconnectAndDispose(inspector_socket_t* socket) {
  if (socket) {
    inspector_close(socket, DisposeInspector);
  }
}

static void OnBufferAlloc(uv_handle_t* handle, size_t len, uv_buf_t* buf) {
  if (len > 0) {
    buf->base = reinterpret_cast<char*>(malloc(len));
    CHECK_NE(buf->base, nullptr);
  }
  buf->len = len;
}

static void SendHttpResponse(inspector_socket_t* socket,
                        const char* response,
                        size_t len) {
  const char HEADERS[] = "HTTP/1.0 200 OK\r\n"
                         "Content-Type: application/json; charset=UTF-8\r\n"
                         "Cache-Control: no-cache\r\n"
                         "Content-Length: %ld\r\n"
                         "\r\n";
  char header[sizeof(HEADERS) + 20];
  int header_len = snprintf(header, sizeof(header), HEADERS, len);
  inspector_write(socket, header, header_len);
  inspector_write(socket, response, len);
}

static void SendVersionResponse(inspector_socket_t* socket) {
  const char VERSION_RESPONSE_TEMPLATE[] =
      "[ {"
      "  \"Browser\": \"node.js/%s\","
      "  \"Protocol-Version\": \"1.1\","
      "  \"User-Agent\": \"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36"
            "(KHTML, like Gecko) Chrome/45.0.2446.0 Safari/537.36\","
      "  \"WebKit-Version\": \"537.36 (@198122)\""
      "} ]";
  char buffer[sizeof(VERSION_RESPONSE_TEMPLATE) + 128];
  size_t len = snprintf(buffer, sizeof(buffer),
                 VERSION_RESPONSE_TEMPLATE, NODE_VERSION);
  ASSERT(len < sizeof(buffer));
  SendHttpResponse(socket, buffer, len);
}

static void SendTargentsListResponse(inspector_socket_t* socket) {
  const char LIST_RESPONSE_TEMPLATE[] =
      "[ {"
      "  \"description\": \"node.js instance\","
      "  \"devtoolsFrontendUrl\": "
            "\"https://chrome-devtools-frontend.appspot.com/serve_file/"
            "@4604d24a75168768584760ba56d175507941852f/inspector.html\","
      "  \"faviconUrl\": \"https://nodejs.org/static/favicon.ico\","
      "  \"id\": \"%d\","
      "  \"title\": \"%s\","
      "  \"type\": \"node\","
      "  \"webSocketDebuggerUrl\": \"ws://%s\""
      "} ]";
  char buffer[sizeof(LIST_RESPONSE_TEMPLATE) + 4096];
  char title[2048]; // uv_get_process_title trims the title if too long
  int err = uv_get_process_title(title, sizeof(title));
  ASSERT_EQ(0, err);
  size_t len = snprintf(buffer, sizeof(buffer), LIST_RESPONSE_TEMPLATE,
                        getpid(), title, DEVTOOLS_PATH);
  ASSERT(len < sizeof(buffer));
  SendHttpResponse(socket, buffer, len);
}

class DispatchOnInspectorBackendTask : public v8::Task {
public:
  DispatchOnInspectorBackendTask(Agent* agent, const char* message,
                                 ssize_t len) :
                                 agent_(agent),
                                 message_(message, len) {}

  void Run() override {
    agent_->inspector_->dispatchMessageFromFrontend(message_);
    uv_async_send(&agent_->dataWritten_);
  }

private:
  Agent* agent_;
  blink::V8Inspector* inspector_;
  String16 message_;
};

class ChannelImpl final : public blink::protocol::FrontendChannel {
public:
  ChannelImpl(Agent* agent): agent_(agent) {}
  virtual ~ChannelImpl() {}
private:
  virtual void sendProtocolResponse(int sessionId, int callId,
                                    PassOwnPtr<DictionaryValue> message) override {
    sendMessageToFrontend(message);
  }

  virtual void sendProtocolNotification(PassOwnPtr<DictionaryValue> message) override {
    sendMessageToFrontend(message);
  }

  virtual void flush() override { }

  void sendMessageToFrontend(PassOwnPtr<DictionaryValue> message) {
    agent_->write(message->toJSONString());
  }

  Agent* const agent_;
};

Agent::Agent(Environment* env) : port_(5858),
                                 wait_(false),
                                 parent_env_(env),
                                 client_socket_(nullptr),
                                 platform_(nullptr) {
  int err;

  err = uv_sem_init(&start_sem_, 0);
  CHECK_EQ(err, 0);
}

bool Agent::Start(v8::Platform* platform, int port, bool wait) {
  WTF::Partitions::initialize(nullptr);
  WTF::initialize(CurrentTime, nullptr, nullptr);

  auto env = parent_env();
  inspector_ = new blink::V8Inspector(env->isolate(), env->context(), platform);
  inspector_->connectFrontend(new ChannelImpl(this));

  int err;

  platform_ = platform;

  err = uv_loop_init(&child_loop_);
  if (err != 0)
    goto loop_init_failed;

  err = uv_async_init(env->event_loop(), &dataWritten_, nullptr);
  if (err != 0)
    goto async_init_failed;

  uv_unref((uv_handle_t*) &dataWritten_);

  port_ = port;
  wait_ = wait;

  err = uv_thread_create(&thread_,
                         reinterpret_cast<uv_thread_cb>(ThreadCb),
                         this);
  if (err != 0)
    goto thread_create_failed;
  uv_sem_wait(&start_sem_);

  return true;

 thread_create_failed:
  uv_close(reinterpret_cast<uv_handle_t*>(&dataWritten_), nullptr);

 async_init_failed:
  err = uv_loop_close(&child_loop_);
  CHECK_EQ(err, 0);

 loop_init_failed:
  return false;
}

void Agent::Stop() {
  DisconnectAndDispose(client_socket_);
  int err = uv_thread_join(&thread_);
  CHECK_EQ(err, 0);

  uv_run(&child_loop_, UV_RUN_NOWAIT);

  err = uv_loop_close(&child_loop_);
  CHECK_EQ(err, 0);
  delete inspector_;
}

void Agent::OnRemoteData(uv_stream_t* stream, ssize_t read, const uv_buf_t* b) {
  inspector_socket_t* socket =
      reinterpret_cast<inspector_socket_t*>(stream->data);
  Agent* agent = reinterpret_cast<Agent*>(socket->data);
  if (read > 0) {
    agent->platform_->CallOnForegroundThread(agent->parent_env()->isolate(),
        new DispatchOnInspectorBackendTask(agent, b->base, read - 1));
    uv_async_send(&agent->dataWritten_);
    free(b->base);
  } else if (read < 0) {
    if (agent->client_socket_ == socket) {
      agent->client_socket_ = nullptr;
    }
    free(socket);
  }
}

class AsyncWriteRequest {
public:
  AsyncWriteRequest(Agent* agent, const String16& message) :
                    agent_(agent), message_(message) {}
  void perform() {
    inspector_socket_t* socket = agent_->client_socket();
    if (socket) {
      inspector_write(socket, message_.utf8().c_str(), message_.length());
    }
  }
private:
  Agent* const agent_;
  const String16 message_;
};

void Agent::WriteCb(uv_async_t* async) {
  struct AsyncWriteRequest* req = (struct AsyncWriteRequest*) async->data;
  req->perform();
  delete req;
  uv_close((uv_handle_t*) async, DisposeAsyncCb);
}

void Agent::write(const String16& message) {
  uv_async_t* async = (uv_async_t*) malloc(sizeof(uv_async_t));
  ASSERT_NE(async, nullptr);
  uv_async_init(&child_loop_, async, WriteCb);
  async->data = new AsyncWriteRequest(this, message);
  ASSERT_EQ(0, uv_async_send(async));
}

void Agent::OnInspectorConnection(inspector_socket_t* socket) {
  if (client_socket_ && inspector_is_active(client_socket_)) {
    DisconnectAndDispose(client_socket_);
  }
  client_socket_ = socket;
  inspector_read_start(socket, OnBufferAlloc, OnRemoteData);
  uv_sem_post(&start_sem_);
}

bool Agent::RespondToGet(inspector_socket_t* socket, const char* path) {
  const char PATH_JSON[] = "/json";
  const char PATH_JSON_LIST[] = "/json/list";
  const char PATH_JSON_VERSION[] = "/json/version";
  int id;
  if (strncmp(PATH_JSON_VERSION, path, sizeof(PATH_JSON_VERSION)) == 0) {
    SendVersionResponse(socket);
  } else if (strncmp(PATH_JSON_LIST, path, sizeof(PATH_JSON_LIST)) == 0
             || strncmp(PATH_JSON, path, sizeof(PATH_JSON)) == 0)  {
    SendTargentsListResponse(socket);
  } else if (sscanf(path, "/json/activate/%d", &id) == 1 && id == getpid()) {
    const char TARGET_ACTIVATED[] = "Target activated";
    SendHttpResponse(socket, TARGET_ACTIVATED, sizeof(TARGET_ACTIVATED) - 1);
  } else {
    return false;
  }
  return true;
}

bool Agent::AcceptsConnection(inspector_socket_t* socket, const char* path) {
  return strncmp(DEVTOOLS_PATH, path, sizeof(DEVTOOLS_PATH)) == 0;
}

bool Agent::OnInspectorHandshake(inspector_socket_t* socket,
                                 enum inspector_handshake_event state,
                                 const char* path) {
  Agent* agent = reinterpret_cast<Agent*>(socket->data);
  switch(state) {
  case kInspectorHandshakeHttpGet:
    return agent->RespondToGet(socket, path);
  case kInspectorHandshakeUpgrading:
    return agent->AcceptsConnection(socket, path);
  case kInspectorHandshakeUpgraded:
    agent->OnInspectorConnection(socket);
    return true;
  case kInspectorHandshakeFailed:
    return false;
  default:
    ASSERT(false);
  }
}

void Agent::OnSocketConnection(uv_stream_t* server, int status) {
  if (status == 0) {
    inspector_socket_t* socket =
        (inspector_socket_t*) malloc(sizeof(inspector_socket_t));
    ASSERT_NE(nullptr, socket);
    memset(socket, 0, sizeof(inspector_socket_t));
    socket->data = server->data;
    if (inspector_accept(server, socket, OnInspectorHandshake) != 0) {
      free(socket);
    }
  }
}

void Agent::WorkerRun() {
  int err;
  sockaddr_in addr;
  uv_tcp_t server;
  uv_tcp_init(&child_loop_, &server);
  uv_ip4_addr("0.0.0.0", port_, &addr);
  server.data = this;
  err = uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
  if (err == 0) {
    err = uv_listen((uv_stream_t*) &server, 0, OnSocketConnection);
  }
  if (err == 0) {
    printf("Start Chrome Canary with the '--remote-debugging-targets=localhost:%d' "
           "command line option and navigate to 'chrome://inspect'\n", port_);
  } else {
    fprintf(stderr, "Unable to open devtools socket: %s\n", uv_strerror(err));
    assert(false);
  }
  if (!wait_) {
    uv_sem_post(&start_sem_);
  }
  uv_run(&child_loop_, UV_RUN_DEFAULT);
  uv_close((uv_handle_t*) &server, nullptr);
  uv_run(&child_loop_, UV_RUN_NOWAIT);
}

void Agent::ThreadCb(Agent* agent) {
  agent->WorkerRun();
}
}  // namespace debugger
}  // namespace node
